#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Detect the primary GPU and populate page->gpu (vendor, driver, VRAM,
 * sysfs/hwmon paths, feature flags, specs from GPU_INDEX.md). */
void hw_gpu_detect (HardwarePage *page);

/* Rebuild the "Features" bullet list under page->gpu_specs_container. */
void hw_gpu_populate_specs (HardwarePage *page);

/* Read an unsigned value from a sysfs file (0 on failure). */
uint64_t hw_gpu_read_sysfs_u64 (const char *path);

/* Query live metrics from nvidia-smi for the given GPU index. */
gboolean hw_gpu_nvidia_query (float vals[8], gboolean ok[8], int gpu_index);

/* Run a nvidia-smi management command via the privilege helper. */
void hw_gpu_nvidia_manage (const char *args);

/* Pick the brand SVG/PNG resource for a GPU. */
const char *hw_gpu_brand_resource (const GpuInfo *gpu);

G_END_DECLS
