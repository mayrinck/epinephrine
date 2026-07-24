#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  GtkWidget *box;

  /* Info section */
  GtkWidget *name_label;
  GtkWidget *vendor_label;
  GtkWidget *speed_label;
  GtkWidget *driver_label;
  GtkWidget *chipset_label;

  /* Settings section */
  AdwSwitchRow *numlock_switch;
  AdwSwitchRow *capslock_switch;
  AdwSwitchRow *scrolllock_switch;
  GtkWidget *repeat_rate_spin;
  GtkWidget *repeat_delay_spin;

  /* Lighting Controls section */
  AdwSwitchRow *backlight_switch;

  GtkWidget *layout_label;
  GtkWidget *save_btn;
} KeyboardPage;

KeyboardPage *keyboard_page_new (void);

G_END_DECLS
