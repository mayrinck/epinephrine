#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef enum
{
  ACCENT_FOLLOW_GPU = 0,
  ACCENT_AMD,
  ACCENT_INTEL,
  ACCENT_NVIDIA,
  ACCENT_TUX,
} AccentChoice;

typedef struct
{
  GtkWidget      *box;
  GtkWidget      *brand_icon;
  GtkWidget      *brand_version;
  AdwComboRow    *accent_combo;
  AdwSwitchRow   *apply_theme_row;
  AdwComboRow    *language_combo;
  AdwComboRow    *graph_speed_combo;
  AdwActionRow   *custom_speed_row;
  GtkSpinButton  *custom_speed_spin;
} SettingsPage;

/* Graph speed presets (index into page-settings.ui combo) */
#define GRAPH_SPEED_REALTIME_MS    25
#define GRAPH_SPEED_BALANCED_MS   250
#define GRAPH_SPEED_POWERSAVE_MS  500
#define GRAPH_SPEED_CUSTOM_IDX      3

/* Data labels always update at 1 second regardless of graph speed */
#define MONITOR_DATA_INTERVAL_MS 1000

extern int g_graph_speed_ms;

void          settings_update_brand_icon (SettingsPage *page, AccentChoice choice);

SettingsPage *settings_page_new (void);
void          settings_apply_accent (AccentChoice choice);
void          settings_apply_full_theme (AccentChoice choice, gboolean enabled);
AccentChoice  settings_load_accent (void);
gboolean      settings_load_full_theme (void);
AccentChoice  settings_detect_gpu_accent (void);
char         *settings_load_language (void);
void          settings_save_language (const char *lang_code);

int           settings_load_graph_speed (void);
void          settings_save_graph_speed (int speed_ms);
void          settings_apply_graph_speed (void);

gboolean      settings_load_untrusted_launchers (void);
void          settings_save_untrusted_launchers (gboolean enabled);

G_END_DECLS
