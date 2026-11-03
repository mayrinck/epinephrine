#pragma once
#include <gio/gio.h>
typedef enum { EPI_COMPONENT_STEAM, EPI_COMPONENT_HEROIC, EPI_COMPONENT_LUTRIS,
 EPI_COMPONENT_BOTTLES, EPI_COMPONENT_OPENRGB, EPI_COMPONENT_MANGOHUD,
 EPI_COMPONENT_GAMESCOPE, EPI_COMPONENT_GAMEMODE, EPI_COMPONENT_COUNT } EpiComponent;
const char *epi_component_flatpak_id (EpiComponent component);
gboolean epi_component_installed (EpiComponent component);
char *epi_component_native_command (EpiComponent component, const char *manager);
void epi_component_install_async (EpiComponent component, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
gboolean epi_component_install_finish (GAsyncResult *result, GError **error);
