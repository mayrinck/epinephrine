#pragma once
#include "page-dashboard.h"

typedef void (*GameArtworkReady) (GtkWidget *target, GKeyFile *result, gpointer data);
void game_artwork_request (GtkWidget *target, const char * const *args,
                           GameArtworkReady callback, gpointer data, GDestroyNotify destroy);
GtkWidget *game_artwork_cover (GameShortcut *game, int width, int height);
void game_artwork_edit (GtkButton *button, DashboardPage *page);
GKeyFile *game_artwork_config_load (void);
gboolean game_artwork_config_save (GKeyFile *config);
gboolean game_artwork_move_identity (const char *old_command, const char *new_command);

char *game_artwork_identity (const char *command);
