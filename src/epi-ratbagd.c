#include "epi-ratbagd.h"
#include <string.h>

/* ─────────────── D-Bus Helpers ─────────────── */

static GDBusConnection *
get_session_bus (void)
{
  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
  if (!conn)
    g_warning ("ratbagd: Failed to connect to session bus: %s", err->message);
  g_clear_error (&err);
  return conn;
}

static GDBusProxy *
make_proxy (GDBusConnection *conn, const char *name, const char *path, const char *iface)
{
  GError *err = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_sync (conn,
    G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
    name, path, iface, NULL, &err);
  if (!proxy)
    g_warning ("ratbagd: Proxy %s: %s", path, err->message);
  g_clear_error (&err);
  return proxy;
}

static GVariant *
call_method (GDBusProxy *proxy, const char *method, GVariant *args)
{
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_sync (proxy, method, args,
    G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
  if (err)
    {
      g_warning ("ratbagd: %s failed: %s", method, err->message);
      g_clear_error (&err);
    }
  return ret;
}

static guint32
get_uint32_prop (GDBusConnection *conn, const char *path, const char *iface, const char *prop)
{
  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd", path,
    "org.freedesktop.DBus.Properties");
  if (!proxy) return 0;

  GVariant *ret = call_method (proxy, "Get",
    g_variant_new ("(ss)", iface, prop));
  g_object_unref (proxy);
  if (!ret) return 0;

  GVariant *inner = g_variant_get_child_value (ret, 0);
  guint32 val = g_variant_get_uint32 (inner);
  g_variant_unref (inner);
  g_variant_unref (ret);
  return val;
}

static char *
get_string_prop (GDBusConnection *conn, const char *path, const char *iface, const char *prop)
{
  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd", path,
    "org.freedesktop.DBus.Properties");
  if (!proxy) return NULL;

  GVariant *ret = call_method (proxy, "Get",
    g_variant_new ("(ss)", iface, prop));
  g_object_unref (proxy);
  if (!ret) return NULL;

  GVariant *inner = g_variant_get_child_value (ret, 0);
  const gchar *str = g_variant_get_string (inner, NULL);
  char *result = g_strdup (str);
  g_variant_unref (inner);
  g_variant_unref (ret);
  return result;
}

/* ─────────────── Manager ─────────────── */

gboolean
ratbagd_is_running (void)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    "/org/freedesktop/ratbagd", "org.freedesktop.DBus.Properties");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "Get",
    g_variant_new ("(ss)", "org.freedesktop.ratbagd.Manager", "Version"));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

GList *
ratbagd_get_devices (void)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return NULL;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    "/org/freedesktop/ratbagd", "org.freedesktop.ratbagd.Manager");
  if (!proxy)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *ret = call_method (proxy, "GetDevices", NULL);
  g_object_unref (proxy);
  if (!ret)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *devices_var = g_variant_get_child_value (ret, 0);
  GVariantIter iter;
  g_variant_iter_init (&iter, devices_var);

  const gchar *path;
  GList *devices = NULL;

  while (g_variant_iter_next (&iter, "o", &path))
    {
      RatbagdDevice *dev = g_new0 (RatbagdDevice, 1);
      dev->path = g_strdup (path);
      dev->name = get_string_prop (conn, path, "org.freedesktop.ratbagd.Device", "Name");
      dev->description = get_string_prop (conn, path, "org.freedesktop.ratbagd.Device", "Description");
      dev->driver = get_string_prop (conn, path, "org.freedesktop.ratbagd.Device", "Driver");
      dev->vid = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Device", "IdVendor");
      dev->pid = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Device", "IdProduct");
      dev->profiles = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Device", "NumProfiles");
      dev->active_profile = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Device", "ActiveProfileIndex");
      devices = g_list_append (devices, dev);
    }

  g_variant_unref (devices_var);
  g_variant_unref (ret);
  g_object_unref (conn);
  return devices;
}

void
ratbagd_free_devices (GList *devices)
{
  for (GList *l = devices; l; l = l->next)
    {
      RatbagdDevice *dev = l->data;
      g_free (dev->path);
      g_free (dev->name);
      g_free (dev->description);
      g_free (dev->driver);
      g_free (dev);
    }
  g_list_free (devices);
}

RatbagdDevice *
ratbagd_find_device (guint32 vid, guint32 pid)
{
  GList *devices = ratbagd_get_devices ();
  RatbagdDevice *found = NULL;

  for (GList *l = devices; l; l = l->next)
    {
      RatbagdDevice *dev = l->data;
      if (dev->vid == vid && dev->pid == pid)
        {
          found = g_memdup2 (dev, sizeof (RatbagdDevice));
          found->path = g_strdup (dev->path);
          found->name = g_strdup (dev->name);
          found->description = g_strdup (dev->description);
          found->driver = g_strdup (dev->driver);
          break;
        }
    }

  ratbagd_free_devices (devices);
  return found;
}

/* ─────────────── Profiles ─────────────── */

GList *
ratbagd_get_profiles (const char *device_path)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return NULL;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    device_path, "org.freedesktop.ratbagd.Device");
  if (!proxy)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *ret = call_method (proxy, "GetProfiles", NULL);
  g_object_unref (proxy);
  if (!ret)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *paths = g_variant_get_child_value (ret, 0);
  GVariantIter iter;
  g_variant_iter_init (&iter, paths);

  const gchar *path;
  GList *profiles = NULL;

  while (g_variant_iter_next (&iter, "o", &path))
    {
      RatbagdProfile *prof = g_new0 (RatbagdProfile, 1);
      prof->path = g_strdup (path);
      prof->enabled = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Profile", "Enabled") != 0;
      prof->resolution = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Profile", "Resolution");
      prof->report_rate = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Profile", "ReportRate");
      profiles = g_list_append (profiles, prof);
    }

  g_variant_unref (paths);
  g_variant_unref (ret);
  g_object_unref (conn);
  return profiles;
}

/* ─────────────── Resolutions ─────────────── */

GList *
ratbagd_get_resolutions (const char *profile_path)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return NULL;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    profile_path, "org.freedesktop.ratbagd.Profile");
  if (!proxy)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *ret = call_method (proxy, "GetResolutions", NULL);
  g_object_unref (proxy);
  if (!ret)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *paths = g_variant_get_child_value (ret, 0);
  GVariantIter iter;
  g_variant_iter_init (&iter, paths);

  const gchar *path;
  GList *resolutions = NULL;

  while (g_variant_iter_next (&iter, "o", &path))
    {
      RatbagdResolution *res = g_new0 (RatbagdResolution, 1);
      res->path = g_strdup (path);
      res->xres = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Resolution", "XResolution");
      res->yres = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Resolution", "YResolution");
      res->is_active = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Resolution", "IsActive") != 0;
      res->is_default = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Resolution", "IsDefault") != 0;
      resolutions = g_list_append (resolutions, res);
    }

  g_variant_unref (paths);
  g_variant_unref (ret);
  g_object_unref (conn);
  return resolutions;
}

/* ─────────────── Buttons ─────────────── */

GList *
ratbagd_get_buttons (const char *profile_path)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return NULL;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    profile_path, "org.freedesktop.ratbagd.Profile");
  if (!proxy)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *ret = call_method (proxy, "GetButtons", NULL);
  g_object_unref (proxy);
  if (!ret)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *paths = g_variant_get_child_value (ret, 0);
  GVariantIter iter;
  g_variant_iter_init (&iter, paths);

  const gchar *path;
  GList *buttons = NULL;

  while (g_variant_iter_next (&iter, "o", &path))
    {
      RatbagdButton *btn = g_new0 (RatbagdButton, 1);
      btn->path = g_strdup (path);
      btn->type = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Button", "ButtonType");
      btn->button = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Button", "Button");
      btn->special = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Button", "Special");
      btn->macro = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Button", "Macro");
      buttons = g_list_append (buttons, btn);
    }

  g_variant_unref (paths);
  g_variant_unref (ret);
  g_object_unref (conn);
  return buttons;
}

/* ─────────────── LEDs ─────────────── */

GList *
ratbagd_get_leds (const char *profile_path)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return NULL;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    profile_path, "org.freedesktop.ratbagd.Profile");
  if (!proxy)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *ret = call_method (proxy, "GetLeds", NULL);
  g_object_unref (proxy);
  if (!ret)
    {
      g_object_unref (conn);
      return NULL;
    }

  GVariant *paths = g_variant_get_child_value (ret, 0);
  GVariantIter iter;
  g_variant_iter_init (&iter, paths);

  const gchar *path;
  GList *leds = NULL;

  while (g_variant_iter_next (&iter, "o", &path))
    {
      RatbagdLed *led = g_new0 (RatbagdLed, 1);
      led->mode = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Led", "Mode");
      led->color = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Led", "Color");
      led->brightness = get_uint32_prop (conn, path, "org.freedesktop.ratbagd.Led", "Brightness");
      leds = g_list_append (leds, led);
    }

  g_variant_unref (paths);
  g_variant_unref (ret);
  g_object_unref (conn);
  return leds;
}

/* ─────────────── Setters ─────────────── */

gboolean
ratbagd_set_active_profile (const char *device_path, guint32 profile_index)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    device_path, "org.freedesktop.ratbagd.Device");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "SetActiveProfile",
    g_variant_new ("(u)", profile_index));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

gboolean
ratbagd_set_resolution (const char *resolution_path, guint32 xres, guint32 yres)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    resolution_path, "org.freedesktop.ratbagd.Resolution");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "SetResolution",
    g_variant_new ("(uu)", xres, yres));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

gboolean
ratbagd_set_report_rate (const char *profile_path, guint32 rate)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    profile_path, "org.freedesktop.ratbagd.Profile");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "SetReportRate",
    g_variant_new ("(u)", rate));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

gboolean
ratbagd_set_button (const char *button_path, guint32 type, guint32 button, guint32 special)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    button_path, "org.freedesktop.ratbagd.Button");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "SetButtonMapping",
    g_variant_new ("(uuu)", type, button, special));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

gboolean
ratbagd_set_led_color (const char *led_path, guint32 color, guint32 brightness)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    led_path, "org.freedesktop.ratbagd.Led");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "SetLedColor",
    g_variant_new ("(uu)", color, brightness));
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

gboolean
ratbagd_commit (const char *device_path)
{
  GDBusConnection *conn = get_session_bus ();
  if (!conn) return FALSE;

  GDBusProxy *proxy = make_proxy (conn, "org.freedesktop.ratbagd",
    device_path, "org.freedesktop.ratbagd.Device");
  if (!proxy)
    {
      g_object_unref (conn);
      return FALSE;
    }

  GVariant *ret = call_method (proxy, "Commit", NULL);
  g_object_unref (proxy);
  g_object_unref (conn);

  if (ret)
    {
      g_variant_unref (ret);
      return TRUE;
    }
  return FALSE;
}

/* ─────────────── G300s Profile Application ─────────────── */

/* G300s raw profile format (35 bytes):
 * [0]   Report ID (F3/F4/F5)
 * [1]   LED flags: bits 0=red, 1=green, 2=blue
 * [2]   Frequency: 0=1000Hz, 1=125Hz, 2=250Hz, 3=500Hz
 * [3-6] DPI levels (4 bytes, each = dpi/250)
 * [7]   Unknown (default profile flag?)
 * [8-34] Button mappings (9 buttons × 3 bytes each)
 */

static guint32
g300_freq_to_rate (guint8 freq)
{
  switch (freq)
    {
    case 0: return 1000;
    case 1: return 125;
    case 2: return 250;
    case 3: return 500;
    default: return 1000;
    }
}

static guint32
g300_led_flags_to_color (guint8 flags)
{
  guint32 r = (flags & 0x01) ? 255 : 0;
  guint32 g = (flags & 0x02) ? 255 : 0;
  guint32 b = (flags & 0x04) ? 255 : 0;
  return (r << 16) | (g << 8) | b;
}

gboolean
ratbagd_apply_g300_profile (const char *device_path, int profile_idx,
                            const guint8 *profile_data)
{
  if (!device_path || profile_idx < 0 || profile_idx > 2 || !profile_data)
    return FALSE;

  GList *profiles = ratbagd_get_profiles (device_path);
  if (!profiles)
    return FALSE;

  GList *prof_link = g_list_nth (profiles, profile_idx);
  if (!prof_link)
    {
      ratbagd_free_devices (profiles); /* profiles use same free */
      return FALSE;
    }

  RatbagdProfile *prof = prof_link->data;

  /* Set report rate */
  guint32 rate = g300_freq_to_rate (profile_data[2]);
  ratbagd_set_report_rate (prof->path, rate);

  /* Set resolutions */
  GList *resolutions = ratbagd_get_resolutions (prof->path);
  int res_idx = 0;
  for (GList *l = resolutions; l && res_idx < 4; l = l->next, res_idx++)
    {
      RatbagdResolution *res = l->data;
      guint32 dpi = profile_data[3 + res_idx] * 250;
      ratbagd_set_resolution (res->path, dpi, dpi);
    }
  g_list_free_full (resolutions, g_free);

  /* Set LEDs */
  guint32 color = g300_led_flags_to_color (profile_data[1]);
  GList *leds = ratbagd_get_leds (prof->path);
  for (GList *l = leds; l; l = l->next)
    {
      /* LED path would be needed here, but we need the actual path */
      /* For now, skip LED setting as it requires getting the LED paths */
    }
  g_list_free_full (leds, g_free);

  /* Set buttons (9 buttons, 3 bytes each starting at offset 8) */
  GList *buttons = ratbagd_get_buttons (prof->path);
  int btn_idx = 0;
  for (GList *l = buttons; l && btn_idx < 9; l = l->next, btn_idx++)
    {
      RatbagdButton *btn = l->data;
      guint8 code = profile_data[8 + btn_idx * 3];
      guint8 mod = profile_data[8 + btn_idx * 3 + 1];
      guint8 key = profile_data[8 + btn_idx * 3 + 2];

      guint32 type = 0; /* button */
      guint32 button = code;
      guint32 special = 0;

      if (code >= 0x0A)
        {
          type = 1; /* special */
          special = code;
        }

      ratbagd_set_button (btn->path, type, button, special);
    }
  g_list_free_full (buttons, g_free);

  return TRUE;
}

gboolean
ratbagd_apply_g300_profiles (const char *device_path, const guint8 profiles[3][35])
{
  if (!device_path || !profiles)
    return FALSE;

  gboolean all_ok = TRUE;

  for (int i = 0; i < 3; i++)
    {
      if (!ratbagd_apply_g300_profile (device_path, i, profiles[i]))
        all_ok = FALSE;
    }

  /* Commit all changes */
  if (!ratbagd_commit (device_path))
    all_ok = FALSE;

  return all_ok;
}
