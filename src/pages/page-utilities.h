#pragma once

#include <adwaita.h>
#include "page-gamescope.h"
#include "page-gamemode.h"
#include "page-mangohud.h"

G_BEGIN_DECLS

typedef struct
{
  GtkWidget         *box;
  AdwViewStack      *sub_stack;
  GamescopePage     *gamescope_page;
  GameModePage      *gamemode_page;
  MangoHudPage      *mangohud_page;
  GtkWidget         *gamescope_empty;
  GtkWidget         *gamemode_empty;
  GtkWidget         *mangohud_empty;
  GtkWidget         *rgb_controller_page;
} UtilidadesPage;

UtilidadesPage *utilidades_page_new (void);

G_END_DECLS
