#define _GNU_SOURCE
#include "page-mouse.h"
#include "mouse-ratbag.h"
#include "mouse-settings.h"
#include "mouse-kwin.h"
#include "page-dashboard.h"
#include "../window.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../epi-i18n.h"

#define tr(x) epi_i18n_get (x)

/* ─────────────── MOUSE_INDEX.md loader ─────────────── */

static void
free_mouse_index_entry (gpointer data)
{
  MouseIndexEntry *e = data;
  if (e)
    {
      g_free (e->name);
      g_free (e->url);
      g_free (e->white_label_vendor);
      g_free (e->manufacturer);
      g_free (e);
    }
}

static void
load_mouse_index (MousePage *page)
{
  if (page->mouse_index) return;

  page->mouse_index = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, free_mouse_index_entry);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/MOUSE_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }

  if (!content) return;

  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);
  g_free (content);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---|---"))
        {
          in_table = TRUE;
          continue;
        }

      if (!in_table || line[0] != '|') continue;

      char **cols = g_strsplit (line, "|", 0);
      if (!cols[1])
        {
          g_strfreev (cols);
          continue;
        }

      /* Columns: Name | VID:PID | URL | Buttons | White-Label | Manufacturer */
      char *name = cols[1] ? g_strstrip (cols[1]) : NULL;
      char *vidpid_str = cols[2] ? g_strstrip (cols[2]) : NULL;
      char *url_str = cols[3] ? g_strstrip (cols[3]) : NULL;
      char *btn_str = cols[4] ? g_strstrip (cols[4]) : NULL;
      char *wl_str = cols[5] ? g_strstrip (cols[5]) : NULL;
      char *mfr_str = cols[6] ? g_strstrip (cols[6]) : NULL;

      if (!name || !name[0] || !vidpid_str || !vidpid_str[0])
        {
          g_strfreev (cols);
          continue;
        }

      unsigned int v = 0, p = 0;
      if (sscanf (vidpid_str, "%04x:%04x", &v, &p) != 2 || v == 0)
        {
          g_strfreev (cols);
          continue;
        }

      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", v, p);

      MouseIndexEntry *entry = g_new0 (MouseIndexEntry, 1);
      entry->name = g_strdup (name);
      entry->url = url_str && url_str[0] ? g_strdup (url_str) : NULL;
      entry->buttons = btn_str && btn_str[0] ? atoi (btn_str) : 0;
      entry->white_label = wl_str && wl_str[0] ? TRUE : FALSE;
      entry->white_label_vendor = (wl_str && wl_str[0]) ? g_strdup (wl_str) : NULL;
      entry->manufacturer = (mfr_str && mfr_str[0]) ? g_strdup (mfr_str) : NULL;

      g_hash_table_insert (page->mouse_index, g_strdup (key), entry);

      g_strfreev (cols);
    }

  g_strfreev (lines);
}

static MouseIndexEntry *
lookup_mouse (MousePage *page, uint16_t vendor, uint16_t product)
{
  if (!page->mouse_index) return NULL;
  char key[16];
  g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
  return g_hash_table_lookup (page->mouse_index, key);
}

/* Get manufacturer logo resource path from USB vendor ID */
static const char *
mouse_vendor_logo_path (uint16_t vendor)
{
  switch (vendor)
    {
    case 0x046d: return "/io/github/mayrinck/epinephrine/icons/manufacturers/logitech.png";
    case 0x1bcf: return "/io/github/mayrinck/epinephrine/icons/manufacturers/multilaser.png";
    case 0x18f8: return "/io/github/mayrinck/epinephrine/icons/manufacturers/multilaser.png"; /* Warrior = Multilaser */
    case 0x1c6e: return "/io/github/mayrinck/epinephrine/icons/manufacturers/positivo.png";
    case 0x1d57: return "/io/github/mayrinck/epinephrine/icons/manufacturers/cce-digibras.png";
    default:     return NULL;
    }
}

/* Clean up lsusb vendor name: remove Corp./Co./Inc. suffixes */
static char *
clean_vendor_name (const char *raw)
{
  if (!raw || !raw[0]) return NULL;
  char *result = g_strdup (raw);
  /* Remove common suffixes */
  const char *suffixes[] = { " Corp.", " Co.", " Inc.", " corp.", " co.", " inc.", NULL };
  for (int i = 0; suffixes[i]; i++)
    {
      char *pos = g_strrstr (result, suffixes[i]);
      if (pos && pos > result)
        {
          *pos = '\0';
          break;
        }
    }
  g_strstrip (result);
  return result;
}

/* Public MOUSE_INDEX.md name lookup (used by hw-devices.c during detection).
 * Loads the index lazily into a module-level cache. */
static GHashTable *s_mouse_index_cache = NULL;

const char *
mouse_index_lookup_name (uint16_t vendor, uint16_t product)
{
  if (!s_mouse_index_cache)
    {
      s_mouse_index_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
        g_free, g_free);

      char *content = NULL;
      GBytes *gbytes = g_resources_lookup_data (
        "/io/github/mayrinck/epinephrine/database/MOUSE_INDEX.md", 0, NULL);
      if (gbytes)
        {
          content = g_strndup (g_bytes_get_data (gbytes, NULL),
            g_bytes_get_size (gbytes));
          g_bytes_unref (gbytes);
        }
      if (!content) return NULL;

      gboolean in_table = FALSE;
      char **lines = g_strsplit (content, "\n", -1);
      g_free (content);

      for (int i = 0; lines[i]; i++)
        {
          const char *line = lines[i];
          if (g_str_has_prefix (line, "|---|---"))
            { in_table = TRUE; continue; }
          if (!in_table || line[0] != '|') continue;

          char **cols = g_strsplit (line, "|", 0);
          char *name = cols[1] ? g_strstrip (cols[1]) : NULL;
          char *vidpid_str = cols[2] ? g_strstrip (cols[2]) : NULL;
          if (!name || !name[0] || !vidpid_str || !vidpid_str[0])
            { g_strfreev (cols); continue; }

          unsigned int v = 0, p = 0;
          if (sscanf (vidpid_str, "%04x:%04x", &v, &p) != 2 || v == 0)
            { g_strfreev (cols); continue; }

          char key[16];
          g_snprintf (key, sizeof (key), "%04x:%04x", v, p);
          g_hash_table_insert (s_mouse_index_cache,
            g_strdup (key), g_strdup (name));
          g_strfreev (cols);
        }
      g_strfreev (lines);
    }

  char key[16];
  g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
  return g_hash_table_lookup (s_mouse_index_cache, key);
}

/* ─────────────── USB device info query (mirrors keyboard approach) ─────────────── */

static char *
mouse_read_sysfs (const char *path)
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
mouse_find_usb_parent (const char *start)
{
  /* Resolve symlinks first (e.g. /sys/class/input/eventN/device → real HID path) */
  char *resolved = realpath (start, NULL);
  char *path = resolved ? resolved : g_strdup (start);
  while (path && path[0])
    {
      char *vendor = g_build_filename (path, "idVendor", NULL);
      if (g_file_test (vendor, G_FILE_TEST_EXISTS))
        { g_free (vendor); return path; }
      g_free (vendor);
      char *parent = g_path_get_dirname (path);
      g_free (path);
      path = parent;
      if (g_strcmp0 (path, "/sys") == 0 || g_strcmp0 (path, "/") == 0)
        { g_free (path); return NULL; }
    }
  return NULL;
}

static gboolean
find_mouse_usb_info (uint16_t vendor, uint16_t product,
                     char **out_vendor, char **out_speed,
                     char **out_driver, char **out_chipset)
{
  GDir *dir = g_dir_open ("/dev/input/by-id", 0, NULL);
  if (!dir) return FALSE;

  gboolean found = FALSE;
  const char *entry;
  while (!found && (entry = g_dir_read_name (dir)))
    {
      if (!g_str_has_suffix (entry, "-event-mouse"))
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

      char *usb_path = mouse_find_usb_parent (sys_path);
      g_free (sys_path);
      if (!usb_path) continue;

      /* Check if this device matches the requested VID:PID */
      char vid_path[256], pid_path[256];
      g_snprintf (vid_path, sizeof (vid_path), "%s/idVendor", usb_path);
      g_snprintf (pid_path, sizeof (pid_path), "%s/idProduct", usb_path);
      char *vid_str = mouse_read_sysfs (vid_path);
      char *pid_str = mouse_read_sysfs (pid_path);
      if (!vid_str || !pid_str)
        { g_free (vid_str); g_free (pid_str); g_free (usb_path); continue; }
      uint16_t v = (uint16_t) strtoul (vid_str, NULL, 16);
      uint16_t p = (uint16_t) strtoul (pid_str, NULL, 16);
      g_free (vid_str); g_free (pid_str);

      if (vendor != 0 && (v != vendor || p != product))
        { g_free (usb_path); continue; }

      found = TRUE;

      /* Query udevadm for vendor/model names */
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
                  if (out_vendor) *out_vendor = g_strdup (lines[i] + 24);
                }
            }
          g_strfreev (lines);
          g_free (out);
        }

      char *speed_path = g_build_filename (usb_path, "speed", NULL);
      char *speed_str = mouse_read_sysfs (speed_path);
      g_free (speed_path);
      if (speed_str)
        {
          double speed_val = atof (speed_str);
          if (speed_val == 1.5)
            { g_free (speed_str); speed_str = g_strdup (tr ("1.5 Mb/s (Low Speed)")); }
          else if (speed_val == 12.0)
            { g_free (speed_str); speed_str = g_strdup (tr ("12 Mb/s (Full Speed)")); }
          else if (speed_val >= 480.0 && speed_val < 5000.0)
            { g_free (speed_str); speed_str = g_strdup_printf (tr ("%.0f Mb/s (High Speed)"), speed_val); }
          else if (speed_val >= 5000.0)
            { g_free (speed_str); speed_str = g_strdup_printf (tr ("%.0f Mb/s (Super Speed)"), speed_val); }
          else
            { g_free (speed_str); speed_str = g_strdup_printf (tr ("%.1f Mb/s"), speed_val); }
          if (out_speed) *out_speed = speed_str;
          else g_free (speed_str);
        }
      else if (out_speed) *out_speed = g_strdup ("—");

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
        { g_free (*out_driver); *out_driver = g_strdup ("—"); }

      char *chipset_path = g_build_filename (usb_path, "manufacturer", NULL);
      char *chipset = mouse_read_sysfs (chipset_path);
      g_free (chipset_path);
      if (out_chipset) *out_chipset = chipset ? chipset : g_strdup ("—");
      else g_free (chipset);

      g_free (usb_path);
    }
  g_dir_close (dir);

  if (!found)
    {
      if (out_vendor) *out_vendor = g_strdup ("—");
      if (out_speed) *out_speed = g_strdup ("—");
      if (out_driver) *out_driver = g_strdup ("—");
      if (out_chipset) *out_chipset = g_strdup ("—");
    }
  return found;
}

#define G300_VID 0x046d
#define G300_PID 0xc246
#define G300_OLD_PID 0xc24a

/* ─────────────── Wayland + X11 mouse settings ─────────────── */

/* GNOME owns these settings on both X11 and Wayland. Merely having the
 * schema installed does not mean another desktop uses it. */
static GSettings *
mouse_gnome_settings (void)
{
  return mouse_settings_open (g_getenv ("XDG_CURRENT_DESKTOP"));
}

static void
load_mouse_settings (MousePage *page)
{
  g_clear_object (&page->kwin_bus);
  g_clear_pointer (&page->kwin_path, g_free);
  g_autoptr(GSettings) settings = mouse_gnome_settings ();
  gboolean available = settings != NULL;
  gboolean has_scroll_factor = FALSE;
  MouseKwinState state = {0};
  const char *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  const char *session = g_getenv ("XDG_SESSION_TYPE");
  if (!settings && desktop && g_str_equal (desktop, "KDE") && session &&
      g_str_equal (session, "wayland") && page->cur_vendor && page->cur_product)
    {
      page->kwin_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
      if (page->kwin_bus && mouse_kwin_find (page->kwin_bus, page->cur_vendor,
          page->cur_product, &page->kwin_path, NULL) &&
          mouse_kwin_read (page->kwin_bus, page->kwin_path, &state, NULL))
        { available = TRUE; has_scroll_factor = state.has_scroll_factor; }
      else { g_clear_object (&page->kwin_bus); g_clear_pointer (&page->kwin_path, g_free); }
    }
  gtk_widget_set_sensitive (page->speed_scale, available);
  gtk_widget_set_sensitive (page->accel_dropdown, available);
  gtk_widget_set_sensitive (page->natural_scroll_switch, available);
  gtk_widget_set_sensitive (page->scroll_factor, has_scroll_factor);
  gtk_widget_set_sensitive (page->scroll_button_switch, available && !settings && state.has_scroll_on_button);
  gtk_widget_set_sensitive (page->save_btn, available);
  if (!available) return;
  if (settings)
    {
      gtk_range_set_value (GTK_RANGE (page->speed_scale), g_settings_get_double (settings, "speed"));
      g_autofree char *profile = g_settings_get_string (settings, "accel-profile");
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->accel_dropdown),
        g_str_equal (profile, "flat") ? 1 : g_str_equal (profile, "adaptive") ? 2 : 0);
      adw_switch_row_set_active (ADW_SWITCH_ROW (page->natural_scroll_switch), g_settings_get_boolean (settings, "natural-scroll"));
    }
  else
    {
      gtk_range_set_value (GTK_RANGE (page->speed_scale), state.speed);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->accel_dropdown), state.profile);
      adw_switch_row_set_active (ADW_SWITCH_ROW (page->natural_scroll_switch), state.natural);
      if (state.has_scroll_on_button) adw_switch_row_set_active (ADW_SWITCH_ROW (page->scroll_button_switch), state.scroll_on_button);
      if (state.has_scroll_factor) gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->scroll_factor), state.scroll_factor);
    }
}

static void
on_mouse_save (GtkButton *btn, MousePage *page)
{
  (void) btn;
  g_autoptr(GSettings) settings = mouse_gnome_settings ();
  if (page->kwin_bus && page->kwin_path)
    {
      MouseKwinState state = { .speed = gtk_range_get_value (GTK_RANGE (page->speed_scale)),
        .profile = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->accel_dropdown)),
        .natural = adw_switch_row_get_active (ADW_SWITCH_ROW (page->natural_scroll_switch)),
        .has_scroll_factor = gtk_widget_get_sensitive (page->scroll_factor),
        .has_scroll_on_button = gtk_widget_get_sensitive (page->scroll_button_switch),
        .scroll_on_button = adw_switch_row_get_active (ADW_SWITCH_ROW (page->scroll_button_switch)),
        .scroll_factor = gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->scroll_factor)) };
      g_autoptr(GError) kwin_error = NULL;
      if (!mouse_kwin_write (page->kwin_bus, page->kwin_path, &state, &kwin_error))
        dashboard_show_error_toast (tr ("Could not apply mouse settings"), kwin_error->message);
      else dashboard_show_toast (tr ("Mouse settings applied (KWin)"));
      return;
    }
  const char *profiles[] = { "default", "flat", "adaptive" };
  guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->accel_dropdown));
  if (selected >= G_N_ELEMENTS (profiles)) return;
  g_autoptr(GError) error = NULL;
  if (!mouse_settings_write (settings, gtk_range_get_value (GTK_RANGE (page->speed_scale)),
       profiles[selected], adw_switch_row_get_active (ADW_SWITCH_ROW (page->natural_scroll_switch)), &error))
    {
      dashboard_show_error_toast (tr ("Could not apply mouse settings"), error->message);
      return;
    }
  dashboard_show_toast (tr ("Mouse settings applied (GNOME)"));
}

static void
on_open_piper (GtkButton *button, MousePage *page)
{
  (void) button; (void) page;
  g_autofree char *program = g_find_program_in_path ("piper");
  g_autoptr(GError) error = NULL;
  if (!program || !g_spawn_async (NULL, (char *[]){ program, NULL }, NULL,
                                   G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
    dashboard_show_error_toast (tr ("Could not open Piper"),
      error ? error->message : tr ("Install Piper and libratbag/ratbagd, then reconnect the mouse."));
}

static void
on_open_ratbag_editor (GtkButton *button, MousePage *page)
{
  (void) button;
  GtkRoot *root = gtk_widget_get_root (page->box);
  mouse_ratbag_open (GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                     page->cur_vendor, page->cur_product);
}

static void
refresh_mouse_info (MousePage *page)
{
  /* Try 1: libinput list-devices (Wayland + X11) */
  char *out = NULL;
  if (g_spawn_sync (NULL, (char *[]){ "libinput", "list-devices", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL) && out)
    {
      char **lines = g_strsplit (out, "\n", -1);
      const char *resolution = NULL;
      for (int i = 0; lines[i]; i++)
        {
          if (strstr (lines[i], "DPI:"))
            {
              /* Parse "DPI: dpi=1200x1200 (logical: 1200x1200)" */
              char *eq = strchr (lines[i], '=');
              char *x = NULL;
              if (eq)
                {
                  eq++;
                  x = strchr (eq, 'x');
                  if (x) *x = '\0';
                  resolution = eq;
                }
              break;
            }
        }
      if (resolution && resolution[0])
        gtk_label_set_text (GTK_LABEL (page->dpi_label), resolution);
      else
        gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
      g_strfreev (lines);
      g_free (out);
      return;
    }
  g_free (out);

  /* Try 2: xinput list-props (X11 fallback) */
  out = NULL;
  if (g_spawn_sync (NULL, (char *[]){ "xinput", "list", "--name-only", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL) && out)
    {
      g_strstrip (out);
      if (out[0])
        {
          char *dpi_out = NULL;
          g_spawn_sync (NULL,
            (char *[]){ "xinput", "list-props", out, NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &dpi_out, NULL, NULL, NULL);
          if (dpi_out)
            {
              char **lines = g_strsplit (dpi_out, "\n", -1);
              const char *resolution = NULL;
              for (int i = 0; lines[i]; i++)
                {
                  if (strstr (lines[i], "Resolution"))
                    {
                      char *br = strchr (lines[i], '[');
                      char *end = NULL;
                      if (br) end = strchr (br + 1, ']');
                      if (end)
                        {
                          *end = '\0';
                          resolution = br + 1;
                        }
                      break;
                    }
                }
              if (resolution && resolution[0])
                gtk_label_set_text (GTK_LABEL (page->dpi_label), resolution);
              else
                gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
              g_strfreev (lines);
              g_free (dpi_out);
              g_free (out);
              return;
            }
          g_free (dpi_out);
        }
      g_free (out);
    }

  gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
}

/* ─────────────── Website button ─────────────── */

static void
on_mouse_website_clicked (GtkButton *btn, MousePage *page)
{
  (void) btn;
  if (page->mouse_url)
    {
      GError *err = NULL;
      g_app_info_launch_default_for_uri (page->mouse_url, NULL, &err);
      if (err)
        {
          g_warning ("Failed to open URL: %s", err->message);
          g_error_free (err);
        }
    }
}

/* ─────────────── Show device (called from page-hardware) ─────────────── */

void
mouse_page_show_device (MousePage *page, uint16_t vendor, uint16_t product, const char *detected_name)
{
  load_mouse_index (page);

  MouseIndexEntry *e = lookup_mouse (page, vendor, product);

  if (e && e->name)
    {
      gtk_label_set_text (GTK_LABEL (page->mouse_name_label), e->name);

      g_free (page->mouse_url);
      page->mouse_url = e->url ? g_strdup (e->url) : NULL;
      gtk_widget_set_sensitive (page->mouse_website_btn, page->mouse_url != NULL);
    }
  else
    {
      char fallback[64];
      g_snprintf (fallback, sizeof (fallback), tr ("Mouse (%04x:%04x)"), vendor, product);
      gtk_label_set_text (GTK_LABEL (page->mouse_name_label),
        detected_name && detected_name[0] ? detected_name : fallback);
      g_clear_pointer (&page->mouse_url, g_free);
      gtk_widget_set_sensitive (page->mouse_website_btn, FALSE);
    }

  /* Manufacturer logo */
  const char *logo = mouse_vendor_logo_path (vendor);
  if (logo)
    {
      gtk_image_set_from_resource (GTK_IMAGE (page->mouse_brand_icon), logo);
      gtk_image_set_pixel_size (GTK_IMAGE (page->mouse_brand_icon), 64);
    }
  else
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (page->mouse_brand_icon), "input-mouse-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (page->mouse_brand_icon), 64);
    }

  /* Release date */
  const char *release_date = NULL;
  if (vendor == 0x046d && product == 0xc246)
    release_date = "13/01/2015";
  if (release_date)
    {
      char *label = g_strdup_printf (tr ("Release Date: %s"), release_date);
      gtk_label_set_text (GTK_LABEL (page->mouse_release_label), label);
      g_free (label);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->mouse_release_label), "");

  /* Populate device info section */
  page->cur_vendor = vendor;
  page->cur_product = product;

  char devid[16];
  g_snprintf (devid, sizeof (devid), "%04x:%04x", vendor, product);
  gtk_label_set_text (GTK_LABEL (page->dev_devid_value), devid);

  /* Buttons count from MOUSE_INDEX */
  if (e && e->buttons > 0)
    {
      char btn_str[16];
      g_snprintf (btn_str, sizeof (btn_str), "%d", e->buttons);
      gtk_label_set_text (GTK_LABEL (page->dev_buttons_value), btn_str);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->dev_buttons_value), "\u2014");

  /* DPI info */
  if (vendor == G300_VID && (product == G300_PID || product == G300_OLD_PID))
    gtk_label_set_text (GTK_LABEL (page->dpi_label), "250\u20132500");
  else
    gtk_label_set_text (GTK_LABEL (page->dpi_label), "\u2014");

  /* Vendor/Manufacturer: MOUSE_INDEX -> lsusb fallback */
  char *mfr = NULL;
  if (e && e->manufacturer && e->manufacturer[0])
    {
      mfr = g_strdup (e->manufacturer);
    }
  else
    {
      char *usb_vendor = NULL, *speed = NULL, *driver = NULL, *chipset = NULL;
      find_mouse_usb_info (vendor, product, &usb_vendor, &speed, &driver, &chipset);
      if (usb_vendor && usb_vendor[0])
        mfr = clean_vendor_name (usb_vendor);
      g_free (usb_vendor); g_free (speed); g_free (driver); g_free (chipset);
    }
  gtk_label_set_text (GTK_LABEL (page->dev_vendor_value),
    mfr && mfr[0] ? mfr : "\u2014");
  g_free (mfr);

  /* White-Label Product info from MOUSE_INDEX */
  if (e && e->white_label && e->white_label_vendor && e->white_label_vendor[0])
    {
      gtk_widget_set_visible (page->dev_white_label_row, TRUE);
      gtk_label_set_text (GTK_LABEL (page->dev_white_label_value), e->white_label_vendor);
    }
  else
    {
      gtk_widget_set_visible (page->dev_white_label_row, FALSE);
      gtk_label_set_text (GTK_LABEL (page->dev_white_label_value), "");
    }

  /* Refresh the desktop preferences and device information. */
  load_mouse_settings (page);
  refresh_mouse_info (page);

  /* Show/hide G300 sections based on whether this mouse is G300/G300s */
  gboolean is_g300 = (vendor == G300_VID && (product == G300_PID || product == G300_OLD_PID));
  if (page->save_mouse_btn)
    gtk_widget_set_visible (page->save_mouse_btn, is_g300);
  if (page->ratbag_editor_btn)
    gtk_widget_set_visible (page->ratbag_editor_btn, is_g300);
}

/* ─────────────── Cleanup ─────────────── */

static void
on_mouse_page_destroy (gpointer data, GObject *box)
{
  (void) box;
  MousePage *page = data;

  g_free (page->mouse_url);
  g_clear_object (&page->kwin_bus);
  g_free (page->kwin_path);
  if (page->mouse_index) g_hash_table_unref (page->mouse_index);
  g_free (page);
}

/* ─────────────── Page constructor ─────────────── */

MousePage *
mouse_page_new (void)
{
  MousePage *page = g_new0 (MousePage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-mouse.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-page"));
  page->speed_scale = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-speed-scale"));
  page->accel_dropdown = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-accel-dropdown"));
  page->natural_scroll_switch = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-natural-scroll-row"));
  page->scroll_factor = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-scroll-factor"));
  page->scroll_button_switch = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-scroll-button-row"));
  page->save_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-save-btn"));
  page->save_mouse_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-save-g300-btn"));
  page->restore_factory_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-restore-factory-btn"));

  /* Branding header */
  page->mouse_brand_icon = GTK_WIDGET (gtk_builder_get_object (builder, "mouse_brand_icon"));
  page->mouse_name_label = GTK_WIDGET (gtk_builder_get_object (builder, "mouse_name_label"));
  page->mouse_release_label = GTK_WIDGET (gtk_builder_get_object (builder, "mouse_release_label"));
  page->mouse_website_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-website-btn"));
  gtk_widget_set_sensitive (page->mouse_website_btn, FALSE);
  g_signal_connect (page->mouse_website_btn, "clicked",
    G_CALLBACK (on_mouse_website_clicked), page);

  /* Device Information — build rows programmatically (same pattern as mobo/GPU) */
  {
    AdwExpanderRow *dev_section = ADW_EXPANDER_ROW (
      gtk_builder_get_object (builder, "mouse-device-section"));

    /* Vendor */
    page->dev_vendor_value = gtk_label_new ("\u2014");
    gtk_widget_set_halign (page->dev_vendor_value, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->dev_vendor_value, TRUE);
    AdwActionRow *vendor_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vendor_row), tr ("Vendor"));
    adw_action_row_add_suffix (vendor_row, page->dev_vendor_value);
    adw_expander_row_add_row (dev_section, GTK_WIDGET (vendor_row));

    /* Device ID */
    page->dev_devid_value = gtk_label_new ("\u2014");
    gtk_widget_set_halign (page->dev_devid_value, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->dev_devid_value, TRUE);
    AdwActionRow *devid_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (devid_row), tr ("Device ID"));
    adw_action_row_add_suffix (devid_row, page->dev_devid_value);
    adw_expander_row_add_row (dev_section, GTK_WIDGET (devid_row));

    /* Buttons */
    page->dev_buttons_value = gtk_label_new ("\u2014");
    gtk_widget_set_halign (page->dev_buttons_value, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->dev_buttons_value, TRUE);
    AdwActionRow *buttons_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (buttons_row), tr ("Buttons"));
    adw_action_row_add_suffix (buttons_row, page->dev_buttons_value);
    adw_expander_row_add_row (dev_section, GTK_WIDGET (buttons_row));

    /* DPI (initially hidden) */
    page->dpi_label = gtk_label_new ("\u2014");
    gtk_widget_set_halign (page->dpi_label, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->dpi_label, TRUE);
    AdwActionRow *dpi_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dpi_row), tr ("Resolution (DPI)"));
    adw_action_row_add_suffix (dpi_row, page->dpi_label);
    adw_expander_row_add_row (dev_section, GTK_WIDGET (dpi_row));

    /* White-Label Product (hidden by default) */
    page->dev_white_label_value = gtk_label_new ("");
    gtk_widget_set_halign (page->dev_white_label_value, GTK_ALIGN_END);
    gtk_widget_set_hexpand (page->dev_white_label_value, TRUE);
    page->dev_white_label_row = GTK_WIDGET (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (page->dev_white_label_row),
      tr ("White-Label Product"));
    adw_action_row_add_suffix (ADW_ACTION_ROW (page->dev_white_label_row),
      page->dev_white_label_value);
    adw_expander_row_add_row (dev_section, page->dev_white_label_row);
    gtk_widget_set_visible (page->dev_white_label_row, FALSE);
  }

  /* ── Populate device info from first detected mouse ── */
  {
    char *usb_vendor = NULL, *speed = NULL, *driver = NULL, *chipset = NULL;
    find_mouse_usb_info (0, 0, &usb_vendor, &speed, &driver, &chipset);
    gtk_label_set_text (GTK_LABEL (page->dev_vendor_value),
      usb_vendor && usb_vendor[0] ? usb_vendor : "\u2014");
    gtk_label_set_text (GTK_LABEL (page->dev_devid_value), "\u2014");
    gtk_label_set_text (GTK_LABEL (page->dev_buttons_value), "\u2014");
    g_free (usb_vendor); g_free (speed); g_free (driver); g_free (chipset);
  }

  g_signal_connect (page->save_btn, "clicked", G_CALLBACK (on_mouse_save), page);
  g_signal_connect (page->save_mouse_btn, "clicked", G_CALLBACK (on_open_piper), page);
  GtkWidget *editor_button = gtk_button_new_with_label (tr ("Edit mouse profiles"));
  gtk_box_append (GTK_BOX (gtk_widget_get_parent (page->save_mouse_btn)), editor_button);
  page->ratbag_editor_btn = editor_button;
  gtk_widget_set_visible (editor_button, FALSE);
  g_signal_connect (editor_button, "clicked", G_CALLBACK (on_open_ratbag_editor), page);
  load_mouse_settings (page);

  g_object_ref_sink (page->box);

  /* Cleanup on destroy */
  g_object_weak_ref (G_OBJECT (page->box), on_mouse_page_destroy, page);

  g_object_unref (builder);

  load_mouse_index (page);

  refresh_mouse_info (page);

  return page;
}
