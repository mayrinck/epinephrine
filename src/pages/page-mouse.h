#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Entry parsed from MOUSE_INDEX.md */
typedef struct {
    char *name;
    char *url;
    int buttons;
    gboolean white_label;
    char *white_label_vendor;
    char *manufacturer;
} MouseIndexEntry;

typedef struct
{
  /* Existing widgets */
  GtkWidget *box;
  GtkWidget *speed_scale;
  GtkWidget *accel_dropdown;
  GtkWidget *natural_scroll_switch;
  GtkWidget *scroll_factor;
  GtkWidget *scroll_button_switch;
  GDBusConnection *kwin_bus;
  char *kwin_path;
  GtkWidget *dpi_label;
  GtkWidget *save_btn;

  /* Branding header (from MOUSE_INDEX.md lookup) */
  GtkWidget *mouse_brand_icon;
  GtkWidget *mouse_name_label;
  GtkWidget *mouse_release_label;
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

  /* Save to mouse button */
  GtkWidget  *save_mouse_btn;
  GtkWidget  *ratbag_editor_btn;

  /* Restore factory profiles button */
  GtkWidget  *restore_factory_btn;

  /* Toast overlay reference */
  GtkWidget *toast_overlay;

  /* MOUSE_INDEX.md lookup */
  GHashTable *mouse_index;
} MousePage;

MousePage *mouse_page_new (void);
void mouse_page_show_device (MousePage *page, uint16_t vendor, uint16_t product, const char *detected_name);

/* MOUSE_INDEX.md name lookup (for use during device detection) */
const char *mouse_index_lookup_name (uint16_t vendor, uint16_t product);

G_END_DECLS
