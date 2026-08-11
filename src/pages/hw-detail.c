#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-detail.h"
#include "hw-gpu-detail.h"
#include "hw-monitor.h"
#include "../epi-privilege.h"
#include "../epi-i18n.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ---- Motherboard Management helpers ---- */

static void on_smbus_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);

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

static const char smbus_enable_script[] =
  "#!/bin/sh\n"
  "cp /etc/default/grub /etc/default/grub.bak.epi 2>/dev/null || true\n"
  "if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub 2>/dev/null; then\n"
  "  if grep -q 'acpi_enforce_resources=lax' /etc/default/grub 2>/dev/null; then\n"
  "    rm -f /etc/default/grub.bak.epi; exit 0\n"
  "  fi\n"
  "  sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=\"\\(.*\\)\"/GRUB_CMDLINE_LINUX_DEFAULT=\"\\1 acpi_enforce_resources=lax\"/' /etc/default/grub\n"
  "else\n"
  "  echo 'GRUB_CMDLINE_LINUX_DEFAULT=\"quiet splash acpi_enforce_resources=lax\"' >> /etc/default/grub\n"
  "fi\n"
  "r=0\n"
  "if command -v update-grub >/dev/null 2>&1; then update-grub; r=$?\n"
  "elif command -v grub2-mkconfig >/dev/null 2>&1; then grub2-mkconfig -o /boot/grub2/grub.cfg; r=$?\n"
  "elif command -v grub-mkconfig >/dev/null 2>&1; then grub-mkconfig -o /boot/grub/grub.cfg; r=$?\n"
  "else r=1; fi\n"
  "if [ \"$r\" -ne 0 ]; then\n"
  "  cp /etc/default/grub.bak.epi /etc/default/grub 2>/dev/null || true\n"
  "fi\n"
  "rm -f /etc/default/grub.bak.epi\n"
  "exit $r\n";

static const char smbus_disable_script[] =
  "#!/bin/sh\n"
  "cp /etc/default/grub /etc/default/grub.bak.epi 2>/dev/null || true\n"
  "sed -i 's/[[:space:]]*acpi_enforce_resources=lax//g' /etc/default/grub\n"
  "sed -i 's/^acpi_enforce_resources=lax[[:space:]]*//g' /etc/default/grub\n"
  "r=0\n"
  "if command -v update-grub >/dev/null 2>&1; then update-grub; r=$?\n"
  "elif command -v grub2-mkconfig >/dev/null 2>&1; then grub2-mkconfig -o /boot/grub2/grub.cfg; r=$?\n"
  "elif command -v grub-mkconfig >/dev/null 2>&1; then grub-mkconfig -o /boot/grub/grub.cfg; r=$?\n"
  "else r=1; fi\n"
  "if [ \"$r\" -ne 0 ]; then\n"
  "  cp /etc/default/grub.bak.epi /etc/default/grub 2>/dev/null || true\n"
  "fi\n"
  "rm -f /etc/default/grub.bak.epi\n"
  "exit $r\n";

static gboolean
run_smbus_pkexec (gboolean enable, char **out_error)
{
  int fd;
  char tmp[] = "/tmp/epi-smbus-XXXXXX";
  fd = g_mkstemp (tmp);
  if (fd < 0) { *out_error = g_strdup (tr ("Failed to create temp file")); return FALSE; }

  const char *script = enable ? smbus_enable_script : smbus_disable_script;
  gsize len = strlen (script);
  if (write (fd, script, len) != (ssize_t) len)
    {
      close (fd); unlink (tmp);
      *out_error = g_strdup (tr ("Failed to write temp script"));
      return FALSE;
    }
  close (fd);

  char *cmd = g_strdup_printf ("bash '%s'", tmp);
  int exit_code = -1;
  GError *err = NULL;
  gboolean ok = epi_privilege_exec_sync (cmd, &exit_code, NULL, &err);

  unlink (tmp);
  g_free (cmd);

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
hw_detail_update_lighting_section (GtkLabel *label)
{
  /* Detect ASUS Aura USB Motherboard controller (VID 0B05, PID 18F3) */
  char *result = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "/bin/sh", "-c", "lsusb -d 0b05:18f3 2>/dev/null | head -1", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &result, NULL, NULL, NULL);
  if (result) { g_strstrip (result); if (result[0] == '\0') { g_free (result); result = NULL; } }
  gboolean has_usb_aura = (result != NULL);
  g_free (result);

  if (has_usb_aura)
    gtk_label_set_text (label,
      tr ("ASUS Aura controller detected."));
  else
    gtk_label_set_text (label,
      tr (
        "The app couldn't find any light source to manage."));
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

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
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

  /* ====== Lighting Controls ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("Lighting Controls"));
    adw_expander_row_set_subtitle (toggle, tr ("RGB and LED management"));

    if (g_find_program_in_path ("openrgb"))
      {
        GtkWidget *openrgb_btn = gtk_button_new_with_label (
          tr ("Edit on OpenRGB"));
        gtk_widget_add_css_class (openrgb_btn, "accent");
        gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
        g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (hw_openrgb_launch), NULL);
        adw_expander_row_add_row (toggle, openrgb_btn);
      }

    page->mobo_lighting_status = gtk_label_new ("");
    gtk_widget_set_halign (page->mobo_lighting_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand (page->mobo_lighting_status, TRUE);
    gtk_label_set_wrap (GTK_LABEL (page->mobo_lighting_status), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (page->mobo_lighting_status), 60);
    adw_expander_row_add_row (toggle, page->mobo_lighting_status);

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
  }

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (page->mobo_detail_box), content);
  gtk_stack_add_named (page->detail_stack, page->mobo_detail_box, "motherboard");
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

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (content), branding_box);

  /* ====== Memory Usage Group ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("Memory Usage"));
    adw_expander_row_set_subtitle (toggle, tr ("RAM utilization"));

    page->ram_total_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_total_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_total_label, TRUE);
    AdwActionRow *total_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (total_row), tr ("Total Memory"));
    adw_action_row_add_suffix (total_row, page->ram_total_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (total_row));

    page->ram_used_label = gtk_label_new ("—");
    gtk_widget_set_halign (page->ram_used_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->ram_used_label, TRUE);
    AdwActionRow *used_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (used_row), tr ("Usage"));
    adw_action_row_add_suffix (used_row, page->ram_used_label);
    adw_expander_row_add_row (toggle, GTK_WIDGET (used_row));

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

  /* ====== Lighting Controls Group ====== */
  {
    AdwPreferencesGroup *wrapper = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwExpanderRow *toggle = ADW_EXPANDER_ROW (adw_expander_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toggle), tr ("Lighting Controls"));
    adw_expander_row_set_subtitle (toggle, tr ("RGB and LED management"));

    if (g_find_program_in_path ("openrgb"))
      {
        GtkWidget *openrgb_btn = gtk_button_new_with_label (
          tr ("Edit on OpenRGB"));
        gtk_widget_add_css_class (openrgb_btn, "accent");
        gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
        g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (hw_openrgb_launch), NULL);
        adw_expander_row_add_row (toggle, openrgb_btn);
      }

    page->ram_lighting_status = gtk_label_new ("");
    gtk_widget_set_halign (page->ram_lighting_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand (page->ram_lighting_status, TRUE);
    gtk_label_set_wrap (GTK_LABEL (page->ram_lighting_status), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (page->ram_lighting_status), 60);
    adw_expander_row_add_row (toggle, page->ram_lighting_status);

    adw_preferences_group_add (wrapper, GTK_WIDGET (toggle));
    gtk_box_append (GTK_BOX (content), GTK_WIDGET (wrapper));
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

  /* Lighting Controls */
  hw_detail_update_lighting_section (GTK_LABEL (page->mobo_lighting_status));

  /* Refresh SMBus status */
  refresh_smbus_status (page);
}

void
hw_detail_show_ram (HardwarePage *page, const char *device_name)
{
  gtk_stack_set_visible_child_name (page->detail_stack, "ram");

  /* Name */
  gtk_label_set_text (GTK_LABEL (page->ram_name_label),
    device_name ? device_name : "");

  /* Total Memory */
  char total_str[64];
  double total_gb = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
  if (total_gb >= 1.0)
    g_snprintf (total_str, sizeof (total_str), "%.2f GB", total_gb);
  else
    g_snprintf (total_str, sizeof (total_str), "%.0f MB",
      page->ram_total_bytes / (1024.0 * 1024.0));
  gtk_label_set_text (GTK_LABEL (page->ram_total_label), total_str);

  /* Usage */
  char used_str[64];
  double used_gb = page->ram_used_bytes / (1024.0 * 1024.0 * 1024.0);
  double total_disp = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
  double used_pct = page->ram_total_bytes > 0
    ? (page->ram_used_bytes * 100.0 / page->ram_total_bytes) : 0.0;
  if (used_gb >= 1.0)
    g_snprintf (used_str, sizeof (used_str), "%.2f GB / %.2f GB (%.0f%%)",
      used_gb, total_disp, used_pct);
  else
    g_snprintf (used_str, sizeof (used_str), "%.0f MB / %.2f GB (%.0f%%)",
      page->ram_used_bytes / (1024.0 * 1024.0), total_disp, used_pct);
  gtk_label_set_text (GTK_LABEL (page->ram_used_label), used_str);

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

  /* Start RAM usage monitor */
  if (page->ram_monitor_timer_id == 0)
    {
      page->ram_monitor_timer_id = g_timeout_add (1000, hw_ram_monitor_tick, page);
      hw_ram_monitor_tick (page);
    }
}
