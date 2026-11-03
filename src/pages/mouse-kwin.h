#pragma once
#include <gio/gio.h>
typedef struct { double speed, scroll_factor; guint profile; gboolean natural, has_scroll_factor, has_scroll_on_button, scroll_on_button; } MouseKwinState;
gboolean mouse_kwin_find (GDBusConnection *bus, guint16 vendor, guint16 product, char **path, GError **error);
gboolean mouse_kwin_read (GDBusConnection *bus, const char *path, MouseKwinState *state, GError **error);
gboolean mouse_kwin_write (GDBusConnection *bus, const char *path, const MouseKwinState *state, GError **error);
