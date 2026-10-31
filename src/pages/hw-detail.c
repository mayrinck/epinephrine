#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-detail.h"
#include "hw-gpu-detail.h"
#include "hw-monitor.h"
#include "../epi-privilege.h"
#include "../epi-i18n.h"
#include "../window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "page-dashboard.h"

#define tr(x) epi_i18n_get (x)

#define SYSCTL_CONF_PATH "/etc/sysctl.conf"

/* vm.vfs_cache_pressure presets offered to the user (Preserve/Default/Free RAM) */
static const int cache_presets[] = { 50, 100, 200 };

/* ---- Motherboard Management helpers ---- */

static void on_smbus_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_stick_website_clicked (GtkButton *btn, gpointer user_data);
static void ram_stick_url_destroy (gpointer data, GClosure *closure);
static void on_mobo_website_clicked (GtkButton *btn, gpointer user_data);
static void on_ram_website_clicked (GtkButton *btn, gpointer user_data);

static char *
read_file (const char *path)
{
  gchar *content = NULL;
  if (!g_file_get_contents (path, &content, NULL, NULL))
    return NULL;
  g_strstrip (content);
  if (content[0] == '\0') { g_free (content); return NULL; }
  return content;
}

/* ── Chipset detection ── */

typedef struct {
  guint16 vendor_id;
  const char *name;
} PciVendorEntry;

static const PciVendorEntry pci_vendors[] = {
  { 0x1022, "AMD" },
  { 0x8086, "Intel" },
  { 0x10DE, "NVIDIA" },
  { 0x1002, "ATI/AMD" },
  { 0x1B36, "Red Hat / QEMU" },
  { 0x15AD, "VMware" },
  { 0x1414, "Microsoft" },
  { 0x1AF4, "Red Hat / VirtIO" },
};

static const char *
pci_vendor_name (guint16 vid)
{
  for (size_t i = 0; i < G_N_ELEMENTS (pci_vendors); i++)
    if (pci_vendors[i].vendor_id == vid)
      return pci_vendors[i].name;
  return NULL;
}

static void
detect_chipset (HardwarePage *page)
{
  if (page->mobo_chipset_vendor)
    return;

  GDir *dir = g_dir_open ("/sys/bus/pci/devices", 0, NULL);
  if (!dir)
    return;

  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      char class_path[512];
      g_snprintf (class_path, sizeof (class_path),
        "/sys/bus/pci/devices/%s/class", entry);
      gchar *class = NULL;
      if (!g_file_get_contents (class_path, &class, NULL, NULL))
        continue;
      g_strstrip (class);
      if (g_strcmp0 (class, "0x060100") != 0)
        { g_free (class); continue; }
      g_free (class);

      char vendor_path[512], device_path[512];
      g_snprintf (vendor_path, sizeof (vendor_path),
        "/sys/bus/pci/devices/%s/vendor", entry);
      g_snprintf (device_path, sizeof (device_path),
        "/sys/bus/pci/devices/%s/device", entry);

      char *vendor_str = read_file (vendor_path);
      char *device_str = read_file (device_path);

      if (vendor_str)
        {
          guint16 vid = (guint16) g_ascii_strtoull (vendor_str, NULL, 16);
          const char *vname = pci_vendor_name (vid);
          page->mobo_chipset_vendor = g_strdup (vname ? vname : vendor_str);
          g_free (vendor_str);
        }
      if (device_str)
        {
          page->mobo_chipset_model = g_strdup (device_str);
          g_free (device_str);
        }
      break;
    }
  g_dir_close (dir);
}

static gboolean
run_smbus_pkexec (gboolean enable, char **out_error)
{
  const char *cmd = enable ? "smbus enable" : "smbus disable";
  int exit_code = -1;
  GError *err = NULL;
  gboolean ok = epi_privilege_exec_sync (cmd, &exit_code, NULL, &err);


  if (ok && exit_code == 0) return TRUE;

  *out_error = err ? g_strdup (err->message)
                   : g_strdup_printf (tr ("Script exited with code %d"), exit_code);
  if (err) g_error_free (err);
  return FALSE;
}

static gboolean
grub_has_smbus_flag (void)
{
  char *content = read_file ("/etc/default/grub");
  if (!content) return FALSE;
  gboolean found = (strstr (content, "acpi_enforce_resources=lax") != NULL);
  g_free (content);
  return found;
}

static gboolean
cmdline_has_smbus_flag (void)
{
  char *content = read_file ("/proc/cmdline");
  if (!content) return FALSE;
  gboolean found = (strstr (content, "acpi_enforce_resources=lax") != NULL);
  g_free (content);
  return found;
}

static void
refresh_smbus_status (HardwarePage *page)
{
  gboolean has_grub = g_file_test ("/etc/default/grub", G_FILE_TEST_EXISTS);
  if (!has_grub)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), FALSE);
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
        tr ("GRUB not detected"));
      return;
    }

  if (!g_find_program_in_path ("pkexec"))
    {
      gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), FALSE);
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
        tr ("pkexec not available"));
      return;
    }

  gboolean persistent = grub_has_smbus_flag ();
  gboolean active_now = cmdline_has_smbus_flag ();

  g_signal_handlers_block_by_func (page->mobo_smbus_toggle,
    on_smbus_toggled, page);
  gtk_switch_set_active (GTK_SWITCH (page->mobo_smbus_toggle), persistent);
  g_signal_handlers_unblock_by_func (page->mobo_smbus_toggle,
    on_smbus_toggled, page);
  gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), TRUE);

  if (persistent && active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      tr ("Active (current boot)"));
  else if (persistent && !active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      tr ("Active (after reboot)"));
  else if (!persistent && active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      tr ("Active (temporary, will be lost on reboot)"));
  else
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      tr ("Inactive"));
}

static void
on_smbus_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  gboolean enable = gtk_switch_get_active (GTK_SWITCH (sw));

  gtk_widget_set_sensitive (GTK_WIDGET (sw), FALSE);
  gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
    tr ("Applying..."));

  char *error = NULL;
  gboolean ok = run_smbus_pkexec (enable, &error);

  if (!ok)
    {
      g_signal_handlers_block_by_func (sw, on_smbus_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (sw), !enable);
      g_signal_handlers_unblock_by_func (sw, on_smbus_toggled, page);
      char buf[256];
      g_snprintf (buf, sizeof (buf), "%s: %s",
        tr ("Error"), error ? error : "?");
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc), buf);
    }

  g_free (error);
  refresh_smbus_status (page);
}

void
hw_detail_build_motherboard (HardwarePage *page)
{
  page->mobo_detail_box = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (page->mobo_detail_box),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (page->mobo_detail_box), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (page->mobo_detail_box), TRUE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (content, GTK_ALIGN_START);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("motherboard-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->mobo_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->mobo_name_label, "title-1");
  gtk_widget_set_halign (page->mobo_name_label, GTK_ALIGN_START);

  page->mobo_bios_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->mobo_bios_label, "dim-label");
  gtk_widget_set_halign (page->mobo_bios_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->mobo_name_label);
  gtk_box_append (GTK_BOX (branding_text), page->mobo_bios_label);

  /* Product website button — standard branding position. No board URL
     database exists yet, so it stays disabled until one is registered. */
  page->mobo_website_btn = gtk_button_new_with_label (tr ("Product Website"));
  gtk_widget_set_valign (page->mobo_website_btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (page->mobo_website_btn, "flat");
  gtk_widget_add_css_class (page->mobo_website_btn, "accent");
  gtk_widget_set_sensitive (page->mobo_website_btn, FALSE);
  g_signal_connect (page->mobo_website_btn, "clicked",
    G_CALLBACK (on_mobo_website_clicked), page);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (branding_box), page->mobo_website_btn);
  gtk_box_append (GTK_BOX (content), branding_box);

  /* ======= General Information ======= */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("General Information"));
    adw_expander_row_set_subtitle (toggle, tr ("Board identification"));

    page->mobo_vendor_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_vendor_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_vendor_label, TRUE);
    AdwActionRow *vendor_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vendor_row), tr ("Vendor"));
    adw_action_row_add_suffix (vendor_row, page->mobo_vendor_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (vendor_row));

    page->mobo_model_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_model_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_model_label, TRUE);
    AdwActionRow *model_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (model_row), tr ("Model"));
    adw_action_row_add_suffix (model_row, page->mobo_model_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (model_row));

    page->mobo_revision_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_revision_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_revision_label, TRUE);
    AdwActionRow *revision_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (revision_row), tr ("Revision"));
    adw_action_row_add_suffix (revision_row, page->mobo_revision_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (revision_row));

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ======= BIOS Information ======= */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("BIOS Information"));
    adw_expander_row_set_subtitle (toggle, tr ("Firmware details"));

    page->mobo_bios_manufacturer_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_bios_manufacturer_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_bios_manufacturer_label, TRUE);
    AdwActionRow *bios_mfg_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_mfg_row), tr ("Manufacturer"));
    adw_action_row_add_suffix (bios_mfg_row, page->mobo_bios_manufacturer_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (bios_mfg_row));

    page->mobo_bios_detail_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_bios_detail_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_bios_detail_label, TRUE);
    AdwActionRow *bios_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_row), tr ("Version"));
    adw_action_row_add_suffix (bios_row, page->mobo_bios_detail_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (bios_row));

    page->mobo_bios_date_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_bios_date_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_bios_date_label, TRUE);
    AdwActionRow *bios_date_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_date_row), tr ("Date"));
    adw_action_row_add_suffix (bios_date_row, page->mobo_bios_date_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (bios_date_row));

    page->mobo_bios_romsize_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_bios_romsize_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_bios_romsize_label, TRUE);
    AdwActionRow *romsize_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (romsize_row), tr ("ROM Size"));
    adw_action_row_add_suffix (romsize_row, page->mobo_bios_romsize_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (romsize_row));

    page->mobo_bios_efi_pk_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_bios_efi_pk_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_bios_efi_pk_label, TRUE);
    gtk_label_set_selectable (GTK_LABEL (page->mobo_bios_efi_pk_label), TRUE);
    AdwActionRow *efi_pk_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (efi_pk_row), tr ("EFI PK"));
    adw_action_row_add_suffix (efi_pk_row, page->mobo_bios_efi_pk_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (efi_pk_row));

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ======= Chipset ======= */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("Chipset"));
    adw_expander_row_set_subtitle (toggle, tr ("Platform controller hub"));

    page->mobo_chipset_vendor_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_chipset_vendor_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_chipset_vendor_label, TRUE);
    AdwActionRow *cs_vendor_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cs_vendor_row), tr ("Provider"));
    adw_action_row_add_suffix (cs_vendor_row, page->mobo_chipset_vendor_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (cs_vendor_row));

    page->mobo_chipset_model_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->mobo_chipset_model_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->mobo_chipset_model_label, TRUE);
    AdwActionRow *cs_model_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cs_model_row), tr ("Model"));
    adw_action_row_add_suffix (cs_model_row, page->mobo_chipset_model_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (cs_model_row));

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ===== Motherboard Management ===== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("Motherboard Management"));
    adw_expander_row_set_subtitle (toggle, tr ("SMBus and management"));

    page->mobo_smbus_toggle = gtk_switch_new ();
    page->mobo_smbus_desc = gtk_label_new ("");
    gtk_widget_set_halign (page->mobo_smbus_desc, GTK_ALIGN_END);
    AdwActionRow *smbus_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (smbus_row), tr ("Unlock SMBus"));
    adw_action_row_set_subtitle (smbus_row,
      tr (
        "Unlocks the SMBus controller for direct memory access by software like OpenRGB."));
    gtk_widget_set_valign (page->mobo_smbus_toggle, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix (smbus_row, page->mobo_smbus_desc);
    adw_action_row_add_suffix (smbus_row, page->mobo_smbus_toggle);
    adw_expander_row_add_row (toggle, GTK_WIDGET (smbus_row));

    g_signal_connect (page->mobo_smbus_toggle, "notify::active",
      G_CALLBACK (on_smbus_toggled), page);

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (page->mobo_detail_box), content);
  gtk_stack_add_named (page->detail_stack, page->mobo_detail_box, "motherboard");
}

/* ====== Memory / Swap (vm sysctl) management ====== */

static void on_ram_swappiness_changed (GObject *widget, GParamSpec *pspec,
                                       gpointer user_data);
static void on_ram_cache_changed (AdwComboRow *row, GParamSpec *pspec,
                                  gpointer user_data);

/* Returns the value declared for `key` in /etc/sysctl.conf, or -1 if absent. */
static int
sysctl_conf_get (const char *key)
{
  char *contents = NULL;
  if (!g_file_get_contents (SYSCTL_CONF_PATH, &contents, NULL, NULL))
    return -1;

  int found = -1;
  char **lines = g_strsplit (contents, "\n", -1);
  for (int i = 0; lines[i] && found < 0; i++)
    {
      char *line = g_strstrip (lines[i]);
      if (line[0] == '#' || line[0] == '\0')
        continue;

      char *key_str = NULL, *val_str = NULL;
      char *eq = strchr (line, '=');
      if (eq)
        {
          *eq = '\0';
          key_str = g_strstrip (line);
          val_str = g_strstrip (eq + 1);
        }
      else
        {
          /* Also accept "key\tvalue" (whitespace separated) */
          char *sp = line;
          while (*sp && *sp != ' ' && *sp != '\t')
            sp++;
          if (*sp)
            {
              *sp = '\0';
              key_str = g_strstrip (line);
              val_str = g_strstrip (sp + 1);
            }
        }

      if (key_str && val_str && key_str[0] && val_str[0]
          && g_strcmp0 (key_str, key) == 0)
        found = atoi (val_str);
    }

  g_strfreev (lines);
  g_free (contents);
  return found;
}

/* Returns the live kernel value for `key` (e.g. "vm.swappiness") from procfs. */
static int
sysctl_live_get (const char *key)
{
  char *path = g_strdup (key);
  for (char *p = path; *p; p++)
    if (*p == '.') *p = '/';
  char *full = g_strdup_printf ("/proc/sys/%s", path);
  g_free (path);

  char *content = NULL;
  if (!g_file_get_contents (full, &content, NULL, NULL))
    {
      g_free (full);
      return -1;
    }
  g_free (full);

  g_strstrip (content);
  int val = atoi (content);
  g_free (content);
  return val;
}

/* True if a stripped sysctl line belongs to `key` (key followed by '=' or
 * whitespace or end of line). */
static gboolean
sysctl_line_is_key (const char *line, const char *key)
{
  size_t n = strlen (key);
  if (strncmp (line, key, n) != 0)
    return FALSE;
  char c = line[n];
  return (c == '\0' || c == '=' || c == ' ' || c == '\t');
}

static void
ram_save_sysctl_values (HardwarePage *page)
{
  int swappiness = (int) adw_spin_row_get_value (
    ADW_SPIN_ROW (page->ram_swap_spin));
  int cache_idx = (int) adw_combo_row_get_selected (
    ADW_COMBO_ROW (page->ram_cache_combo));
  if (cache_idx < 0 || cache_idx > 2)
    cache_idx = 1;
  int cache_pressure = cache_presets[cache_idx];

  /* Rebuild /etc/sysctl.conf, replacing our two keys in place. */
  char *orig = NULL;
  GString *out = g_string_new (NULL);
  g_file_get_contents (SYSCTL_CONF_PATH, &orig, NULL, NULL);
  gboolean wrote_swap = FALSE, wrote_cache = FALSE;

  if (orig)
    {
      char **lines = g_strsplit (orig, "\n", -1);
      for (int i = 0; lines[i]; i++)
        {
          char *stripped = g_strstrip (lines[i]);
          if (sysctl_line_is_key (stripped, "vm.swappiness"))
            {
              g_string_append_printf (out, "vm.swappiness = %d\n", swappiness);
              wrote_swap = TRUE;
            }
          else if (sysctl_line_is_key (stripped, "vm.vfs_cache_pressure"))
            {
              g_string_append_printf (out, "vm.vfs_cache_pressure = %d\n",
                cache_pressure);
              wrote_cache = TRUE;
            }
          else
            g_string_append_printf (out, "%s\n", lines[i]);
        }
      g_strfreev (lines);
      g_free (orig);
    }

  if (!wrote_swap)
    g_string_append_printf (out, "vm.swappiness = %d\n", swappiness);
  if (!wrote_cache)
    g_string_append_printf (out, "vm.vfs_cache_pressure = %d\n", cache_pressure);

  /* Write through the admin session */
  char tmp[] = "/tmp/epi-sysctl-XXXXXX";
  int fd = g_mkstemp (tmp);
  gboolean ok = FALSE;
  if (fd >= 0)
    {
      FILE *f = fdopen (fd, "w");
      if (f)
        {
          fputs (out->str, f);
          fclose (f);
        }
      else
        close (fd);
    }

  int exit_code = -1;
  GError *err = NULL;
  if (fd >= 0)
    {
      char *install_cmd = g_strdup_printf (
        "install -m 0644 '%s' %s", tmp, SYSCTL_CONF_PATH);
      ok = epi_privilege_exec_sync (install_cmd, &exit_code, NULL, &err)
           && exit_code == 0;
      g_free (install_cmd);
      unlink (tmp);
      g_clear_error (&err);

      if (ok)
        {
          char *apply_cmd = g_strdup_printf (
            "sysctl -w vm.swappiness=%d vm.vfs_cache_pressure=%d",
            swappiness, cache_pressure);
          int ec2 = -1;
          GError *err2 = NULL;
          if (!epi_privilege_exec_sync (apply_cmd, &ec2, NULL, &err2) || ec2 != 0)
            ok = FALSE;
          g_clear_error (&err2);
          g_free (apply_cmd);
        }
    }

  g_string_free (out, TRUE);

  if (ok)
    {
      page->ram_swap_saved = swappiness;
      page->ram_cache_saved = cache_pressure;
    }
  else
    {
      dashboard_show_error_toast (tr ("Failed to apply memory settings"),
        err && err->message ? err->message : tr ("Unknown error"));
      g_clear_error (&err);

      /* Revert widgets to the last persisted state */
      g_signal_handlers_block_by_func (page->ram_swap_spin,
        on_ram_swappiness_changed, page);
      g_signal_handlers_block_by_func (page->ram_cache_combo,
        on_ram_cache_changed, page);
      adw_spin_row_set_value (ADW_SPIN_ROW (page->ram_swap_spin),
        page->ram_swap_saved);
      adw_combo_row_set_selected (ADW_COMBO_ROW (page->ram_cache_combo),
        page->ram_cache_saved);
      g_signal_handlers_unblock_by_func (page->ram_swap_spin,
        on_ram_swappiness_changed, page);
      g_signal_handlers_unblock_by_func (page->ram_cache_combo,
        on_ram_cache_changed, page);
    }
}

static void
on_ram_swappiness_changed (GObject *widget, GParamSpec *pspec,
                           gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  if (epi_i18n_in_apply ())
    return;
  int val = (int) adw_spin_row_get_value (ADW_SPIN_ROW (widget));
  if (val == page->ram_swap_saved)
    return;
  ram_save_sysctl_values (page);
}

static void
on_ram_cache_changed (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  if (epi_i18n_in_apply ())
    return;
  int idx = (int) adw_combo_row_get_selected (row);
  if (idx < 0 || idx > 2)
    idx = 1;
  if (cache_presets[idx] == page->ram_cache_saved)
    return;
  ram_save_sysctl_values (page);
}

void
hw_detail_build_rams (HardwarePage *page)
{
  page->ram_detail_box = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (page->ram_detail_box),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (page->ram_detail_box), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (page->ram_detail_box), TRUE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (content, GTK_ALIGN_START);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("memory-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->ram_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->ram_name_label, "title-1");
  gtk_widget_set_halign (page->ram_name_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->ram_name_label);

  /* Product website button — standard branding position; enabled when a
     registered manufacturer URL is found for an installed module. */
  page->ram_website_btn = gtk_button_new_with_label (tr ("Product Website"));
  gtk_widget_set_valign (page->ram_website_btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (page->ram_website_btn, "flat");
  gtk_widget_add_css_class (page->ram_website_btn, "accent");
  gtk_widget_set_sensitive (page->ram_website_btn, FALSE);
  g_signal_connect (page->ram_website_btn, "clicked",
    G_CALLBACK (on_ram_website_clicked), page);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (branding_box), page->ram_website_btn);
  gtk_box_append (GTK_BOX (content), branding_box);

  /* ====== Monitor Group (right after branding) ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (wrapper, tr ("Monitor"));

    GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start (panel, 8);
    gtk_widget_set_margin_bottom (panel, 8);

    page->ram_graph = gtk_drawing_area_new ();
    gtk_widget_set_hexpand (page->ram_graph, TRUE);
    gtk_widget_set_size_request (page->ram_graph, -1, 160);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->ram_graph),
      hw_ram_monitor_graph_draw, page, NULL);
    gtk_box_append (GTK_BOX (panel), page->ram_graph);

    /* Legend rows (dot + name + live value) */
    static const char *ram_legend_names[RAM_MON_METRICS] = {
      "RAM Usage", "Swap Usage"
    };
    for (int i = 0; i < RAM_MON_METRICS; i++)
      {
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_top (row, 1);
        gtk_widget_set_margin_bottom (row, 1);

        GtkWidget *dot = gtk_drawing_area_new ();
        gtk_widget_set_size_request (dot, 8, 8);
        gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
        /* Metric colors: 1 = green (RAM usage), 5 = yellow (swap usage) */
        gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot),
          hw_monitor_metric_dot_draw, GINT_TO_POINTER (i == 0 ? 1 : 5), NULL);

        GtkWidget *lbl = gtk_label_new (tr (ram_legend_names[i]));
        gtk_widget_set_halign (lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand (lbl, TRUE);
        gtk_widget_add_css_class (lbl, "dim-label");

        GtkWidget *val = gtk_label_new ("—");
        gtk_widget_set_halign (val, GTK_ALIGN_END);

        gtk_box_append (GTK_BOX (row), dot);
        gtk_box_append (GTK_BOX (row), lbl);
        gtk_box_append (GTK_BOX (row), val);
        gtk_box_append (GTK_BOX (panel), row);

        page->ram_monitor_labels[i] = lbl;
        page->ram_monitor_values[i] = val;
        page->ram_monitor_avail[i] = FALSE;
      }

    adw_preferences_group_add (wrapper, GTK_WIDGET (panel));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ====== Memory / Swap Management ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle),
      tr ("Memory/Swap Management"));
    adw_expander_row_set_subtitle (toggle,
      tr ("Swap and kernel memory-pressure tuning"));

    /* Swappiness value (0-200, auto-saved) */
    GtkAdjustment *sw_adj = gtk_adjustment_new (60, 0, 200, 1, 10, 0);
    page->ram_swap_spin = adw_spin_row_new (sw_adj, 0.25, 0);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (page->ram_swap_spin),
      "Swappiness");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (page->ram_swap_spin),
      "Defines the kernel's balance choice between ways of freeing RAM. The lower the value, the less Swap is favored.");
    adw_spin_row_set_numeric (ADW_SPIN_ROW (page->ram_swap_spin), TRUE);
    gtk_widget_set_tooltip_text (GTK_WIDGET (page->ram_swap_spin),
      "vm.swappiness");
    adw_expander_row_add_row (toggle, GTK_WIDGET (page->ram_swap_spin));

    /* Cache pressure preset (Conservar=50, Padrão=100, Liberar RAM=200) */
    static const char *cache_options[] = {
      "Preserve", "Default", "Free RAM", NULL
    };
    GtkStringList *cache_model = gtk_string_list_new (cache_options);

    page->ram_cache_combo = GTK_WIDGET (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (page->ram_cache_combo),
      "Cache Pressure");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (page->ram_cache_combo),
      "Defines the pressure (priority) that the memory management system applies when trying to deallocate the VFS cache (dentries/inodes) relative to file page cache and application anonymous memory.");
    gtk_widget_set_tooltip_text (GTK_WIDGET (page->ram_cache_combo),
      "vm.vfs_cache_pressure");
    adw_combo_row_set_model (ADW_COMBO_ROW (page->ram_cache_combo),
      G_LIST_MODEL (cache_model));
    g_object_unref (cache_model);
    adw_expander_row_add_row (toggle, GTK_WIDGET (page->ram_cache_combo));

    /* Load effective values: /etc/sysctl.conf first, kernel default fallback */
    int swap_effective = sysctl_conf_get ("vm.swappiness");
    if (swap_effective < 0)
      swap_effective = sysctl_live_get ("vm.swappiness");
    page->ram_swap_saved = (swap_effective < 0) ? 60 : swap_effective;

    int cache_effective = sysctl_conf_get ("vm.vfs_cache_pressure");
    if (cache_effective < 0)
      cache_effective = sysctl_live_get ("vm.vfs_cache_pressure");
    page->ram_cache_saved = (cache_effective < 0) ? 100 : cache_effective;

    int cache_idx = 1; /* Default */
    if (cache_effective < 0)
      cache_idx = 1;
    else if (cache_effective <= 75)
      cache_idx = 0;
    else if (cache_effective > 150)
      cache_idx = 2;

    g_signal_handlers_block_by_func (page->ram_swap_spin,
      on_ram_swappiness_changed, page);
    adw_spin_row_set_value (ADW_SPIN_ROW (page->ram_swap_spin),
      page->ram_swap_saved);
    g_signal_handlers_unblock_by_func (page->ram_swap_spin,
      on_ram_swappiness_changed, page);

    g_signal_handlers_block_by_func (page->ram_cache_combo,
      on_ram_cache_changed, page);
    adw_combo_row_set_selected (ADW_COMBO_ROW (page->ram_cache_combo), cache_idx);
    g_signal_handlers_unblock_by_func (page->ram_cache_combo,
      on_ram_cache_changed, page);

    g_signal_connect (page->ram_swap_spin, "notify::value",
      G_CALLBACK (on_ram_swappiness_changed), page);
    g_signal_connect (page->ram_cache_combo, "notify::selected",
      G_CALLBACK (on_ram_cache_changed), page);

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ====== Memory Information Group ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle),
      tr ("Memory Information"));
    adw_expander_row_set_subtitle (toggle,
      tr ("Technical data of the RAM memory"));

    page->ram_total_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_total_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_total_label, TRUE);
    AdwActionRow *total_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (total_row), tr ("Total Memory"));
    adw_action_row_add_suffix (total_row, page->ram_total_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (total_row));

    page->ram_slots_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_slots_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_slots_label, TRUE);
    AdwActionRow *slots_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (slots_row), tr ("Memory Slots"));
    adw_action_row_add_suffix (slots_row, page->ram_slots_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (slots_row));

    page->ram_tech_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_tech_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_tech_label, TRUE);
    AdwActionRow *tech_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (tech_row), tr ("Technology"));
    adw_action_row_add_suffix (tech_row, page->ram_tech_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (tech_row));

    page->ram_freq_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_freq_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_freq_label, TRUE);
    AdwActionRow *freq_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (freq_row), tr ("RAM Frequency"));
    adw_action_row_add_suffix (freq_row, page->ram_freq_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (freq_row));
adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  /* ====== Memory Stick Information Group (below Memory Information) ====== */
  {
    AdwPreferencesGroup *group =
      ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (group, tr ("Memory Stick Information"));
    adw_preferences_group_set_description (group,
      tr ("Individual data of each installed memory stick"));
    page->ram_sticks_group = GTK_WIDGET (group);
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (group));
  }

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (page->ram_detail_box), content);
  gtk_stack_add_named (page->detail_stack, page->ram_detail_box, "ram");
}

void
hw_detail_show_motherboard (HardwarePage *page)
{
  gtk_stack_set_visible_child_name (page->detail_stack, "motherboard");

  /* Name */
  char *full = g_strdup_printf ("%s %s", page->mobo_vendor, page->mobo_model);
  gtk_label_set_text (GTK_LABEL (page->mobo_name_label), full);
  g_free (full);

  /* BIOS info */
  char *bios_str = g_strdup_printf ("%s %s",
    tr ("BIOS Version:"),
    page->mobo_bios_version);
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_label), bios_str);
  g_free (bios_str);

  /* General Information */
  gtk_label_set_text (GTK_LABEL (page->mobo_vendor_label), page->mobo_vendor ? page->mobo_vendor : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_model_label), page->mobo_model ? page->mobo_model : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_revision_label), page->mobo_revision ? page->mobo_revision : "—");

  /* BIOS Information */
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_manufacturer_label), page->mobo_bios_vendor ? page->mobo_bios_vendor : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_detail_label), page->mobo_bios_version ? page->mobo_bios_version : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_date_label), page->mobo_bios_date ? page->mobo_bios_date : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_romsize_label), page->mobo_bios_romsize ? page->mobo_bios_romsize : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_bios_efi_pk_label), page->mobo_bios_efi_pk ? page->mobo_bios_efi_pk : "—");

  /* Chipset */
  detect_chipset (page);
  gtk_label_set_text (GTK_LABEL (page->mobo_chipset_vendor_label), page->mobo_chipset_vendor ? page->mobo_chipset_vendor : "—");
  gtk_label_set_text (GTK_LABEL (page->mobo_chipset_model_label), page->mobo_chipset_model ? page->mobo_chipset_model : "—");

  /* Refresh SMBus status */
  refresh_smbus_status (page);
}

/* Append one "label → value" row to a stick expander, skipping empty or
   meaningless SMBIOS placeholders. When `url` is non-NULL the row gets a
   trailing planet button that opens the module's official product page. */
static void
stick_add_field_row (AdwExpanderRow *exp, const char *title, const char *value,
                     const char *url)
{
  if (!value || !value[0]) return;
  if (g_strcmp0 (value, "Not Specified") == 0
      || g_strcmp0 (value, "Unknown") == 0
      || g_strcmp0 (value, "No Module Installed") == 0)
    return;

  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);

  if (url && url[0])
    {
      GtkWidget *btn = gtk_button_new_from_icon_name ("planet-symbolic");
      gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (btn, "flat");
      gtk_widget_set_tooltip_text (btn, tr ("Product Website"));
      g_signal_connect_data (btn, "clicked",
        G_CALLBACK (on_stick_website_clicked),
        g_strdup (url), ram_stick_url_destroy, 0);
      adw_action_row_add_suffix (row, btn);
    }

  adw_expander_row_add_row (exp, GTK_WIDGET (row));
}

char *
ram_size_mb_str (uint64_t mb)
{
  if (mb >= 1024)
    {
      double gb = mb / 1024.0;
      if (mb % 1024 == 0)
        return g_strdup_printf ("%.0f GB", gb);
      return g_strdup_printf ("%.1f GB", gb);
    }
  return g_strdup_printf ("%.0f MB", (double) mb);
}

gboolean
ram_parse_size_mb (const char *size, uint64_t *out_mb)
{
  if (!size || !size[0] || !out_mb)
    return FALSE;
  char *end = NULL;
  double v = strtod (size, &end);
  if (end == size)
    return FALSE;
  while (end && *end == ' ')
    end++;
  if (g_str_has_prefix (end, "GB"))
    *out_mb = (uint64_t) (v * 1024.0);
  else if (g_str_has_prefix (end, "MB") || *end == '\0')
    *out_mb = (uint64_t) v;
  else
    return FALSE;
  return TRUE;
}

/* ---- RAM module index (database/RAM_INDEX.md) ---- */

static GHashTable *ram_idx_exact;  /* "manufacturer\x1fpart" → url */
static GHashTable *ram_idx_mfg;    /* "manufacturer" → url (module not matched) */
static gboolean ram_idx_loaded = FALSE;

/* Normalize a manufacturer/part number key: lowercase, strip leading/trailing
   whitespace and collapse inner whitespace runs to a single space. */
static gchar *
ram_idx_norm (const char *s)
{
  GString *out = g_string_new (NULL);
  gboolean sp = FALSE;
  if (s)
    for (const char *p = s; *p; p++)
      {
        if (g_ascii_isspace (*p)) { sp = TRUE; continue; }
        if (sp && out->len)
          g_string_append_c (out, ' ');
        sp = FALSE;
        g_string_append_c (out, g_ascii_tolower (*p));
      }
  return g_string_free (out, FALSE);
}

static void
ram_idx_ensure_loaded (void)
{
  if (ram_idx_loaded)
    return;
  ram_idx_exact = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  ram_idx_mfg = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/RAM_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    g_file_get_contents (DATADIR "/epinephrine/database/RAM_INDEX.md",
      &content, NULL, NULL);
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "database", "RAM_INDEX.md", NULL);
      g_file_get_contents (rel, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
    }

  if (!content)
    {
      ram_idx_loaded = TRUE;
      return;
    }

  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);
  g_free (content);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---"))
        {
          in_table = TRUE;
          continue;
        }
      if (!in_table || line[0] != '|')
        continue;

      /* Memory Name | Manufacturer | Part Number | Official Product Page */
      char **cols = g_strsplit (line, "|", 0);
      if (!cols[4])
        {
          g_strfreev (cols);
          continue;
        }

      char *mfg  = g_strstrip (cols[2]);
      char *part = g_strstrip (cols[3]);
      char *url  = g_strstrip (cols[4]);
      if (!mfg[0] || !url[0])
        {
          g_strfreev (cols);
          continue;
        }

      gchar *k_mfg = ram_idx_norm (mfg);
      if (part[0])
        {
          gchar *k_part = ram_idx_norm (part);
          gchar *key = g_strdup_printf ("%s\x1f%s", k_mfg, k_part);
          if (!g_hash_table_contains (ram_idx_exact, key))
            g_hash_table_insert (ram_idx_exact, key, g_strdup (url));
          else
            g_free (key);
          g_free (k_part);
        }
      else if (!g_hash_table_contains (ram_idx_mfg, k_mfg))
        g_hash_table_insert (ram_idx_mfg, k_mfg, g_strdup (url));
      else
        g_free (k_mfg);

      g_strfreev (cols);
    }
  g_strfreev (lines);

  ram_idx_loaded = TRUE;
}

char *
epi_ram_find_url (const char *manufacturer, const char *part_number)
{
  ram_idx_ensure_loaded ();
  if (!ram_idx_exact || !manufacturer || !manufacturer[0])
    return NULL;

  gchar *k_mfg = ram_idx_norm (manufacturer);

  if (part_number && part_number[0])
    {
      gchar *k_part = ram_idx_norm (part_number);
      gchar *key = g_strdup_printf ("%s\x1f%s", k_mfg, k_part);
      const char *url = g_hash_table_lookup (ram_idx_exact, key);
      g_free (key);
      g_free (k_part);
      if (url)
        {
          g_free (k_mfg);
          return g_strdup (url);
        }
    }

  /* Fall back to a manufacturer-level page when the module is not matched. */
  const char *murl = g_hash_table_lookup (ram_idx_mfg, k_mfg);
  g_free (k_mfg);
  return murl ? g_strdup (murl) : NULL;
}

static void
on_stick_website_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  const char *url = user_data;
  if (!url || !url[0])
    return;
  GError *err = NULL;
  g_app_info_launch_default_for_uri (url, NULL, &err);
  if (err)
    {
      char *details = g_strdup_printf (tr ("Could not open URL: %s\n\nError: %s"), url, err->message);
      dashboard_show_error_toast (tr ("Could not open URL"), details);
      g_free (details);
      g_error_free (err);
    }
}

static void
ram_stick_url_destroy (gpointer data, GClosure *closure)
{
  (void) closure;
  g_free (data);
}

static void
on_mobo_website_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  if (!page->mobo_url || !page->mobo_url[0]) return;
  GError *err = NULL;
  g_app_info_launch_default_for_uri (page->mobo_url, NULL, &err);
  if (err)
    {
      char *details = g_strdup_printf (tr ("Could not open URL: %s\n\nError: %s"), page->mobo_url, err->message);
      dashboard_show_error_toast (tr ("Could not open URL"), details);
      g_free (details);
      g_error_free (err);
    }
}

static void
on_ram_website_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  if (!page->ram_url || !page->ram_url[0]) return;
  GError *err = NULL;
  g_app_info_launch_default_for_uri (page->ram_url, NULL, &err);
  if (err)
    {
      char *details = g_strdup_printf (tr ("Could not open URL: %s\n\nError: %s"), page->ram_url, err->message);
      dashboard_show_error_toast (tr ("Could not open URL"), details);
      g_free (details);
      g_error_free (err);
    }
}

/* Remove every stick expander previously added to ram_sticks_group so a
   re-scan (or a language change) never duplicates the section. */
static void
clear_ram_stick_rows (HardwarePage *page)
{
  if (!page->ram_stick_rows)
    return;
#if defined (ADW_CHECK_VERSION) && ADW_CHECK_VERSION (1, 5, 0)
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (page->ram_sticks_group);
  for (guint i = 0; i < page->ram_stick_rows->len; i++)
    adw_preferences_group_remove (group,
      g_ptr_array_index (page->ram_stick_rows, i));
#else
  /* libadwaita < 1.5 has no remove API: reach the internal GtkListBox and
     clear it directly (widget layout is stable across versions). */
  GtkWidget *child = gtk_widget_get_first_child (page->ram_sticks_group);
  while (child)
    {
      GtkWidget *sub = gtk_widget_get_first_child (child);
      while (sub)
        {
          if (GTK_IS_LIST_BOX (sub))
            {
              gtk_list_box_remove_all (GTK_LIST_BOX (sub));
              break;
            }
          sub = gtk_widget_get_next_sibling (sub);
        }
      child = gtk_widget_get_next_sibling (child);
    }
#endif
  g_ptr_array_set_size (page->ram_stick_rows, 0);
}

void
hw_detail_show_ram (HardwarePage *page, const char *device_name)
{
  gtk_stack_set_visible_child_name (page->detail_stack, "ram");

  /* Name */
  gtk_label_set_text (GTK_LABEL (page->ram_name_label),
    device_name ? device_name : "");

  /* Product website — enabled when the first installed module has a
     registered manufacturer URL in RAM_INDEX. */
  g_free (page->ram_url);
  page->ram_url = NULL;
  if (page->ram_sticks && page->ram_sticks->len > 0)
    {
      const RamStick *s = g_ptr_array_index (page->ram_sticks, 0);
      page->ram_url = epi_ram_find_url (s->manufacturer, s->part_number);
    }
  gtk_widget_set_sensitive (page->ram_website_btn, page->ram_url != NULL);

  /* Total Memory */
  char *total_str = ram_size_mb_str (page->ram_total_bytes >> 20);
  gtk_label_set_text (GTK_LABEL (page->ram_total_label), total_str);
  g_free (total_str);

  /* Technology */
  gtk_label_set_text (GTK_LABEL (page->ram_tech_label),
    page->ram_technology ? page->ram_technology : "—");

  /* RAM Frequency */
  if (page->ram_freq_mhz > 0)
    {
      char freq_str[32];
      g_snprintf (freq_str, sizeof (freq_str), "%ld MHz", (long) page->ram_freq_mhz);
      gtk_label_set_text (GTK_LABEL (page->ram_freq_label), freq_str);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->ram_freq_label), "—");

  /* Memory Slots: "used / total" */
  if (page->ram_slots_used > 0 && page->ram_slots_total > 0)
    {
      char slots[32];
      g_snprintf (slots, sizeof (slots), "%d / %d",
        page->ram_slots_used, page->ram_slots_total);
      gtk_label_set_text (GTK_LABEL (page->ram_slots_label), slots);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->ram_slots_label), "—");

  /* One expander row per installed stick, below the general info */
  {
    /* Remove any rows added by a previous call so the section never
       duplicates (e.g. after a device re-scan or a language change). */
    clear_ram_stick_rows (page);

    if (page->ram_sticks)
      {
        for (guint i = 0; i < page->ram_sticks->len; i++)
          {
            RamStick *s = g_ptr_array_index (page->ram_sticks, i);

            char title[48];
            g_snprintf (title, sizeof (title), "%s #%u",
              tr ("Memory Stick"), i + 1);

            /* Normalize a dmidecode size like "16384 MB" to "16 GB" */
            uint64_t size_mb = 0;
            char *size_str = ram_parse_size_mb (s->size, &size_mb)
              ? ram_size_mb_str (size_mb) : g_strdup (s->size ? s->size : "");

            AdwExpanderRow *exp = ADW_EXPANDER_ROW (adw_expander_row_new ());
            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (exp), title);
            adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (exp), size_str);
            adw_expander_row_set_expanded (exp, FALSE);

            /* Official product page for this module (when registered);
               it is attached to the Manufacturer row as a planet button. */
            g_autofree char *url = epi_ram_find_url (s->manufacturer,
              s->part_number);

            stick_add_field_row (exp, tr ("Size"), size_str, NULL);
            stick_add_field_row (exp, tr ("Form Factor"), s->form_factor, NULL);
            stick_add_field_row (exp, tr ("Type"), s->type, NULL);
            stick_add_field_row (exp, tr ("Type Detail"), s->type_detail, NULL);
            stick_add_field_row (exp, tr ("Manufacturer"), s->manufacturer, url);
            stick_add_field_row (exp, tr ("Part Number"), s->part_number, NULL);
            stick_add_field_row (exp, tr ("Serial Number"), s->serial_number, NULL);
            stick_add_field_row (exp, tr ("Rank"), s->rank, NULL);
            stick_add_field_row (exp, tr ("Speed"), s->speed, NULL);
            stick_add_field_row (exp, tr ("Configured Memory Speed"), s->configured_speed, NULL);
            stick_add_field_row (exp, tr ("Locator"), s->locator, NULL);
            stick_add_field_row (exp, tr ("Bank Locator"), s->bank_locator, NULL);
            stick_add_field_row (exp, tr ("Total Width"), s->total_width, NULL);
            stick_add_field_row (exp, tr ("Data Width"), s->data_width, NULL);

            g_free (size_str);

            adw_preferences_group_add (
              ADW_PREFERENCES_GROUP (page->ram_sticks_group), GTK_WIDGET (exp));
            if (page->ram_stick_rows)
              g_ptr_array_add (page->ram_stick_rows, exp);
          }
      }
  }

  /* Start RAM monitor (graph speed aware) */
  hw_ram_monitor_start (page);
}
