#pragma once

#include "application.h"

G_BEGIN_DECLS

GtkWindow *epinephrine_window_new (EpinephrineApplication *app);
void dashboard_navigate_to_games (void);
void dashboard_navigate_to_hardware (void);
void epinephrine_show_toast (const char *message);
void epinephrine_retranslate (void);
void eph_restart_monitors (void);

G_END_DECLS
