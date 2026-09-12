#define _GNU_SOURCE
#include "page-keyboard.h"
#include "page-dashboard.h"
#include "epi-privilege.h"
#include "../window.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/hidraw.h>
#include "../epi-i18n.h"

/* With -std=c17 the _IOC macro may not be available from kernel headers. */
#ifndef _IOC
#include <asm-generic/ioctl.h>
#endif

#define tr(x) epi_i18n_get (x)

/* ---- Config helpers ---- */

static char *
get_kb_config_dir (void)
{
  const char *home = g_get_home_dir ();
  char *dir = g_build_filename (home, ".config", "epinephrine", NULL);
  g_mkdir_with_parents (dir, 0755);
  return dir;
}

static char *
get_kb_config_path (const char *name)
{
  char *dir = get_kb_config_dir ();
  char *path = g_build_filename (dir, name, NULL);
  g_free (dir);
  return path;
}

static void
save_kb_boot_setting (const char *name, gboolean enabled)
{
  char *path = get_kb_config_path (name);
  char *data = g_strdup_printf ("%d", enabled ? 1 : 0);
  g_file_set_contents (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

static gboolean
load_kb_boot_setting (const char *name)
{
  char *path = get_kb_config_path (name);
  char *contents = NULL;
  gboolean val = FALSE;
  if (g_file_get_contents (path, &contents, NULL, NULL))
    {
      g_strstrip (contents);
      val = (g_strcmp0 (contents, "1") == 0);
      g_free (contents);
    }
  g_free (path);
  return val;
}

/* ---- Sysfs helpers ---- */

static char *
read_sysfs (const char *path)
{
  char buf[256] = {0};
  int fd = open (path, O_RDONLY);
  if (fd < 0) return NULL;
  ssize_t n = read (fd, buf, sizeof (buf) - 1);
  close (fd);
  if (n <= 0) return NULL;
  buf[n] = '\0';
  g_strstrip (buf);
  return buf[0] ? g_strdup (buf) : NULL;
}

static char *
find_usb_parent (const char *start)
{
  /* Resolve symlinks first (e.g. /sys/class/input/eventN/device → real HID path) */
  char *resolved = realpath (start, NULL);
  char *path = resolved ? resolved : g_strdup (start);
  while (path && path[0])
    {
      char *vendor = g_build_filename (path, "idVendor", NULL);
      if (g_file_test (vendor, G_FILE_TEST_EXISTS))
        {
          g_free (vendor);
          return path;
        }
      g_free (vendor);
      char *parent = g_path_get_dirname (path);
      g_free (path);
      path = parent;
      if (g_strcmp0 (path, "/sys") == 0 || g_strcmp0 (path, "/") == 0)
        {
          g_free (path);
          return NULL;
        }
    }
  return NULL;
}

/* ---- Keyboard info querying ---- */

static gboolean
find_keyboard_usb_info (uint16_t vendor, uint16_t product,
                        char **out_name, char **out_vendor,
                        char **out_speed, char **out_driver,
                        char **out_chipset)
{
  GDir *dir = g_dir_open ("/dev/input/by-id", 0, NULL);
  if (!dir) return FALSE;

  const char *entry;
  gboolean found = FALSE;
  char *byid_name = NULL;
  while (!found && (entry = g_dir_read_name (dir)))
    {
      if (!g_str_has_suffix (entry, "-event-kbd"))
        continue;

      char *full = g_build_filename ("/dev/input/by-id", entry, NULL);
      char target[256] = {0};
      ssize_t len = readlink (full, target, sizeof (target) - 1);
      g_free (full);
      if (len < 0) continue;
      target[len] = '\0';

      const char *base = target;
      if (g_str_has_prefix (base, "../")) base += 3;
      char *sys_path = g_build_filename ("/sys/class/input", base, "device", NULL);

      char *usb_path = find_usb_parent (sys_path);
      g_free (sys_path);
      if (!usb_path) continue;

      /* When specific VID:PID requested, verify this entry matches */
      if (vendor != 0 || product != 0)
        {
          char vid_buf[16] = {0}, pid_buf[16] = {0};
          char *vid_file = g_build_filename (usb_path, "idVendor", NULL);
          char *pid_file = g_build_filename (usb_path, "idProduct", NULL);
          int fv = open (vid_file, O_RDONLY);
          int fp = open (pid_file, O_RDONLY);
          g_free (vid_file);
          g_free (pid_file);
          if (fv >= 0)
            {
              ssize_t n = read (fv, vid_buf, sizeof (vid_buf) - 1);
              close (fv);
              if (n > 0) vid_buf[n] = '\0';
              g_strstrip (vid_buf);
            }
          if (fp >= 0)
            {
              ssize_t n = read (fp, pid_buf, sizeof (pid_buf) - 1);
              close (fp);
              if (n > 0) pid_buf[n] = '\0';
              g_strstrip (pid_buf);
            }
          uint16_t ev_vendor = (uint16_t) strtoul (vid_buf, NULL, 16);
          uint16_t ev_product = (uint16_t) strtoul (pid_buf, NULL, 16);
          if (ev_vendor != vendor || ev_product != product)
            { g_free (usb_path); continue; }
        }

      /* Extract device name from the by-id entry itself (most reliable) */
      {
        char *d = g_strdup (entry);
        char *usb_part = strstr (d, "usb-");
        char *name_start = usb_part ? usb_part + 4 : d;
        char *suffix = strstr (name_start, "-event-kbd");
        if (suffix) *suffix = '\0';
        /* Skip leading hex VID:PID if present */
        if (name_start[0] && g_ascii_isxdigit (name_start[0]))
          {
            char *first_us = strchr (name_start, '_');
            if (first_us) name_start = first_us + 1;
          }
        /* Convert underscores to spaces */
        for (int i = 0; name_start[i]; i++)
          if (name_start[i] == '_') name_start[i] = ' ';
        byid_name = g_strstrip (g_strdup (name_start));
        g_free (d);
      }

      found = TRUE;

      char *out = NULL;
      g_spawn_sync (NULL,
        (char *[]){"udevadm", "info", "-q", "property", usb_path, NULL},
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);
      if (out)
        {
          char **lines = g_strsplit (out, "\n", -1);
          for (int i = 0; lines[i]; i++)
            {
              if (g_str_has_prefix (lines[i], "ID_VENDOR_FROM_DATABASE="))
                {
                  if (out_vendor)
                    *out_vendor = g_strdup (lines[i] + 24);
                }
              else if (g_str_has_prefix (lines[i], "ID_MODEL_FROM_DATABASE="))
                {
                  if (out_name)
                    *out_name = g_strdup (lines[i] + 23);
                }
            }
          g_strfreev (lines);
          g_free (out);
        }

      /* Use by-id name as fallback if udevadm didn't provide one */
      if (out_name && !*out_name && byid_name && byid_name[0])
        {
          *out_name = byid_name;
          byid_name = NULL;
        }

      char *manufacturer = g_build_filename (usb_path, "manufacturer", NULL);
      char *chipset = read_sysfs (manufacturer);
      g_free (manufacturer);
      if (out_chipset)
        *out_chipset = chipset ? chipset : g_strdup ("—");
      else
        g_free (chipset);

      char *speed_path = g_build_filename (usb_path, "speed", NULL);
      char *speed_str = read_sysfs (speed_path);
      g_free (speed_path);
      if (speed_str)
        {
          double speed_val = atof (speed_str);
          if (speed_val == 1.5)
            {
              g_free (speed_str);
              speed_str = g_strdup (tr ("1.5 Mb/s (Low Speed)"));
            }
          else if (speed_val == 12.0)
            {
              g_free (speed_str);
              speed_str = g_strdup (tr ("12 Mb/s (Full Speed)"));
            }
          else if (speed_val >= 480.0 && speed_val < 5000.0)
            {
              g_free (speed_str);
              speed_str = g_strdup_printf (tr ("%.0f Mb/s (High Speed)"), speed_val);
            }
          else if (speed_val >= 5000.0)
            {
              g_free (speed_str);
              speed_str = g_strdup_printf (tr ("%.0f Mb/s (Super Speed)"), speed_val);
            }
          else
            {
              g_free (speed_str);
              speed_str = g_strdup_printf (tr ("%.1f Mb/s"), speed_val);
            }
          if (out_speed) *out_speed = speed_str;
          else g_free (speed_str);
        }
      else if (out_speed)
        *out_speed = g_strdup ("—");

      GDir *iface_dir = g_dir_open (usb_path, 0, NULL);
      if (iface_dir)
        {
          const char *if_entry;
          while ((if_entry = g_dir_read_name (iface_dir)) && !*out_driver)
            {
              if (strchr (if_entry, ':'))
                {
                  char *driver_link = g_build_filename (usb_path, if_entry, "driver", NULL);
                  char driver_buf[256] = {0};
                  ssize_t dlen = readlink (driver_link, driver_buf, sizeof (driver_buf) - 1);
                  if (dlen > 0)
                    {
                      driver_buf[dlen] = '\0';
                      char *drv = strrchr (driver_buf, '/');
                      if (out_driver) *out_driver = g_strdup (drv ? drv + 1 : driver_buf);
                    }
                  g_free (driver_link);
                }
            }
          g_dir_close (iface_dir);
        }
      if (!out_driver || !*out_driver)
        {
          g_free (*out_driver);
          *out_driver = g_strdup ("—");
        }

      g_free (usb_path);
    }
  g_free (byid_name);
  g_dir_close (dir);

  if (!found)
    {
      if (out_name) *out_name = g_strdup ("—");
      if (out_vendor) *out_vendor = g_strdup ("—");
      if (out_speed) *out_speed = g_strdup ("—");
      if (out_driver) *out_driver = g_strdup ("—");
      if (out_chipset) *out_chipset = g_strdup ("—");
    }
  return found;
}

/* ---- LED application ---- */

static void
apply_kb_led (const char *setting, gboolean enable)
{
  const char *op = enable ? "+" : "-";
  char *cmd = g_strdup_printf ("setleds %s%s 2>/dev/null", op, setting);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
}

/* ---- Keyboard backlight control ---- */

/* Find a keyboard backlight LED in /sys/class/leds/.
 * Skips lock-key indicators (::numlock, ::capslock, ::scrolllock). */
static char *
find_kbd_backlight_led (void)
{
  GDir *dir = g_dir_open ("/sys/class/leds", 0, NULL);
  if (!dir) return NULL;

  const char *entry;
  while ((entry = g_dir_read_name (dir)))
    {
      if (g_str_has_suffix (entry, "::numlock") ||
          g_str_has_suffix (entry, "::capslock") ||
          g_str_has_suffix (entry, "::scrolllock"))
        continue;

      if (strstr (entry, "kbd_backlight") ||
          strstr (entry, "keyboard") ||
          strstr (entry, "kbd"))
        {
          char *bp = g_build_filename ("/sys/class/leds", entry, "brightness", NULL);
          if (g_file_test (bp, G_FILE_TEST_EXISTS))
            { g_dir_close (dir); return bp; }
          g_free (bp);
        }
    }
  g_dir_close (dir);
  return NULL;
}

static gboolean
sysfs_set_brightness (const char *path, int value)
{
  int fd = open (path, O_WRONLY);
  if (fd < 0) return FALSE;
  char buf[16];
  int len = g_snprintf (buf, sizeof (buf), "%d", value);
  ssize_t n = write (fd, buf, len);
  close (fd);
  return n == len;
}

/* ---- Keyboard LED state reading ---- */

static gboolean
read_sysfs_led (const char *name)
{
  char path[256];
  g_snprintf (path, sizeof (path), "/sys/class/leds/%s/brightness", name);
  char *content = read_sysfs (path);
  if (!content) return FALSE;
  gboolean on = (g_strcmp0 (content, "1") == 0);
  g_free (content);
  return on;
}

static gboolean
read_kb_led_state (gboolean *out_num, gboolean *out_caps, gboolean *out_scroll)
{
  /* Try EVIOCGLED via evdev first */
  GDir *dir = g_dir_open ("/dev/input/by-id", 0, NULL);
  if (dir)
    {
      const char *entry;
      while ((entry = g_dir_read_name (dir)))
        {
          if (!g_str_has_suffix (entry, "-event-kbd"))
            continue;

          char *full = g_build_filename ("/dev/input/by-id", entry, NULL);
          char target[256] = {0};
          ssize_t len = readlink (full, target, sizeof (target) - 1);
          g_free (full);
          if (len < 0) continue;
          target[len] = '\0';

          const char *base = target;
          if (g_str_has_prefix (base, "../")) base += 3;
          char *dev_path = g_build_filename ("/dev/input", base, NULL);

          int fd = open (dev_path, O_RDONLY);
          g_free (dev_path);
          if (fd >= 0)
            {
              unsigned int led_bits = 0;
              if (ioctl (fd, EVIOCGLED (sizeof (led_bits)), &led_bits) >= 0)
                {
                  if (out_num)    *out_num    = (led_bits & (1 << LED_NUML))    != 0;
                  if (out_caps)   *out_caps   = (led_bits & (1 << LED_CAPSL))   != 0;
                  if (out_scroll) *out_scroll = (led_bits & (1 << LED_SCROLLL)) != 0;
                  close (fd);
                  g_dir_close (dir);
                  return TRUE;
                }
              close (fd);
            }
        }
      g_dir_close (dir);
    }

  /* Fallback: read from sysfs /sys/class/leds/ (world-readable, no special perms) */
  GDir *led_dir = g_dir_open ("/sys/class/leds", 0, NULL);
  if (led_dir)
    {
      const char *led_entry;
      gboolean got_num = FALSE, got_caps = FALSE, got_scroll = FALSE;
      while ((led_entry = g_dir_read_name (led_dir)))
        {
          if (g_str_has_suffix (led_entry, "::numlock") && !got_num)
            { if (out_num) *out_num = read_sysfs_led (led_entry); got_num = TRUE; }
          else if (g_str_has_suffix (led_entry, "::capslock") && !got_caps)
            { if (out_caps) *out_caps = read_sysfs_led (led_entry); got_caps = TRUE; }
          else if (g_str_has_suffix (led_entry, "::scrolllock") && !got_scroll)
            { if (out_scroll) *out_scroll = read_sysfs_led (led_entry); got_scroll = TRUE; }
        }
      g_dir_close (led_dir);
      if (got_num || got_caps || got_scroll)
        return TRUE;
    }

  return FALSE;
}

/* ---- Signal handlers ---- */

static void
on_boot_toggled (AdwSwitchRow *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  KeyboardPage *page = user_data;
  gboolean state = adw_switch_row_get_active (sw);
  const char *setting = NULL, *config_name = NULL;

  if (sw == page->numlock_switch)
    { setting = "num"; config_name = "kb-numlock"; }
  else if (sw == page->capslock_switch)
    { setting = "caps"; config_name = "kb-capslock"; }
  else if (sw == page->scrolllock_switch)
    { setting = "scroll"; config_name = "kb-scrolllock"; }
  else
    return;

  save_kb_boot_setting (config_name, state);
  apply_kb_led (setting, state);
}

static void
on_backlight_toggled (AdwSwitchRow *sw, GParamSpec *pspec, KeyboardPage *page)
{
  (void) pspec;
  (void) page;
  gboolean want_on = adw_switch_row_get_active (sw);

  char *led_path = find_kbd_backlight_led ();
  if (led_path)
    {
      char *max_path = g_strdup_printf ("%s/../max_brightness", led_path);
      char *max_str = read_sysfs (max_path);
      g_free (max_path);
      int max_val = max_str ? atoi (max_str) : 1;
      if (max_val < 1) max_val = 1;
      g_free (max_str);

      gboolean ok = sysfs_set_brightness (led_path, want_on ? max_val : 0);
      g_free (led_path);

      if (ok)
        {
          dashboard_show_toast (want_on
            ? tr ("Keyboard backlight enabled")
            : tr ("Keyboard backlight disabled"));
          return;
        }
    }

  dashboard_show_toast (tr ("No keyboard backlight device found"));
}

static void
on_lock_led_toggled (AdwSwitchRow *sw, GParamSpec *pspec, KeyboardPage *page)
{
  (void) pspec;
  (void) page;
  gboolean want_on = adw_switch_row_get_active (sw);

  /* Apply to all three lock key LEDs via setleds */
  const char *keys[] = {"num", "caps", "scroll"};
  for (size_t i = 0; i < G_N_ELEMENTS (keys); i++)
    apply_kb_led (keys[i], want_on);

  dashboard_show_toast (want_on
    ? tr ("Lock key LEDs enabled")
    : tr ("Lock key LEDs disabled"));
}

static void
on_keyboard_website_clicked (GtkButton *btn, KeyboardPage *page)
{
  (void) btn;
  if (page->brand_url)
    {
      GError *err = NULL;
      g_app_info_launch_default_for_uri (page->brand_url, NULL, &err);
      if (err)
        {
          g_warning ("Failed to open URL: %s", err->message);
          g_error_free (err);
        }
    }
}

static void
on_keyboard_save (GtkButton *btn, KeyboardPage *page)
{
  (void) btn;

  int rate = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->repeat_rate_spin));
  int delay = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->repeat_delay_spin));

  char rate_str[16], delay_str[16];
  g_snprintf (rate_str, sizeof (rate_str), "%d", rate);
  g_snprintf (delay_str, sizeof (delay_str), "%d", delay);
  char *cmd = g_strdup_printf ("kbdrate -r %s -d %s", rate_str, delay_str);
  int exit_code = -1;
  GError *err = NULL;
  gboolean ok = epi_privilege_exec_sync (cmd, &exit_code, NULL, &err);
  g_free (cmd);
  if (ok && exit_code == 0)
    dashboard_show_toast (tr ("Keyboard settings applied"));
  else
    {
      if (err && err->message)
        dashboard_show_error_toast (tr ("Failed to set keyboard rate"), err->message);
      else
        dashboard_show_error_toast (tr ("Failed to set keyboard rate: kbdrate may not be installed"),
          tr ("The keyboard repeat rate could not be set. "
          "Make sure 'kbdrate' is installed (e.g., 'sudo apt install kbdrate')."));
    }
  g_clear_error (&err);
}

static void
refresh_keyboard_info (KeyboardPage *page)
{
  char *out = NULL;
  if (g_spawn_sync (NULL, (char *[]){ "setxkbmap", "-query", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL) && out)
    {
      char **lines = g_strsplit (out, "\n", -1);
      const char *layout = NULL;
      for (int i = 0; lines[i]; i++)
        {
          if (g_str_has_prefix (lines[i], "layout:"))
            {
              layout = lines[i] + 7;
              while (*layout == ' ') layout++;
              break;
            }
        }
      if (layout && layout[0])
        gtk_label_set_text (GTK_LABEL (page->layout_label), layout);
      else
        gtk_label_set_text (GTK_LABEL (page->layout_label), "—");
      g_strfreev (lines);
      g_free (out);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->layout_label), "—");
}

/* ---- Constructor ---- */

KeyboardPage *
keyboard_page_new (void)
{
  KeyboardPage *page = g_new0 (KeyboardPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-keyboard.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "keyboard-page"));

  /* Settings section */
  page->repeat_rate_spin = GTK_WIDGET (gtk_builder_get_object (builder, "kb-repeat-rate-spin"));
  page->repeat_delay_spin = GTK_WIDGET (gtk_builder_get_object (builder, "kb-repeat-delay-spin"));
  page->layout_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb-layout-label"));

  /* Lock key boot options */
  page->numlock_switch = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "kb-numlock-row"));
  page->capslock_switch = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "kb-capslock-row"));
  page->scrolllock_switch = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "kb-scrolllock-row"));

  /* Lighting Controls */
  page->backlight_switch = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "kb-backlight-switch-row"));
  page->lock_led_switch = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "kb-lock-led-switch-row"));

  /* Save button */
  page->save_btn = GTK_WIDGET (gtk_builder_get_object (builder, "kb-save-btn"));

  /* ---- Branding from UI ---- */
  page->branding_box = GTK_WIDGET (gtk_builder_get_object (builder, "kb_branding_box"));
  page->brand_name_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb_name_label"));
  page->brand_vendor_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb_vendor_label"));
  page->brand_website_btn = GTK_WIDGET (gtk_builder_get_object (builder, "kb-website-btn"));
  gtk_widget_set_sensitive (page->brand_website_btn, FALSE);
  g_signal_connect (page->brand_website_btn, "clicked",
    G_CALLBACK (on_keyboard_website_clicked), page);

  /* ---- Device Information labels ---- */
  page->info_vendor_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb-vendor-value"));
  page->info_speed_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb-speed-value"));
  page->info_driver_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb-driver-value"));
  page->info_chipset_label = GTK_WIDGET (gtk_builder_get_object (builder, "kb-chipset-value"));

  /* Connect signals */
  g_signal_connect (page->save_btn, "clicked", G_CALLBACK (on_keyboard_save), page);
  g_signal_connect (page->numlock_switch, "notify::active", G_CALLBACK (on_boot_toggled), page);
  g_signal_connect (page->capslock_switch, "notify::active", G_CALLBACK (on_boot_toggled), page);
  g_signal_connect (page->scrolllock_switch, "notify::active", G_CALLBACK (on_boot_toggled), page);
  g_signal_connect (page->backlight_switch, "notify::active", G_CALLBACK (on_backlight_toggled), page);
  g_signal_connect (page->lock_led_switch, "notify::active", G_CALLBACK (on_lock_led_toggled), page);

  g_object_ref_sink (page->box);

  /* Populate branding + device info from USB device */
  {
    char *name = NULL, *vendor = NULL, *speed = NULL, *driver = NULL, *chipset = NULL;
    find_keyboard_usb_info (0, 0, &name, &vendor, &speed, &driver, &chipset);

    if (name && name[0])
      gtk_label_set_text (GTK_LABEL (page->brand_name_label), name);

    if (vendor && vendor[0])
      {
        gtk_label_set_text (GTK_LABEL (page->brand_vendor_label), vendor);
        gtk_label_set_text (GTK_LABEL (page->info_vendor_label), vendor);
      }
    else
      gtk_label_set_text (GTK_LABEL (page->info_vendor_label), "—");

    gtk_label_set_text (GTK_LABEL (page->info_speed_label),
      speed && speed[0] ? speed : "—");
    gtk_label_set_text (GTK_LABEL (page->info_driver_label),
      driver && driver[0] ? driver : "—");
    gtk_label_set_text (GTK_LABEL (page->info_chipset_label),
      chipset && chipset[0] ? chipset : "—");

    g_free (name);
    g_free (vendor);
    g_free (speed);
    g_free (driver);
    g_free (chipset);
  }

  /* Initialize lock key switches from actual keyboard LED state */
  {
    gboolean num = FALSE, caps = FALSE, scroll = FALSE;
    if (read_kb_led_state (&num, &caps, &scroll))
      {
        adw_switch_row_set_active (page->numlock_switch, num);
        adw_switch_row_set_active (page->capslock_switch, caps);
        adw_switch_row_set_active (page->scrolllock_switch, scroll);
      }
    else
      {
        adw_switch_row_set_active (page->numlock_switch,
          load_kb_boot_setting ("kb-numlock"));
        adw_switch_row_set_active (page->capslock_switch,
          load_kb_boot_setting ("kb-capslock"));
        adw_switch_row_set_active (page->scrolllock_switch,
          load_kb_boot_setting ("kb-scrolllock"));
      }
  }

  /* Initialize backlight toggle from sysfs state */
  {
    char *led_path = find_kbd_backlight_led ();
    if (led_path)
      {
        char *content = read_sysfs (led_path);
        int val = content ? atoi (content) : 0;
        g_free (content);
        g_free (led_path);
        adw_switch_row_set_active (page->backlight_switch, val > 0);
      }
  }

  /* Initialize lock LED toggle: on if any lock key LED is currently on */
  {
    gboolean num = FALSE, caps = FALSE, scroll = FALSE;
    read_kb_led_state (&num, &caps, &scroll);
    adw_switch_row_set_active (page->lock_led_switch, num || caps || scroll);
  }

  refresh_keyboard_info (page);

  return page;
}

void
keyboard_page_show_device (KeyboardPage *page, uint16_t vendor, uint16_t product)
{
  if (!page) return;

  char *name = NULL, *vnd = NULL, *spd = NULL, *drv = NULL, *csp = NULL;
  find_keyboard_usb_info (vendor, product, &name, &vnd, &spd, &drv, &csp);

  if (name && name[0])
    gtk_label_set_text (GTK_LABEL (page->brand_name_label), name);
  else
    {
      char fallback[64];
      g_snprintf (fallback, sizeof (fallback), tr ("Keyboard (%04x:%04x)"), vendor, product);
      gtk_label_set_text (GTK_LABEL (page->brand_name_label), fallback);
    }

  if (vnd && vnd[0])
    {
      gtk_label_set_text (GTK_LABEL (page->brand_vendor_label), vnd);
      gtk_label_set_text (GTK_LABEL (page->info_vendor_label), vnd);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (page->brand_vendor_label), "");
      gtk_label_set_text (GTK_LABEL (page->info_vendor_label), "\u2014");
    }

  gtk_label_set_text (GTK_LABEL (page->info_speed_label),
    spd && spd[0] ? spd : "\u2014");
  gtk_label_set_text (GTK_LABEL (page->info_driver_label),
    drv && drv[0] ? drv : "\u2014");
  gtk_label_set_text (GTK_LABEL (page->info_chipset_label),
    csp && csp[0] ? csp : "\u2014");

  g_free (name);
  g_free (vnd);
  g_free (spd);
  g_free (drv);
  g_free (csp);

  /* Refresh layout info */
  refresh_keyboard_info (page);

  /* Refresh lock LED state for this keyboard */
  gboolean num = FALSE, caps = FALSE, scroll = FALSE;
  if (read_kb_led_state (&num, &caps, &scroll))
    {
      adw_switch_row_set_active (page->numlock_switch, num);
      adw_switch_row_set_active (page->capslock_switch, caps);
      adw_switch_row_set_active (page->scrolllock_switch, scroll);
    }
}
