#define _GNU_SOURCE
#include "page-mouse.h"
#include "page-dashboard.h"
#include "epi-privilege.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/hidraw.h>
#include <linux/input-event-codes.h>
#include "../epi-i18n.h"

#ifndef _IOC
#include <asm-generic/ioctl.h>
#endif

#define tr(x) epi_i18n_get (x)

/* ---- bitmap helpers (from page-hardware.c) ---- */
#ifndef test_bit
#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static inline int
test_bit (int nr, const unsigned long *addr)
{
  return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}
#endif

/* ─────────────── MICE_INDEX.md loader ─────────────── */

static void
free_mouse_index_entry (gpointer data)
{
  MouseIndexEntry *e = data;
  if (e)
    {
      g_free (e->name);
      g_free (e->url);
      g_free (e);
    }
}

static void
load_mouse_index (MousePage *page)
{
  if (page->mouse_index) return;

  page->mouse_index = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, free_mouse_index_entry);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/MICE_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }

  if (!content) return;

  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);
  g_free (content);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---|---"))
        {
          in_table = TRUE;
          continue;
        }

      if (!in_table || line[0] != '|') continue;

      char **cols = g_strsplit (line, "|", 0);
      if (!cols[1])
        {
          g_strfreev (cols);
          continue;
        }

      /* Columns: Name | VID:PID | URL | Buttons */
      char *name = cols[1] ? g_strstrip (cols[1]) : NULL;
      char *vidpid_str = cols[2] ? g_strstrip (cols[2]) : NULL;
      char *url_str = cols[3] ? g_strstrip (cols[3]) : NULL;
      char *btn_str = cols[4] ? g_strstrip (cols[4]) : NULL;

      if (!name || !name[0] || !vidpid_str || !vidpid_str[0])
        {
          g_strfreev (cols);
          continue;
        }

      unsigned int v = 0, p = 0;
      if (sscanf (vidpid_str, "%04x:%04x", &v, &p) != 2 || v == 0)
        {
          g_strfreev (cols);
          continue;
        }

      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", v, p);

      MouseIndexEntry *entry = g_new0 (MouseIndexEntry, 1);
      entry->name = g_strdup (name);
      entry->url = url_str && url_str[0] ? g_strdup (url_str) : NULL;
      entry->buttons = btn_str && btn_str[0] ? atoi (btn_str) : 0;

      g_hash_table_insert (page->mouse_index, g_strdup (key), entry);

      g_strfreev (cols);
    }

  g_strfreev (lines);
}

static MouseIndexEntry *
lookup_mouse (MousePage *page, uint16_t vendor, uint16_t product)
{
  if (!page->mouse_index) return NULL;
  char key[16];
  g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
  return g_hash_table_lookup (page->mouse_index, key);
}

/* ─────────────────────── Logitech G300s Protocol ─────────────────────── */

#define G300_VID         0x046d
#define G300_PID         0xc246

#define G300_REPORT_F0   0xF0
#define G300_REPORT_F3   0xF3
#define G300_REPORT_F4   0xF4
#define G300_REPORT_F5   0xF5

#define G300_PROFILE_SIZE  35
#define G300_NUM_PROFILES  3
#define G300_NUM_BUTTONS   9
#define G300_NUM_DPI       4

/* Button function codes */
#define BTN_FUNC_KEY          0x00
#define BTN_FUNC_LEFT         0x01
#define BTN_FUNC_RIGHT        0x02
#define BTN_FUNC_MIDDLE       0x03
#define BTN_FUNC_BACK         0x04
#define BTN_FUNC_FORWARD      0x05
#define BTN_FUNC_BTN6         0x06
#define BTN_FUNC_BTN7         0x07
#define BTN_FUNC_BTN8         0x08
#define BTN_FUNC_BTN9         0x09
#define BTN_FUNC_DPI_UP       0x0A
#define BTN_FUNC_DPI_DOWN     0x0B
#define BTN_FUNC_DPI_CYCLE    0x0C
#define BTN_FUNC_PROFILE_CYCLE 0x0D
#define BTN_FUNC_DPI_SHIFT    0x0E
#define BTN_FUNC_DPI_DEFAULT  0x0F
#define BTN_FUNC_DISABLED     0x10

/* Modifier bitmask */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LMETA  0x08
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RMETA  0x80

/* Polling rate values */
static const int rate_values[] = { 1000, 125, 250, 500 };

/* LED color names and RGBA tuples (for button styling) */
static const char *led_color_names[] = {
  "Off", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"
};

static const GdkRGBA led_colors[] = {
  { 0.0,   0.0,   0.0,   1.0 },
  { 1.0,   0.0,   0.0,   1.0 },
  { 0.0,   1.0,   0.0,   1.0 },
  { 1.0,   1.0,   0.0,   1.0 },
  { 0.0,   0.0,   1.0,   1.0 },
  { 1.0,   0.0,   1.0,   1.0 },
  { 0.0,   1.0,   1.0,   1.0 },
  { 1.0,   1.0,   1.0,   1.0 },
};

/* Button function names for dropdown */
static const char *button_func_names[] = {
  "Key",
  "Left Click",
  "Right Click",
  "Middle Click",
  "Back",
  "Forward",
  "Button 6",
  "Button 7",
  "Button 8",
  "Button 9",
  "DPI Up",
  "DPI Down",
  "DPI Cycle",
  "Profile Cycle",
  "DPI Shift",
  "DPI Default",
  "Disabled",
};

static const uint8_t button_func_values[] = {
  BTN_FUNC_KEY,
  BTN_FUNC_LEFT,
  BTN_FUNC_RIGHT,
  BTN_FUNC_MIDDLE,
  BTN_FUNC_BACK,
  BTN_FUNC_FORWARD,
  BTN_FUNC_BTN6,
  BTN_FUNC_BTN7,
  BTN_FUNC_BTN8,
  BTN_FUNC_BTN9,
  BTN_FUNC_DPI_UP,
  BTN_FUNC_DPI_DOWN,
  BTN_FUNC_DPI_CYCLE,
  BTN_FUNC_PROFILE_CYCLE,
  BTN_FUNC_DPI_SHIFT,
  BTN_FUNC_DPI_DEFAULT,
  BTN_FUNC_DISABLED,
};

/* n_button_funcs derived from button_func_names length */

/* Modifier names */
static const char *mod_names[] = {
  "LCtrl", "LShift", "LAlt", "LSuper", "RCtrl", "RShift", "RAlt", "RSuper"
};

static const uint8_t mod_values[] = {
  MOD_LCTRL, MOD_LSHIFT, MOD_LALT, MOD_LMETA,
  MOD_RCTRL, MOD_RSHIFT, MOD_RALT, MOD_RMETA
};

static const int n_mods = G_N_ELEMENTS (mod_names);

/* Button names (G1–G9) */
static const char *button_names[] = {
  "G1 (Left)", "G2 (Right)", "G3 (Middle)",
  "G4 (Back)", "G5 (Forward)", "G6",
  "G7", "G8", "G9"
};

/* ─────────────── evdev KEY_* to HID usage code mapping ─────────────── */
/* Reverse of the kernel's hid_keyboard[] table for common keys */
static uint8_t
evdev_to_hid_code (int evcode)
{
  switch (evcode)
    {
    case KEY_ESC:         return 0x29;
    case KEY_1:           return 0x1E;  case KEY_2:  return 0x1F;
    case KEY_3:           return 0x20;  case KEY_4:  return 0x21;
    case KEY_5:           return 0x22;  case KEY_6:  return 0x23;
    case KEY_7:           return 0x24;  case KEY_8:  return 0x25;
    case KEY_9:           return 0x26;  case KEY_0:  return 0x27;
    case KEY_MINUS:       return 0x2D;  case KEY_EQUAL:     return 0x2E;
    case KEY_BACKSPACE:   return 0x2A;  case KEY_TAB:       return 0x2B;
    case KEY_Q:           return 0x14;  case KEY_W: return 0x1A;
    case KEY_E:           return 0x08;  case KEY_R: return 0x15;
    case KEY_T:           return 0x17;  case KEY_Y: return 0x1C;
    case KEY_U:           return 0x18;  case KEY_I: return 0x0C;
    case KEY_O:           return 0x12;  case KEY_P: return 0x13;
    case KEY_LEFTBRACE:   return 0x2F;  case KEY_RIGHTBRACE: return 0x30;
    case KEY_ENTER:       return 0x28;
    case KEY_A:           return 0x04;  case KEY_S: return 0x16;
    case KEY_D:           return 0x07;  case KEY_F: return 0x09;
    case KEY_G:           return 0x0A;  case KEY_H: return 0x0B;
    case KEY_J:           return 0x0D;  case KEY_K: return 0x0E;
    case KEY_L:           return 0x0F;
    case KEY_SEMICOLON:   return 0x33;  case KEY_APOSTROPHE:  return 0x34;
    case KEY_GRAVE:       return 0x35;  case KEY_BACKSLASH:   return 0x31;
    case KEY_Z:           return 0x1D;  case KEY_X: return 0x1B;
    case KEY_C:           return 0x06;  case KEY_V: return 0x19;
    case KEY_B:           return 0x05;  case KEY_N: return 0x11;
    case KEY_M:           return 0x10;
    case KEY_COMMA:       return 0x36;  case KEY_DOT: return 0x37;
    case KEY_SLASH:       return 0x38;  case KEY_SPACE:       return 0x2C;
    case KEY_CAPSLOCK:    return 0x39;
    case KEY_F1:          return 0x3A;  case KEY_F2:  return 0x3B;
    case KEY_F3:          return 0x3C;  case KEY_F4:  return 0x3D;
    case KEY_F5:          return 0x3E;  case KEY_F6:  return 0x3F;
    case KEY_F7:          return 0x40;  case KEY_F8:  return 0x41;
    case KEY_F9:          return 0x42;  case KEY_F10: return 0x43;
    case KEY_F11:         return 0x44;  case KEY_F12: return 0x45;
    case KEY_SYSRQ:       return 0x46;  case KEY_SCROLLLOCK:  return 0x47;
    case KEY_PAUSE:       return 0x48;  case KEY_INSERT:      return 0x49;
    case KEY_HOME:        return 0x4A;  case KEY_PAGEUP:      return 0x4B;
    case KEY_DELETE:      return 0x4C;  case KEY_END: return 0x4D;
    case KEY_PAGEDOWN:    return 0x4E;  case KEY_RIGHT:       return 0x4F;
    case KEY_LEFT:        return 0x50;  case KEY_DOWN:        return 0x51;
    case KEY_UP:          return 0x52;
    case KEY_NUMLOCK:     return 0x53;
    case KEY_KPSLASH:     return 0x54;  case KEY_KPASTERISK:  return 0x55;
    case KEY_KPMINUS:     return 0x56;  case KEY_KPPLUS:      return 0x57;
    case KEY_KPENTER:     return 0x58;
    case KEY_KP1:         return 0x59;  case KEY_KP2: return 0x5A;
    case KEY_KP3:         return 0x5B;  case KEY_KP4: return 0x5C;
    case KEY_KP5:         return 0x5D;  case KEY_KP6: return 0x5E;
    case KEY_KP7:         return 0x5F;  case KEY_KP8: return 0x60;
    case KEY_KP9:         return 0x61;  case KEY_KP0: return 0x62;
    case KEY_KPDOT:       return 0x63;
    case KEY_102ND:       return 0x64;
    case KEY_COMPOSE:     return 0x65;
    case KEY_POWER:       return 0x66;  case KEY_KPEQUAL:     return 0x67;
    case KEY_F13:         return 0x68;  case KEY_F14: return 0x69;
    case KEY_F15:         return 0x6A;  case KEY_F16: return 0x6B;
    case KEY_F17:         return 0x6C;  case KEY_F18: return 0x6D;
    case KEY_F19:         return 0x6E;  case KEY_F20: return 0x6F;
    case KEY_F21:         return 0x70;  case KEY_F22: return 0x71;
    case KEY_F23:         return 0x72;  case KEY_F24: return 0x73;
    case KEY_LEFTCTRL:    return 0xE0;  case KEY_LEFTSHIFT:   return 0xE1;
    case KEY_LEFTALT:     return 0xE2;  case KEY_LEFTMETA:    return 0xE3;
    case KEY_RIGHTCTRL:   return 0xE4;  case KEY_RIGHTSHIFT:  return 0xE5;
    case KEY_RIGHTALT:    return 0xE6;  case KEY_RIGHTMETA:   return 0xE7;
    default:              return 0x00;
    }
}

/* Reverse mapping: HID usage code → name string */
static const char *
hid_code_to_name (uint8_t code)
{
  switch (code)
    {
    case 0x00: return "None";
    case 0x04: return "A";       case 0x05: return "B";
    case 0x06: return "C";       case 0x07: return "D";
    case 0x08: return "E";       case 0x09: return "F";
    case 0x0A: return "G";       case 0x0B: return "H";
    case 0x0C: return "I";       case 0x0D: return "J";
    case 0x0E: return "K";       case 0x0F: return "L";
    case 0x10: return "M";       case 0x11: return "N";
    case 0x12: return "O";       case 0x13: return "P";
    case 0x14: return "Q";       case 0x15: return "R";
    case 0x16: return "S";       case 0x17: return "T";
    case 0x18: return "U";       case 0x19: return "V";
    case 0x1A: return "W";       case 0x1B: return "X";
    case 0x1C: return "Y";       case 0x1D: return "Z";
    case 0x1E: return "1";       case 0x1F: return "2";
    case 0x20: return "3";       case 0x21: return "4";
    case 0x22: return "5";       case 0x23: return "6";
    case 0x24: return "7";       case 0x25: return "8";
    case 0x26: return "9";       case 0x27: return "0";
    case 0x28: return "Enter";   case 0x29: return "Esc";
    case 0x2A: return "Bksp";    case 0x2B: return "Tab";
    case 0x2C: return "Space";   case 0x2D: return "-";
    case 0x2E: return "=";       case 0x2F: return "[";
    case 0x30: return "]";       case 0x31: return "\\";
    case 0x33: return ";";       case 0x34: return "'";
    case 0x35: return "`";       case 0x36: return ",";
    case 0x37: return ".";       case 0x38: return "/";
    case 0x39: return "Caps";
    case 0x3A: return "F1";      case 0x3B: return "F2";
    case 0x3C: return "F3";      case 0x3D: return "F4";
    case 0x3E: return "F5";      case 0x3F: return "F6";
    case 0x40: return "F7";      case 0x41: return "F8";
    case 0x42: return "F9";      case 0x43: return "F10";
    case 0x44: return "F11";     case 0x45: return "F12";
    case 0x46: return "PrtSc";   case 0x47: return "Scrl";
    case 0x48: return "Pause";   case 0x49: return "Ins";
    case 0x4A: return "Home";    case 0x4B: return "PgUp";
    case 0x4C: return "Del";     case 0x4D: return "End";
    case 0x4E: return "PgDn";
    case 0x4F: return "→";       case 0x50: return "←";
    case 0x51: return "↓";       case 0x52: return "↑";
    case 0x53: return "NumLk";
    case 0x54: return "KP/";     case 0x55: return "KP*";
    case 0x56: return "KP-";     case 0x57: return "KP+";
    case 0x58: return "KPEnt";   case 0x59: return "KP1";
    case 0x5A: return "KP2";     case 0x5B: return "KP3";
    case 0x5C: return "KP4";     case 0x5D: return "KP5";
    case 0x5E: return "KP6";     case 0x5F: return "KP7";
    case 0x60: return "KP8";     case 0x61: return "KP9";
    case 0x62: return "KP0";     case 0x63: return "KP.";
    case 0xE0: return "LCtrl";   case 0xE1: return "LShift";
    case 0xE2: return "LAlt";    case 0xE3: return "LSuper";
    case 0xE4: return "RCtrl";   case 0xE5: return "RShift";
    case 0xE6: return "RAlt";    case 0xE7: return "RSuper";
    default:   return "?";
    }
}

/* ─────────────── Forward declarations ─────────────── */
static void     populate_g300_widgets      (MousePage *page);
static void     read_g300_widgets          (MousePage *page, int profile_idx);
static int      g300_feature_get           (const char *path, int fd, uint8_t id, uint8_t *buf, size_t len);
static int      g300_feature_set           (const char *path, int fd, uint8_t *buf, size_t len);
static int      g300_read_profile          (const char *path, int fd, int idx, uint8_t *out);
static int      g300_write_profile         (const char *path, int fd, int idx, const uint8_t *data);
static int      find_g300_hidraw           (void);
static char    *find_g300_hidraw_priv      (void);
static gboolean detect_ratbagd             (void);
static void     dump_profile               (const uint8_t *data, int idx);
static void     on_profile_clicked         (GtkButton *btn, MousePage *page);
static void     on_dpi_changed             (GtkSpinButton *spin, MousePage *page);
static void     on_dpi_default_clicked     (GtkButton *btn, MousePage *page);
static void     on_rate_clicked            (GtkButton *btn, MousePage *page);
static void     on_led_clicked             (GtkButton *btn, MousePage *page);
static void     on_button_func_changed     (GObject *combo, GParamSpec *pspec, MousePage *page);
static void     on_capture_clicked         (GtkButton *btn, MousePage *page);
static void     on_mod_toggled             (GtkCheckButton *cb, MousePage *page);
static void     on_save_g300_clicked       (GtkButton *btn, MousePage *page);

/* ─────────────── Admin session helpers (epi-hidraw) ─────────────── */

static char *
epi_hidraw_program (void)
{
  char linkbuf[4096];
  ssize_t n = readlink ("/proc/self/exe", linkbuf, sizeof (linkbuf) - 1);
  if (n > 0)
    {
      linkbuf[n] = '\0';
      char *slash = strrchr (linkbuf, '/');
      if (slash)
        {
          *slash = '\0';
          char *cand = g_build_filename (linkbuf, "epi-hidraw", NULL);
          if (g_file_test (cand, G_FILE_TEST_IS_EXECUTABLE))
            return cand;
          g_free (cand);
        }
    }
  return g_strdup ("epi-hidraw");
}

static int
hex_to_bytes (const char *hex, uint8_t *out, size_t max)
{
  size_t n = strlen (hex);
  if (n == 0 || n % 2 != 0 || n / 2 > max) return -1;
  for (size_t i = 0; i < n; i += 2)
    {
      char byte[3] = { hex[i], hex[i + 1], '\0' };
      char *end = NULL;
      long v = strtol (byte, &end, 16);
      if (*end != '\0' || v < 0 || v > 0xFF) return -1;
      out[i / 2] = (uint8_t) v;
    }
  return (int) (n / 2);
}

static char *
bytes_to_hex (const unsigned char *data, size_t len)
{
  GString *s = g_string_new (NULL);
  for (size_t i = 0; i < len; i++)
    g_string_append_printf (s, "%02x", data[i]);
  return g_string_free (s, FALSE);
}

/* Run epi-hidraw through the persistent admin session; returns TRUE and
   (optionally) the captured stdout on success. */
static gboolean
run_epi_hidraw (const char *dev, const char *op, const char *arg, char **out)
{
  char *prog = epi_hidraw_program ();
  char *cmd;
  if (arg)
    cmd = g_strdup_printf ("'%s' '%s' %s %s 2>/dev/null", prog, dev, op, arg);
  else
    cmd = g_strdup_printf ("'%s' '%s' %s", prog, dev, op);
  g_free (prog);

  int exit_code = -1;
  char *stdout = NULL;
  GError *err = NULL;
  gboolean ok = epi_privilege_exec_sync (cmd, &exit_code, &stdout, &err);
  g_free (cmd);
  g_clear_error (&err);

  if (!ok || exit_code != 0 || !stdout)
    {
      g_free (stdout);
      return FALSE;
    }

  if (out)
    {
      g_strstrip (stdout);
      *out = stdout;
    }
  else
    g_free (stdout);
  return TRUE;
}

/* ─────────────── HID feature report helpers ─────────────── */

static int
g300_feature_get (const char *path, int fd, uint8_t id, uint8_t *buf, size_t len)
{
  memset (buf, 0, len);
  buf[0] = id;

  if (fd >= 0)
    return ioctl (fd, HIDIOCGFEATURE (len), buf) < 0 ? -1 : 0;

  if (path)
    {
      char id_hex[4];
      g_snprintf (id_hex, sizeof (id_hex), "%02x", id);
      char len_str[16];
      g_snprintf (len_str, sizeof (len_str), "%zu", len);
      char *out = NULL;
      if (run_epi_hidraw (path, "get-feature", id_hex, &out))
        {
          int n = hex_to_bytes (out, buf, len);
          g_free (out);
          return n < 0 ? -1 : 0;
        }
    }
  return -1;
}

static int
g300_feature_set (const char *path, int fd, uint8_t *buf, size_t len)
{
  if (fd >= 0)
    return ioctl (fd, HIDIOCSFEATURE (len), buf) < 0 ? -1 : 0;

  if (path)
    {
      char *hex = bytes_to_hex (buf, len);
      gboolean ok = run_epi_hidraw (path, "set-feature", hex, NULL);
      g_free (hex);
      return ok ? 0 : -1;
    }
  return -1;
}

/* ─────────────── Profile helpers ─────────────── */

static int
g300_read_profile (const char *path, int fd, int idx, uint8_t *out)
{
  uint8_t id = G300_REPORT_F3 + idx;
  return g300_feature_get (path, fd, id, out, G300_PROFILE_SIZE);
}

static int
g300_write_profile (const char *path, int fd, int idx, const uint8_t *data)
{
  uint8_t buf[G300_PROFILE_SIZE];
  memcpy (buf, data, G300_PROFILE_SIZE);
  buf[0] = G300_REPORT_F3 + idx;
  return g300_feature_set (path, fd, buf, G300_PROFILE_SIZE);
}

/* ─────────────── HIDRAW detection ─────────────── */

static int
find_g300_hidraw (void)
{
  GDir *dir = g_dir_open ("/sys/class/hidraw", 0, NULL);
  if (!dir) return -1;

  int fd = -1;
  const char *entry;
  while ((entry = g_dir_read_name (dir)))
    {
      char *uevent = g_build_filename ("/sys/class/hidraw", entry, "device", "uevent", NULL);
      char *content = NULL;
      if (g_file_get_contents (uevent, &content, NULL, NULL))
        {
          if (strstr (content, "HID_ID=0003:0000046D:0000C246"))
            {
              char *path = g_build_filename ("/dev", entry, NULL);
              int test_fd = open (path, O_RDWR);
              if (test_fd >= 0)
                {
                  uint8_t test_buf[4] = {0xF0, 0, 0, 0};
                  if (ioctl (test_fd, HIDIOCGFEATURE (4), test_buf) >= 0
                      && test_buf[0] == 0xF0)
                    {
                      fd = test_fd;
                      g_free (path);
                      g_free (content);
                      g_free (uevent);
                      break;
                    }
                  close (test_fd);
                }
              g_free (path);
            }
          g_free (content);
        }
      g_free (uevent);
    }
  g_dir_close (dir);

  return fd;
}

/* Same detection but through the persistent admin session (works when the
   device is only accessible as root). Returns a g_malloc'd /dev/hidrawX path. */
static char *
find_g300_hidraw_priv (void)
{
  GDir *dir = g_dir_open ("/sys/class/hidraw", 0, NULL);
  if (!dir) return NULL;

  char *result = NULL;
  const char *entry;
  while ((entry = g_dir_read_name (dir)))
    {
      char *uevent = g_build_filename ("/sys/class/hidraw", entry, "device", "uevent", NULL);
      char *content = NULL;
      if (g_file_get_contents (uevent, &content, NULL, NULL))
        {
          if (strstr (content, "HID_ID=0003:0000046D:0000C246"))
            {
              char *path = g_build_filename ("/dev", entry, NULL);
              char *out = NULL;
              if (run_epi_hidraw (path, "get-feature", "f0 4", &out))
                {
                  uint8_t test_buf[4];
                  if (hex_to_bytes (out, test_buf, sizeof (test_buf)) == 4
                      && test_buf[0] == G300_REPORT_F0)
                    result = path;
                  g_free (out);
                }
              if (!result)
                g_free (path);
              g_free (content);
              g_free (uevent);
              break;
            }
          g_free (content);
        }
      g_free (uevent);
    }
  g_dir_close (dir);

  return result;
}

/* ─────────────── Ratbagd detection ─────────────── */

static gboolean
detect_ratbagd (void)
{
  GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!conn) return FALSE;

  gboolean running = FALSE;
  GDBusProxy *proxy = g_dbus_proxy_new_sync (conn,
    G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
    "org.freedesktop.ratbagd", "/org/freedesktop/ratbagd",
    "org.freedesktop.DBus.Properties", NULL, NULL);

  if (proxy)
    {
      GVariant *ret = g_dbus_proxy_call_sync (proxy, "Get",
        g_variant_new ("(ss)", "org.freedesktop.ratbagd.Manager", "Version"),
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
      if (ret)
        {
          running = TRUE;
          g_variant_unref (ret);
        }
      g_object_unref (proxy);
    }

  g_object_unref (conn);
  return running;
}

/* ─────────────── Profile data parsing ─────────────── */

static void
dump_profile (const uint8_t *data, int idx)
{
  g_debug ("G300 Profile %d:", idx);
  g_debug ("  LED: %d", data[1] & 0x07);
  g_debug ("  Rate: %d Hz", rate_values[data[2] & 0x03]);
  for (int i = 0; i < 4; i++)
    {
      uint8_t dpi_val = data[3 + i] & 0x7F;
      gboolean is_def = (data[3 + i] & 0x80) != 0;
      g_debug ("  DPI %d: %d%s", i, dpi_val * 250, is_def ? " (default)" : "");
    }
  g_debug ("  DPI Shift: %d (en=%d)", data[7] & 0x0F, !(data[7] & 0x40));
  for (int i = 0; i < 9; i++)
    {
      int off = 8 + i * 3;
      uint8_t code = data[off];
      uint8_t mod = data[off + 1];
      uint8_t key = data[off + 2];
      if (code == BTN_FUNC_KEY && key == 0 && mod == 0)
        g_debug ("  Button %d (G%d): Unassigned", i, i + 1);
      else if (code == BTN_FUNC_KEY)
        g_debug ("  Button %d (G%d): Key=%s mod=0x%02x", i, i + 1,
          hid_code_to_name (key), mod);
      else if (code >= BTN_FUNC_LEFT && code <= BTN_FUNC_BTN9)
        g_debug ("  Button %d (G%d): %s", i, i + 1,
          code <= 0x09 ? button_func_names[code] : "?");
      else
        g_debug ("  Button %d (G%d): Special=0x%02x", i, i + 1, code);
    }
}

/* ─────────────── UI Construction ─────────────── */

static GtkWidget *
create_g300_section (MousePage *page, GtkWidget *parent_box)
{
  GtkWidget *section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (section, 0);
  gtk_widget_set_visible (section, page->g300_detected);
  gtk_box_append (GTK_BOX (parent_box), section);
  return section;
}

static void
build_g300_ui (MousePage *page, GtkWidget *parent_box)
{
  /* ── Active Profile ── */
  GtkWidget *prof_section = create_g300_section (page, parent_box);
  page->g300_section_revealer[0] = prof_section;

  GtkWidget *prof_title = gtk_label_new (tr ("Profiles (Onboard Memory)"));
  gtk_widget_add_css_class (prof_title, "heading");
  gtk_widget_set_halign (prof_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (prof_section), prof_title);

  GtkWidget *prof_subtitle = gtk_label_new (
    tr ("The G300s stores 3 profiles in its onboard memory. "
    "Select a profile to configure its settings."));
  gtk_widget_add_css_class (prof_subtitle, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (prof_subtitle), TRUE);
  gtk_box_append (GTK_BOX (prof_section), prof_subtitle);

  GtkWidget *prof_btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (prof_btn_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (prof_btn_box, 6);
  for (int i = 0; i < 3; i++)
    {
      char label[16];
      g_snprintf (label, sizeof (label), "F%d", i + 3);
      page->profile_btn[i] = gtk_toggle_button_new_with_label (label);
      g_object_set_data (G_OBJECT (page->profile_btn[i]), "prof-idx",
        GINT_TO_POINTER (i));
      if (i == 0) gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->profile_btn[i]), TRUE);
      g_signal_connect (page->profile_btn[i], "clicked",
        G_CALLBACK (on_profile_clicked), page);
      gtk_box_append (GTK_BOX (prof_btn_box), page->profile_btn[i]);
    }
  gtk_box_append (GTK_BOX (prof_section), prof_btn_box);

  /* ── DPI Settings ── */
  GtkWidget *dpi_section = create_g300_section (page, parent_box);
  page->g300_section_revealer[1] = dpi_section;

  GtkWidget *dpi_title = gtk_label_new (tr ("DPI Settings"));
  gtk_widget_add_css_class (dpi_title, "heading");
  gtk_widget_set_halign (dpi_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (dpi_section), dpi_title);

  GtkWidget *dpi_sub = gtk_label_new (tr ("Configure up to 4 DPI levels (250–2500, steps of 250)."));
  gtk_widget_add_css_class (dpi_sub, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (dpi_sub), TRUE);
  gtk_box_append (GTK_BOX (dpi_section), dpi_sub);

  for (int i = 0; i < 4; i++)
    {
      char label[32];
      g_snprintf (label, sizeof (label), tr ("Level %d"), i + 1);
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_top (row, 4);

      GtkWidget *lbl = gtk_label_new (label);
      gtk_widget_set_size_request (lbl, 60, -1);
      gtk_widget_set_halign (lbl, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (row), lbl);

      GtkAdjustment *adj = gtk_adjustment_new (1000, 250, 2500, 250, 250, 0);
      page->dpi_spin_btns[i] = gtk_spin_button_new (adj, 250, 0);
      gtk_widget_set_size_request (page->dpi_spin_btns[i], 90, -1);
      g_object_set_data (G_OBJECT (page->dpi_spin_btns[i]), "dpi-idx",
        GINT_TO_POINTER (i));
      g_signal_connect (page->dpi_spin_btns[i], "value-changed",
        G_CALLBACK (on_dpi_changed), page);
      gtk_box_append (GTK_BOX (row), page->dpi_spin_btns[i]);

      page->dpi_default_btns[i] = gtk_toggle_button_new ();
      GtkWidget *star = gtk_label_new ("★");
      gtk_widget_set_halign (star, GTK_ALIGN_CENTER);
      gtk_button_set_child (GTK_BUTTON (page->dpi_default_btns[i]), star);
      gtk_widget_set_tooltip_text (page->dpi_default_btns[i], tr ("Default DPI"));
      g_object_set_data (G_OBJECT (page->dpi_default_btns[i]), "dpi-idx",
        GINT_TO_POINTER (i));
      g_signal_connect (page->dpi_default_btns[i], "clicked",
        G_CALLBACK (on_dpi_default_clicked), page);
      gtk_box_append (GTK_BOX (row), page->dpi_default_btns[i]);

      gtk_box_append (GTK_BOX (dpi_section), row);
    }

  /* ── Polling Rate ── */
  GtkWidget *rate_section = create_g300_section (page, parent_box);
  page->g300_section_revealer[2] = rate_section;

  GtkWidget *rate_title = gtk_label_new (tr ("Polling Rate"));
  gtk_widget_add_css_class (rate_title, "heading");
  gtk_widget_set_halign (rate_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (rate_section), rate_title);

  GtkWidget *rate_btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (rate_btn_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (rate_btn_box, 4);
  for (int i = 0; i < 4; i++)
    {
      char label[16];
      g_snprintf (label, sizeof (label), tr ("%d Hz"), rate_values[i]);
      page->rate_btn[i] = gtk_toggle_button_new_with_label (label);
      g_object_set_data (G_OBJECT (page->rate_btn[i]), "rate-idx",
        GINT_TO_POINTER (i));
      if (rate_values[i] == 1000)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->rate_btn[i]), TRUE);
      g_signal_connect (page->rate_btn[i], "clicked",
        G_CALLBACK (on_rate_clicked), page);
      gtk_box_append (GTK_BOX (rate_btn_box), page->rate_btn[i]);
    }
  gtk_box_append (GTK_BOX (rate_section), rate_btn_box);

  /* ── LED Color ── */
  GtkWidget *led_section = create_g300_section (page, parent_box);
  page->g300_section_revealer[3] = led_section;

  GtkWidget *led_title = gtk_label_new (tr ("LED Color"));
  gtk_widget_add_css_class (led_title, "heading");
  gtk_widget_set_halign (led_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (led_section), led_title);

  GtkWidget *led_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (led_grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (led_grid), 4);
  gtk_widget_set_halign (led_grid, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (led_grid, 4);

  for (int i = 0; i < 8; i++)
    {
      char label[16];
      g_snprintf (label, sizeof (label), "  %s  ", led_color_names[i]);
      page->led_btn[i] = gtk_toggle_button_new_with_label (label);
      gtk_widget_set_size_request (page->led_btn[i], 80, 36);
      g_object_set_data (G_OBJECT (page->led_btn[i]), "led-idx",
        GINT_TO_POINTER (i));

      char css[128];
      g_snprintf (css, sizeof (css),
        "button { background: rgb(%d,%d,%d); min-height: 30px; }",
        (int)(led_colors[i].red * 255),
        (int)(led_colors[i].green * 255),
        (int)(led_colors[i].blue * 255));
      gtk_widget_set_name (page->led_btn[i], "mouse-led-btn");

      g_signal_connect (page->led_btn[i], "clicked",
        G_CALLBACK (on_led_clicked), page);

      int row = i / 4;
      int col = i % 4;
      gtk_grid_attach (GTK_GRID (led_grid), page->led_btn[i], col, row, 1, 1);
    }
  gtk_box_append (GTK_BOX (led_section), led_grid);

  /* ── Button Mapping ── */
  GtkWidget *btn_section = create_g300_section (page, parent_box);
  page->g300_section_revealer[4] = btn_section;

  GtkWidget *btn_title = gtk_label_new (tr ("Button Mapping"));
  gtk_widget_add_css_class (btn_title, "heading");
  gtk_widget_set_halign (btn_title, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (btn_section), btn_title);

  GtkWidget *btn_sub = gtk_label_new (
    tr ("Configure what each button (G1–G9) does. "
    "Choose a function or assign a keyboard key."));
  gtk_widget_add_css_class (btn_sub, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (btn_sub), TRUE);
  gtk_box_append (GTK_BOX (btn_section), btn_sub);

  for (int b = 0; b < G300_NUM_BUTTONS; b++)
    {
      GtkWidget *frame = gtk_frame_new (button_names[b]);
      page->button_frames[b] = frame;
      gtk_widget_set_margin_top (frame, 6);

      GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_start (vbox, 6);
      gtk_widget_set_margin_end (vbox, 6);
      gtk_widget_set_margin_top (vbox, 4);
      gtk_widget_set_margin_bottom (vbox, 4);
      gtk_frame_set_child (GTK_FRAME (frame), vbox);

      /* Function dropdown */
      GtkStringList *func_list = gtk_string_list_new (button_func_names);
      page->button_func_combo[b] = gtk_drop_down_new (G_LIST_MODEL (func_list), NULL);
      gtk_widget_set_hexpand (page->button_func_combo[b], TRUE);
      g_object_set_data (G_OBJECT (page->button_func_combo[b]), "btn-idx",
        GINT_TO_POINTER (b));
      g_signal_connect (page->button_func_combo[b], "notify::selected",
        G_CALLBACK (on_button_func_changed), page);
      gtk_box_append (GTK_BOX (vbox), page->button_func_combo[b]);

      /* Key capture area (visible only when function is Key) */
      page->button_key_box[b] = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_widget_set_visible (page->button_key_box[b], FALSE);
      gtk_widget_set_margin_top (page->button_key_box[b], 2);

      page->button_capture_label[b] = gtk_label_new (tr ("None"));
      gtk_widget_set_hexpand (page->button_capture_label[b], TRUE);
      gtk_widget_set_halign (page->button_capture_label[b], GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (page->button_key_box[b]), page->button_capture_label[b]);

      page->button_capture_btn[b] = gtk_button_new_with_label (tr ("Capture Key"));
      g_object_set_data (G_OBJECT (page->button_capture_btn[b]), "btn-idx",
        GINT_TO_POINTER (b));
      g_signal_connect (page->button_capture_btn[b], "clicked",
        G_CALLBACK (on_capture_clicked), page);
      gtk_box_append (GTK_BOX (page->button_key_box[b]), page->button_capture_btn[b]);

      gtk_box_append (GTK_BOX (vbox), page->button_key_box[b]);

      /* Modifier checkboxes */
      page->button_mod_grid[b] = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
      gtk_widget_set_visible (page->button_mod_grid[b], FALSE);
      gtk_widget_set_margin_top (page->button_mod_grid[b], 2);
      for (int m = 0; m < n_mods; m++)
        {
          GtkWidget *cb = gtk_check_button_new_with_label (mod_names[m]);
          g_object_set_data (G_OBJECT (cb), "btn-idx", GINT_TO_POINTER (b));
          g_object_set_data (G_OBJECT (cb), "mod-idx", GINT_TO_POINTER (m));
          g_signal_connect (cb, "toggled", G_CALLBACK (on_mod_toggled), page);
          gtk_box_append (GTK_BOX (page->button_mod_grid[b]), cb);
        }
      gtk_box_append (GTK_BOX (vbox), page->button_mod_grid[b]);

      gtk_box_append (GTK_BOX (btn_section), frame);
    }
}

/* ─────────────── Populate UI from profile data ─────────────── */

static void
populate_g300_widgets (MousePage *page)
{
  int pi = page->active_profile_idx;
  if (pi < 0 || pi > 2) return;
  const uint8_t *data = page->profile_raw[pi];

  /* DPI */
  for (int i = 0; i < 4; i++)
    {
      uint8_t dpi_raw = data[3 + i] & 0x7F;
      int dpi_val = dpi_raw * 250;
      if (dpi_val < 250) dpi_val = 1000;
      g_signal_handlers_block_by_func (page->dpi_spin_btns[i], on_dpi_changed, page);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->dpi_spin_btns[i]), dpi_val);
      g_signal_handlers_unblock_by_func (page->dpi_spin_btns[i], on_dpi_changed, page);

      gboolean is_default = (data[3 + i] & 0x80) != 0;
      g_signal_handlers_block_by_func (page->dpi_default_btns[i], on_dpi_default_clicked, page);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->dpi_default_btns[i]), is_default);
      g_signal_handlers_unblock_by_func (page->dpi_default_btns[i], on_dpi_default_clicked, page);
    }

  /* Polling rate */
  int rate_idx = data[2] & 0x03;
  for (int i = 0; i < 4; i++)
    {
      g_signal_handlers_block_by_func (page->rate_btn[i], on_rate_clicked, page);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->rate_btn[i]),
        i == rate_idx);
      g_signal_handlers_unblock_by_func (page->rate_btn[i], on_rate_clicked, page);
    }

  /* LED */
  int led = data[1] & 0x07;
  for (int i = 0; i < 8; i++)
    {
      g_signal_handlers_block_by_func (page->led_btn[i], on_led_clicked, page);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->led_btn[i]), i == led);
      g_signal_handlers_unblock_by_func (page->led_btn[i], on_led_clicked, page);
    }

  /* Buttons */
  for (int b = 0; b < G300_NUM_BUTTONS; b++)
    {
      int off = 8 + b * 3;
      uint8_t code = data[off];
      uint8_t mod = data[off + 1];
      uint8_t key = data[off + 2];

      /* Find function index in our value list */
      int func_idx = 0; /* Key */
      if (code >= BTN_FUNC_LEFT && code <= BTN_FUNC_BTN9)
        func_idx = code; /* 1:1 mapping for button codes */
      else if (code >= BTN_FUNC_DPI_UP && code <= BTN_FUNC_DPI_DEFAULT)
        func_idx = code - BTN_FUNC_DPI_UP + 10; /* 10-15 */
      else if (code == BTN_FUNC_DISABLED || (code == BTN_FUNC_KEY && key == 0 && mod == 0))
        func_idx = 16; /* Disabled */
      else if (code == BTN_FUNC_KEY)
        func_idx = 0; /* Key */

      g_signal_handlers_block_by_func (page->button_func_combo[b],
        on_button_func_changed, page);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->button_func_combo[b]), func_idx);
      g_signal_handlers_unblock_by_func (page->button_func_combo[b],
        on_button_func_changed, page);

      /* Show/hide key capture area */
      gboolean is_key = (code == BTN_FUNC_KEY && (key != 0 || mod != 0));
      gtk_widget_set_visible (page->button_key_box[b], is_key);
      gtk_widget_set_visible (page->button_mod_grid[b], is_key);

      if (is_key)
        {
          char buf[64];
          g_snprintf (buf, sizeof (buf), "%s", hid_code_to_name (key));
          gtk_label_set_text (GTK_LABEL (page->button_capture_label[b]), buf);

          /* Set modifier checkboxes */
          for (int m = 0; m < n_mods; m++)
            {
              gboolean active = (mod & mod_values[m]) != 0;
              GtkWidget *row = gtk_widget_get_first_child (page->button_mod_grid[b]);
              for (int c = 0; row && c < m; c++)
                row = gtk_widget_get_next_sibling (row);
              if (row && GTK_IS_CHECK_BUTTON (row))
                {
                  g_signal_handlers_block_by_func (row, on_mod_toggled, page);
                  gtk_check_button_set_active (GTK_CHECK_BUTTON (row), active);
                  g_signal_handlers_unblock_by_func (row, on_mod_toggled, page);
                }
            }
        }
    }
}

/* ─────────────── Read widgets back into profile ─────────────── */

static void
read_g300_widgets (MousePage *page, int profile_idx)
{
  if (profile_idx < 0 || profile_idx > 2) return;
  uint8_t *data = page->profile_raw[profile_idx];

  /* DPI */
  for (int i = 0; i < 4; i++)
    {
      int dpi_val = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->dpi_spin_btns[i]));
      if (dpi_val < 250) dpi_val = 250;
      if (dpi_val > 2500) dpi_val = 2500;
      uint8_t dpi_raw = dpi_val / 250;
      gboolean is_def = gtk_toggle_button_get_active (
        GTK_TOGGLE_BUTTON (page->dpi_default_btns[i]));
      data[3 + i] = dpi_raw | (is_def ? 0x80 : 0);
    }

  /* Polling rate */
  for (int i = 0; i < 4; i++)
    {
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->rate_btn[i])))
        {
          data[2] = (data[2] & ~0x03) | i;
          break;
        }
    }

  /* LED */
  for (int i = 0; i < 8; i++)
    {
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (page->led_btn[i])))
        {
          data[1] = (data[1] & ~0x07) | i;
          break;
        }
    }

  /* Buttons */
  for (int b = 0; b < G300_NUM_BUTTONS; b++)
    {
      int off = 8 + b * 3;
      int func_idx = (int) gtk_drop_down_get_selected (
        GTK_DROP_DOWN (page->button_func_combo[b]));

      uint8_t code, mod, key;
      if (func_idx >= 1 && func_idx <= 9)
        {
          /* Mouse button function */
          code = button_func_values[func_idx];
          mod = 0;
          key = 0;
        }
      else if (func_idx >= 10 && func_idx <= 15)
        {
          /* Special function (DPI, profile, etc.) */
          code = button_func_values[func_idx];
          mod = 0;
          key = 0;
        }
      else if (func_idx == 16)
        {
          /* Disabled = same as unassigned key */
          code = 0x00;
          mod = 0;
          key = 0;
        }
      else
        {
          /* Key press */
          code = 0x00;
          mod = 0;
          key = 0; /* preserve existing key if any */

          /* Read modifiers from checkboxes */
          GtkWidget *child = gtk_widget_get_first_child (page->button_mod_grid[b]);
          for (int m = 0; child && m < n_mods; m++)
            {
              if (GTK_IS_CHECK_BUTTON (child) &&
                  gtk_check_button_get_active (GTK_CHECK_BUTTON (child)))
                mod |= mod_values[m];
              child = gtk_widget_get_next_sibling (child);
            }

          /* Get key from capture label data */
          key = GPOINTER_TO_UINT (g_object_get_data (
            G_OBJECT (page->button_capture_label[b]), "hid-key"));
        }

      data[off] = code;
      data[off + 1] = mod;
      data[off + 2] = key;
    }
}

/* ─────────────── Evdev Key Capture ─────────────── */

static void stop_key_capture (MousePage *page);

static gboolean
on_capture_evdev_event (GIOChannel *src, GIOCondition cond, gpointer user_data)
{
  MousePage *page = user_data;
  int b = page->capture_btn_idx;
  if (b < 0 || b >= G300_NUM_BUTTONS) return G_SOURCE_REMOVE;

  if (cond & G_IO_IN)
    {
      struct input_event ev;
      int fd = g_io_channel_unix_get_fd (src);
      int n = read (fd, &ev, sizeof (ev));
      if (n == (int) sizeof (ev) && ev.type == EV_KEY && ev.value == 1)
        {
          uint8_t hid = evdev_to_hid_code (ev.code);
          if (hid != 0)
            {
              /* Store key in capture label data */
              g_object_set_data (G_OBJECT (page->button_capture_label[b]),
                "hid-key", GUINT_TO_POINTER (hid));
              gtk_label_set_text (GTK_LABEL (page->button_capture_label[b]),
                hid_code_to_name (hid));

              dashboard_show_toast (tr ("Key captured!"));
              stop_key_capture (page);
              return G_SOURCE_REMOVE;
            }
        }
      return G_SOURCE_CONTINUE;
    }

  if (cond & (G_IO_HUP | G_IO_ERR))
    {
      stop_key_capture (page);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
on_capture_timeout (gpointer user_data)
{
  MousePage *page = user_data;
  if (page->button_capturing[page->capture_btn_idx])
    {
      stop_key_capture (page);
      dashboard_show_toast (tr ("Key capture timed out. Try again."));
    }
  return G_SOURCE_REMOVE;
}

static void
stop_key_capture (MousePage *page)
{
  if (page->capture_source_id)
    {
      g_source_remove (page->capture_source_id);
      page->capture_source_id = 0;
    }
  if (page->capture_timeout_id)
    {
      g_source_remove (page->capture_timeout_id);
      page->capture_timeout_id = 0;
    }
  if (page->capture_ev_fd >= 0)
    {
      close (page->capture_ev_fd);
      page->capture_ev_fd = -1;
    }
  if (page->capture_btn_idx >= 0)
    {
      page->button_capturing[page->capture_btn_idx] = FALSE;
      gtk_button_set_label (GTK_BUTTON (page->button_capture_btn[page->capture_btn_idx]),
        tr ("Capture Key"));
    }
  page->capture_btn_idx = -1;
}

static void
start_key_capture (MousePage *page, int btn_idx)
{
  if (btn_idx < 0 || btn_idx >= G300_NUM_BUTTONS) return;

  /* Cancel any existing capture */
  if (page->capture_btn_idx >= 0) stop_key_capture (page);

  /* Find a keyboard evdev device */
  GDir *dir = g_dir_open ("/dev/input", 0, NULL);
  if (!dir)
    {
      dashboard_show_toast (tr ("Cannot access /dev/input for key capture"));
      return;
    }

  int found_fd = -1;
  const char *entry;
  while ((entry = g_dir_read_name (dir)))
    {
      if (!g_str_has_prefix (entry, "event")) continue;
      char *path = g_build_filename ("/dev/input", entry, NULL);
      int fd = open (path, O_RDONLY | O_NONBLOCK);
      if (fd < 0) { g_free (path); continue; }

      unsigned long key_bits[NLONGS (KEY_MAX)] = {0};
      if (ioctl (fd, EVIOCGBIT (EV_KEY, sizeof (key_bits)), key_bits) >= 0)
        {
          if (test_bit (KEY_A, key_bits) || test_bit (KEY_1, key_bits))
            {
              /* Check it's NOT a mouse or joystick */
              if (!test_bit (BTN_LEFT, key_bits) &&
                  !test_bit (BTN_A, key_bits))
                {
                  found_fd = fd;
                  g_free (path);
                  break;
                }
            }
        }
      close (fd);
      g_free (path);
    }
  g_dir_close (dir);

  if (found_fd < 0)
    {
      dashboard_show_toast (tr ("No keyboard found for key capture"));
      return;
    }

  page->capture_ev_fd = found_fd;
  page->capture_btn_idx = btn_idx;
  page->button_capturing[btn_idx] = TRUE;
  gtk_button_set_label (GTK_BUTTON (page->button_capture_btn[btn_idx]),
    tr ("Press a key..."));

  /* Set up GIOChannel for evdev monitoring */
  GIOChannel *chan = g_io_channel_unix_new (found_fd);
  g_io_channel_set_encoding (chan, NULL, NULL);
  g_io_channel_set_buffered (chan, FALSE);
  page->capture_source_id = g_io_add_watch (chan,
    G_IO_IN | G_IO_HUP | G_IO_ERR,
    on_capture_evdev_event, page);
  g_io_channel_unref (chan);

  /* 5-second timeout */
  page->capture_timeout_id = g_timeout_add_seconds (5, on_capture_timeout, page);
}

/* ─────────────── Callbacks ─────────────── */

static void
on_profile_clicked (GtkButton *btn, MousePage *page)
{
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "prof-idx"));

  /* Save current profile data from widgets */
  read_g300_widgets (page, page->active_profile_idx);

  /* Update active profile */
  for (int i = 0; i < 3; i++)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->profile_btn[i]), i == idx);

  page->active_profile_idx = idx;
  populate_g300_widgets (page);
}

static void
on_dpi_changed (GtkSpinButton *spin, MousePage *page)
{
  int dpi_idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (spin), "dpi-idx"));
  int pi = page->active_profile_idx;
  int dpi_val = (int) gtk_spin_button_get_value (spin);
  if (dpi_val < 250) dpi_val = 250;
  if (dpi_val > 2500) dpi_val = 2500;
  uint8_t dpi_raw = dpi_val / 250;
  gboolean is_def = gtk_toggle_button_get_active (
    GTK_TOGGLE_BUTTON (page->dpi_default_btns[dpi_idx]));
  page->profile_raw[pi][3 + dpi_idx] = dpi_raw | (is_def ? 0x80 : 0);
}

static void
on_dpi_default_clicked (GtkButton *btn, MousePage *page)
{
  int dpi_idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "dpi-idx"));
  int pi = page->active_profile_idx;

  /* Unset all others, set this one */
  for (int i = 0; i < 4; i++)
    {
      gboolean is_this = (i == dpi_idx);
      g_signal_handlers_block_by_func (page->dpi_default_btns[i],
        on_dpi_default_clicked, page);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->dpi_default_btns[i]),
        is_this);
      g_signal_handlers_unblock_by_func (page->dpi_default_btns[i],
        on_dpi_default_clicked, page);

      page->profile_raw[pi][3 + i] = (page->profile_raw[pi][3 + i] & 0x7F)
        | (is_this ? 0x80 : 0);
    }
}

static void
on_rate_clicked (GtkButton *btn, MousePage *page)
{
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "rate-idx"));
  int pi = page->active_profile_idx;

  for (int i = 0; i < 4; i++)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->rate_btn[i]), i == idx);

  page->profile_raw[pi][2] = (page->profile_raw[pi][2] & ~0x03) | idx;
}

static void
on_led_clicked (GtkButton *btn, MousePage *page)
{
  int led_idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "led-idx"));
  int pi = page->active_profile_idx;

  for (int i = 0; i < 8; i++)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (page->led_btn[i]), i == led_idx);

  page->profile_raw[pi][1] = (page->profile_raw[pi][1] & ~0x07) | led_idx;
}

static void
on_button_func_changed (GObject *combo, GParamSpec *pspec, MousePage *page)
{
  (void) pspec;
  int b = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "btn-idx"));
  int func_idx = (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (combo));

  /* Show key capture area only if function is "Key" (index 0) */
  gboolean is_key = (func_idx == 0);
  gtk_widget_set_visible (page->button_key_box[b], is_key);
  gtk_widget_set_visible (page->button_mod_grid[b], is_key);

  /* Update profile data immediately */
  int pi = page->active_profile_idx;
  int off = 8 + b * 3;
  if (func_idx >= 1 && func_idx <= 9)
    {
      page->profile_raw[pi][off] = button_func_values[func_idx];
      page->profile_raw[pi][off + 1] = 0;
      page->profile_raw[pi][off + 2] = 0;
    }
  else if (func_idx >= 10 && func_idx <= 15)
    {
      page->profile_raw[pi][off] = button_func_values[func_idx];
      page->profile_raw[pi][off + 1] = 0;
      page->profile_raw[pi][off + 2] = 0;
    }
  else if (func_idx == 16)
    {
      page->profile_raw[pi][off] = 0x00;
      page->profile_raw[pi][off + 1] = 0;
      page->profile_raw[pi][off + 2] = 0;
    }
  /* For Key (index 0), preserve existing data */
}

static void
on_capture_clicked (GtkButton *btn, MousePage *page)
{
  int b = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "btn-idx"));
  start_key_capture (page, b);
}

static void
on_mod_toggled (GtkCheckButton *cb, MousePage *page)
{
  int b = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cb), "btn-idx"));
  int m = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cb), "mod-idx"));

  /* Update profile data */
  int pi = page->active_profile_idx;
  int off = 8 + b * 3;
  uint8_t mod = page->profile_raw[pi][off + 1];
  if (gtk_check_button_get_active (cb))
    mod |= mod_values[m];
  else
    mod &= ~mod_values[m];
  page->profile_raw[pi][off + 1] = mod;
}

static void
on_save_g300_clicked (GtkButton *btn, MousePage *page)
{
  (void) btn;

  /* Read all widget values into current profile */
  read_g300_widgets (page, page->active_profile_idx);

  /* Write all 3 profiles to hardware */
  int wrote = 0;
  for (int i = 0; i < 3; i++)
    {
      if (g300_write_profile (page->hidraw_path, page->hidraw_fd, i, page->profile_raw[i]) == 0)
        wrote++;
    }

  if (wrote == 3)
    dashboard_show_toast (tr ("All 3 profiles saved to G300s"));
  else if (wrote > 0)
    {
      char *partial_msg = g_strdup_printf (tr ("Partial write: only %d of 3 profiles saved"), wrote);
      dashboard_show_toast (partial_msg);
      g_free (partial_msg);
    }
  else
    dashboard_show_toast (tr ("Failed to write profiles to G300s"));

  if (wrote > 0)
    dump_profile (page->profile_raw[page->active_profile_idx],
      page->active_profile_idx);
}

/* ─────────────── Wayland + X11 mouse settings ─────────────── */

static void
on_mouse_save (GtkButton *btn, MousePage *page)
{
  (void) btn;

  double speed = gtk_range_get_value (GTK_RANGE (page->speed_scale));
  guint accel_sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->accel_dropdown));
  gboolean natural = adw_switch_row_get_active (ADW_SWITCH_ROW (page->natural_scroll_switch));

  char speed_str[32];
  g_snprintf (speed_str, sizeof (speed_str), "%f", speed);

  /* Map dropdown index → gsettings accel-profile */
  const char *accel_gsettings[] = { "flat", "flat", "adaptive" };
  const char *gsettings_profile = (accel_sel < 3) ? accel_gsettings[accel_sel] : "default";

  /* Try 1: gsettings (GNOME on Wayland or X11) */
  if (g_find_program_in_path ("gsettings"))
    {
      g_spawn_sync (NULL,
        (char *[]){ "gsettings", "set",
          "org.gnome.desktop.peripherals.mouse", "speed",
          speed_str, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);

      g_spawn_sync (NULL,
        (char *[]){ "gsettings", "set",
          "org.gnome.desktop.peripherals.mouse", "accel-profile",
          (char *) gsettings_profile, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);

      g_spawn_sync (NULL,
        (char *[]){ "gsettings", "set",
          "org.gnome.desktop.peripherals.mouse", "natural-scroll",
          natural ? "true" : "false", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);

      dashboard_show_toast (tr ("Mouse settings applied (GNOME)"));
      return;
    }

  /* Try 2: xinput (X11 fallback) */
  if (!g_find_program_in_path ("xinput"))
    {
      dashboard_show_toast (tr ("No supported config tool found (gsettings or xinput)"));
      return;
    }

  const char *profile_xinput[] = { "0 0 0", "0 1 0", "1 0 0" };
  const char *accel_vals = (accel_sel < 3) ? profile_xinput[accel_sel] : profile_xinput[1];

  GError *err = NULL;
  char *id_list = NULL;
  g_spawn_sync (NULL, (char *[]){ "xinput", "list", "--id-only", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &id_list, NULL, NULL, &err);

  if (!id_list || err || id_list[0] == '\0')
    {
      if (err) g_error_free (err);
      g_free (id_list);
      dashboard_show_toast (tr ("No XInput devices found. Are you on Wayland?"));
      return;
    }

  char **ids = g_strsplit (id_list, "\n", -1);
  for (int i = 0; ids[i] && i < 20; i++)
    {
      g_strstrip (ids[i]);
      if (ids[i][0] == '\0') continue;
      g_spawn_sync (NULL,
        (char *[]){ "xinput", "set-prop", ids[i], "libinput Accel Speed", speed_str, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
      g_spawn_sync (NULL,
        (char *[]){ "xinput", "set-prop", ids[i], "libinput Accel Profile Enabled",
          (char *) accel_vals, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
    }
  g_strfreev (ids);
  g_free (id_list);

  dashboard_show_toast (tr ("Mouse settings applied (X11)"));
}

static void
refresh_mouse_info (MousePage *page)
{
  /* Try 1: libinput list-devices (Wayland + X11) */
  char *out = NULL;
  if (g_spawn_sync (NULL, (char *[]){ "libinput", "list-devices", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL) && out)
    {
      char **lines = g_strsplit (out, "\n", -1);
      const char *resolution = NULL;
      for (int i = 0; lines[i]; i++)
        {
          if (strstr (lines[i], "DPI:"))
            {
              /* Parse "DPI: dpi=1200x1200 (logical: 1200x1200)" */
              char *eq = strchr (lines[i], '=');
              char *x = NULL;
              if (eq)
                {
                  eq++;
                  x = strchr (eq, 'x');
                  if (x) *x = '\0';
                  resolution = eq;
                }
              break;
            }
        }
      if (resolution && resolution[0])
        gtk_label_set_text (GTK_LABEL (page->dpi_label), resolution);
      else
        gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
      g_strfreev (lines);
      g_free (out);
      return;
    }
  g_free (out);

  /* Try 2: xinput list-props (X11 fallback) */
  out = NULL;
  if (g_spawn_sync (NULL, (char *[]){ "xinput", "list", "--name-only", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL) && out)
    {
      g_strstrip (out);
      if (out[0])
        {
          char *dpi_out = NULL;
          g_spawn_sync (NULL,
            (char *[]){ "xinput", "list-props", out, NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &dpi_out, NULL, NULL, NULL);
          if (dpi_out)
            {
              char **lines = g_strsplit (dpi_out, "\n", -1);
              const char *resolution = NULL;
              for (int i = 0; lines[i]; i++)
                {
                  if (strstr (lines[i], "Resolution"))
                    {
                      char *br = strchr (lines[i], '[');
                      char *end = NULL;
                      if (br) end = strchr (br + 1, ']');
                      if (end)
                        {
                          *end = '\0';
                          resolution = br + 1;
                        }
                      break;
                    }
                }
              if (resolution && resolution[0])
                gtk_label_set_text (GTK_LABEL (page->dpi_label), resolution);
              else
                gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
              g_strfreev (lines);
              g_free (dpi_out);
              g_free (out);
              return;
            }
          g_free (dpi_out);
        }
      g_free (out);
    }

  gtk_label_set_text (GTK_LABEL (page->dpi_label), "—");
}

static void
on_mouse_openrgb_clicked (GtkButton *btn, MousePage *page)
{
  (void) btn;
  (void) page;
  GError *err = NULL;
  g_spawn_async (NULL, (char *[]){ "openrgb", NULL },
    NULL, G_SPAWN_DEFAULT | G_SPAWN_SEARCH_PATH,
    NULL, NULL, NULL, &err);
  if (err)
    {
      dashboard_show_toast (tr ("OpenRGB not installed. Install it from openrgb.org"));
      g_error_free (err);
    }
}

/* ─────────────── Website button ─────────────── */

static void
on_mouse_website_clicked (GtkButton *btn, MousePage *page)
{
  (void) btn;
  if (page->mouse_url)
    {
      GError *err = NULL;
      g_app_info_launch_default_for_uri (page->mouse_url, NULL, &err);
      if (err)
        {
          g_warning ("Failed to open URL: %s", err->message);
          g_error_free (err);
        }
    }
}

/* ─────────────── Show device (called from page-hardware) ─────────────── */

void
mouse_page_show_device (MousePage *page, uint16_t vendor, uint16_t product)
{
  load_mouse_index (page);

  MouseIndexEntry *e = lookup_mouse (page, vendor, product);

  if (e && e->name)
    {
      gtk_label_set_text (GTK_LABEL (page->mouse_name_label), e->name);

      g_free (page->mouse_url);
      page->mouse_url = e->url ? g_strdup (e->url) : NULL;
      gtk_widget_set_visible (page->mouse_website_btn, page->mouse_url != NULL);
    }
  else
    {
      char fallback[64];
      g_snprintf (fallback, sizeof (fallback), tr ("Mouse (%04x:%04x)"), vendor, product);
      gtk_label_set_text (GTK_LABEL (page->mouse_name_label), fallback);
      gtk_widget_set_visible (page->mouse_website_btn, FALSE);
    }
}

/* ─────────────── Cleanup ─────────────── */

static void
on_mouse_page_destroy (gpointer data, GObject *box)
{
  (void) box;
  MousePage *page = data;

  stop_key_capture (page);

  if (page->hidraw_fd >= 0)
    {
      close (page->hidraw_fd);
      page->hidraw_fd = -1;
    }
  g_free (page->hidraw_path);
  page->hidraw_path = NULL;

  g_free (page->mouse_url);
  if (page->mouse_index) g_hash_table_unref (page->mouse_index);
  g_free (page);
}

/* ─────────────── Page constructor ─────────────── */

MousePage *
mouse_page_new (void)
{
  MousePage *page = g_new0 (MousePage, 1);
  page->hidraw_fd = -1;
  page->capture_ev_fd = -1;
  page->capture_btn_idx = -1;

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-mouse.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-page"));
  page->speed_scale = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-speed-scale"));
  page->accel_dropdown = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-accel-dropdown"));
  page->natural_scroll_switch = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-natural-scroll-row"));
  page->dpi_label = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-dpi-label"));
  page->save_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-save-btn"));
  page->save_mouse_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-save-g300-btn"));

  /* Branding header */
  page->mouse_name_label = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-branding-name"));
  page->mouse_website_btn = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-website-btn"));
  g_signal_connect (page->mouse_website_btn, "clicked",
    G_CALLBACK (on_mouse_website_clicked), page);

  /* Lighting Controls */
  page->lighting_status = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-lighting-status"));
  gtk_label_set_text (GTK_LABEL (page->lighting_status),
    tr ("The app couldn't find any light source to manage."));

  GtkWidget *mouse_lighting_header = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-lighting-header"));
  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        tr ("Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (mouse_lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_mouse_openrgb_clicked), page);
    }

  /* ── G300s Detection ── */
  page->hidraw_fd = find_g300_hidraw ();
  if (page->hidraw_fd < 0)
    page->hidraw_path = find_g300_hidraw_priv ();
  page->g300_detected = (page->hidraw_fd >= 0 || page->hidraw_path != NULL);

  if (page->g300_detected)
    {
      /* Read all 3 profiles */
      for (int i = 0; i < 3; i++)
        {
          if (g300_read_profile (page->hidraw_path, page->hidraw_fd, i, page->profile_raw[i]) < 0)
            {
              g_warning ("Failed to read G300 profile %d", i);
              memset (page->profile_raw[i], 0, G300_PROFILE_SIZE);
              page->profile_raw[i][0] = G300_REPORT_F3 + i;
            }
        }

      if (page->profile_raw[0][0] == G300_REPORT_F3)
        {
          dump_profile (page->profile_raw[0], 0);
          dump_profile (page->profile_raw[1], 1);
          dump_profile (page->profile_raw[2], 2);
        }
    }

  /* ── Build G300-specific UI sections ── */
  GtkWidget *scroll_box = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-scroll-box"));

  /* Find position after Device Info section to insert G300 sections */
  GtkWidget *dev_section = GTK_WIDGET (gtk_builder_get_object (builder, "mouse-device-section"));
  if (dev_section)
    {
      gtk_box_remove (GTK_BOX (scroll_box), dev_section);
      build_g300_ui (page, scroll_box);
      gtk_box_prepend (GTK_BOX (scroll_box), dev_section);
    }
  else
    build_g300_ui (page, scroll_box);

  /* If G300s detected, populate widgets */
  if (page->g300_detected)
    {
      page->active_profile_idx = 0;
      populate_g300_widgets (page);
    }

  /* ── Ratbagd warning ── */
  if (page->g300_detected && detect_ratbagd ())
    {
      page->ratbagd_warned = TRUE;
      dashboard_show_toast (
        tr ("ratbagd is running on your system and may conflict with G300 configuration. "
        "Consider stopping ratbagd for exclusive access."));
    }

  /* ── Connect signals ── */
  g_signal_connect (page->save_btn, "clicked", G_CALLBACK (on_mouse_save), page);

  if (page->g300_detected && page->save_mouse_btn)
    {
      gtk_widget_set_visible (page->save_mouse_btn, TRUE);
      g_signal_connect (page->save_mouse_btn, "clicked",
        G_CALLBACK (on_save_g300_clicked), page);
    }

  g_object_ref_sink (page->box);

  /* Cleanup on destroy */
  g_object_weak_ref (G_OBJECT (page->box), on_mouse_page_destroy, page);

  g_object_unref (builder);

  load_mouse_index (page);

  refresh_mouse_info (page);

  return page;
}
