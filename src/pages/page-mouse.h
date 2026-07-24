#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Entry parsed from MICE_INDEX.md */
typedef struct {
    char *name;
    char *url;
    int buttons;
} MouseIndexEntry;

typedef struct
{
  /* Existing widgets */
  GtkWidget *box;
  GtkWidget *speed_scale;
  GtkWidget *accel_dropdown;
  GtkWidget *natural_scroll_switch;
  GtkWidget *dpi_label;
  GtkWidget *save_btn;
  GtkWidget *lighting_status;

  /* Branding header (from MICE_INDEX.md lookup) */
  GtkWidget *mouse_name_label;
  GtkWidget *mouse_website_btn;
  char *mouse_url;

  /* G300s detection */
  int         hidraw_fd;
  gboolean    g300_detected;
  gboolean    ratbagd_warned;

  /* G300 profile data (raw 35-byte reports) */
  uint8_t     profile_raw[3][35];
  int         active_profile_idx;

  /* Section revealers for G300-specific UI */
  GtkWidget  *g300_section_revealer[5];   /* profiles, dpi, rate, led, buttons */

  /* Profile selector */
  GtkWidget  *profile_btn[3];             /* F3/F4/F5 toggle buttons */

  /* DPI */
  GtkWidget  *dpi_scales[4];
  GtkWidget  *dpi_spin_btns[4];
  GtkWidget  *dpi_default_btns[4];

  /* Polling rate */
  GtkWidget  *rate_btn[4];               /* 1000/125/250/500 Hz */

  /* LED */
  GtkWidget  *led_btn[8];                /* 8 color buttons */

  /* Button mapping */
  GtkWidget  *button_frames[9];          /* frames around each button config */
  GtkWidget  *button_func_combo[9];
  GtkWidget  *button_key_box[9];         /* revealed when function=Key */
  GtkWidget  *button_capture_btn[9];
  GtkWidget  *button_capture_label[9];
  GtkWidget  *button_mod_grid[9];        /* modifier checkboxes */
  gboolean    button_capturing[9];

  /* Key capture evdev */
  int         capture_ev_fd;
  guint       capture_source_id;
  int         capture_btn_idx;
  int         capture_timeout_id;

  /* Save to mouse button */
  GtkWidget  *save_mouse_btn;

  /* Toast overlay reference */
  GtkWidget *toast_overlay;

  /* MICE_INDEX.md lookup */
  GHashTable *mouse_index;
} MousePage;

MousePage *mouse_page_new (void);
void mouse_page_show_device (MousePage *page, uint16_t vendor, uint16_t product);

G_END_DECLS
