#include "page-settings.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#define VERSION "0.1.0"

static const char *ACCENT_COLORS[] = {
  NULL,        /* Follow GPU  */
  "#c01c28",   /* AMD Red     */
  "#0071c5",   /* Intel Blue  */
  "#76B900",   /* NVIDIA Green */
  "#ff8307",   /* TUX Orange  */
};

static GtkCssProvider *g_accent_provider = NULL;
static GtkCssProvider *g_theme_provider = NULL;
static GtkCssProvider *g_tema_provider = NULL;
static char *g_config_dir = NULL;

static const char *BREEZE_CSS =
  "* {"
  "  -GtkDialog-action-area-border: 5;"
  "  -GtkDialog-button-spacing: 6;"
  "  -GtkDialog-content-area-spacing: 6;"
  "}"
  "button {"
  "  border-radius: 4px;"
  "  padding: 4px 12px;"
  "  border: 1px solid rgba(0,0,0,0.15);"
  "  background-image: linear-gradient(to bottom, #fcfcfc, #eaeaea);"
  "  color: #31363b;"
  "  font-weight: normal;"
  "}"
  "button:hover {"
  "  background-image: linear-gradient(to bottom, #ffffff, #f4f4f4);"
  "}"
  "button:active {"
  "  background-image: linear-gradient(to bottom, #d6d6d6, #c8c8c8);"
  "}"
  "button.suggested-action {"
  "  background-image: linear-gradient(to bottom, #63b1f5, #3da2f0);"
  "  color: #ffffff;"
  "  border-color: #2980d1;"
  "}"
  "button.suggested-action:hover {"
  "  background-image: linear-gradient(to bottom, #77baf7, #4dabf2);"
  "}"
  "button.suggested-action:active {"
  "  background-image: linear-gradient(to bottom, #3da2f0, #2990e0);"
  "}"
  "button.destructive-action {"
  "  background-image: linear-gradient(to bottom, #e57373, #e53935);"
  "  color: #ffffff;"
  "  border-color: #c62828;"
  "}"
  "entry {"
  "  border-radius: 4px;"
  "  border: 1px solid rgba(0,0,0,0.15);"
  "  padding: 4px 8px;"
  "  background-color: #ffffff;"
  "  color: #31363b;"
  "}"
  "entry:focus {"
  "  border-color: #3da2f0;"
  "  box-shadow: 0 0 3px 1px rgba(61,162,240,0.4);"
  "}"
  "scrollbar {"
  "  background-color: rgba(0,0,0,0.05);"
  "  border: none;"
  "}"
  "scrollbar slider {"
  "  background-color: rgba(0,0,0,0.25);"
  "  border-radius: 4px;"
  "  min-width: 8px;"
  "  min-height: 8px;"
  "}"
  "scrollbar slider:hover {"
  "  background-color: rgba(0,0,0,0.35);"
  "}"
  "headerbar {"
  "  background-image: linear-gradient(to bottom, #f5f6f7, #e3e5e8);"
  "  border-bottom: 1px solid rgba(0,0,0,0.12);"
  "  padding: 4px 8px;"
  "}"
  "window, .background {"
  "  background-color: #eff0f1;"
  "  color: #31363b;"
  "}"
  "row {"
  "  background-color: #ffffff;"
  "  border-radius: 4px;"
  "  border: 1px solid rgba(0,0,0,0.08);"
  "  padding: 6px 12px;"
  "}"
  "row:hover {"
  "  background-color: #f5f5f5;"
  "}"
  "label {"
  "  color: #31363b;"
  "}"
  ".dim-label {"
  "  opacity: 0.65;"
  "}"
  ".heading {"
  "  font-weight: bold;"
  "  font-size: 1.15em;"
  "}"
  "switch {"
  "  background-color: rgba(0,0,0,0.15);"
  "  border-radius: 10px;"
  "}"
  "switch:checked {"
  "  background-color: #3da2f0;"
  "}"
  "dropdown {"
  "  border-radius: 4px;"
  "  border: 1px solid rgba(0,0,0,0.15);"
  "  background-image: linear-gradient(to bottom, #fcfcfc, #eaeaea);"
  "}"
  ".dashboard-card {"
  "  background-color: #ffffff;"
  "  border-radius: 8px;"
  "  border: 1px solid rgba(0,0,0,0.10);"
  "  padding: 4px;"
  "}"
  ".game-shortcut {"
  "  background-color: #ffffff;"
  "  border-radius: 8px;"
  "  border: 1px solid rgba(0,0,0,0.10);"
  "  padding: 16px;"
  "}"
  ".game-shortcut:hover {"
  "  background-color: #f5f5f5;"
  "  border-color: #3da2f0;"
  "}"
  ".gaming-score-card {"
  "  background-color: #ffffff;"
  "  border-radius: 8px;"
  "  border: 1px solid rgba(0,0,0,0.10);"
  "  padding: 20px 24px;"
  "}"
  "flowboxchild {"
  "  border: none;"
  "  box-shadow: none;"
  "  background: none;"
  "}"
  ".status-ok { color: #27ae60; }"
  ".status-warn { color: #f39c12; }"
  ".ctrl-indicator {"
  "  border-radius: 20px;"
  "  background: none;"
  "  border: 2px solid rgba(0,0,0,0.15);"
  "}"
  ".ctrl-indicator-active {"
  "  background-color: #3da2f0;"
  "  border-color: #3da2f0;"
  "  box-shadow: 0 0 12px 4px rgba(61,162,240,0.5);"
  "}"
  ".ctrl-label {"
  "  font-size: 12px;"
  "  font-weight: bold;"
  "  color: rgba(0,0,0,0.7);"
  "}"
  ".ctrl-indicator-active .ctrl-label {"
  "  color: #ffffff;"
  "}"
  "adw-preferences-page {"
  "  max-width: 65%;"
  "}"
  ".device-list-panel {"
  "  min-width: 240px;"
  "}"
  ".accent-label {"
  "  color: #3da2f0;"
  "}"
  ".gaming-score-value {"
  "  font-size: 2.5em;"
  "  font-weight: bold;"
  "  color: #3da2f0;"
  "}";

static void
ensure_config_dir (void)
{
  if (g_config_dir) return;
  const char *home = g_get_home_dir ();
  g_config_dir = g_build_filename (home, ".config", "epinephrine", NULL);
  g_mkdir_with_parents (g_config_dir, 0755);
}

static char *
get_config_file_path (const char *name)
{
  ensure_config_dir ();
  return g_build_filename (g_config_dir, name, NULL);
}

static char *
file_read_all (const char *path)
{
  char *contents = NULL;
  GError *err = NULL;
  if (g_file_get_contents (path, &contents, NULL, &err))
    {
      g_strstrip (contents);
      return contents;
    }
  g_error_free (err);
  return NULL;
}

/* ---- Hex parsing ---- */

static gboolean
parse_hex_color (const char *hex, int *r, int *g, int *b)
{
  if (!hex || hex[0] != '#' || strlen (hex) != 7)
    return FALSE;

  char *end = NULL;
  long val = strtol (hex + 1, &end, 16);
  if (end && *end == '\0' && val >= 0 && val <= 0xFFFFFF)
    {
      *r = (val >> 16) & 0xFF;
      *g = (val >> 8) & 0xFF;
      *b = val & 0xFF;
      return TRUE;
    }
  return FALSE;
}

static int
clamp_int (int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- WCAG contrast helpers ---- */

static double
relative_luminance (int r, int g, int b)
{
  double rs = r / 255.0, gs = g / 255.0, bs = b / 255.0;
  rs = rs <= 0.03928 ? rs / 12.92 : pow ((rs + 0.055) / 1.055, 2.4);
  gs = gs <= 0.03928 ? gs / 12.92 : pow ((gs + 0.055) / 1.055, 2.4);
  bs = bs <= 0.03928 ? bs / 12.92 : pow ((bs + 0.055) / 1.055, 2.4);
  return 0.2126 * rs + 0.7152 * gs + 0.0722 * bs;
}

static double
contrast_ratio (int r1, int g1, int b1, int r2, int g2, int b2)
{
  double l1 = relative_luminance (r1, g1, b1);
  double l2 = relative_luminance (r2, g2, b2);
  if (l1 < l2) { double t = l1; l1 = l2; l2 = t; }
  return (l1 + 0.05) / (l2 + 0.05);
}

/* ---- Theme palette generation ---- */

static void
generate_theme_css (const char *hex, const char *accent_fg_hex, char *buf, size_t buf_len)
{
  int ar, ag, ab;
  if (!parse_hex_color (hex, &ar, &ag, &ab))
    {
      buf[0] = '\0';
      return;
    }

  int afg_r, afg_g, afg_b;
  parse_hex_color (accent_fg_hex, &afg_r, &afg_g, &afg_b);
  int fg_r = 255, fg_g = 255, fg_b = 255;

  /* Background: very dark, tinted with accent (8% of accent) */
  int bg_r = clamp_int ((int)(ar * 0.08), 0, 255);
  int bg_g = clamp_int ((int)(ag * 0.08), 0, 255);
  int bg_b = clamp_int ((int)(ab * 0.08), 0, 255);

  /* View bg: slightly darker */
  int view_r = clamp_int ((int)(ar * 0.05), 0, 255);
  int view_g = clamp_int ((int)(ag * 0.05), 0, 255);
  int view_b = clamp_int ((int)(ab * 0.05), 0, 255);

  /* Headerbar: slightly lighter than bg */
  int hb_r = clamp_int ((int)(ar * 0.12), 0, 255);
  int hb_g = clamp_int ((int)(ag * 0.12), 0, 255);
  int hb_b = clamp_int ((int)(ab * 0.12), 0, 255);

  /* Card bg: elevated surface */
  int card_r = clamp_int ((int)(ar * 0.14), 0, 255);
  int card_g = clamp_int ((int)(ag * 0.14), 0, 255);
  int card_b = clamp_int ((int)(ab * 0.14), 0, 255);

  /* Border color: accent at low opacity */
  int brd_r = clamp_int ((int)(ar * 0.35), 0, 255);
  int brd_g = clamp_int ((int)(ag * 0.35), 0, 255);
  int brd_b = clamp_int ((int)(ab * 0.35), 0, 255);

  /* Ensure minimum contrast for text: white on backgrounds must be AAA (>=7:1) */
  /* If contrast is too low, darken the background further */
  while (contrast_ratio (255, 255, 255, bg_r, bg_g, bg_b) < 7.0 && bg_r > 0 && bg_g > 0 && bg_b > 0)
    {
      bg_r = (int)(bg_r * 0.9);
      bg_g = (int)(bg_g * 0.9);
      bg_b = (int)(bg_b * 0.9);
    }

  while (contrast_ratio (255, 255, 255, view_r, view_g, view_b) < 7.0 && view_r > 0 && view_g > 0 && view_b > 0)
    {
      view_r = (int)(view_r * 0.9);
      view_g = (int)(view_g * 0.9);
      view_b = (int)(view_b * 0.9);
    }

  while (contrast_ratio (255, 255, 255, card_r, card_g, card_b) < 7.0 && card_r > 0 && card_g > 0 && card_b > 0)
    {
      card_r = (int)(card_r * 0.9);
      card_g = (int)(card_g * 0.9);
      card_b = (int)(card_b * 0.9);
    }

  while (contrast_ratio (255, 255, 255, hb_r, hb_g, hb_b) < 7.0 && hb_r > 0 && hb_g > 0 && hb_b > 0)
    {
      hb_r = (int)(hb_r * 0.9);
      hb_g = (int)(hb_g * 0.9);
      hb_b = (int)(hb_b * 0.9);
    }

  snprintf (buf, buf_len,
    "@define-color accent_color %s;\n"
    "@define-color accent_bg_color %s;\n"
    "@define-color accent_fg_color rgb(%d,%d,%d);\n"
    "\n"
    "@define-color window_bg_color rgb(%d,%d,%d);\n"
    "@define-color window_fg_color rgb(%d,%d,%d);\n"
    "\n"
    "@define-color view_bg_color rgb(%d,%d,%d);\n"
    "@define-color view_fg_color rgb(%d,%d,%d);\n"
    "\n"
    "@define-color headerbar_bg_color rgb(%d,%d,%d);\n"
    "@define-color headerbar_fg_color rgb(%d,%d,%d);\n"
    "@define-color headerbar_border_color alpha(@accent_color, 0.15);\n"
    "\n"
    "@define-color card_bg_color rgb(%d,%d,%d);\n"
    "@define-color card_fg_color rgb(%d,%d,%d);\n"
    "@define-color card_shade_color alpha(rgb(0,0,0), 0.36);\n"
    "\n"
    "@define-color dialog_bg_color rgb(%d,%d,%d);\n"
    "@define-color dialog_fg_color rgb(%d,%d,%d);\n"
    "\n"
    "@define-color sidebar_bg_color rgb(%d,%d,%d);\n"
    "@define-color sidebar_fg_color rgb(%d,%d,%d);\n"
    "@define-color sidebar_shade_color alpha(rgb(0,0,0), 0.25);\n"
    "\n"
    "@define-color shade_color alpha(rgb(0,0,0), 0.36);\n"
    "@define-color borders alpha(rgb(%d,%d,%d), 0.15);\n"
    "@define-color insensitive_fg_color rgb(120,120,120);\n"
    "@define-color success_color #26a269;\n"
    "@define-color warning_color #e5a50a;\n"
    "@define-color error_color %s;\n",

    hex, hex,
    afg_r, afg_g, afg_b,

    bg_r, bg_g, bg_b,
    fg_r, fg_g, fg_b,

    view_r, view_g, view_b,
    fg_r, fg_g, fg_b,

    hb_r, hb_g, hb_b,
    fg_r, fg_g, fg_b,

    card_r, card_g, card_b,
    fg_r, fg_g, fg_b,

    card_r, card_g, card_b,
    fg_r, fg_g, fg_b,

    hb_r, hb_g, hb_b,
    fg_r, fg_g, fg_b,

    brd_r, brd_g, brd_b,

    hex
  );
}

/* ---- Public API ---- */

AccentChoice
settings_load_accent (void)
{
  char *path = get_config_file_path ("accent-color");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents)
    {
      int val = atoi (contents);
      g_free (contents);
      if (val >= ACCENT_FOLLOW_GPU && val <= ACCENT_TUX)
        return (AccentChoice) val;
    }
  return ACCENT_FOLLOW_GPU;
}

gboolean
settings_load_full_theme (void)
{
  char *path = get_config_file_path ("full-theme");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents)
    {
      gboolean val = (g_strcmp0 (contents, "1") == 0);
      g_free (contents);
      return val;
    }
  return FALSE;
}

static void
save_accent (AccentChoice choice)
{
  char *path = get_config_file_path ("accent-color");
  char *data = g_strdup_printf ("%d", (int) choice);
  g_file_set_contents (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

static void
save_full_theme (gboolean enabled)
{
  char *path = get_config_file_path ("full-theme");
  char *data = g_strdup_printf ("%d", enabled ? 1 : 0);
  g_file_set_contents (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

/* ---- Tema ---- */

TemaChoice
settings_load_tema (void)
{
  char *path = get_config_file_path ("tema");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents)
    {
      int val = atoi (contents);
      g_free (contents);
      if (val >= TEMA_SYSTEM && val <= TEMA_BREEZE)
        return (TemaChoice) val;
    }
  return TEMA_SYSTEM;
}

void
settings_save_tema (TemaChoice choice)
{
  char *path = get_config_file_path ("tema");
  char *data = g_strdup_printf ("%d", (int) choice);
  g_file_set_contents (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

void
settings_apply_tema (TemaChoice choice)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!display) return;

  /* Remove previous tema provider */
  if (g_tema_provider)
    {
      gtk_style_context_remove_provider_for_display (display,
        GTK_STYLE_PROVIDER (g_tema_provider));
      g_object_unref (g_tema_provider);
      g_tema_provider = NULL;
    }

  AdwStyleManager *sm = adw_style_manager_get_default ();

  switch (choice)
    {
    case TEMA_SYSTEM:
      adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_DEFAULT);
      break;

    case TEMA_LIBADWAITA:
      adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_PREFER_DARK);
      break;

    case TEMA_BREEZE:
      adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_FORCE_LIGHT);
      g_tema_provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (g_tema_provider, BREEZE_CSS);
      gtk_style_context_add_provider_for_display (display,
        GTK_STYLE_PROVIDER (g_tema_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      break;
    }
}

/* ---- Language ---- */

int
settings_load_language (void)
{
  char *path = get_config_file_path ("language");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents)
    {
      int val = atoi (contents);
      g_free (contents);
      if (val >= 0 && val <= 2)
        return val;
    }
  return 0;
}

void
settings_save_language (int lang)
{
  char *path = get_config_file_path ("language");
  char *data = g_strdup_printf ("%d", lang);
  g_file_set_contents (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

/* ---- GPU detection ---- */

static char *
detect_gpu_vendor (void)
{
  char *out = NULL;
  GError *err = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "/bin/sh", "-c",
      "lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, &err);

  if (err)
    {
      g_error_free (err);
      return NULL;
    }

  if (out)
    {
      g_strstrip (out);
      if (out[0] == '\0') { g_free (out); return NULL; }
    }
  return out;
}

AccentChoice
settings_detect_gpu_accent (void)
{
  char *gpu = detect_gpu_vendor ();
  if (!gpu)
    {
      return ACCENT_TUX;
    }

  char *lower = g_ascii_strdown (gpu, -1);
  AccentChoice choice;

  if (strstr (lower, "nvidia") || strstr (lower, "geforce") ||
      strstr (lower, "quadro"))
    choice = ACCENT_NVIDIA;
  else if (strstr (lower, "amd") || strstr (lower, "radeon"))
    choice = ACCENT_AMD;
  else if (strstr (lower, "intel"))
    choice = ACCENT_INTEL;
  else if (strstr (lower, "qualcomm") || strstr (lower, "adreno"))
    choice = ACCENT_TUX;
  else if (strstr (lower, "innosilicon"))
    choice = ACCENT_TUX;
  else
    choice = ACCENT_TUX;

  g_free (lower);
  g_free (gpu);
  return choice;
}

void
settings_apply_accent (AccentChoice choice)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!display) return;

  if (g_accent_provider)
    {
      gtk_style_context_remove_provider_for_display (display,
        GTK_STYLE_PROVIDER (g_accent_provider));
      g_object_unref (g_accent_provider);
      g_accent_provider = NULL;
    }

  /* Resolve Follow GPU to detected brand */
  if (choice == ACCENT_FOLLOW_GPU)
    choice = settings_detect_gpu_accent ();

  if (!ACCENT_COLORS[choice])
    return;

  const char *fg = (choice == ACCENT_TUX) ? "#000000" : "#ffffff";

  static char css[512];
  snprintf (css, sizeof (css),
    "@define-color accent_color %s;\n"
    "@define-color accent_bg_color %s;\n"
    "@define-color accent_fg_color %s;\n",
    ACCENT_COLORS[choice], ACCENT_COLORS[choice], fg);

  g_accent_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (g_accent_provider, css);
  gtk_style_context_add_provider_for_display (display,
    GTK_STYLE_PROVIDER (g_accent_provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

void
settings_apply_full_theme (AccentChoice choice, gboolean enabled)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (!display) return;

  if (g_theme_provider)
    {
      gtk_style_context_remove_provider_for_display (display,
        GTK_STYLE_PROVIDER (g_theme_provider));
      g_object_unref (g_theme_provider);
      g_theme_provider = NULL;
    }

  /* Resolve Follow GPU to detected brand */
  if (choice == ACCENT_FOLLOW_GPU)
    choice = settings_detect_gpu_accent ();

  if (!enabled || !ACCENT_COLORS[choice])
    return;

  const char *fg = (choice == ACCENT_TUX) ? "#000000" : "#ffffff";

  char css[4096];
  generate_theme_css (ACCENT_COLORS[choice], fg, css, sizeof (css));

  if (css[0] == '\0')
    return;

  g_theme_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (g_theme_provider, css);
  gtk_style_context_add_provider_for_display (display,
    GTK_STYLE_PROVIDER (g_theme_provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
}

/* ---- Brand icon helpers ---- */

static const char *
brand_resource_for_accent (AccentChoice choice)
{
  switch (choice)
    {
      case ACCENT_AMD:    return "/io/github/mayrinck/epinephrine/icons/brand-default.svg";
      case ACCENT_INTEL:  return "/io/github/mayrinck/epinephrine/icons/brand-blue.svg";
      case ACCENT_NVIDIA: return "/io/github/mayrinck/epinephrine/icons/brand-green.svg";
      case ACCENT_TUX:    return "/io/github/mayrinck/epinephrine/icons/brand-orange.svg";
      default:            return "/io/github/mayrinck/epinephrine/icons/brand-default.svg";
    }
}

static AccentChoice
resolve_accent (AccentChoice choice)
{
  if (choice == ACCENT_FOLLOW_GPU)
    return settings_detect_gpu_accent ();
  return choice;
}

void
settings_update_brand_icon (SettingsPage *page, AccentChoice choice)
{
  gtk_image_set_from_resource (GTK_IMAGE (page->brand_icon),
    brand_resource_for_accent (resolve_accent (choice)));
}

/* ---- Signal handlers ---- */

static void
update_theme_row_visibility (SettingsPage *page, AccentChoice choice)
{
  (void) choice;
  gtk_widget_set_visible (GTK_WIDGET (page->apply_theme_row), TRUE);
}

static void
on_accent_changed (AdwComboRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  AccentChoice choice = (AccentChoice) adw_combo_row_get_selected (row);
  AccentChoice resolved = resolve_accent (choice);

  settings_update_brand_icon (page, resolved);
  settings_apply_accent (choice);
  save_accent (choice);
  update_theme_row_visibility (page, choice);

  gboolean theme_on = adw_switch_row_get_active (page->apply_theme_row);
  settings_apply_full_theme (choice, theme_on);
}

static void
on_theme_toggled (AdwSwitchRow *sw, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  AccentChoice choice = (AccentChoice) adw_combo_row_get_selected (page->accent_combo);
  gboolean enabled = adw_switch_row_get_active (sw);
  settings_apply_full_theme (choice, enabled);
  save_full_theme (enabled);
}

static void
on_language_changed (AdwComboRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  (void) page;
  int lang = (int) adw_combo_row_get_selected (row);
  settings_save_language (lang);
}

static void
on_theme_changed (AdwComboRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  (void) page;
  TemaChoice choice = (TemaChoice) adw_combo_row_get_selected (row);
  settings_save_tema (choice);
  settings_apply_tema (choice);
}

SettingsPage *
settings_page_new (void)
{
  SettingsPage *page = g_new0 (SettingsPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-settings.ui");

  page->box            = GTK_WIDGET (gtk_builder_get_object (builder, "settings-page"));
  page->brand_icon     = GTK_WIDGET (gtk_builder_get_object (builder, "brand-icon"));
  page->brand_version  = GTK_WIDGET (gtk_builder_get_object (builder, "brand-version"));
  page->accent_combo   = ADW_COMBO_ROW (gtk_builder_get_object (builder, "accent-color-row"));
  page->apply_theme_row = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "apply-theme-row"));
  page->language_combo = ADW_COMBO_ROW (gtk_builder_get_object (builder, "language-row"));
  page->theme_combo    = ADW_COMBO_ROW (gtk_builder_get_object (builder, "theme-row"));

  AccentChoice saved = settings_load_accent ();
  gboolean theme_saved = settings_load_full_theme ();
  int lang_saved = settings_load_language ();
  TemaChoice tema_saved = settings_load_tema ();

  g_signal_connect (page->accent_combo, "notify::selected",
    G_CALLBACK (on_accent_changed), page);
  g_signal_connect (page->apply_theme_row, "notify::active",
    G_CALLBACK (on_theme_toggled), page);
  g_signal_connect (page->language_combo, "notify::selected",
    G_CALLBACK (on_language_changed), page);
  g_signal_connect (page->theme_combo, "notify::selected",
    G_CALLBACK (on_theme_changed), page);

  adw_combo_row_set_selected (page->accent_combo, saved);
  adw_switch_row_set_active (page->apply_theme_row, theme_saved);
  adw_combo_row_set_selected (page->language_combo, lang_saved);
  adw_combo_row_set_selected (page->theme_combo, tema_saved);

  settings_update_brand_icon (page, resolve_accent (saved));

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, 24, 16);
  gtk_widget_set_visible (spacer, TRUE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (page->apply_theme_row), spacer);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
