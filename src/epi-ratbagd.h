#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Device information from ratbagd */
typedef struct {
  char *path;           /* D-Bus object path */
  char *name;           /* Device name */
  char *description;    /* Device description */
  char *driver;         /* Driver name (e.g., "hid-logitech-g300") */
  guint32 vid;          /* USB Vendor ID */
  guint32 pid;          /* USB Product ID */
  guint32 profiles;     /* Number of profiles */
  guint32 active_profile; /* Active profile index */
} RatbagdDevice;

/* Profile information */
typedef struct {
  char *path;
  guint32 index;
  gboolean enabled;
  guint32 resolution;
  guint32 report_rate;
} RatbagdProfile;

/* Resolution information */
typedef struct {
  char *path;
  guint32 index;
  guint32 xres;
  guint32 yres;
  gboolean is_active;
  gboolean is_default;
} RatbagdResolution;

/* Button mapping */
typedef struct {
  char *path;
  guint32 index;
  guint32 type;        /* 0=button, 1=special, 2=macro */
  guint32 button;      /* Button code */
  guint32 special;     /* Special action */
  guint32 macro;       /* Macro index */
} RatbagdButton;

/* LED information */
typedef struct {
  guint32 index;
  guint32 mode;        /* 0=off, 1=solid, 2=breathing */
  guint32 color;       /* RGB color (0xRRGGBB) */
  guint32 brightness;  /* 0-255 */
} RatbagdLed;

/* Check if ratbagd is running */
gboolean ratbagd_is_running (void);

/* Get list of devices managed by ratbagd */
GList *ratbagd_get_devices (void);

/* Free device list */
void ratbagd_free_devices (GList *devices);

/* Get device by VID:PID */
RatbagdDevice *ratbagd_find_device (guint32 vid, guint32 pid);

/* Get profiles for a device */
GList *ratbagd_get_profiles (const char *device_path);

/* Get resolutions for a profile */
GList *ratbagd_get_resolutions (const char *profile_path);

/* Get buttons for a profile */
GList *ratbagd_get_buttons (const char *profile_path);

/* Get LEDs for a profile */
GList *ratbagd_get_leds (const char *profile_path);

/* Set active profile */
gboolean ratbagd_set_active_profile (const char *device_path, guint32 profile_index);

/* Set resolution values */
gboolean ratbagd_set_resolution (const char *resolution_path, guint32 xres, guint32 yres);

/* Set report rate */
gboolean ratbagd_set_report_rate (const char *profile_path, guint32 rate);

/* Set button mapping */
gboolean ratbagd_set_button (const char *button_path, guint32 type, guint32 button, guint32 special);

/* Set LED color */
gboolean ratbagd_set_led_color (const char *led_path, guint32 color, guint32 brightness);

/* Commit all changes to device */
gboolean ratbagd_commit (const char *device_path);

/* Apply G300s profile from raw data */
gboolean ratbagd_apply_g300_profile (const char *device_path, int profile_idx,
                                     const guint8 *profile_data);

/* Apply all three G300s profiles from raw data */
gboolean ratbagd_apply_g300_profiles (const char *device_path, const guint8 profiles[3][35]);

G_END_DECLS
