#pragma once
#include <gio/gio.h>
GSettings *mouse_settings_open (const char *desktop);
gboolean mouse_settings_write (GSettings *settings, double speed, const char *profile,
                               gboolean natural, GError **error);
