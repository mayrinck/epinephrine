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
  AdwActionRow   *language_row;
  AdwToggleGroup *language_group;
} SettingsPage;

void          settings_update_brand_icon (SettingsPage *page, AccentChoice choice);

SettingsPage *settings_page_new (void);
void          settings_apply_accent (AccentChoice choice);
void          settings_apply_full_theme (AccentChoice choice, gboolean enabled);
AccentChoice  settings_load_accent (void);
gboolean      settings_load_full_theme (void);
AccentChoice  settings_detect_gpu_accent (void);
char         *settings_load_language (void);
void          settings_save_language (const char *lang_code);

G_END_DECLS
