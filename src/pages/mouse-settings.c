#include "mouse-settings.h"
#include <math.h>
GSettings *
mouse_settings_open (const char *desktop)
{
  g_auto(GStrv) names = desktop ? g_strsplit (desktop, ":", -1) : NULL;
  gboolean supported = FALSE;
  for (guint i=0; names && names[i]; i++)
    if (g_ascii_strcasecmp(names[i], "GNOME") == 0)
      supported = TRUE;
  if (!supported) return NULL;
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  if (!source) return NULL;
  g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup (source,
    "org.gnome.desktop.peripherals.mouse", TRUE);
  if (!schema || !g_settings_schema_has_key (schema, "speed") ||
      !g_settings_schema_has_key (schema, "accel-profile") ||
      !g_settings_schema_has_key (schema, "natural-scroll")) return NULL;
  return g_settings_new_full (schema, NULL, NULL);
}
gboolean
mouse_settings_write (GSettings *settings, double speed, const char *profile,
                      gboolean natural, GError **error)
{
  if (!settings || !profile || !isfinite(speed) || speed < -1.0 || speed > 1.0 ||
      !(g_str_equal(profile,"default") || g_str_equal(profile,"flat") || g_str_equal(profile,"adaptive")))
    {
      g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Invalid mouse settings");
      return FALSE;
    }
  if (!g_settings_is_writable(settings,"speed") || !g_settings_is_writable(settings,"accel-profile") ||
      !g_settings_is_writable(settings,"natural-scroll"))
    {
      g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_PERMISSION_DENIED,"Mouse settings are not writable");
      return FALSE;
    }
  g_settings_delay(settings);
  gboolean ok = g_settings_set_double(settings,"speed",speed);
  ok = g_settings_set_string(settings,"accel-profile",profile) && ok;
  ok = g_settings_set_boolean(settings,"natural-scroll",natural) && ok;
  if (!ok)
    {
      g_settings_revert(settings);
      g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Desktop rejected mouse settings");
      return FALSE;
    }
  g_settings_apply(settings);
  return TRUE;
}
