#pragma once

#include "page-hardware.h"
#include <linux/input.h>

G_BEGIN_DECLS

#define TEST_BIT(nr, addr) \
  (((unsigned long *)(addr))[(nr) / (8 * sizeof (unsigned long))] \
    & (1UL << ((nr) % (8 * sizeof (unsigned long)))))

enum {
  IND_A, IND_B, IND_X, IND_Y,
  IND_DPAD_UP, IND_DPAD_DOWN, IND_DPAD_LEFT, IND_DPAD_RIGHT,
  IND_START, IND_SELECT,
  IND_L1, IND_R1, IND_L2, IND_R2,
  IND_GUIDE,
  IND_L3, IND_R3,
  IND_M1, IND_M2, IND_M3, IND_M4,
  IND_COUNT
};

struct _ControllerState
{
  guint buttons[KEY_MAX + 1];
  int lx, ly, rx, ry;
  gboolean thumb_l_pressed;
  gboolean thumb_r_pressed;
};

/* Build the controller detail page (branding + settings/test views). */
void hw_controller_build_detail (HardwarePage *page);

/* Load the controller database from JOY_INDEX.md. */
void hw_controller_load_index (HardwarePage *page);

/* Controller name/url/category lookups (used by device detection). */
const char *hw_controller_lookup_name (HardwarePage *page, uint16_t vendor, uint16_t product);
const char *hw_controller_lookup_url (HardwarePage *page, uint16_t vendor, uint16_t product);
const char *hw_controller_lookup_category (HardwarePage *page, uint16_t vendor, uint16_t product);

/* Populate the controller detail page for the selected device. */
void hw_controller_show (HardwarePage *page);

/* Close the evdev device and release the controller state. */
void hw_controller_close (HardwarePage *page);

/* Rebuild the extra buttons (M1–M4) section for the current controller. */
void hw_controller_update_extra_buttons (HardwarePage *page);

/* evdev watcher callback (g_unix_fd_add). */
gboolean hw_controller_event (gint fd, GIOCondition condition, gpointer user_data);

G_END_DECLS
