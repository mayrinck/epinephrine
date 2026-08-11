#define _GNU_SOURCE
#include <linux/input.h>
#include "page-hardware.h"
#include "hw-devices.h"
#include "hw-gpu.h"
#include "hw-controller.h"
#include "../epi-privilege.h"
#include "../epi-i18n.h"

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

  /* Joystick/gamepad */
  if (test_bit (BTN_A, key_bits) || test_bit (BTN_B, key_bits) ||
      test_bit (BTN_X, key_bits) || test_bit (BTN_Y, key_bits) ||
      test_bit (BTN_START, key_bits) || test_bit (BTN_SELECT, key_bits) ||
      test_bit (BTN_TL, key_bits) || test_bit (BTN_TR, key_bits))
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
                      GHashTable *joy_phys)
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
      add_device (page, display_name, type ? type : "input", type_to_subtitle (type), devpath,
        vendor, product, url, category);
    }

  g_free (display_name);
  g_free (devpath);
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

  /* RAM — per-DIMM via dmidecode, fallback to /proc/meminfo */
  {
    gboolean dimm_found = FALSE;
    GPtrArray *dimms = g_ptr_array_new_with_free_func (g_free);
    char *dmi_out = NULL;
    if (g_spawn_sync (NULL,
          (char *[]){ "/bin/sh", "-c",
            "dmidecode -t memory 2>/dev/null", NULL },
          NULL, G_SPAWN_DEFAULT, NULL, NULL, &dmi_out, NULL, NULL, NULL)
        && dmi_out)
      {
        char *line = dmi_out;
        char *mfr = NULL, *part = NULL, *sz_str = NULL;
        gboolean in_dev = FALSE;

        while (line && *line)
          {
            char *nl = strchr (line, '\n');
            if (nl) *nl = '\0';

            char *trim = line;
            while (*trim == '\t' || *trim == ' ') trim++;

            if (trim[0] == '\0' && in_dev)
              {
                if (sz_str && sz_str[0]
                    && g_strcmp0 (sz_str, "No Module Installed") != 0
                    && g_strcmp0 (sz_str, "No") != 0)
                  {
                    if (mfr && (g_strcmp0 (mfr, "Not Specified") == 0
                        || g_strcmp0 (mfr, "Unknown") == 0
                        || mfr[0] == '\0'))
                      { g_free (mfr); mfr = NULL; }
                    if (part && (g_strcmp0 (part, "Not Specified") == 0
                        || g_strcmp0 (part, "Unknown") == 0
                        || part[0] == '\0'))
                      { g_free (part); part = NULL; }

                    char *name;
                    if (mfr && part)
                      name = g_strdup_printf ("%s %s", mfr, part);
                    else if (part) name = g_strdup (part);
                    else if (mfr)  name = g_strdup (mfr);
                    else           name = g_strdup ("Unknown");

                    /* Collect DIMM info instead of adding directly;
                       we'll group identical sticks below. */
                    g_ptr_array_add (dimms, name);
                    g_ptr_array_add (dimms, g_strdup (sz_str));
                    dimm_found = TRUE;
                  }
                g_free (mfr); mfr = NULL;
                g_free (part); part = NULL;
                g_free (sz_str); sz_str = NULL;
                in_dev = FALSE;
              }
            else if (g_str_has_prefix (trim, "Memory Device"))
              {
                if (in_dev)
                  {
                    g_free (mfr); mfr = NULL;
                    g_free (part); part = NULL;
                    g_free (sz_str); sz_str = NULL;
                  }
                in_dev = TRUE;
              }
            else if (in_dev && g_str_has_prefix (trim, "Size:"))
              {
                const char *v = trim + 5;
                while (*v == ' ') v++;
                g_free (sz_str);
                sz_str = g_strdup (v);
              }
            else if (in_dev && g_str_has_prefix (trim, "Manufacturer:"))
              {
                const char *v = trim + 13;
                while (*v == ' ') v++;
                g_free (mfr);
                mfr = g_strdup (v);
              }
            else if (in_dev && g_str_has_prefix (trim, "Part Number:"))
              {
                const char *v = trim + 12;
                while (*v == ' ') v++;
                g_free (part);
                part = g_strdup (v);
              }
            else if (in_dev && g_str_has_prefix (trim, "Type:"))
              {
                const char *v = trim + 5;
                while (*v == ' ') v++;
                if (!page->ram_technology)
                  page->ram_technology = g_strdup (v);
              }
            else if (in_dev && g_str_has_prefix (trim, "Speed:"))
              {
                const char *v = trim + 6;
                while (*v == ' ') v++;
                char *end;
                uint64_t freq = g_ascii_strtoull (v, &end, 10);
                if (freq > 0 && page->ram_freq_mhz == 0)
                  page->ram_freq_mhz = freq;
              }

            if (nl) { *nl = '\n'; line = nl + 1; }
            else break;
          }

        g_free (mfr);
        g_free (part);
        g_free (sz_str);
        g_free (dmi_out);
        dmi_out = NULL;
      }

    if (dimm_found)
      {
        /* Group identical DIMM sticks and display as:
           "Brand Model (Nx SIZE - TOTAL Total)" */
        int n_dimms = dimms->len / 2;
        gboolean *used = g_new0 (gboolean, n_dimms);

        /* First pass: collect per-DIMM info for the detail page */
        GPtrArray *dimm_infos = g_ptr_array_new_with_free_func (g_free);
        for (int i = 0; i < n_dimms; i++)
          {
            const char *name = g_ptr_array_index (dimms, i * 2);
            const char *size0 = g_ptr_array_index (dimms, i * 2 + 1);
            g_ptr_array_add (dimm_infos,
              g_strdup_printf ("%s — %s", name, size0));
          }

        for (int i = 0; i < n_dimms; i++)
          {
            if (used[i]) continue;

            const char *name = g_ptr_array_index (dimms, i * 2);
            const char *size0 = g_ptr_array_index (dimms, i * 2 + 1);
            int count = 1;

            /* Parse per-stick size */
            char *end;
            double per_val = strtod (size0, &end);
            while (*end == ' ') end++;
            uint64_t total_mb = 0;
            if (g_str_has_prefix (end, "GB"))
              total_mb = (uint64_t) (per_val * 1024);
            else if (g_str_has_prefix (end, "MB"))
              total_mb = (uint64_t) per_val;
            else
              total_mb = (uint64_t) per_val;

            for (int j = i + 1; j < n_dimms; j++)
              {
                if (used[j]) continue;
                const char *nj = g_ptr_array_index (dimms, j * 2);
                if (g_strcmp0 (name, nj) == 0)
                  {
                    count++;
                    const char *sz = g_ptr_array_index (dimms, j * 2 + 1);
                    double v = strtod (sz, &end);
                    while (*end == ' ') end++;
                    if (g_str_has_prefix (end, "GB"))
                      total_mb += (uint64_t) (v * 1024);
                    else if (g_str_has_prefix (end, "MB"))
                      total_mb += (uint64_t) v;
                    else
                      total_mb += (uint64_t) v;
                    used[j] = TRUE;
                  }
              }

            char *ram_str;
            if (count > 1)
              {
                /* Per-stick display size */
                uint64_t per_mb = total_mb / count;
                char per_str[32];
                if (per_mb >= 1024 && per_mb % 1024 == 0)
                  g_snprintf (per_str, sizeof (per_str), "%.0f GB", per_mb / 1024.0);
                else if (per_mb >= 1024)
                  g_snprintf (per_str, sizeof (per_str), "%.1f GB", per_mb / 1024.0);
                else
                  g_snprintf (per_str, sizeof (per_str), "%ld MB", (long) per_mb);

                char total_str[32];
                if (total_mb >= 1024 && total_mb % 1024 == 0)
                  g_snprintf (total_str, sizeof (total_str), tr ("%.0f GB Total"),
                    total_mb / 1024.0);
                else if (total_mb >= 1024)
                  g_snprintf (total_str, sizeof (total_str), tr ("%.1f GB Total"),
                    total_mb / 1024.0);
                else
                  g_snprintf (total_str, sizeof (total_str), tr ("%ld MB Total"),
                    (long) total_mb);

                ram_str = g_strdup_printf ("%s (%dx %s - %s)", name, count, per_str, total_str);
              }
            else
              ram_str = g_strdup_printf ("%s %s", name, size0);

            add_device (page, ram_str, "ram", tr ("Memory"), NULL, 0, 0, NULL, NULL);
            g_free (ram_str);
          }

        g_free (used);

        /* Store per-DIMM info for the detail page */
        page->n_ram_dimms = dimm_infos->len;
        page->ram_dimm_names = g_new0 (char *, page->n_ram_dimms);
        for (guint i = 0; i < dimm_infos->len; i++)
          page->ram_dimm_names[i] = g_strdup (g_ptr_array_index (dimm_infos, i));
        g_ptr_array_unref (dimm_infos);
      }
    else
      {
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
                        char *ram_str;
                        if (kb > 1048576)
                          ram_str = g_strdup_printf ("%.1f GB", kb / 1048576.0);
                        else
                          ram_str = g_strdup_printf ("%.0f MB", kb / 1024.0);
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

    g_ptr_array_unref (dimms);
    g_free (dmi_out);
  }

  /* Input devices — 3-phase scan */
  {
    /* Phase 1+2: /dev/input/by-id/ and /dev/input/by-path/ */
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *kbd_phys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *joy_phys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    const char *scan_dirs[] = { "/dev/input/by-id", "/dev/input/by-path" };
    for (size_t scan = 0; scan < G_N_ELEMENTS (scan_dirs); scan++)
      {
        GDir *dir = g_dir_open (scan_dirs[scan], 0, NULL);
        if (!dir) continue;

        const char *entry;
        while ((entry = g_dir_read_name (dir)))
          {
            if (g_str_has_suffix (entry, "event-if"))
              continue;

            char *full = g_build_filename (scan_dirs[scan], entry, NULL);
            char target[256] = {0};
            ssize_t len = readlink (full, target, sizeof (target) - 1);
            g_free (full);
            if (len < 0) continue;
            target[len] = '\0';
            if (!strstr (target, "event")) continue;

            const char *type = NULL;
            if (g_str_has_suffix (entry, "-event-joystick") || g_str_has_suffix (entry, "-joystick"))
              type = "joystick";
            else if (g_str_has_suffix (entry, "-event-kbd") || g_str_has_suffix (entry, "-kbd"))
              type = "keyboard";
            else if (g_str_has_suffix (entry, "-event-mouse") || g_str_has_suffix (entry, "-mouse"))
              type = "mouse";
            else
              continue;

            char *from_name = (scan == 0) ? extract_byid_name (entry) : g_strdup ("");
            process_input_device (page, seen, target, from_name, type, kbd_phys, joy_phys);
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
  }
}
