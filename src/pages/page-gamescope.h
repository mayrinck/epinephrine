#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  AdwSpinRow  *output_width;
  AdwSpinRow  *output_height;
  AdwSpinRow  *nested_width;
  AdwSpinRow  *nested_height;
  AdwSpinRow  *nested_refresh;
  AdwSpinRow  *nested_unfocused_refresh;
  AdwSwitchRow *fullscreen;
  AdwSwitchRow *borderless;
  AdwSpinRow  *display_index;

  AdwComboRow *scaler;
  AdwComboRow *filter;
  GtkRange    *sharpness;
  AdwSpinRow  *max_scale;

  AdwSpinRow  *framerate_limit;

  AdwSwitchRow *hdr_enabled;
  AdwSpinRow  *hdr_sdr_content_nits;
  AdwSwitchRow *hdr_itm_enabled;
  AdwSpinRow  *hdr_itm_sdr_nits;
  AdwSpinRow  *hdr_itm_target_nits;
  GtkRange    *sdr_gamut_wideness;

  AdwComboRow *backend;
  AdwSwitchRow *steam_integration;
  AdwSwitchRow *adaptive_sync;
  AdwSpinRow  *mouse_sensitivity;
  AdwSwitchRow *grab_keyboard;
  AdwSwitchRow *force_grab_cursor;
  AdwSwitchRow *expose_wayland;
  AdwSwitchRow *mangoapp;
  AdwComboRow *force_orientation;
  GtkEntry    *prefer_output;
  AdwSwitchRow *immediate_flips;
  AdwSwitchRow *keep_alive;
  AdwSpinRow  *cursor_scale_height;

  GtkEntry    *reshade_effect;
  AdwSpinRow  *reshade_technique_idx;

  GtkEntry    *command_entry;
  GtkButton   *copy_button;
  GtkButton   *save_button;
  GtkButton   *restore_button;

  GtkWidget   *box;
} GamescopePage;

GamescopePage *gamescope_page_new (void);

G_END_DECLS
