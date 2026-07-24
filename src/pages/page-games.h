#pragma once

#include <adwaita.h>
#include "page-dashboard.h"

G_BEGIN_DECLS

GtkWidget *games_page_new (DashboardPage *dash_page);
void games_page_register_actions (GActionMap *map, DashboardPage *dash_page);
void refresh_games_grid (DashboardPage *page);

G_END_DECLS
