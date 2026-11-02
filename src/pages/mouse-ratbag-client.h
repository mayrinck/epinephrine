#pragma once
#include <gio/gio.h>
#define MOUSE_RATBAG_BUS "org.freedesktop.ratbag1"
GVariant *mouse_ratbag_call (GDBusConnection *bus, const char *path, const char *iface,
                             const char *method, GVariant *args, const GVariantType *type,
                             GError **error);
GVariant *mouse_ratbag_get (GDBusConnection *bus, const char *path, const char *iface,
                            const char *name, GError **error);
gboolean mouse_ratbag_set (GDBusConnection *bus, const char *path, const char *iface,
                           const char *name, GVariant *value, GError **error);
gboolean mouse_ratbag_method (GDBusConnection *bus, const char *path, const char *iface,
                              const char *method, GError **error);
