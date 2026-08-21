#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  GtkWidget *box;

  /* Branding section (built programmatically) */
  GtkWidget *branding_box;
  GtkWidget *brand_name_label;
  GtkWidget *brand_vendor_label;

  /* Settings section */
  GtkWidget *repeat_rate_spin;
  GtkWidget *repeat_delay_spin;
  GtkWidget *layout_label;

  /* Lock key boot options */
  AdwSwitchRow *numlock_switch;
  AdwSwitchRow *capslock_switch;
  AdwSwitchRow *scrolllock_switch;

  /* Lighting Controls */
  AdwSwitchRow *backlight_switch;
  AdwSwitchRow *lock_led_switch;

  GtkWidget *save_btn;
} KeyboardPage;

KeyboardPage *keyboard_page_new (void);

G_END_DECLS
