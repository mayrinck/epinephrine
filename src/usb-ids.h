#pragma once

#include <adwaita.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct _UsbIds UsbIds;

UsbIds *usb_ids_new (void);
void    usb_ids_free (UsbIds *ids);

gboolean     usb_ids_load (UsbIds *ids);
const char  *usb_ids_lookup (UsbIds *ids, uint16_t vendor, uint16_t product);
void         usb_ids_refresh_async (UsbIds *ids);

G_END_DECLS
