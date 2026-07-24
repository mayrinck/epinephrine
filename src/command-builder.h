#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  int      output_width;
  int      output_height;
  int      nested_width;
  int      nested_height;
  int      nested_refresh;
  int      nested_unfocused_refresh;
  gboolean fullscreen;
  gboolean borderless;
  int      display_index;

  char    *scaler;
  char    *filter;
  int      sharpness;
  double   max_scale;

  int      framerate_limit;

  gboolean hdr_enabled;
  int      hdr_sdr_content_nits;
  gboolean hdr_itm_enabled;
  int      hdr_itm_sdr_nits;
  int      hdr_itm_target_nits;
  double   sdr_gamut_wideness;

  char    *backend;
  gboolean steam_integration;
  gboolean adaptive_sync;
  double   mouse_sensitivity;
  gboolean grab_keyboard;
  gboolean force_grab_cursor;
  gboolean expose_wayland;
  gboolean mangoapp;
  char    *force_orientation;
  char    *prefer_output;
  gboolean immediate_flips;
  gboolean keep_alive;
  int      cursor_scale_height;

  char    *reshade_effect;
  int      reshade_technique_idx;
} EpinephrineSettings;

void command_builder_build (EpinephrineSettings *settings, char **out_command);

G_END_DECLS
