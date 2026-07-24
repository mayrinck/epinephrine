#include "command-builder.h"

#include <string.h>
#include <stdio.h>

static void
append_flag (GString *cmd, const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  g_string_append_c (cmd, ' ');
  g_string_append_vprintf (cmd, fmt, args);
  va_end (args);
}

static void
append_switch (GString *cmd, const char *flag, gboolean condition)
{
  if (condition)
    {
      g_string_append_c (cmd, ' ');
      g_string_append (cmd, flag);
    }
}

static gboolean
is_default_int (int value, int default_value)
{
  return value == default_value;
}

static gboolean
is_default_double (double value, double default_value)
{
  return value == default_value;
}

static gboolean
is_default_string (const char *value, const char *default_value)
{
  if (value == NULL && default_value == NULL)
    return TRUE;
  if (value == NULL || default_value == NULL)
    return FALSE;
  return strcmp (value, default_value) == 0;
}

void
command_builder_build (EpinephrineSettings *settings, char **out_command)
{
  GString *cmd = g_string_new ("gamescope");

  if (settings->output_width > 0)
    append_flag (cmd, "-W %d", settings->output_width);
  if (settings->output_height > 0)
    append_flag (cmd, "-H %d", settings->output_height);

  if (settings->nested_width > 0)
    append_flag (cmd, "-w %d", settings->nested_width);
  if (settings->nested_height > 0)
    append_flag (cmd, "-h %d", settings->nested_height);

  if (!is_default_int (settings->nested_refresh, 0))
    append_flag (cmd, "-r %d", settings->nested_refresh);

  if (!is_default_int (settings->nested_unfocused_refresh, 0))
    append_flag (cmd, "-o %d", settings->nested_unfocused_refresh);

  if (!is_default_string (settings->scaler, "auto"))
    append_flag (cmd, "-S %s", settings->scaler);
  if (!is_default_string (settings->filter, "linear"))
    append_flag (cmd, "-F %s", settings->filter);

  if (!is_default_int (settings->sharpness, 2))
    append_flag (cmd, "--sharpness %d", settings->sharpness);

  if (!is_default_double (settings->max_scale, 0.0))
    append_flag (cmd, "-m %.1f", settings->max_scale);

  if (!is_default_int (settings->framerate_limit, 0))
    append_flag (cmd, "--framerate-limit %d", settings->framerate_limit);

  append_switch (cmd, "-f", settings->fullscreen);
  append_switch (cmd, "-b", settings->borderless);

  if (!is_default_int (settings->display_index, 0))
    append_flag (cmd, "--display-index %d", settings->display_index);

  if (!is_default_string (settings->backend, "auto"))
    append_flag (cmd, "--backend %s", settings->backend);

  append_switch (cmd, "--adaptive-sync", settings->adaptive_sync);
  append_switch (cmd, "-e", settings->steam_integration);
  append_switch (cmd, "-g", settings->grab_keyboard);
  append_switch (cmd, "--force-grab-cursor", settings->force_grab_cursor);
  append_switch (cmd, "--expose-wayland", settings->expose_wayland);
  append_switch (cmd, "--mangoapp", settings->mangoapp);
  append_switch (cmd, "--immediate-flips", settings->immediate_flips);
  append_switch (cmd, "--keep-alive", settings->keep_alive);

  if (!is_default_string (settings->force_orientation, "normal"))
    append_flag (cmd, "--force-orientation %s", settings->force_orientation);

  if (!is_default_string (settings->prefer_output, ""))
    append_flag (cmd, "-O %s", settings->prefer_output);

  if (settings->cursor_scale_height > 0)
    append_flag (cmd, "--cursor-scale-height %d", settings->cursor_scale_height);

  if (!is_default_double (settings->mouse_sensitivity, 1.0))
    append_flag (cmd, "-s %.2f", settings->mouse_sensitivity);

  append_switch (cmd, "--hdr-enabled", settings->hdr_enabled);
  if (settings->hdr_enabled)
    {
      if (!is_default_int (settings->hdr_sdr_content_nits, 400))
        append_flag (cmd, "--hdr-sdr-content-nits %d", settings->hdr_sdr_content_nits);
    }

  append_switch (cmd, "--hdr-itm-enabled", settings->hdr_itm_enabled);
  if (settings->hdr_itm_enabled)
    {
      if (!is_default_int (settings->hdr_itm_sdr_nits, 100))
        append_flag (cmd, "--hdr-itm-sdr-nits %d", settings->hdr_itm_sdr_nits);
      if (!is_default_int (settings->hdr_itm_target_nits, 1000))
        append_flag (cmd, "--hdr-itm-target-nits %d", settings->hdr_itm_target_nits);
    }

  if (!is_default_double (settings->sdr_gamut_wideness, 0.0))
    append_flag (cmd, "--sdr-gamut-wideness %.2f", settings->sdr_gamut_wideness);

  if (!is_default_string (settings->reshade_effect, ""))
    append_flag (cmd, "--reshade-effect %s", settings->reshade_effect);
  if (!is_default_int (settings->reshade_technique_idx, 0))
    append_flag (cmd, "--reshade-technique-idx %d", settings->reshade_technique_idx);

  g_string_append (cmd, " -- %command%");

  *out_command = g_string_free (cmd, FALSE);
}
