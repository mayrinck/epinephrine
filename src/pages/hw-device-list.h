#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Populate the left-hand device list from the detected device arrays. */
void hw_device_list_populate (HardwarePage *page);

/* "Reload devices" button callback. */
void hw_device_list_on_reload (GtkButton *btn, gpointer user_data);

/* Device list row-selected callback (shows the matching detail page). */
void hw_device_list_on_selected (GtkListBox *box, GtkListBoxRow *row,
                                 gpointer user_data);

G_END_DECLS
