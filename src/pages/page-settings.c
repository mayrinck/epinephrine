#include "page-settings.h"
#include "page-hardware.h"

#include "../epi-i18n.h"
#include "../window.h"
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
static char *g_config_dir = NULL;

static void
ensure_config_dir (void)
{
  if (g_config_dir) return;
  g_config_dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  g_mkdir_with_parents (g_config_dir, 0755);
}

static char *
get_config_file_path (const char *name)
{
  ensure_config_dir ();
  return g_build_filename (g_config_dir, name, NULL);
}

static gboolean
save_setting (const char *path, const char *contents, gssize length, GError **unused)
{
  (void) unused;
  g_autoptr(GError) error = NULL;
  if (g_file_set_contents_full (path, contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error)) return TRUE;
  g_warning ("Could not save settings: %s", error->message);
  epinephrine_show_toast (error->message);
  return FALSE;
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
  save_setting (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

static void
save_full_theme (gboolean enabled)
{
  char *path = get_config_file_path ("full-theme");
  char *data = g_strdup_printf ("%d", enabled ? 1 : 0);
  save_setting (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

/* ---- Graph Speed ---- */

int g_graph_speed_ms = GRAPH_SPEED_BALANCED_MS; /* default 100ms */

int
settings_load_graph_speed (void)
{
  char *path = get_config_file_path ("graph-speed-ms");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents)
    {
      int val = atoi (contents);
      g_free (contents);
      if (val > 0 && val <= 2500)
        return val;
    }
  return GRAPH_SPEED_BALANCED_MS;
}

void
settings_save_graph_speed (int speed_ms)
{
  char *path = get_config_file_path ("graph-speed-ms");
  char *data = g_strdup_printf ("%d", speed_ms);
  save_setting (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

/* ---- Untrusted Launchers ---- */

gboolean
settings_load_untrusted_launchers (void)
{
  char *path = get_config_file_path ("untrusted-launchers");
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

void
settings_save_untrusted_launchers (gboolean enabled)
{
  char *path = get_config_file_path ("untrusted-launchers");
  char *data = g_strdup_printf ("%d", enabled ? 1 : 0);
  save_setting (path, data, -1, NULL);
  g_free (data);
  g_free (path);
}

void
settings_apply_graph_speed (void)
{
  extern void eph_restart_monitors (void);
  eph_restart_monitors ();
}

/* ---- Language ---- */

char *
settings_load_language (void)
{
  char *path = get_config_file_path ("language");
  char *contents = file_read_all (path);
  g_free (path);

  if (contents && contents[0])
    {
      /* Migrate legacy numeric values (0 = System, 1 = pt_BR, 2 = en_US) */
      if (g_str_equal (contents, "0"))
        {
          g_free (contents);
          return g_strdup ("system");
        }
      if (g_str_equal (contents, "1"))
        {
          g_free (contents);
          return g_strdup ("pt_BR");
        }
      if (g_str_equal (contents, "2"))
        {
          g_free (contents);
          return g_strdup ("en_US");
        }
      char *code = g_strdup (contents);
      g_free (contents);
      return code;
    }
  g_free (contents);
  return g_strdup ("system");
}

void
settings_save_language (const char *lang_code)
{
  char *path = get_config_file_path ("language");
  save_setting (path,
    lang_code && lang_code[0] ? lang_code : "system", -1, NULL);
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

  static char css[1024];
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

  /* Update hardware page gradient divider */
  hw_update_accent_color (choice);

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
on_custom_speed_changed (GtkSpinButton *spin, SettingsPage *page)
{
  (void) page;
  guint sel = adw_combo_row_get_selected (page->graph_speed_combo);
  if (sel == GRAPH_SPEED_CUSTOM_IDX)
    {
      g_graph_speed_ms = (int) gtk_spin_button_get_value (spin);
      settings_save_graph_speed (g_graph_speed_ms);
      settings_apply_graph_speed ();
    }
}

static void
on_graph_speed_changed (AdwComboRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  static const int speed_values[] = {
    GRAPH_SPEED_REALTIME_MS,
    GRAPH_SPEED_BALANCED_MS,
    GRAPH_SPEED_POWERSAVE_MS
  };
  guint idx = adw_combo_row_get_selected (row);

  /* Show/hide custom row */
  gboolean is_custom = (idx == GRAPH_SPEED_CUSTOM_IDX);
  gtk_widget_set_visible (GTK_WIDGET (page->custom_speed_row), is_custom);

  if (idx < G_N_ELEMENTS (speed_values))
    {
      g_graph_speed_ms = speed_values[idx];
      settings_save_graph_speed (g_graph_speed_ms);
      settings_apply_graph_speed ();
    }
  else if (is_custom)
    {
      g_graph_speed_ms = (int) gtk_spin_button_get_value (page->custom_speed_spin);
      settings_save_graph_speed (g_graph_speed_ms);
      settings_apply_graph_speed ();
    }
}

static void
on_language_changed (AdwComboRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  (void) page;
  if (epi_i18n_in_apply ())
    return;

  guint sel = adw_combo_row_get_selected (row);
  int count = 0;
  EpiLanguage *langs = epi_i18n_list_languages (&count);

  const char *code = "system";
  if (sel > 0 && sel <= (guint) count)
    code = langs[sel - 1].code;

  epi_i18n_free_languages (langs, count);

  settings_save_language (code);
  epi_i18n_set_language (code);
  epinephrine_retranslate ();
}

static void
on_untrusted_launchers_toggled (AdwSwitchRow *row, GParamSpec *pspec, SettingsPage *page)
{
  (void) pspec;
  (void) page;
  gboolean active = adw_switch_row_get_active (row);
  settings_save_untrusted_launchers (active);
}

/* ---- Language toggle group ---- */

static const char *
flag_emoji_for_code (const char *code)
{
  if (g_strcmp0 (code, "pt_BR") == 0)
    return "\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7";  /* 🇧🇷 */
  if (g_strcmp0 (code, "en_US") == 0)
    return "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";  /* 🇺🇸 */
  if (g_strcmp0 (code, "es_ES") == 0)
    return "\xf0\x9f\x87\xaa\xf0\x9f\x87\xb8";  /* 🇪🇸 */
  return "\xf0\x9f\x8f\xb3\xef\xb8\x8f";        /* 🏳️  (white flag) */
}

static guint
language_index_for_code (const char *code)
{
  if (!code || g_strcmp0 (code, "system") == 0)
    return 0;

  int count = 0;
  EpiLanguage *langs = epi_i18n_list_languages (&count);
  guint index = 0;
  for (int i = 0; i < count; i++)
    if (g_strcmp0 (langs[i].code, code) == 0)
      index = (guint) i + 1;
  epi_i18n_free_languages (langs, count);
  return index;
}

static void
language_combo_rebuild (SettingsPage *page)
{
  GtkStringList *model = gtk_string_list_new (NULL);

  /* Item 0: System */
  char *sys_label = g_strdup_printf ("%s %s",
    flag_emoji_for_code (NULL), epi_i18n_get ("System"));
  gtk_string_list_append (model, sys_label);
  g_free (sys_label);

  /* Items 1..N: each available language */
  int count = 0;
  EpiLanguage *langs = epi_i18n_list_languages (&count);
  for (int i = 0; i < count; i++)
    {
      char *label = g_strdup_printf ("%s %s",
        flag_emoji_for_code (langs[i].code), langs[i].native_name);
      gtk_string_list_append (model, label);
      g_free (label);
    }
  epi_i18n_free_languages (langs, count);

  adw_combo_row_set_model (page->language_combo, G_LIST_MODEL (model));
  g_object_unref (model);
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
  page->graph_speed_combo = ADW_COMBO_ROW (gtk_builder_get_object (builder, "graph-speed-row"));
  page->custom_speed_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "custom-speed-row"));
  page->custom_speed_spin = GTK_SPIN_BUTTON (gtk_builder_get_object (builder, "custom-speed-spin"));

  AdwSwitchRow *untrusted_row = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "untrusted-launchers-row"));

  AccentChoice saved = settings_load_accent ();
  gboolean theme_saved = settings_load_full_theme ();
  char *lang_saved = settings_load_language ();
  g_graph_speed_ms = settings_load_graph_speed ();

  g_signal_connect (page->accent_combo, "notify::selected",
    G_CALLBACK (on_accent_changed), page);
  g_signal_connect (page->apply_theme_row, "notify::active",
    G_CALLBACK (on_theme_toggled), page);
  g_signal_connect (page->language_combo, "notify::selected",
    G_CALLBACK (on_language_changed), page);
  g_signal_connect (page->graph_speed_combo, "notify::selected",
    G_CALLBACK (on_graph_speed_changed), page);
  g_signal_connect (page->custom_speed_spin, "value-changed",
    G_CALLBACK (on_custom_speed_changed), page);
  g_signal_connect (untrusted_row, "notify::active",
    G_CALLBACK (on_untrusted_launchers_toggled), page);

  adw_combo_row_set_selected (page->accent_combo, saved);
  adw_switch_row_set_active (page->apply_theme_row, theme_saved);
  adw_switch_row_set_active (untrusted_row, settings_load_untrusted_launchers ());

  language_combo_rebuild (page);
  adw_combo_row_set_selected (page->language_combo,
    language_index_for_code (lang_saved));
  g_free (lang_saved);

  /* Set graph speed combo to match loaded value */
  {
    guint speed_idx = 1; /* default Balanced */
    if (g_graph_speed_ms == GRAPH_SPEED_REALTIME_MS)
      speed_idx = 0;
    else if (g_graph_speed_ms == GRAPH_SPEED_BALANCED_MS)
      speed_idx = 1;
    else if (g_graph_speed_ms == GRAPH_SPEED_POWERSAVE_MS)
      speed_idx = 2;
    else
      speed_idx = GRAPH_SPEED_CUSTOM_IDX;

    gtk_spin_button_set_value (page->custom_speed_spin, g_graph_speed_ms);
    gtk_widget_set_visible (GTK_WIDGET (page->custom_speed_row),
      speed_idx == GRAPH_SPEED_CUSTOM_IDX);
    adw_combo_row_set_selected (page->graph_speed_combo, speed_idx);
  }

  settings_update_brand_icon (page, resolve_accent (saved));

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, 24, 16);
  gtk_widget_set_visible (spacer, TRUE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (page->apply_theme_row), spacer);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
