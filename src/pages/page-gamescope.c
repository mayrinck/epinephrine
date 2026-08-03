#include "page-gamescope.h"
#include "command-builder.h"
#include "page-dashboard.h"
#include "epi-privilege.h"
#include "../epi-i18n.h"

#include <string.h>
#include <fcntl.h>

#define tr(x) epi_i18n_get (x)

static const char *SCALER_VALUES[] = { "auto", "integer", "fit", "fill", "stretch" };
static const char *FILTER_VALUES[] = { "linear", "nearest", "fsr", "nis", "pixel" };
static const char *BACKEND_VALUES[] = { "auto", "drm", "sdl", "wayland", "headless" };
static const char *ORIENTATION_VALUES[] = { "normal", "left", "right", "upsidedown" };

static EpinephrineSettings *
collect_settings (GamescopePage *page)
{
  EpinephrineSettings *s = g_new0 (EpinephrineSettings, 1);

  s->output_width   = (int) adw_spin_row_get_value (page->output_width);
  s->output_height  = (int) adw_spin_row_get_value (page->output_height);
  s->nested_width   = (int) adw_spin_row_get_value (page->nested_width);
  s->nested_height  = (int) adw_spin_row_get_value (page->nested_height);
  s->nested_refresh = (int) adw_spin_row_get_value (page->nested_refresh);
  s->nested_unfocused_refresh = (int) adw_spin_row_get_value (page->nested_unfocused_refresh);
  s->fullscreen     = adw_switch_row_get_active (page->fullscreen);
  s->borderless     = adw_switch_row_get_active (page->borderless);
  s->display_index  = (int) adw_spin_row_get_value (page->display_index);

  {
    guint idx = adw_combo_row_get_selected (page->scaler);
    if (idx < G_N_ELEMENTS (SCALER_VALUES))
      s->scaler = g_strdup (SCALER_VALUES[idx]);
    else
      s->scaler = g_strdup ("auto");
  }

  {
    guint idx = adw_combo_row_get_selected (page->filter);
    if (idx < G_N_ELEMENTS (FILTER_VALUES))
      s->filter = g_strdup (FILTER_VALUES[idx]);
    else
      s->filter = g_strdup ("linear");
  }

  s->sharpness = (int) gtk_range_get_value (page->sharpness);
  s->max_scale = adw_spin_row_get_value (page->max_scale);
  s->framerate_limit = (int) adw_spin_row_get_value (page->framerate_limit);

  s->hdr_enabled          = adw_switch_row_get_active (page->hdr_enabled);
  s->hdr_sdr_content_nits = (int) adw_spin_row_get_value (page->hdr_sdr_content_nits);
  s->hdr_itm_enabled       = adw_switch_row_get_active (page->hdr_itm_enabled);
  s->hdr_itm_sdr_nits      = (int) adw_spin_row_get_value (page->hdr_itm_sdr_nits);
  s->hdr_itm_target_nits   = (int) adw_spin_row_get_value (page->hdr_itm_target_nits);
  s->sdr_gamut_wideness    = gtk_range_get_value (page->sdr_gamut_wideness);

  {
    guint idx = adw_combo_row_get_selected (page->backend);
    if (idx < G_N_ELEMENTS (BACKEND_VALUES))
      s->backend = g_strdup (BACKEND_VALUES[idx]);
    else
      s->backend = g_strdup ("auto");
  }

  s->steam_integration  = adw_switch_row_get_active (page->steam_integration);
  s->adaptive_sync      = adw_switch_row_get_active (page->adaptive_sync);
  s->mouse_sensitivity  = adw_spin_row_get_value (page->mouse_sensitivity);
  s->grab_keyboard      = adw_switch_row_get_active (page->grab_keyboard);
  s->force_grab_cursor  = adw_switch_row_get_active (page->force_grab_cursor);
  s->expose_wayland     = adw_switch_row_get_active (page->expose_wayland);
  s->mangoapp           = adw_switch_row_get_active (page->mangoapp);

  {
    guint idx = adw_combo_row_get_selected (page->force_orientation);
    if (idx < G_N_ELEMENTS (ORIENTATION_VALUES))
      s->force_orientation = g_strdup (ORIENTATION_VALUES[idx]);
    else
      s->force_orientation = g_strdup ("normal");
  }

  {
    const char *text = gtk_editable_get_text (GTK_EDITABLE (page->prefer_output));
    s->prefer_output = g_strdup (text);
  }

  s->immediate_flips     = adw_switch_row_get_active (page->immediate_flips);
  s->keep_alive           = adw_switch_row_get_active (page->keep_alive);
  s->cursor_scale_height  = (int) adw_spin_row_get_value (page->cursor_scale_height);

  {
    const char *text = gtk_editable_get_text (GTK_EDITABLE (page->reshade_effect));
    s->reshade_effect = g_strdup (text);
  }

  s->reshade_technique_idx = (int) adw_spin_row_get_value (page->reshade_technique_idx);

  return s;
}

static void
free_settings (EpinephrineSettings *s)
{
  g_free (s->scaler);
  g_free (s->filter);
  g_free (s->backend);
  g_free (s->force_orientation);
  g_free (s->prefer_output);
  g_free (s->reshade_effect);
  g_free (s);
}

static void
update_command (GamescopePage *page)
{
  EpinephrineSettings *settings = collect_settings (page);
  char *command = NULL;

  command_builder_build (settings, &command);

  gtk_editable_set_text (GTK_EDITABLE (page->command_entry), command);

  g_free (command);
  free_settings (settings);
}

static void
on_restore_defaults_clicked (GtkButton *button, GamescopePage *page)
{
  (void) button;
  adw_spin_row_set_value (page->output_width, 0);
  adw_spin_row_set_value (page->output_height, 0);
  adw_spin_row_set_value (page->nested_width, 0);
  adw_spin_row_set_value (page->nested_height, 0);
  adw_spin_row_set_value (page->nested_refresh, 0);
  adw_spin_row_set_value (page->nested_unfocused_refresh, 0);
  adw_switch_row_set_active (page->fullscreen, FALSE);
  adw_switch_row_set_active (page->borderless, FALSE);
  adw_spin_row_set_value (page->display_index, 0);
  adw_combo_row_set_selected (page->scaler, 0);
  adw_combo_row_set_selected (page->filter, 0);
  gtk_range_set_value (page->sharpness, 2);
  adw_spin_row_set_value (page->max_scale, 0);
  adw_spin_row_set_value (page->framerate_limit, 0);
  adw_switch_row_set_active (page->hdr_enabled, FALSE);
  adw_spin_row_set_value (page->hdr_sdr_content_nits, 400);
  adw_switch_row_set_active (page->hdr_itm_enabled, FALSE);
  adw_spin_row_set_value (page->hdr_itm_sdr_nits, 100);
  adw_spin_row_set_value (page->hdr_itm_target_nits, 1000);
  gtk_range_set_value (page->sdr_gamut_wideness, 0);
  adw_combo_row_set_selected (page->backend, 0);
  adw_switch_row_set_active (page->steam_integration, FALSE);
  adw_switch_row_set_active (page->adaptive_sync, FALSE);
  adw_spin_row_set_value (page->mouse_sensitivity, 1);
  adw_switch_row_set_active (page->grab_keyboard, FALSE);
  adw_switch_row_set_active (page->force_grab_cursor, FALSE);
  adw_switch_row_set_active (page->expose_wayland, FALSE);
  adw_switch_row_set_active (page->mangoapp, FALSE);
  adw_combo_row_set_selected (page->force_orientation, 0);
  gtk_editable_set_text (GTK_EDITABLE (page->prefer_output), "");
  adw_switch_row_set_active (page->immediate_flips, FALSE);
  adw_switch_row_set_active (page->keep_alive, FALSE);
  adw_spin_row_set_value (page->cursor_scale_height, 0);
  gtk_editable_set_text (GTK_EDITABLE (page->reshade_effect), "");
  adw_spin_row_set_value (page->reshade_technique_idx, 0);

  dashboard_show_toast (tr("Defaults restored (not saved)"));
  update_command (page);
}

static void
set_combo_by_value (AdwComboRow *row, const char *value,
                    const char *const *values, guint n)
{
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (values[i], value) == 0)
      {
        adw_combo_row_set_selected (row, i);
        return;
      }
}

static void
apply_command_to_widgets (GamescopePage *page, const char *command)
{
  if (!command || !command[0]) return;

  char *dup = g_strdup (command);
  g_strstrip (dup);
  char **toks = g_strsplit (dup, " ", -1);
  gboolean in_app = FALSE;

  for (int i = 0; toks[i]; i++)
    {
      const char *t = toks[i];
      if (t[0] == '\0') continue;
      if (strcmp (t, "gamescope") == 0) continue;
      if (strcmp (t, "--") == 0) { in_app = TRUE; continue; }
      if (in_app) break;

      const char *val = toks[i + 1];

      if      (g_strcmp0 (t, "-W") == 0)
        { if (val) adw_spin_row_set_value (page->output_width, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-H") == 0)
        { if (val) adw_spin_row_set_value (page->output_height, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-w") == 0)
        { if (val) adw_spin_row_set_value (page->nested_width, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-h") == 0)
        { if (val) adw_spin_row_set_value (page->nested_height, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-r") == 0)
        { if (val) adw_spin_row_set_value (page->nested_refresh, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-o") == 0)
        { if (val) adw_spin_row_set_value (page->nested_unfocused_refresh, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-S") == 0)
        { if (val) set_combo_by_value (page->scaler, val, SCALER_VALUES, G_N_ELEMENTS (SCALER_VALUES)); i++; }
      else if (g_strcmp0 (t, "-F") == 0)
        { if (val) set_combo_by_value (page->filter, val, FILTER_VALUES, G_N_ELEMENTS (FILTER_VALUES)); i++; }
      else if (g_strcmp0 (t, "--sharpness") == 0)
        { if (val) gtk_range_set_value (page->sharpness, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-m") == 0)
        { if (val) adw_spin_row_set_value (page->max_scale, g_ascii_strtod (val, NULL)); i++; }
      else if (g_strcmp0 (t, "--framerate-limit") == 0)
        { if (val) adw_spin_row_set_value (page->framerate_limit, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-f") == 0)
        adw_switch_row_set_active (page->fullscreen, TRUE);
      else if (g_strcmp0 (t, "-b") == 0)
        adw_switch_row_set_active (page->borderless, TRUE);
      else if (g_strcmp0 (t, "--display-index") == 0)
        { if (val) adw_spin_row_set_value (page->display_index, atoi (val)); i++; }
      else if (g_strcmp0 (t, "--backend") == 0)
        { if (val) set_combo_by_value (page->backend, val, BACKEND_VALUES, G_N_ELEMENTS (BACKEND_VALUES)); i++; }
      else if (g_strcmp0 (t, "--adaptive-sync") == 0)
        adw_switch_row_set_active (page->adaptive_sync, TRUE);
      else if (g_strcmp0 (t, "-e") == 0)
        adw_switch_row_set_active (page->steam_integration, TRUE);
      else if (g_strcmp0 (t, "-g") == 0)
        adw_switch_row_set_active (page->grab_keyboard, TRUE);
      else if (g_strcmp0 (t, "--force-grab-cursor") == 0)
        adw_switch_row_set_active (page->force_grab_cursor, TRUE);
      else if (g_strcmp0 (t, "--expose-wayland") == 0)
        adw_switch_row_set_active (page->expose_wayland, TRUE);
      else if (g_strcmp0 (t, "--mangoapp") == 0)
        adw_switch_row_set_active (page->mangoapp, TRUE);
      else if (g_strcmp0 (t, "--immediate-flips") == 0)
        adw_switch_row_set_active (page->immediate_flips, TRUE);
      else if (g_strcmp0 (t, "--keep-alive") == 0)
        adw_switch_row_set_active (page->keep_alive, TRUE);
      else if (g_strcmp0 (t, "--force-orientation") == 0)
        { if (val) set_combo_by_value (page->force_orientation, val, ORIENTATION_VALUES, G_N_ELEMENTS (ORIENTATION_VALUES)); i++; }
      else if (g_strcmp0 (t, "-O") == 0)
        { if (val) gtk_editable_set_text (GTK_EDITABLE (page->prefer_output), val); i++; }
      else if (g_strcmp0 (t, "--cursor-scale-height") == 0)
        { if (val) adw_spin_row_set_value (page->cursor_scale_height, atoi (val)); i++; }
      else if (g_strcmp0 (t, "-s") == 0)
        { if (val) adw_spin_row_set_value (page->mouse_sensitivity, g_ascii_strtod (val, NULL)); i++; }
      else if (g_strcmp0 (t, "--hdr-enabled") == 0)
        adw_switch_row_set_active (page->hdr_enabled, TRUE);
      else if (g_strcmp0 (t, "--hdr-sdr-content-nits") == 0)
        { if (val) adw_spin_row_set_value (page->hdr_sdr_content_nits, atoi (val)); i++; }
      else if (g_strcmp0 (t, "--hdr-itm-enabled") == 0)
        adw_switch_row_set_active (page->hdr_itm_enabled, TRUE);
      else if (g_strcmp0 (t, "--hdr-itm-sdr-nits") == 0)
        { if (val) adw_spin_row_set_value (page->hdr_itm_sdr_nits, atoi (val)); i++; }
      else if (g_strcmp0 (t, "--hdr-itm-target-nits") == 0)
        { if (val) adw_spin_row_set_value (page->hdr_itm_target_nits, atoi (val)); i++; }
      else if (g_strcmp0 (t, "--sdr-gamut-wideness") == 0)
        { if (val) gtk_range_set_value (page->sdr_gamut_wideness, g_ascii_strtod (val, NULL)); i++; }
      else if (g_strcmp0 (t, "--reshade-effect") == 0)
        { if (val) gtk_editable_set_text (GTK_EDITABLE (page->reshade_effect), val); i++; }
      else if (g_strcmp0 (t, "--reshade-technique-idx") == 0)
        { if (val) adw_spin_row_set_value (page->reshade_technique_idx, atoi (val)); i++; }
    }

  g_strfreev (toks);
  g_free (dup);
}

static void
on_load_defaults_clicked (GtkButton *button, GamescopePage *page)
{
  (void) button;
  char *content = NULL;
  if (!g_file_get_contents ("/etc/gamescope/epinephrine.conf", &content, NULL, NULL))
    {
      dashboard_show_toast (tr("No saved configuration found"));
      return;
    }
  apply_command_to_widgets (page, content);
  g_free (content);
  update_command (page);
  dashboard_show_toast (tr("Loaded saved configuration"));
}

static void
on_save_defaults_clicked (GtkButton *button, GamescopePage *page)
{
  (void) button;
  EpinephrineSettings *settings = collect_settings (page);
  char *command = NULL;
  command_builder_build (settings, &command);
  free_settings (settings);

  /* /etc/gamescope is root-owned: write through the admin session */
  char tmp_path[] = "/tmp/gamescope-conf-XXXXXX";
  int fd = g_mkstemp_full (tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      dashboard_show_toast (tr("Failed to create temp file for elevation"));
      g_free (command);
      return;
    }

  FILE *f = fdopen (fd, "w");
  if (f)
    {
      fputs (command, f);
      fclose (f);
    }
  else
    close (fd);

  char *install_cmd = g_strdup_printf (
    "mkdir -p /etc/gamescope && install -D -m 644 '%s' /etc/gamescope/epinephrine.conf", tmp_path);
  int exit_code = -1;
  GError *err2 = NULL;
  if (epi_privilege_exec_sync (install_cmd, &exit_code, NULL, &err2))
    {
      if (exit_code == 0)
        dashboard_show_toast (tr("Saved to /etc/gamescope/epinephrine.conf (elevated)"));
      else
        dashboard_show_toast (tr("Failed to save (permission denied)"));
    }
  else
    {
      if (err2)
        dashboard_show_toast (err2->message);
      else
        dashboard_show_toast (tr("Failed to launch pkexec"));
      g_clear_error (&err2);
    }
  g_free (install_cmd);
  unlink (tmp_path);

  g_free (command);
}

static void
on_copy_clicked (GtkButton *button, GamescopePage *page)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (page->command_entry));

  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (button));
  GdkClipboard *clipboard = gdk_display_get_clipboard (display);
  gdk_clipboard_set_text (clipboard, text);

  dashboard_show_toast (tr("Command copied to clipboard"));
}

static void
on_setting_changed (GObject *object, GParamSpec *pspec, GamescopePage *page)
{
  (void) object;
  (void) pspec;
  update_command (page);
}

static void
connect_signals (GamescopePage *page)
{
  g_signal_connect (page->output_width, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->output_height, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->nested_width, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->nested_height, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->nested_refresh, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->nested_unfocused_refresh, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->fullscreen, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->borderless, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->display_index, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->scaler, "notify::selected", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->filter, "notify::selected", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->sharpness, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->max_scale, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->framerate_limit, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->hdr_enabled, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->hdr_sdr_content_nits, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->hdr_itm_enabled, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->hdr_itm_sdr_nits, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->hdr_itm_target_nits, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->sdr_gamut_wideness, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->backend, "notify::selected", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->steam_integration, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->adaptive_sync, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->mouse_sensitivity, "notify::value", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->grab_keyboard, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->force_grab_cursor, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->expose_wayland, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->mangoapp, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->force_orientation, "notify::selected", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->prefer_output, "notify::text", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->immediate_flips, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->keep_alive, "notify::active", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->cursor_scale_height, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->reshade_effect, "notify::text", G_CALLBACK (on_setting_changed), page);
  g_signal_connect (page->reshade_technique_idx, "notify::value", G_CALLBACK (on_setting_changed), page);

  g_signal_connect (page->copy_button, "clicked", G_CALLBACK (on_copy_clicked), page);
  g_signal_connect (page->save_button, "clicked", G_CALLBACK (on_save_defaults_clicked), page);
  g_signal_connect (page->restore_button, "clicked", G_CALLBACK (on_restore_defaults_clicked), page);
  g_signal_connect (page->load_button, "clicked", G_CALLBACK (on_load_defaults_clicked), page);
}

GamescopePage *
gamescope_page_new (void)
{
  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-gamescope.ui");

  GamescopePage *page = g_new0 (GamescopePage, 1);

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "gamescope-page"));

  page->output_width   = ADW_SPIN_ROW (gtk_builder_get_object (builder, "output-width"));
  page->output_height  = ADW_SPIN_ROW (gtk_builder_get_object (builder, "output-height"));
  page->nested_width   = ADW_SPIN_ROW (gtk_builder_get_object (builder, "nested-width"));
  page->nested_height  = ADW_SPIN_ROW (gtk_builder_get_object (builder, "nested-height"));
  page->nested_refresh = ADW_SPIN_ROW (gtk_builder_get_object (builder, "nested-refresh"));
  page->nested_unfocused_refresh = ADW_SPIN_ROW (gtk_builder_get_object (builder, "nested-unfocused-refresh"));
  page->fullscreen     = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "fullscreen"));
  page->borderless     = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "borderless"));
  page->display_index  = ADW_SPIN_ROW (gtk_builder_get_object (builder, "display-index"));

  page->scaler   = ADW_COMBO_ROW (gtk_builder_get_object (builder, "scaler"));
  page->filter   = ADW_COMBO_ROW (gtk_builder_get_object (builder, "filter"));
  page->sharpness = GTK_RANGE (gtk_builder_get_object (builder, "sharpness"));
  page->max_scale = ADW_SPIN_ROW (gtk_builder_get_object (builder, "max-scale"));

  page->framerate_limit = ADW_SPIN_ROW (gtk_builder_get_object (builder, "framerate-limit"));

  page->hdr_enabled          = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "hdr-enabled"));
  page->hdr_sdr_content_nits = ADW_SPIN_ROW (gtk_builder_get_object (builder, "hdr-sdr-content-nits"));
  page->hdr_itm_enabled      = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "hdr-itm-enabled"));
  page->hdr_itm_sdr_nits     = ADW_SPIN_ROW (gtk_builder_get_object (builder, "hdr-itm-sdr-nits"));
  page->hdr_itm_target_nits  = ADW_SPIN_ROW (gtk_builder_get_object (builder, "hdr-itm-target-nits"));
  page->sdr_gamut_wideness   = GTK_RANGE (gtk_builder_get_object (builder, "sdr-gamut-wideness"));

  page->backend             = ADW_COMBO_ROW (gtk_builder_get_object (builder, "backend"));
  page->steam_integration   = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "steam-integration"));
  page->adaptive_sync       = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "adaptive-sync"));
  page->mouse_sensitivity   = ADW_SPIN_ROW (gtk_builder_get_object (builder, "mouse-sensitivity"));
  page->grab_keyboard       = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "grab-keyboard"));
  page->force_grab_cursor   = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "force-grab-cursor"));
  page->expose_wayland      = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "expose-wayland"));
  page->mangoapp            = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "mangoapp"));
  page->force_orientation   = ADW_COMBO_ROW (gtk_builder_get_object (builder, "force-orientation"));
  page->prefer_output       = GTK_ENTRY (gtk_builder_get_object (builder, "prefer-output"));
  page->immediate_flips     = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "immediate-flips"));
  page->keep_alive          = ADW_SWITCH_ROW (gtk_builder_get_object (builder, "keep-alive"));
  page->cursor_scale_height = ADW_SPIN_ROW (gtk_builder_get_object (builder, "cursor-scale-height"));

  page->reshade_effect       = GTK_ENTRY (gtk_builder_get_object (builder, "reshade-effect"));
  page->reshade_technique_idx = ADW_SPIN_ROW (gtk_builder_get_object (builder, "reshade-technique-idx"));

  page->command_entry  = GTK_ENTRY (gtk_builder_get_object (builder, "command-entry"));
  page->copy_button    = GTK_BUTTON (gtk_builder_get_object (builder, "copy-button"));
  page->save_button    = GTK_BUTTON (gtk_builder_get_object (builder, "save-defaults-button"));
  page->restore_button = GTK_BUTTON (gtk_builder_get_object (builder, "restore-defaults-button"));
  page->load_button    = GTK_BUTTON (gtk_builder_get_object (builder, "load-defaults-button"));

  connect_signals (page);
  update_command (page);

  {
    /* Read back the saved system-wide defaults, if present */
    char *content = NULL;
    if (g_file_get_contents ("/etc/gamescope/epinephrine.conf", &content, NULL, NULL))
      {
        apply_command_to_widgets (page, content);
        g_free (content);
        update_command (page);
      }
  }

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
