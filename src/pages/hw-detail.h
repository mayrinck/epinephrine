#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Build the motherboard detail page (info rows + SMBus toggle). */
void hw_detail_build_motherboard (HardwarePage *page);

/* Build the RAM detail page (usage rows + lighting status). */
void hw_detail_build_rams (HardwarePage *page);

/* Populate the motherboard detail page for the selected motherboard. */
void hw_detail_show_motherboard (HardwarePage *page);

/* Populate the RAM detail page for the selected RAM device. */
void hw_detail_show_ram (HardwarePage *page, const char *device_name);

/* Update a lighting status label (ASUS Aura detection). */
void hw_detail_update_lighting_section (GtkLabel *label);

G_END_DECLS
