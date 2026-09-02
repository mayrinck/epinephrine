#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Convert a memory amount given in MiB to a display string using the larger
   unit ("16 GB", "1.5 GB", "512 MB"). Caller must free the result. */
char *ram_size_mb_str (uint64_t mb);

/* Parse a dmidecode "Size" value (e.g. "16384 MB", "16 GB") into MiB.
   Returns FALSE when the value cannot be parsed. */
gboolean ram_parse_size_mb (const char *size, uint64_t *out_mb);

/* Build the motherboard detail page (info rows + SMBus toggle). */
void hw_detail_build_motherboard (HardwarePage *page);

/* Build the RAM detail page (usage rows + memory information). */
void hw_detail_build_rams (HardwarePage *page);

/* Populate the motherboard detail page for the selected motherboard. */
void hw_detail_show_motherboard (HardwarePage *page);

/* Populate the RAM detail page for the selected RAM device. */
void hw_detail_show_ram (HardwarePage *page, const char *device_name);

/* Look up the official product page for a memory module in RAM_INDEX.md.
   Matching: first by (manufacturer, part number), then by manufacturer alone.
   Returns a newly allocated URL or NULL when no entry matches. */
char *epi_ram_find_url (const char *manufacturer, const char *part_number);

G_END_DECLS
