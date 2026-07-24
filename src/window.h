#pragma once

#include "application.h"

G_BEGIN_DECLS

GtkWindow *epinephrine_window_new (EpinephrineApplication *app);
void dashboard_navigate_to_games (void);
void epinephrine_show_toast (const char *message);

G_END_DECLS
