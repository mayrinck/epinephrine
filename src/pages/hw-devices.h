#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Detect all devices (motherboard, GPU, CPU, RAM, input) into the list. */
void hw_devices_detect (HardwarePage *page);

G_END_DECLS
