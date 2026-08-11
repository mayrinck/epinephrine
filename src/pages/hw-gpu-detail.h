#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Build the GPU detail page (widgets + management rows). */
void hw_gpu_detail_build (HardwarePage *page);

/* Load saved GPU settings from ~/.config/epinephrine/epinephrine.conf. */
void hw_gpu_detail_load_config (HardwarePage *page);

/* Populate the GPU detail page for the selected GPU. */
void hw_gpu_detail_show (HardwarePage *page);

/* Connect the GPU management row signals (called after config load). */
void hw_gpu_detail_connect_signals (HardwarePage *page);

/* Connect the NVIDIA management signals (widgets always exist). */
void hw_gpu_detail_connect_nvidia_signals (HardwarePage *page);

/* Launch the OpenRGB application (generic lighting editor). */
void hw_openrgb_launch (GtkButton *btn, gpointer user_data);

G_END_DECLS
