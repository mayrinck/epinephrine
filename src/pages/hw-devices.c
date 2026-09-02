#define _GNU_SOURCE
#include <linux/input.h>
#include "page-hardware.h"
#include "hw-devices.h"
#include "hw-gpu.h"
#include "hw-controller.h"
#include "hw-monitor.h"
#include "hw-detail.h"
#include "../epi-i18n.h"
#include "../epi-privilege.h"
#include "page-mouse.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ── Input device helpers ── */

#ifndef test_bit
#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static inline int
test_bit (int nr, const unsigned long *addr)
{
  return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}
#endif

static gboolean
read_evdev_vid_pid (const char *devpath, uint16_t *vendor, uint16_t *product)
{
  *vendor = *product = 0;

  /* /dev/input/eventN → /sys/class/input/eventN/device/id/vendor */
  const char *base = strrchr (devpath, '/');
  if (!base) return FALSE;
  base++;

  char vendor_path[256], product_path[256];
  g_snprintf (vendor_path, sizeof (vendor_path),
    "/sys/class/input/%s/device/id/vendor", base);
  g_snprintf (product_path, sizeof (product_path),
    "/sys/class/input/%s/device/id/product", base);

  char vbuf[16] = {0}, pbuf[16] = {0};
  int fd_v = open (vendor_path, O_RDONLY);
  if (fd_v < 0) return FALSE;
  ssize_t n = read (fd_v, vbuf, sizeof (vbuf) - 1);
  close (fd_v);
  if (n <= 0) return FALSE;
  vbuf[n] = '\0';
  g_strstrip (vbuf);

  int fd_p = open (product_path, O_RDONLY);
  if (fd_p < 0) return FALSE;
  n = read (fd_p, pbuf, sizeof (pbuf) - 1);
  close (fd_p);
  if (n <= 0) return FALSE;
  pbuf[n] = '\0';
  g_strstrip (pbuf);

  *vendor = (uint16_t) strtoul (vbuf, NULL, 16);
  *product = (uint16_t) strtoul (pbuf, NULL, 16);
  return (*vendor != 0);
}

static char *
normalize_device_name (const char *name)
{
  if (!name) return NULL;
  GString *s = g_string_new (name);
  g_string_replace (s, "(R)", "\xc2\xae", 0);    /* ® */
  g_string_replace (s, "(r)", "\xc2\xae", 0);
  g_string_replace (s, "(TM)", "\xe2\x84\xa2", 0); /* ™ */
  g_string_replace (s, "(tm)", "\xe2\x84\xa2", 0);
  return g_string_free (s, FALSE);
}

static void
add_device (HardwarePage *page, const char *name, const char *type,
            const char *info, const char *path,
            uint16_t vendor, uint16_t product, const char *url,
            const char *category)
{
  page->n_devices++;
  page->device_names = g_realloc (page->device_names,
    page->n_devices * sizeof (char *));
  page->device_types = g_realloc (page->device_types,
    page->n_devices * sizeof (char *));
  page->device_info = g_realloc (page->device_info,
    page->n_devices * sizeof (char *));
  page->device_paths = g_realloc (page->device_paths,
    page->n_devices * sizeof (char *));
  page->device_vendor = g_realloc (page->device_vendor,
    page->n_devices * sizeof (uint16_t));
  page->device_product = g_realloc (page->device_product,
    page->n_devices * sizeof (uint16_t));
  page->device_urls = g_realloc (page->device_urls,
    page->n_devices * sizeof (char *));
  page->device_categories = g_realloc (page->device_categories,
    page->n_devices * sizeof (char *));

  page->device_names[page->n_devices - 1] = normalize_device_name (name);
  page->device_types[page->n_devices - 1] = g_strdup (type);
  page->device_info[page->n_devices - 1] = g_strdup (info ? info : "");
  page->device_paths[page->n_devices - 1] = g_strdup (path ? path : "");
  page->device_vendor[page->n_devices - 1] = vendor;
  page->device_product[page->n_devices - 1] = product;
  page->device_urls[page->n_devices - 1] = url ? g_strdup (url) : NULL;
  page->device_categories[page->n_devices - 1] = category
    ? g_strdup (category) : g_strdup ("Controller");
}

static char *
extract_byid_name (const char *entry)
{
  char *display = g_strdup (entry);
  char *usb_part = strstr (display, "usb-");
  char *name_start = usb_part ? usb_part + 4 : display;

  if (name_start[0] && g_ascii_isxdigit (name_start[0]))
    {
      char *first_us = strchr (name_start, '_');
      if (first_us) name_start = first_us + 1;
    }

  char *suffixes[] = {
    "-event-joystick", "-joystick",
    "-event-kbd", "-kbd",
    "-event-mouse", "-mouse",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (suffixes); i++)
    {
      char *p = strstr (name_start, suffixes[i]);
      if (p) { *p = '\0'; break; }
    }

  char *if_part = strstr (name_start, "-if");
  if (if_part)
    {
      char *next = if_part + 3;
      if (*next && g_ascii_isdigit (*next)) *if_part = '\0';
    }

  for (int i = 0; name_start[i]; i++)
    if (name_start[i] == '_') name_start[i] = ' ';

  char *result = g_strstrip (g_strdup (name_start));
  g_free (display);
  return result;
}

static char *
read_evdev_name (const char *devpath)
{
  int fd = open (devpath, O_RDONLY | O_NONBLOCK);
  if (fd < 0) return NULL;
  char buf[256] = {0};
  char *result = NULL;
  if (ioctl (fd, EVIOCGNAME (sizeof (buf)), buf) >= 0 && buf[0])
    result = g_strdup (buf);
  close (fd);
  return result;
}

static char *
read_evdev_phys (const char *devpath)
{
  /* Derive sysfs path: /dev/input/eventN → /sys/class/input/eventN/device/phys */
  const char *base = strrchr (devpath, '/');
  if (!base) return NULL;
  base++;
  char *sys_path = g_build_filename ("/sys/class/input", base, "device", "phys", NULL);
  int fd = open (sys_path, O_RDONLY);
  g_free (sys_path);
  if (fd < 0) return NULL;
  char buf[256] = {0};
  char *result = NULL;
  ssize_t n = read (fd, buf, sizeof (buf) - 1);
  close (fd);
  if (n > 0)
    {
      buf[n] = '\0';
      g_strstrip (buf);
      if (buf[0])
        result = g_strdup (buf);
    }
  return result;
}

static const char *
classify_evdev (int fd)
{
  unsigned long key_bits[NLONGS (KEY_MAX)] = {0};
  if (ioctl (fd, EVIOCGBIT (EV_KEY, sizeof (key_bits)), key_bits) < 0)
    return NULL;

  /* Check for analog axes — real joysticks have ABS_X/ABS_Y.
     Keyboards with multimedia keys (BTN_B, BTN_X) do NOT have
     analog axes, so requiring ABS prevents misclassification. */
  unsigned long abs_bits[NLONGS (ABS_MAX)] = {0};
  ioctl (fd, EVIOCGBIT (EV_ABS, sizeof (abs_bits)), abs_bits);
  gboolean has_abs = test_bit (ABS_X, abs_bits) || test_bit (ABS_Y, abs_bits);

  /* Joystick/gamepad: needs BOTH gamepad buttons AND analog axes */
  if (has_abs &&
      (test_bit (BTN_A, key_bits) || test_bit (BTN_B, key_bits) ||
       test_bit (BTN_X, key_bits) || test_bit (BTN_Y, key_bits) ||
       test_bit (BTN_START, key_bits) || test_bit (BTN_SELECT, key_bits) ||
       test_bit (BTN_TL, key_bits) || test_bit (BTN_TR, key_bits)))
    return "joystick";

  /* Mouse */
  if (test_bit (BTN_LEFT, key_bits) || test_bit (BTN_RIGHT, key_bits) ||
      test_bit (BTN_MIDDLE, key_bits))
    return "mouse";

  /* Keyboard */
  if (test_bit (KEY_A, key_bits) || test_bit (KEY_B, key_bits) ||
      test_bit (KEY_1, key_bits) || test_bit (KEY_SPACE, key_bits) ||
      test_bit (KEY_ENTER, key_bits))
    return "keyboard";

  return NULL;
}

static const char *
type_to_subtitle (const char *type)
{
  if (!type) return tr ("Input Device");
  switch (type[0])
    {
    case 'c': return tr ("Controller");
    case 'j': return tr ("Joystick");
    case 'k': return tr ("Keyboard");
    case 'm': return tr ("Mouse");
    default:  return tr ("Input Device");
    }
}

static void
process_input_device (HardwarePage *page, GHashTable *seen,
                      const char *target, const char *from_name,
                       const char *from_type, GHashTable *kbd_phys,
                       GHashTable *joy_phys, GHashTable *all_phys)
{
  char *devpath = NULL;
  {
    const char *base = target;
    if (g_str_has_prefix (base, "../")) base += 3;
    devpath = g_build_filename ("/dev/input", base, NULL);
  }

  if (g_hash_table_contains (seen, devpath))
    {
      g_free (devpath);
      return;
    }
  g_hash_table_add (seen, g_strdup (devpath));

  /* Cross-type deduplication: if same physical device already added as
     any type (joystick, keyboard, mouse), skip duplicate interfaces.
     This prevents composite USB devices (e.g. GameSir T3 Lite) from
     appearing as joystick + mouse + keyboard. */
  if (all_phys)
    {
      char *phys = read_evdev_phys (devpath);
      if (phys)
        {
          char *inp = strstr (phys, "/input");
          if (inp) *inp = '\0';
          if (g_hash_table_contains (all_phys, phys))
            {
              g_free (phys);
              g_free (devpath);
              return;
            }
          g_hash_table_add (all_phys, g_strdup (phys));
          g_free (phys);
        }
    }

  const char *type = from_type;
  char *real_name = read_evdev_name (devpath);
  char *display_name = NULL;

  if (real_name && real_name[0])
    display_name = real_name;
  else if (from_name && from_name[0])
    display_name = g_strdup (from_name);
  else
    display_name = NULL;

  /* Deduplicate keyboards from the same physical USB device */
  if (g_strcmp0 (type, "keyboard") == 0 && kbd_phys)
    {
      char *phys = read_evdev_phys (devpath);
      if (phys)
        {
          char *inp = strstr (phys, "/input");
          if (inp) *inp = '\0';
          if (g_hash_table_contains (kbd_phys, phys))
            {
              g_free (phys);
              g_free (display_name);
              g_free (devpath);
              return;
            }
          g_hash_table_add (kbd_phys, g_strdup (phys));
          g_free (phys);
        }
    }

  /* Deduplicate joysticks/controllers from the same physical USB device */
  if ((g_strcmp0 (type, "joystick") == 0 || g_strcmp0 (type, "controller") == 0) && joy_phys)
    {
      char *phys = read_evdev_phys (devpath);
      if (phys)
        {
          char *inp = strstr (phys, "/input");
          if (inp) *inp = '\0';
          if (g_hash_table_contains (joy_phys, phys))
            {
              g_free (phys);
              g_free (display_name);
              g_free (devpath);
              return;
            }
          g_hash_table_add (joy_phys, g_strdup (phys));
          g_free (phys);
        }
    }

  if (!display_name || !display_name[0])
    {
      const char *sub = type_to_subtitle (type);
      char *fallback = g_strdup_printf (tr ("Unknown %s"), sub);
      add_device (page, fallback, type ? type : "input", type_to_subtitle (type), devpath,
        0, 0, NULL, NULL);
      g_free (fallback);
    }
  else
    {
      /* Read USB VID/PID and look up proper name for controllers */
      uint16_t vendor = 0, product = 0;
      const char *url = NULL;
      const char *category = NULL;
      if (g_strcmp0 (type, "joystick") == 0 || g_strcmp0 (type, "controller") == 0)
        {
          if (read_evdev_vid_pid (devpath, &vendor, &product))
            {
              const char *override = hw_controller_lookup_name (page, vendor, product);
              if (override)
                {
                  g_free (display_name);
                  display_name = g_strdup (override);
                }
              url = hw_controller_lookup_url (page, vendor, product);
              category = hw_controller_lookup_category (page, vendor, product);
            }
        }
      else if (g_strcmp0 (type, "mouse") == 0)
        {
          if (read_evdev_vid_pid (devpath, &vendor, &product))
            {
              const char *override = mouse_index_lookup_name (vendor, product);
              if (override)
                {
                  g_free (display_name);
                  display_name = g_strdup (override);
                }
            }
        }
      else
        {
          /* For keyboards and other devices — still read VID/PID */
          read_evdev_vid_pid (devpath, &vendor, &product);
        }
      add_device (page, display_name, type ? type : "input", type_to_subtitle (type), devpath,
        vendor, product, url, category);
    }

  g_free (display_name);
  g_free (devpath);
}

/* Free one RamStick (array free-func). */
void
ram_stick_free (gpointer ptr)
{
  RamStick *s = ptr;
  if (!s) return;
  g_free (s->size);
  g_free (s->form_factor);
  g_free (s->type);
  g_free (s->type_detail);
  g_free (s->speed);
  g_free (s->manufacturer);
  g_free (s->part_number);
  g_free (s->rank);
  g_free (s->configured_speed);
  g_free (s->serial_number);
  g_free (s->locator);
  g_free (s->bank_locator);
  g_free (s->total_width);
  g_free (s->data_width);
  g_free (s);
}

static void
ram_stick_set_str (char **dst, const char *value)
{
  g_free (*dst);
  *dst = g_strdup (value);
  g_strstrip (*dst);
}

static void
ram_stick_set_size (RamStick *s, const char *value)
{
  ram_stick_set_str (&s->size, value);
}

/* Keep a stick only when it is actually installed (Size != "No Module
   Installed"). Also derives the aggregate page->ram_technology/freq from the
   first installed stick found. */
static void
ram_stick_finalize (HardwarePage *page, RamStick *cur, GPtrArray *sticks,
                    int *found)
{
  if (!cur) return;
  gboolean installed = cur->size && cur->size[0]
    && g_strcmp0 (cur->size, "No Module Installed") != 0
    && g_strcmp0 (cur->size, "No") != 0;

  if (!installed)
    {
      ram_stick_free (cur);
      return;
    }

  if (!page->ram_technology && cur->type && cur->type[0])
    page->ram_technology = g_strdup (cur->type);
  if (page->ram_freq_mhz == 0 && cur->speed)
    {
      char *end;
      uint64_t val = g_ascii_strtoull (cur->speed, &end, 10);
      if (val > 0) page->ram_freq_mhz = val;
    }

  g_ptr_array_add (sticks, cur);
  (*found)++;
}

/* Parse `dmidecode -t memory` output (Type 17 "Memory Device" blocks) into
   fresh RamStick entries appended to `sticks`. Also counts every slot present
   in the output through `*slots_total`. Returns the number of installed
   sticks found. */
static int
ram_parse_dmi (HardwarePage *page, GPtrArray *sticks, int *slots_total,
               const char *output)
{
  int found = 0;
  gboolean in_dev = FALSE;
  RamStick *cur = NULL;

  char *line = (char *) output;
  while (line && *line)
    {
      char *nl = strchr (line, '\n');
      if (nl) *nl = '\0';

      char *trim = line;
      while (*trim == '\t' || *trim == ' ') trim++;

      if (trim[0] == '\0' && in_dev)
        {
          ram_stick_finalize (page, cur, sticks, &found);
          cur = NULL;
          in_dev = FALSE;
        }
      else if (g_str_has_prefix (trim, "Memory Device"))
        {
          if (cur) ram_stick_finalize (page, cur, sticks, &found);
          in_dev = TRUE;
          (*slots_total)++;
          cur = g_new0 (RamStick, 1);
        }
      else if (in_dev && cur)
        {
          if (g_str_has_prefix (trim, "Size:"))
            ram_stick_set_size (cur, trim + 5);
          else if (g_str_has_prefix (trim, "Form Factor:"))
            ram_stick_set_str (&cur->form_factor, trim + 12);
          else if (g_str_has_prefix (trim, "Type Detail:"))
            ram_stick_set_str (&cur->type_detail, trim + 12);
          else if (g_str_has_prefix (trim, "Type:"))
            ram_stick_set_str (&cur->type, trim + 5);
          else if (g_str_has_prefix (trim, "Configured Memory Speed:"))
            ram_stick_set_str (&cur->configured_speed, trim + 23);
          else if (g_str_has_prefix (trim, "Speed:"))
            ram_stick_set_str (&cur->speed, trim + 6);
          else if (g_str_has_prefix (trim, "Manufacturer:"))
            ram_stick_set_str (&cur->manufacturer, trim + 13);
          else if (g_str_has_prefix (trim, "Part Number:"))
            ram_stick_set_str (&cur->part_number, trim + 12);
          else if (g_str_has_prefix (trim, "Rank:"))
            ram_stick_set_str (&cur->rank, trim + 5);
          else if (g_str_has_prefix (trim, "Serial Number:"))
            ram_stick_set_str (&cur->serial_number, trim + 14);
          else if (g_str_has_prefix (trim, "Bank Locator:"))
            ram_stick_set_str (&cur->bank_locator, trim + 13);
          else if (g_str_has_prefix (trim, "Locator:"))
            ram_stick_set_str (&cur->locator, trim + 8);
          else if (g_str_has_prefix (trim, "Total Width:"))
            ram_stick_set_str (&cur->total_width, trim + 12);
          else if (g_str_has_prefix (trim, "Data Width:"))
            ram_stick_set_str (&cur->data_width, trim + 11);
        }

      if (nl) { *nl = '\n'; line = nl + 1; }
      else break;
    }

  if (cur) ram_stick_finalize (page, cur, sticks, &found);

  return found;
}

void
hw_devices_detect (HardwarePage *page)
{
  /* Motherboard — always first in the list */
  page->mobo_detected = FALSE;
  {
    char buf[256] = {0};
    char *vendor = NULL, *model = NULL, *bios = NULL;

    FILE *f = fopen ("/sys/class/dmi/id/board_vendor", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); vendor = g_strdup (buf); fclose (f); }
    f = fopen ("/sys/class/dmi/id/board_name", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); model = g_strdup (buf); fclose (f); }
    f = fopen ("/sys/class/dmi/id/bios_version", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); bios = g_strdup (buf); fclose (f); }

    /* Extended DMI info */
    f = fopen ("/sys/devices/virtual/dmi/id/board_version", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); page->mobo_revision = g_strdup (buf); fclose (f); }
    f = fopen ("/sys/devices/virtual/dmi/id/bios_vendor", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); page->mobo_bios_vendor = g_strdup (buf); fclose (f); }
    f = fopen ("/sys/devices/virtual/dmi/id/bios_date", "r");
    if (f) { g_strstrip (fgets (buf, sizeof (buf), f)); page->mobo_bios_date = g_strdup (buf); fclose (f); }

    /* ROM Size via dmidecode -t 0 (try direct, fallback to elevated) */
    {
      char *result = NULL;
      g_spawn_sync (NULL,
        (char *[]){ "/bin/sh", "-c",
          "dmidecode -t 0 2>/dev/null | grep -i 'rom.*size' | sed 's/.*://;s/^[[:space:]]*//' | head -1",
          NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &result, NULL, NULL, NULL);
      if (result) { g_strstrip (result); if (!result[0]) { g_free (result); result = NULL; } }
      if (!result)
        {
          char *elev_out = NULL;
          int exit_code = -1;
          if (epi_privilege_exec_sync (
                "dmidecode -t 0 2>/dev/null | grep -i 'rom.*size' | sed 's/.*://;s/^[[:space:]]*//' | head -1",
                &exit_code, &elev_out, NULL)
              && exit_code == 0 && elev_out && elev_out[0])
            {
              g_strstrip (elev_out);
              result = elev_out;
            }
          else
            g_free (elev_out);
        }
      if (result) { page->mobo_bios_romsize = g_strdup (result); g_free (result); }
    }

    /* EFI PK via efi-readvar or efivarfs + strings */
    {
      char *result = NULL;
      g_spawn_sync (NULL,
        (char *[]){ "/bin/sh", "-c",
          "efi-readvar -v PK 2>/dev/null | head -5",
          NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &result, NULL, NULL, NULL);
      if (result) { g_strstrip (result); if (!result[0]) { g_free (result); result = NULL; } }
      if (!result)
        {
          g_spawn_sync (NULL,
            (char *[]){ "/bin/sh", "-c",
              "cat /sys/firmware/efi/efivars/PK-* 2>/dev/null | strings | head -5",
              NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &result, NULL, NULL, NULL);
          if (result) { g_strstrip (result); if (!result[0]) { g_free (result); result = NULL; } }
        }
      if (result) { page->mobo_bios_efi_pk = g_strdup (result); g_free (result); }
    }

    if (vendor && model)
      {
        page->mobo_vendor = g_strdup (vendor);
        page->mobo_model = g_strdup (model);
        page->mobo_bios_version = g_strdup (bios ? bios : "?");
        page->mobo_detected = TRUE;

        char *name = g_strdup_printf ("%s %s", vendor, model);
        char *info = g_strdup_printf ("%s %s",
          tr ("BIOS:"),
          bios ? bios : "?");
        add_device (page, name, "motherboard", info, NULL, 0, 0, NULL, NULL);
        g_free (name);
        g_free (info);
      }
    else
      add_device (page, tr ("Unknown Motherboard"),
        "motherboard", NULL, NULL, 0, 0, NULL, NULL);

    g_free (vendor);
    g_free (model);
    g_free (bios);
  }

  /* GPU */
  hw_gpu_detect (page);

  if (page->gpu && page->gpu->device_name)
    {
      char info[64];
      if (page->gpu->vram_total > 0)
        {
          double gb = page->gpu->vram_total / (1024.0 * 1024.0 * 1024.0);
          g_snprintf (info, sizeof (info), "%s (%.1f GB)",
            tr ("Video Card"), gb);
        }
      else
        g_snprintf (info, sizeof (info), "%s",
          tr ("Video Card"));
      add_device (page, page->gpu->device_name, "gpu", info, NULL, 0, 0, NULL, NULL);
    }
  else
    add_device (page, tr ("Unknown GPU"), "gpu", NULL, NULL, 0, 0, NULL, NULL);

  /* CPU */
  {
    char buf[512] = {0};
    FILE *f = fopen ("/proc/cpuinfo", "r");
    if (f)
      {
        while (fgets (buf, sizeof (buf), f))
          {
            if (g_str_has_prefix (buf, "model name"))
              {
                char *colon = strchr (buf, ':');
                if (colon)
                  {
                    g_strstrip (colon + 1);
                    add_device (page, colon + 1, "cpu",
                      tr ("Processor"), NULL, 0, 0, NULL, NULL);
                    break;
                  }
              }
          }
        fclose (f);
      }
    else
      add_device (page, tr ("Unknown CPU"), "cpu", NULL, NULL, 0, 0, NULL, NULL);
  }

  /* RAM — per-stick detail via dmidecode (elevated fallback). Without the
     elevated read we still fall back to /proc/meminfo totals only. */
  {
    gboolean dimm_found = FALSE;
    GPtrArray *sticks = g_ptr_array_new_with_free_func (ram_stick_free);
    int slots_total = 0;
    char *dmi_out = NULL;

    gboolean got = g_spawn_sync (NULL,
      (char *[]){ "/bin/sh", "-c",
        "dmidecode -t memory 2>/dev/null", NULL },
      NULL, G_SPAWN_DEFAULT, NULL, NULL, &dmi_out, NULL, NULL, NULL);

    /* dmidecode needs root to read the SMBIOS tables; when the direct read
       yields no installed stick, retry through the privileged helper. */
    if (got && dmi_out
        && ram_parse_dmi (page, sticks, &slots_total, dmi_out) == 0)
      {
        char *elev = NULL;
        int ec = -1;
        if (epi_privilege_exec_sync (
              "dmidecode -t memory 2>/dev/null", &ec, &elev, NULL)
            && ec == 0 && elev && elev[0])
          {
            slots_total = 0;
            ram_parse_dmi (page, sticks, &slots_total, elev);
          }
        g_free (elev);
      }

    g_free (dmi_out);
    dimm_found = (sticks->len > 0);

    if (dimm_found)
      {
        page->ram_sticks = sticks;          /* transfer ownership */
        page->ram_slots_total = slots_total;
        page->ram_slots_used = (int) sticks->len;

        /* Total installed memory from the per-stick sizes */
        uint64_t total_mb = 0;
        for (guint i = 0; i < sticks->len; i++)
          {
            RamStick *s = g_ptr_array_index (sticks, i);
            if (!s->size) continue;
            char *end;
            double v = strtod (s->size, &end);
            while (*end == ' ') end++;
            if (g_str_has_prefix (end, "GB"))
              total_mb += (uint64_t) (v * 1024);
            else if (g_str_has_prefix (end, "MB"))
              total_mb += (uint64_t) v;
            else
              total_mb += (uint64_t) v;
          }

        char *amount = ram_size_mb_str (total_mb);

        /* Name: "RAM <Technology> x<installed>" (RAM + Type + xN). */
        GString *ram_name = g_string_new (tr ("RAM"));
        if (page->ram_technology && page->ram_technology[0])
          g_string_append_printf (ram_name, " %s", page->ram_technology);
        g_string_append_printf (ram_name, " x%d", page->ram_slots_used);

        char *info = g_strdup_printf ("%s (%s)", tr ("Memory"), amount);
        add_device (page, ram_name->str, "ram", info, NULL, 0, 0, NULL, NULL);
        g_string_free (ram_name, TRUE);
        g_free (info);
        g_free (amount);
      }
    else
      {
        g_ptr_array_unref (sticks);
        page->ram_sticks = NULL;
        page->ram_slots_total = 0;
        page->ram_slots_used = 0;

        char buf[256] = {0};
        FILE *f = fopen ("/proc/meminfo", "r");
        if (f)
          {
            while (fgets (buf, sizeof (buf), f))
              {
                if (g_str_has_prefix (buf, "MemTotal"))
                  {
                    char *colon = strchr (buf, ':');
                    if (colon)
                      {
                        char *val = colon + 1;
                        g_strstrip (val);
                        long kb = atol (val);
                        char *ram_str = ram_size_mb_str (kb / 1024ULL);
                        add_device (page, ram_str, "ram",
                          tr ("Memory"), NULL, 0, 0, NULL, NULL);
                        g_free (ram_str);
                      }
                    break;
                  }
              }
            fclose (f);
          }
      }
    /* Read total and available memory from /proc/meminfo */
    {
      FILE *f = fopen ("/proc/meminfo", "r");
      if (f)
        {
          char buf[256];
          uint64_t mem_total_kb = 0, mem_avail_kb = 0;
          while (fgets (buf, sizeof (buf), f))
            {
              if (g_str_has_prefix (buf, "MemTotal:"))
                {
                  char *v = strchr (buf, ':') + 1;
                  while (*v == ' ') v++;
                  mem_total_kb = strtoull (v, NULL, 10);
                }
              else if (g_str_has_prefix (buf, "MemAvailable:"))
                {
                  char *v = strchr (buf, ':') + 1;
                  while (*v == ' ') v++;
                  mem_avail_kb = strtoull (v, NULL, 10);
                }
            }
          fclose (f);
          page->ram_total_bytes = mem_total_kb * 1024;
          page->ram_used_bytes = (mem_total_kb - mem_avail_kb) * 1024;
        }
    }
  }

  /* Input devices — 3-phase scan */
  {
    /* Phase 1+2: /dev/input/by-id/ and /dev/input/by-path/
       Multi-pass by priority: joystick first, then mouse, then keyboard.
       This ensures that composite USB devices (e.g. GameSir T3 Lite) are
       claimed by their primary category before secondary interfaces. */
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *kbd_phys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *joy_phys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *all_phys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    struct { const char *sfx1; const char *sfx2; const char *type; } passes[] = {
      { "-event-joystick", "-joystick", "joystick"  },
      { "-event-mouse",    "-mouse",    "mouse"     },
      { "-event-kbd",      "-kbd",      "keyboard"  },
    };

    const char *scan_dirs[] = { "/dev/input/by-id", "/dev/input/by-path" };
    for (size_t p = 0; p < G_N_ELEMENTS (passes); p++)
      for (size_t scan = 0; scan < G_N_ELEMENTS (scan_dirs); scan++)
        {
          GDir *dir = g_dir_open (scan_dirs[scan], 0, NULL);
          if (!dir) continue;

          const char *entry;
          while ((entry = g_dir_read_name (dir)))
            {
              if (g_str_has_suffix (entry, "event-if"))
                continue;

              if (!g_str_has_suffix (entry, passes[p].sfx1) &&
                  !g_str_has_suffix (entry, passes[p].sfx2))
                continue;

              char *full = g_build_filename (scan_dirs[scan], entry, NULL);
              char target[256] = {0};
              ssize_t len = readlink (full, target, sizeof (target) - 1);
              g_free (full);
              if (len < 0) continue;
              target[len] = '\0';
              if (!strstr (target, "event")) continue;

              char *from_name = (scan == 0) ? extract_byid_name (entry) : g_strdup ("");
              process_input_device (page, seen, target, from_name, passes[p].type,
                kbd_phys, joy_phys, all_phys);
              g_free (from_name);
            }
          g_dir_close (dir);
        }

    /* Phase 3: /dev/input/event* fallback */
    {
      GDir *dir = g_dir_open ("/dev/input", 0, NULL);
      if (dir)
        {
          const char *entry;
          while ((entry = g_dir_read_name (dir)))
            {
              if (!g_str_has_prefix (entry, "event")) continue;
              char *devpath = g_build_filename ("/dev/input", entry, NULL);

              /* Skip if already seen */
              if (g_hash_table_contains (seen, devpath))
                { g_free (devpath); continue; }
              g_hash_table_add (seen, g_strdup (devpath));

              int fd = open (devpath, O_RDONLY | O_NONBLOCK);
              if (fd < 0) { g_free (devpath); continue; }

              char buf[256] = {0};
              char *name = NULL;
              if (ioctl (fd, EVIOCGNAME (sizeof (buf)), buf) >= 0 && buf[0])
                name = g_strdup (buf);

              const char *type = classify_evdev (fd);
              close (fd);

              if (!name && !type)
                { g_free (devpath); continue; }

              /* Cross-type deduplication: if same physical device was already
                 added in Phase 1+2, skip this duplicate interface. */
              if (all_phys)
                {
                  char *phys = read_evdev_phys (devpath);
                  if (phys)
                    {
                      char *inp = strstr (phys, "/input");
                      if (inp) *inp = '\0';
                      if (g_hash_table_contains (all_phys, phys))
                        {
                          g_free (phys);
                          g_free (name);
                          g_free (devpath);
                          continue;
                        }
                      g_hash_table_add (all_phys, g_strdup (phys));
                      g_free (phys);
                    }
                }

              /* Deduplicate joysticks/controllers from the same physical device */
              if ((g_strcmp0 (type, "joystick") == 0 || g_strcmp0 (type, "controller") == 0) && joy_phys)
                {
                  char *phys = read_evdev_phys (devpath);
                  if (phys)
                    {
                      char *inp = strstr (phys, "/input");
                      if (inp) *inp = '\0';
                      if (g_hash_table_contains (joy_phys, phys))
                        {
                          g_free (phys);
                          g_free (name);
                          g_free (devpath);
                          continue;
                        }
                      g_hash_table_add (joy_phys, g_strdup (phys));
                      g_free (phys);
                    }
                }

              if (name)
                {
                  uint16_t vendor = 0, product = 0;
                  const char *url = NULL;
                  const char *category = NULL;
                  if ((g_strcmp0 (type, "joystick") == 0 || g_strcmp0 (type, "controller") == 0)
                      && read_evdev_vid_pid (devpath, &vendor, &product))
                    {
                      const char *override = hw_controller_lookup_name (page, vendor, product);
                      if (override) { g_free (name); name = g_strdup (override); }
                      url = hw_controller_lookup_url (page, vendor, product);
                      category = hw_controller_lookup_category (page, vendor, product);
                    }
                  add_device (page, name, type ? type : "input", type_to_subtitle (type), devpath,
                    vendor, product, url, category);
                }
              else
                {
                  char *fallback = g_strdup_printf (tr ("Unknown %s"), type_to_subtitle (type));
                  add_device (page, fallback, type, type_to_subtitle (type), devpath,
                    0, 0, NULL, NULL);
                  g_free (fallback);
                }
              g_free (name);
              g_free (devpath);
            }
          g_dir_close (dir);
        }
    }

    g_hash_table_destroy (seen);
    g_hash_table_destroy (kbd_phys);
    g_hash_table_destroy (joy_phys);
    g_hash_table_destroy (all_phys);
  }
}
