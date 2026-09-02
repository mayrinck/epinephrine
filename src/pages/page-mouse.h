#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Entry parsed from MICE_INDEX.md */
typedef struct {
    char *name;
    char *url;
    int buttons;
    gboolean white_label;
    char *white_label_vendor;
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

  /* Branding header (from MICE_INDEX.md lookup) */
  GtkWidget *mouse_name_label;
  GtkWidget *mouse_vendor_label;
  GtkWidget *mouse_website_btn;
  char *mouse_url;

  /* Device Information labels */
  GtkWidget *dev_vendor_value;
  GtkWidget *dev_devid_value;
  GtkWidget *dev_buttons_value;
  GtkWidget *dev_white_label_row;
  GtkWidget *dev_white_label_value;

  /* Current device for info refresh */
  uint16_t    cur_vendor;
  uint16_t    cur_product;

  /* G300s detection */
  int         hidraw_fd;
  char       *hidraw_path;   /* set when direct access failed but admin session works */
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

/* MICE_INDEX.md name lookup (for use during device detection) */
const char *mouse_index_lookup_name (uint16_t vendor, uint16_t product);

G_END_DECLS
