#include "mouse-ratbag-client.h"
GVariant *
mouse_ratbag_call (GDBusConnection *bus, const char *path, const char *iface,
                   const char *method, GVariant *args, const GVariantType *type,
                   GError **error)
{
  return g_dbus_connection_call_sync (bus, MOUSE_RATBAG_BUS, path, iface, method,
                                     args, type, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
}
GVariant *
mouse_ratbag_get (GDBusConnection *bus, const char *path, const char *iface,
                  const char *name, GError **error)
{
  g_autoptr(GVariant) reply = mouse_ratbag_call (bus, path, "org.freedesktop.DBus.Properties",
    "Get", g_variant_new ("(ss)", iface, name), G_VARIANT_TYPE ("(v)"), error);
  GVariant *value = NULL;
  if (reply) g_variant_get (reply, "(v)", &value);
  return value;
}
gboolean
mouse_ratbag_set (GDBusConnection *bus, const char *path, const char *iface,
                  const char *name, GVariant *value, GError **error)
{
  g_autoptr(GVariant) reply = mouse_ratbag_call (bus, path, "org.freedesktop.DBus.Properties",
    "Set", g_variant_new ("(ssv)", iface, name, value), G_VARIANT_TYPE ("()"), error);
  return reply != NULL;
}
gboolean
mouse_ratbag_method (GDBusConnection *bus, const char *path, const char *iface,
                     const char *method, GError **error)
{
  g_autoptr(GVariant) reply = mouse_ratbag_call (bus, path, iface, method, NULL,
                                                  G_VARIANT_TYPE ("(u)"), error);
  if (!reply) return FALSE;
  guint32 status;
  g_variant_get (reply, "(u)", &status);
  if (status != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "ratbagd %s returned status %d", method, (gint32) status);
      return FALSE;
    }
  return TRUE;
}
