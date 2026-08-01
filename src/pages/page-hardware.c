#define _GNU_SOURCE
#include <linux/input.h>
#include "page-hardware.h"
#include "page-dashboard.h"
#include "epi-privilege.h"
#include "usb-ids.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glib-unix.h>

/* Forward declarations for load_gpu_config / save callbacks */
static void write_epinephrine_gpu_config (const char *key, const char *value);
static void on_perf_level_changed (GtkDropDown *dd, GParamSpec *pspec, gpointer user_data);
static void on_power_profile_changed (GtkDropDown *dd, GParamSpec *pspec, gpointer user_data);
static void on_power_limit_changed (GtkWidget *widget, gpointer user_data);
static void on_frtc_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_frtc_fps_changed (GtkSpinButton *spin, gpointer user_data);
static void on_chill_switch_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_chill_fps_changed (GtkWidget *widget, gpointer user_data);
static void on_anti_lag_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_vrs_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_zero_rpm_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_zero_rpm_temp_changed (GtkWidget *widget, gpointer user_data);
static void on_nvidia_power_limit_changed (GtkWidget *widget, gpointer user_data);
static void on_nvidia_fan_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);
static void on_nvidia_fan_speed_changed (GtkRange *range, gpointer user_data);
static void close_controller (HardwarePage *page);

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

/* ── Controller Name Database (USB VID:PID → proper name) ── */

typedef struct {
    uint16_t vendor;
    uint16_t product;
    const char *name;
    const char *url;
    const char *category;
} ControllerEntry;

/* Runtime entries from JOY_INDEX.md */
typedef struct {
    char *name;
    char *url;
    char *category;
} ControllerIndexEntry;

static void
free_controller_index_entry (gpointer data)
{
  ControllerIndexEntry *e = data;
  if (e)
    {
      g_free (e->name);
      g_free (e->url);
      g_free (e->category);
      g_free (e);
    }
}

static const ControllerEntry controller_db[] = {
  { 0x0079, 0x187c, "KROM KUMITE",
    "https://www.kromgaming.com/pt/comandos/kumite", "Arcade" },
};
static const int controller_db_count = G_N_ELEMENTS (controller_db);

static const char *
lookup_controller_name (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  /* Try online/usb.ids database first */
  if (page && page->usb_ids)
    {
      const char *name = usb_ids_lookup (page->usb_ids, vendor, product);
      if (name) return name;
    }
  /* Fall back to JOY_INDEX.md */
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->name) return e->name;
    }
  /* Fall back to hardcoded database */
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].name;
  return NULL;
}

static const char *
lookup_controller_url (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->url) return e->url;
    }
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].url;
  return NULL;
}

static const char *
lookup_controller_category (HardwarePage *page, uint16_t vendor, uint16_t product)
{
  if (page && page->controller_index)
    {
      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
      ControllerIndexEntry *e = g_hash_table_lookup (page->controller_index, key);
      if (e && e->category) return e->category;
    }
  for (int i = 0; i < controller_db_count; i++)
    if (controller_db[i].vendor == vendor && controller_db[i].product == product)
      return controller_db[i].category;
  return NULL;
}

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

/* ---- GPU Info ---- */

static const char *
gpu_vendor_display_name (const char *name)
{
  if (!name || !name[0]) return "Other";
  char *lower = g_ascii_strdown (name, -1);
  const char *result = "Other";
  if (strstr (lower, "nvidia") || strstr (lower, "geforce") || strstr (lower, "quadro"))
    result = "NVIDIA";
  else if (strstr (lower, "amd") || strstr (lower, "radeon") || strstr (lower, "ati"))
    result = "AMD";
  else if (strstr (lower, "intel"))
    result = "Intel";
  else if (strstr (lower, "qualcomm") || strstr (lower, "adreno"))
    result = "Qualcomm";
  else if (strstr (lower, "innosilicon"))
    result = "Innosilicon";
  g_free (lower);
  return result;
}

/* Find and load GPU specs from GPU_INDEX.md */
static void
load_gpu_specs (HardwarePage *page)
{
  if (!page->gpu || !page->gpu->device_name) return;

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/GPU_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    g_file_get_contents (DATADIR "/epinephrine/database/GPU_INDEX.md", &content, NULL, NULL);
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "database", "GPU_INDEX.md", NULL);
      g_file_get_contents (rel, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
    }

  if (!content) return;

  /* Normalize detected name for fuzzy matching:
   * Remove (R), (TM), ®, ™, extra whitespace, parenthesized suffixes */
  char *norm_detected = g_strdup (page->gpu->device_name);
  {
    char *lower = g_ascii_strdown (norm_detected, -1);
    g_free (norm_detected);
    norm_detected = lower;
  }
  /* Remove OEM decorations: (r), (tm), ®, ™ */
  {
    char *tmp = g_strdup (norm_detected);
    g_free (norm_detected);
    /* Remove "(r)" and "(tm)" substrings */
    char *r;
    while ((r = strstr (tmp, "(r)")) != NULL)
      {
        memmove (r, r + 3, strlen (r + 3) + 1);
      }
    while ((r = strstr (tmp, "(tm)")) != NULL)
      {
        memmove (r, r + 4, strlen (r + 4) + 1);
      }
    /* Remove ® and ™ characters */
    char *w = tmp;
    for (char *p = tmp; *p; p++)
      {
        if ((unsigned char)*p == 0xC2 && (unsigned char)p[1] == 0xAE)
          { p++; continue; }  /* ® = 0xC2 0xAE */
        if ((unsigned char)*p == 0xE2 && (unsigned char)p[1] == 0x84
            && (unsigned char)p[2] == 0xA2)
          { p += 2; continue; }  /* ™ = 0xE2 0x84 0xA2 */
        *w++ = *p;
      }
    *w = '\0';
    norm_detected = tmp;
  }
  g_strstrip (norm_detected);
  {
    char *tmp = g_strdup (norm_detected);
    g_free (norm_detected);
    /* Remove content in parentheses at the end, e.g. "(MESA XE2)" or "(BMG G21)" */
    char *paren = strrchr (tmp, '(');
    if (paren && paren != tmp)
      {
        *paren = '\0';
        g_strstrip (tmp);
      }
    norm_detected = tmp;
  }

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

      char *gpu_name = g_strstrip (cols[1]);

      /* Fuzzy match: normalize index name and check if key words match */
      char *norm_index = g_strdup (gpu_name);
      {
        char *lower = g_ascii_strdown (norm_index, -1);
        g_free (norm_index);
        norm_index = lower;
      }
      /* Remove OEM decorations: (r), (tm), ®, ™ */
      {
        char *tmp = g_strdup (norm_index);
        g_free (norm_index);
        char *r;
        while ((r = strstr (tmp, "(r)")) != NULL)
          memmove (r, r + 3, strlen (r + 3) + 1);
        while ((r = strstr (tmp, "(tm)")) != NULL)
          memmove (r, r + 4, strlen (r + 4) + 1);
        char *w = tmp;
        for (char *p = tmp; *p; p++)
          {
            if ((unsigned char)*p == 0xC2 && (unsigned char)p[1] == 0xAE)
              { p++; continue; }
            if ((unsigned char)*p == 0xE2 && (unsigned char)p[1] == 0x84
                && (unsigned char)p[2] == 0xA2)
              { p += 2; continue; }
            *w++ = *p;
          }
        *w = '\0';
        norm_index = tmp;
      }
      g_strstrip (norm_index);
      {
        char *tmp = g_strdup (norm_index);
        g_free (norm_index);
        char *paren = strrchr (tmp, '(');
        if (paren && paren != tmp)
          {
            *paren = '\0';
            g_strstrip (tmp);
          }
        norm_index = tmp;
      }

      /* Check if normalized names match or one contains the other */
      gboolean match = (g_strcmp0 (norm_detected, norm_index) == 0);
      if (!match)
        match = g_str_has_prefix (norm_detected, norm_index)
             || g_str_has_prefix (norm_index, norm_detected);
      /* Also try matching without vendor prefix (e.g. "arc b580") */
      if (!match)
        {
          const char *key_detected = strstr (norm_detected, "arc");
          if (!key_detected) key_detected = strstr (norm_detected, "geforce");
          if (!key_detected) key_detected = strstr (norm_detected, "radeon");
          if (!key_detected) key_detected = strstr (norm_detected, "iris");
          if (!key_detected) key_detected = strstr (norm_detected, "uhd");
          if (!key_detected) key_detected = strstr (norm_detected, "hd graphics");
          if (!key_detected) key_detected = strstr (norm_detected, "adreno");
          if (!key_detected) key_detected = strstr (norm_detected, "fenghua");

          const char *key_index = strstr (norm_index, "arc");
          if (!key_index) key_index = strstr (norm_index, "geforce");
          if (!key_index) key_index = strstr (norm_index, "radeon");
          if (!key_index) key_index = strstr (norm_index, "iris");
          if (!key_index) key_index = strstr (norm_index, "uhd");
          if (!key_index) key_index = strstr (norm_index, "hd graphics");
          if (!key_index) key_index = strstr (norm_index, "adreno");
          if (!key_index) key_index = strstr (norm_index, "fenghua");

          if (key_detected && key_index)
            {
              /* Compare from the keyword onwards */
              size_t len_d = strlen (key_detected);
              size_t len_i = strlen (key_index);
              size_t min_len = MIN (len_d, len_i);
              match = (strncmp (key_detected, key_index, min_len) == 0);
            }
        }

      g_free (norm_index);

      if (match)
        {
          if (cols[2])
            {
              char *vram_str = g_strstrip (cols[2]);
              if (vram_str[0] && g_strcmp0 (vram_str, "NULL") != 0
                  && page->gpu->vram_total == 0)
                {
                  /* Parse VRAM from index as fallback, e.g. "8 GB", "12GB", "Up to 12GB" */
                  char *end = NULL;
                  double gb = g_ascii_strtod (vram_str, &end);
                  /* If parsing from start failed, scan for first digit */
                  if (gb <= 0)
                    {
                      const char *p = vram_str;
                      while (*p && !g_ascii_isdigit (*p)) p++;
                      if (*p) gb = g_ascii_strtod (p, &end);
                    }
                  if (gb > 0)
                    {
                      page->gpu->vram_total = (uint64_t)(gb * 1024 * 1024 * 1024);
                      page->gpu->vram_from_index = TRUE;
                    }
                }
            }

          if (cols[3])
            {
              char *url = g_strstrip (cols[3]);
              if (url[0] && g_str_has_prefix (url, "http"))
                page->gpu->gpu_specs_url = g_strdup (url);
            }

          if (cols[4])
            {
              char *specs = g_strstrip (cols[4]);
              if (specs[0])
                {
                  for (char *p = specs; *p; p++)
                    if (*p == '\t') *p = ' ';
                  page->gpu->gpu_specs = g_strdup (specs);
                }
            }

          if (cols[5])
            {
              char *date = g_strstrip (cols[5]);
              if (date[0] && g_strcmp0 (date, "NULL") != 0)
                page->gpu->gpu_release_date = g_strdup (date);
            }

          g_strfreev (cols);
          break;
        }
      g_strfreev (cols);
    }

  g_free (norm_detected);
  g_strfreev (lines);
}

/* Load controller database from JOY_INDEX.md (GResource → DATADIR → CWD) */
static void
load_controller_index (HardwarePage *page)
{
  if (page->controller_index) return;

  page->controller_index = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, free_controller_index_entry);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/JOY_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    g_file_get_contents (DATADIR "/epinephrine/database/JOY_INDEX.md", &content, NULL, NULL);
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "database", "JOY_INDEX.md", NULL);
      g_file_get_contents (rel, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
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

      /* Columns: Name | VID:PID | Category | URL | Features | Connection | Buttons */
      char *name = g_strstrip (cols[1]);
      char *vidpid_str = cols[2] ? g_strstrip (cols[2]) : NULL;
      char *cat_str = cols[3] ? g_strstrip (cols[3]) : NULL;
      char *url_str = cols[4] ? g_strstrip (cols[4]) : NULL;

      if (!name || !name[0] || !vidpid_str || !vidpid_str[0])
        {
          g_strfreev (cols);
          continue;
        }

      /* Parse VID:PID format "XXXX:XXXX" */
      unsigned int v = 0, p = 0;
      if (sscanf (vidpid_str, "%04x:%04x", &v, &p) != 2 || v == 0)
        {
          g_strfreev (cols);
          continue;
        }

      char key[16];
      g_snprintf (key, sizeof (key), "%04x:%04x", v, p);

      ControllerIndexEntry *entry = g_new0 (ControllerIndexEntry, 1);
      entry->name = g_strdup (name);
      entry->url = url_str && url_str[0] ? g_strdup (url_str) : NULL;
      entry->category = cat_str && cat_str[0] ? g_strdup (cat_str) : NULL;

      g_hash_table_insert (page->controller_index, g_strdup (key), entry);

      g_strfreev (cols);
    }

  g_strfreev (lines);
}

/* Forward declarations */
static uint64_t read_sysfs_u64 (const char *path);
static char *read_sysfs_string (const char *path);

/* Find the DRM card sysfs path for the GPU */
static char *
find_gpu_sysfs (char **out_driver)
{
  if (out_driver) *out_driver = NULL;
  GDir *dir = g_dir_open ("/sys/class/drm", 0, NULL);
  if (!dir) return NULL;

  char *best_path = NULL;
  char *best_driver = NULL;
  int best_score = -1;

  const char *entry;
  while ((entry = g_dir_read_name (dir)))
    {
      if (!g_str_has_prefix (entry, "card") || strchr (entry, '-'))
        continue;

      char *dev_path = g_build_filename ("/sys/class/drm", entry, "device", NULL);
      char *real_dev = realpath (dev_path, NULL);
      if (!real_dev) { g_free (dev_path); continue; }
      g_free (dev_path);

      char *vendor_str = read_sysfs_string (g_strdup_printf ("%s/vendor", real_dev));
      if (!vendor_str) { free (real_dev); continue; }

      int score = 0;
      char *driver = NULL;

      /* Read kernel driver name from driver symlink */
      char *driver_link = g_file_read_link (
        g_strdup_printf ("%s/driver", real_dev), NULL);
      if (driver_link)
        {
          char *base = strrchr (driver_link, '/');
          if (base) driver = g_strdup (base + 1);
          else driver = g_strdup (driver_link);
          g_free (driver_link);
        }

      /* Check if this is a discrete GPU (has VRAM) */
      char *vram_path = g_strdup_printf ("%s/mem_info_vram_total", real_dev);
      gboolean has_vram = (access (vram_path, F_OK) == 0);
      g_free (vram_path);

      /* Scoring */
      if (has_vram) score += 100;           /* dGPU > iGPU */

      if (driver && g_strcmp0 (driver, "xe") == 0)
        score += 50;                         /* prefer Xe (Intel Arc) */
      else if (driver && g_strcmp0 (driver, "nvidia") == 0)
        score += 40;
      else if (driver && g_strcmp0 (driver, "amdgpu") == 0 && has_vram)
        score += 30;                         /* AMD dGPU */
      else if (g_strcmp0 (vendor_str, "0x1002") == 0 && !has_vram)
        score += 5;                          /* AMD iGPU — low priority */

      if (g_strcmp0 (vendor_str, "0x8086") == 0 && has_vram)
        score += 10;                         /* Intel dGPU bonus */

      /* Also check for active display */
      char *status_path = g_build_filename ("/sys/class/drm",
        entry, "status", NULL);
      char *status = read_sysfs_string (status_path);
      g_free (status_path);
      if (status && g_strcmp0 (status, "connected") == 0)
        score += 20;                         /* active display = likely main GPU */
      g_free (status);

      g_free (vendor_str);

      if (score > best_score)
        {
          g_free (best_path);
          g_free (best_driver);
          best_path = g_strdup (real_dev);
          best_driver = g_strdup (driver);
          best_score = score;
        }

      g_free (driver);
      g_free (real_dev);
    }
  g_dir_close (dir);

  if (out_driver) *out_driver = best_driver;
  else g_free (best_driver);
  return best_path;
}

static uint64_t
read_sysfs_u64 (const char *path)
{
  if (!path) return 0;
  char *content = NULL;
  if (!g_file_get_contents (path, &content, NULL, NULL))
    return 0;
  uint64_t val = strtoull (content, NULL, 10);
  g_free (content);
  return val;
}

static char *
read_sysfs_string (const char *path)
{
  if (!path) return NULL;
  char *content = NULL;
  if (!g_file_get_contents (path, &content, NULL, NULL))
    return NULL;
  g_strstrip (content);
  if (content[0] == '\0')
    {
      g_free (content);
      return NULL;
    }
  return content;
}

/* ---- NVIDIA Helpers ---- */

static gboolean
nvidia_smi_available (void)
{
  return g_find_program_in_path ("nvidia-smi") != NULL;
}

/* Parse a line from "nvidia-smi --query-gpu=... --format=csv,noheader,nounits"
   Returns 8 parsed values (temp, usage, core_clock, mem_clock, vram_mib, power_w, fan_pct), with *ok set */
static gboolean
nvidia_smi_query (float vals[8], gboolean ok[8], int gpu_index)
{
  char *out = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "nvidia-smi", "--query-gpu=index,temperature.gpu,utilization.gpu,"
      "clocks.current.graphics,clocks.current.memory,memory.used,"
      "power.draw,fan.speed", "--format=csv,noheader,nounits", NULL },
    NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, NULL, NULL);
  if (!out || out[0] == '\0')
    {
      g_free (out);
      return FALSE;
    }

  gboolean found = FALSE;
  char **lines = g_strsplit (out, "\n", -1);
  g_free (out);

  for (int i = 0; lines[i] && !found; i++)
    {
      g_strstrip (lines[i]);
      if (lines[i][0] == '\0') continue;

      /* Robust CSV parsing: split on commas and read each field with
         g_ascii_strtod, so the format doesn't depend on sscanf matching.
         Tolerates "N/A"/"[N/A]", missing/extra spaces and trailing "\r". */
      int idx = -1;
      float data[7] = { 0 };
      gboolean have[7] = { FALSE };

      char **tok = g_strsplit_set (lines[i], ",", -1);
      if (tok[0] && sscanf (tok[0], "%d", &idx) == 1 && idx == gpu_index)
        {
          for (int t = 1; t <= 7 && tok[t]; t++)
            {
              g_strstrip (tok[t]);
              if (tok[t][0] == '\0' || strstr (tok[t], "N/A"))
                continue;
              char *end = NULL;
              double v = g_ascii_strtod (tok[t], &end);
              if (end != tok[t])
                { data[t - 1] = (float) v; have[t - 1] = TRUE; }
            }
          vals[0] = data[0]; ok[0] = have[0] && data[0] > 0;   /* Temp (°C) */
          vals[1] = data[1]; ok[1] = have[1];                   /* GPU Usage (%) */
          vals[2] = data[2]; ok[2] = have[2] && data[2] > 0;   /* Core Clock (MHz) */
          vals[3] = data[3]; ok[3] = have[3] && data[3] > 0;   /* Mem Clock (MHz) */
          vals[4] = data[4] / 1024.0f; ok[4] = have[4] && data[4] > 0; /* VRAM (MiB→GB) */
          vals[5] = data[5]; ok[5] = have[5] && data[5] > 0;   /* Power (W) */
          vals[6] = 0; ok[6] = FALSE;                           /* Voltage */
          vals[7] = data[6]; ok[7] = have[6] && data[6] > 0;   /* Fan Speed (%) */
          found = TRUE;
        }
      g_strfreev (tok);
    }

  g_strfreev (lines);
  return found;
}

/* Run nvidia-smi with a management command via privilege helper */
static void
nvidia_smi_manage (const char *args)
{
  char *cmd = g_strdup_printf ("nvidia-smi %s", args);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
}

/* Read a numeric value from nvidia-smi query; returns -1 on error */
static double
nvidia_smi_read (const char *field, int gpu_index)
{
  char index_str[16];
  g_snprintf (index_str, sizeof (index_str), "%d", gpu_index);
  char *out = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "nvidia-smi", "--query-gpu", (char *)field, "--format=csv,noheader,nounits",
      "-i", index_str, NULL },
    NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, NULL, NULL);
  if (!out) return -1;
  g_strstrip (out);
  double val = atof (out);
  g_free (out);
  return val;
}

/* Extract the "BB:DD.F" suffix from a PCI bus ID such as
   "0000:01:00.0" (sysfs) or "00000000:01:00.0" (nvidia-smi),
   so both formats can be compared reliably. */
static const char *
bus_id_suffix (const char *bus_id)
{
  if (!bus_id || !bus_id[0]) return NULL;
  const char *s = strrchr (bus_id, ':');
  if (!s || s == bus_id) return NULL;
  const char *prev = s - 1;
  while (prev > bus_id && *prev != ':')
    prev--;
  if (*prev != ':') return NULL;
  return prev;
}

static void
detect_gpu_info (HardwarePage *page)
{
  page->gpu = g_new0 (GpuInfo, 1);

  /* 1. Try vulkaninfo for the detailed GPU name */
  char *vk_out = NULL;
  const char *vk_name_argv[] = { "/bin/sh", "-c",
    "vulkaninfo 2>/dev/null | sed -n '/^GPU0:/,/^GPU1:/p' | grep 'deviceName' | sed 's/.*= //'",
    NULL };
  if (g_spawn_sync (NULL, (char **) vk_name_argv, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &vk_out, NULL, NULL, NULL) && vk_out)
    {
      g_strstrip (vk_out);
      if (vk_out[0])
        page->gpu->device_name = g_strstrip (vk_out);
      else
        { g_free (vk_out); vk_out = NULL; }
    }

  /* 2. Get vulkan driver info and version */
  char *vk_driver = NULL;
  const char *vk_drv_argv[] = { "/bin/sh", "-c",
    "vulkaninfo 2>/dev/null | sed -n '/^GPU0:/,/^GPU1:/p' | grep 'driverInfo' | sed 's/.*= //'",
    NULL };
  if (g_spawn_sync (NULL, (char **) vk_drv_argv, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &vk_driver, NULL, NULL, NULL) && vk_driver)
    {
      g_strstrip (vk_driver);
      if (vk_driver[0])
        page->gpu->driver_info = g_strstrip (vk_driver);
      else
        { g_free (vk_driver); vk_driver = NULL; }
    }

  char *vk_drvname = NULL;
  const char *vk_drn_argv[] = { "/bin/sh", "-c",
    "vulkaninfo 2>/dev/null | sed -n '/^GPU0:/,/^GPU1:/p' | grep 'driverName' | sed 's/.*= //'",
    NULL };
  if (g_spawn_sync (NULL, (char **) vk_drn_argv, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &vk_drvname, NULL, NULL, NULL) && vk_drvname)
    {
      g_strstrip (vk_drvname);
      if (vk_drvname[0])
        page->gpu->driver_name = g_strstrip (vk_drvname);
      else
        { g_free (vk_drvname); vk_drvname = NULL; }
    }

  char *vk_ver = NULL;
  const char *vk_ver_argv[] = { "/bin/sh", "-c",
    "vulkaninfo 2>/dev/null | sed -n '/^GPU0:/,/^GPU1:/p' | grep 'apiVersion' | sed 's/.*= //'",
    NULL };
  if (g_spawn_sync (NULL, (char **) vk_ver_argv, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &vk_ver, NULL, NULL, NULL) && vk_ver)
    {
      g_strstrip (vk_ver);
      if (vk_ver[0])
        page->gpu->vulkan_version = g_strstrip (vk_ver);
      else
        { g_free (vk_ver); vk_ver = NULL; }
    }

  /* 3. Get lspci info for kernel driver and fallback name */
  char *lspci_out = NULL;
  const char *gpu_argv[] = { "/bin/sh", "-c",
    "lspci 2>/dev/null | grep -im1 'vga\\|3d\\|display'", NULL };
  if (g_spawn_sync (NULL, (char **) gpu_argv, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, &lspci_out, NULL, NULL, NULL) && lspci_out)
    {
      g_strstrip (lspci_out);
      char *space = strchr (lspci_out, ' ');
      char *name = space ? g_strstrip (space + 1) : lspci_out;

      /* Get kernel driver from lspci -k */
      char *drv_out = NULL;
      const char *drv_argv[] = { "/bin/sh", "-c",
        "lspci -k 2>/dev/null | grep -A2 'vga\\|3d\\|display' | grep 'Kernel driver in use' | head -1",
        NULL };
      if (g_spawn_sync (NULL, (char **) drv_argv, NULL, G_SPAWN_DEFAULT,
            NULL, NULL, &drv_out, NULL, NULL, NULL) && drv_out)
        {
          g_strstrip (drv_out);
          char *colon = strrchr (drv_out, ':');
          if (colon)
            {
              g_strstrip (colon + 1);
              page->gpu->kernel_driver = g_strdup (colon + 1);
            }
          g_free (drv_out);
        }

      /* Use lspci name as fallback if vulkaninfo didn't work */
      if (!page->gpu->device_name)
        {
          const char *bracket_start = strchr (name, '[');
          if (bracket_start)
            {
              bracket_start++;
              const char *bracket_end = strchr (bracket_start, ']');
              if (bracket_end && bracket_end > bracket_start)
                {
                  size_t len = bracket_end - bracket_start;
                  char *short_name = g_malloc (len + 1);
                  memcpy (short_name, bracket_start, len);
                  short_name[len] = '\0';
                  g_strstrip (short_name);
                  if (short_name[0])
                    page->gpu->device_name = g_strstrip (short_name);
                  else
                    g_free (short_name);
                }
            }
          if (!page->gpu->device_name)
            {
              const char *prefixes[] = {
                "VGA compatible controller: ", "VGA compatible: ",
                "3D controller: ", "Display controller: ", NULL
              };
              const char *start = name;
              for (const char **p = prefixes; *p; p++)
                if (g_str_has_prefix (start, *p)) { start += strlen (*p); break; }
              char *result = g_strdup (start);
              char *rev = strstr (result, " (rev ");
              if (rev)
                {
                  char *paren = strchr (rev + 6, ')');
                  if (paren) *rev = '\0';
                }
              g_strstrip (result);
              page->gpu->device_name = result;
            }
        }
      g_free (lspci_out);
    }

  /* 3b. glxinfo fallback for GPU name if still unknown */
  if (!page->gpu->device_name)
    {
      char *glx_out = NULL;
      g_spawn_sync (NULL,
        (char *[]){ "/bin/sh", "-c",
          "glxinfo 2>/dev/null | grep 'OpenGL renderer' | sed 's/.*: //'",
          NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &glx_out, NULL, NULL, NULL);
      if (glx_out)
        {
          g_strstrip (glx_out);
          if (glx_out[0])
            page->gpu->device_name = glx_out;
          else
            g_free (glx_out);
        }
    }

  g_debug ("GPU detected: name=%s vendor=%s driver=%s",
           page->gpu->device_name ? page->gpu->device_name : "N/A",
           page->gpu->vendor ? page->gpu->vendor : "N/A",
           page->gpu->kernel_driver ? page->gpu->kernel_driver : "N/A");

  /* 4. Detect vendor (may need update from glxinfo fallback) */
  if (!page->gpu->vendor)
    page->gpu->vendor = g_strdup (
      gpu_vendor_display_name (page->gpu->device_name));

  /* 5. Load specs from GPU_INDEX.md */
  load_gpu_specs (page);

  /* 6. Sysfs path for management and VRAM */
  char *sysfs_driver = NULL;
  page->gpu->sysfs_path = find_gpu_sysfs (&sysfs_driver);
  if (sysfs_driver)
    {
      if (page->gpu->kernel_driver)
        g_free (page->gpu->kernel_driver);
      page->gpu->kernel_driver = sysfs_driver;
    }
  else if (!page->gpu->kernel_driver)
    page->gpu->kernel_driver = g_strdup ("unknown");

  /* The real-time monitor and management features act on the card selected
     by find_gpu_sysfs(), so the vendor must match the driver actually bound
     to that card. This prevents hybrid setups (e.g. AMD iGPU + NVIDIA dGPU)
     from mislabelling the GPU when vulkaninfo lists another device first. */
  if (page->gpu->kernel_driver)
    {
      if (g_strcmp0 (page->gpu->kernel_driver, "nvidia") == 0)
        {
          g_free (page->gpu->vendor);
          page->gpu->vendor = g_strdup ("NVIDIA");
        }
      else if (g_strcmp0 (page->gpu->kernel_driver, "amdgpu") == 0)
        {
          g_free (page->gpu->vendor);
          page->gpu->vendor = g_strdup ("AMD");
        }
      else if (g_strcmp0 (page->gpu->kernel_driver, "xe") == 0
               || g_strcmp0 (page->gpu->kernel_driver, "i915") == 0)
        {
          g_free (page->gpu->vendor);
          page->gpu->vendor = g_strdup ("Intel");
        }
    }

  if (page->gpu->sysfs_path)
    {
      /* Check for performance level */
      char *perf_path = g_build_filename (page->gpu->sysfs_path,
        "power_dpm_force_performance_level", NULL);
      page->gpu->has_perf_level = (access (perf_path, F_OK) == 0);
      g_free (perf_path);

      /* Check for hwmon */
      page->gpu->hwmon_temp_file = NULL;
      GDir *hwmon_dir = g_dir_open (
        g_build_filename (page->gpu->sysfs_path, "hwmon", NULL), 0, NULL);
      if (hwmon_dir)
        {
          const char *hwmon_entry;
          while ((hwmon_entry = g_dir_read_name (hwmon_dir)))
            {
              if (!g_str_has_prefix (hwmon_entry, "hwmon")) continue;

              char *base = g_build_filename (page->gpu->sysfs_path,
                "hwmon", hwmon_entry, NULL);

              /* For Xe driver: package temperature is at temp2_input */
              if (g_strcmp0 (page->gpu->kernel_driver, "xe") == 0)
                {
                  page->gpu->hwmon_path = base;

                  char *tp = g_build_filename (base, "temp2_input", NULL);
                  if (access (tp, F_OK) == 0)
                    page->gpu->hwmon_temp_file = g_strdup ("temp2_input");
                  g_free (tp);

                  if (!page->gpu->hwmon_temp_file)
                    {
                      for (int ti = 1; ti <= 10; ti++)
                        {
                          char *t = g_strdup_printf ("%s/temp%d_input", base, ti);
                          if (access (t, F_OK) == 0)
                            {
                              page->gpu->hwmon_temp_file = g_strdup_printf ("temp%d_input", ti);
                              g_free (t);
                              break;
                            }
                          g_free (t);
                        }
                    }

                  if (page->gpu->hwmon_temp_file)
                    break;
                  page->gpu->hwmon_path = NULL;
                  g_free (base);
                }
              else
                {
                  /* AMD/i915: look for pwm1_enable or temp1_input */
                  char *pwm_path = g_build_filename (base, "pwm1_enable", NULL);
                  gboolean has_pwm = (access (pwm_path, F_OK) == 0);
                  g_free (pwm_path);

                  char *temp_path = g_build_filename (base, "temp1_input", NULL);
                  gboolean has_temp = (access (temp_path, F_OK) == 0);
                  g_free (temp_path);

                  if (has_pwm || has_temp)
                    {
                      page->gpu->hwmon_path = base;
                      page->gpu->hwmon_temp_file = g_strdup ("temp1_input");
                      if (has_pwm)
                        page->gpu->has_fan_control = TRUE;
                      break;
                    }
                  g_free (base);
                }
            }
          g_dir_close (hwmon_dir);
        }

      /* VRAM total — only overwrite if sysfs has a real value */
      char *vram_path = g_build_filename (page->gpu->sysfs_path,
        "mem_info_vram_total", NULL);
      uint64_t sysfs_vram = read_sysfs_u64 (vram_path);
      g_free (vram_path);
      if (sysfs_vram > 0)
        {
          page->gpu->vram_total = sysfs_vram;
          page->gpu->vram_from_index = FALSE;
        }

      /* Fallback VRAM for Xe: tile0/gt0/addr_range */
      if (page->gpu->vram_total == 0 &&
          g_strcmp0 (page->gpu->kernel_driver, "xe") == 0)
        {
          char *ar_path = g_build_filename (page->gpu->sysfs_path,
            "tile0", "gt0", "addr_range", NULL);
          uint64_t ar = read_sysfs_u64 (ar_path);
          g_free (ar_path);
          if (ar > 0)
            page->gpu->vram_total = ar;
        }

      /* VRAM fallback via nvidia-smi for NVIDIA GPUs */
      if (page->gpu->vram_total == 0 &&
          g_strcmp0 (page->gpu->vendor, "NVIDIA") == 0)
        {
          char *nv_out = NULL;
          g_spawn_sync (NULL,
            (char *[]){ "/bin/sh", "-c",
              "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null",
              NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &nv_out, NULL, NULL, NULL);
          if (nv_out)
            {
              g_strstrip (nv_out);
              int mb = atoi (nv_out);
              if (mb > 0)
                page->gpu->vram_total = (uint64_t) mb * 1024 * 1024;
              g_free (nv_out);
            }
        }

      /* VRAM fallback via glxinfo for other GPUs */
      if (page->gpu->vram_total == 0)
        {
          char *glx_out = NULL;
          g_spawn_sync (NULL,
            (char *[]){ "/bin/sh", "-c",
              "glxinfo 2>/dev/null | grep 'Video memory' | sed 's/.* //'",
              NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &glx_out, NULL, NULL, NULL);
          if (glx_out)
            {
              g_strstrip (glx_out);
              int mb = atoi (glx_out);
              if (mb > 0)
                page->gpu->vram_total = (uint64_t) mb * 1024 * 1024;
              g_free (glx_out);
            }
        }

      g_debug ("GPU VRAM total: %lu bytes (vendor: %s, driver: %s)",
               (unsigned long) page->gpu->vram_total,
               page->gpu->vendor ? page->gpu->vendor : "unknown",
               page->gpu->kernel_driver ? page->gpu->kernel_driver : "unknown");

      char *vram_used_path = g_build_filename (page->gpu->sysfs_path,
        "mem_info_vram_used", NULL);
      page->gpu->vram_used = read_sysfs_u64 (vram_used_path);
      g_free (vram_used_path);

      /* Initialize power tracking */
      page->gpu->power_last_energy = 0;
      page->gpu->power_last_time_ns = 0;
    }

  /* 7. Detect AMD-specific features */
  page->gpu->has_radeon_chill = FALSE;
  page->gpu->has_anti_lag = FALSE;
  if (page->gpu->vendor && strcmp (page->gpu->vendor, "AMD") == 0)
    {
      page->gpu->has_radeon_chill = TRUE;

      const char *al_paths[] = {
        "/usr/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json",
        "/usr/local/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json",
        "/etc/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json",
        NULL
      };
      for (int i = 0; al_paths[i]; i++)
        {
          if (access (al_paths[i], F_OK) == 0)
            {
              page->gpu->has_anti_lag = TRUE;
              break;
            }
        }
    }

  /* 8. Power Profile Mode (sysfs pp_power_profile_mode) */
  page->gpu->has_power_profile = FALSE;
  page->power_profile_names = NULL;
  page->n_power_profiles = 0;
  if (page->gpu->sysfs_path)
    {
      char *pp_path = g_build_filename (page->gpu->sysfs_path,
        "pp_power_profile_mode", NULL);
      page->gpu->has_power_profile = (access (pp_path, F_OK) == 0);
      if (page->gpu->has_power_profile)
        {
          FILE *f = fopen (pp_path, "r");
          if (f)
            {
              GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
              char line[256];
              while (fgets (line, sizeof (line), f))
                {
                  char *star = strchr (line, '*');
                  if (star) *star = '\0';
                  char *colon = strchr (line, ':');
                  if (colon)
                    {
                      char *name = colon + 1;
                      while (*name == ' ') name++;
                      g_strstrip (name);
                      if (name[0])
                        g_ptr_array_add (names, g_strdup (name));
                    }
                }
              fclose (f);
              g_ptr_array_add (names, NULL);
              page->n_power_profiles = names->len - 1;
              page->power_profile_names = (char **) g_ptr_array_free (names, FALSE);
              if (page->n_power_profiles == 0)
                {
                  g_strfreev (page->power_profile_names);
                  page->power_profile_names = NULL;
                }
            }
          g_free (pp_path);
        }
      else
        g_free (pp_path);
    }

  /* 9. Frame Rate Target Control (MangoHud) */
  page->gpu->has_frtc = (g_find_program_in_path ("mangohud") != NULL);

  /* 10. Power Limit (sysfs power1_cap — must be writable) */
  page->gpu->has_power_limit = FALSE;
  if (page->gpu->hwmon_path)
    {
      char *cap_path = g_build_filename (page->gpu->hwmon_path,
        "power1_cap", NULL);
      page->gpu->has_power_limit = (access (cap_path, F_OK) == 0);
      g_free (cap_path);
    }

  /* 11. Zero RPM Fan Mode (sysfs fan_zero_rpm_enable) */
  page->gpu->has_zero_rpm_fan = FALSE;
  if (page->gpu->hwmon_path)
    {
      char *zrpm_path = g_build_filename (page->gpu->hwmon_path,
        "fan_zero_rpm_enable", NULL);
      page->gpu->has_zero_rpm_fan = (access (zrpm_path, F_OK) == 0);
      g_free (zrpm_path);
    }

  /* 12. Variable Rate Shading (RDNA2+ / Intel Arc Xe HPG+) */
  page->gpu->has_vrs = FALSE;
  if (page->gpu->device_name)
    {
      char *lower = g_ascii_strdown (page->gpu->device_name, -1);
      if (strstr (lower, "navi2") || strstr (lower, "navi3")
          || strstr (lower, "rdna2") || strstr (lower, "rdna3")
          || strstr (lower, "arc") || strstr (lower, "xe2"))
          page->gpu->has_vrs = TRUE;
        g_free (lower);
    }
  if (!page->gpu->has_vrs)
    {
      /* Fallback: check Vulkan for VK_KHR_variable_rate_shading */
      const char *vrs_argv[] = {
        "/bin/sh", "-c",
        "vulkaninfo 2>/dev/null | grep -q VK_KHR_variable_rate_shading",
        NULL
      };
      int ret;
      if (g_spawn_sync (NULL, (char **) vrs_argv, NULL, G_SPAWN_DEFAULT,
            NULL, NULL, NULL, NULL, &ret, NULL) && ret == 0)
        page->gpu->has_vrs = TRUE;
    }

  /* 13. NVIDIA nvidia-smi detection */
  page->gpu->has_nvidia_smi = FALSE;
  page->gpu->nvidia_gpu_index = 0;
  if (g_strcmp0 (page->gpu->vendor, "NVIDIA") == 0 && nvidia_smi_available ())
    {
      page->gpu->has_nvidia_smi = TRUE;
      /* Find our NVIDIA GPU index (try to match by PCI bus) */
      if (page->gpu->sysfs_path)
        {
          char *pci_addr = strrchr (page->gpu->sysfs_path, '/');
          if (pci_addr)
            {
              pci_addr++;
              const char *sysfs_suffix = bus_id_suffix (pci_addr);
              /* Get all GPU indices and bus IDs, then match by bus ID.
                 nvidia-smi formats the bus ID as "00000000:01:00.0" while
                 sysfs uses "0000:01:00.0", so we compare only the
                 "BB:DD.F" suffix of both. */
              char *nvidia_out = NULL;
              g_spawn_sync (NULL,
                (char *[]){ "nvidia-smi", "--query-gpu=index,pci.bus_id",
                  "--format=csv,noheader", NULL },
                NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &nvidia_out, NULL, NULL, NULL);
              if (nvidia_out)
                {
                  char **lines = g_strsplit (nvidia_out, "\n", -1);
                  for (int li = 0; lines[li]; li++)
                    {
                      g_strstrip (lines[li]);
                      if (lines[li][0] == '\0')
                        continue;
                      /* Format: "0, 00000000:01:00.0" */
                      int nv_idx = -1;
                      char nv_bus[64] = {0};
                      if (sscanf (lines[li], "%d, %63[^,\n]", &nv_idx, nv_bus) >= 2)
                        {
                          g_strstrip (nv_bus);
                          const char *nv_suffix = bus_id_suffix (nv_bus);
                          if (sysfs_suffix && nv_suffix &&
                              g_ascii_strcasecmp (sysfs_suffix, nv_suffix) == 0)
                            {
                              page->gpu->nvidia_gpu_index = nv_idx;
                              break;
                            }
                        }
                    }
                  g_strfreev (lines);
                  g_free (nvidia_out);
                }
            }
        }

      /* Read power limits */
      double pl_min = nvidia_smi_read ("power.min_limit", page->gpu->nvidia_gpu_index);
      double pl_max = nvidia_smi_read ("power.max_limit", page->gpu->nvidia_gpu_index);
      page->gpu->nvidia_power_min = pl_min > 0 ? pl_min : 50;
      page->gpu->nvidia_power_max = pl_max > 0 ? pl_max : 500;
    }

  /* 14. OpenGL version */
  {
    char *gl_out = NULL;
    const char *gl_argv[] = { "/bin/sh", "-c",
      "glxinfo 2>/dev/null | grep 'OpenGL version string' | sed 's/.*: //'",
      NULL };
    if (g_spawn_sync (NULL, (char **) gl_argv, NULL, G_SPAWN_DEFAULT,
          NULL, NULL, &gl_out, NULL, NULL, NULL) && gl_out)
      {
        g_strstrip (gl_out);
        if (gl_out[0])
          page->gpu->opengl_version = gl_out;
        else
          g_free (gl_out);
      }
  }

  /* 15. Mesa version (extract from driver_info) */
  if (page->gpu->driver_info)
    {
      const char *mesa_prefix = strstr (page->gpu->driver_info, "Mesa ");
      if (mesa_prefix)
        {
          const char *ver = mesa_prefix + 5;
          const char *end = ver;
          while (*end && *end != ' ' && *end != '\n')
            end++;
          page->gpu->mesa_version = g_strndup (ver, end - ver);
        }
    }

  /* 16. Device ID (PCI vendor:device) */
  {
    char *devid_out = NULL;
    const char *devid_argv[] = { "/bin/sh", "-c",
      "lspci -n 2>/dev/null | grep -im1 'vga\\|3d\\|display' | awk '{print $NF}'",
      NULL };
    if (g_spawn_sync (NULL, (char **) devid_argv, NULL, G_SPAWN_DEFAULT,
          NULL, NULL, &devid_out, NULL, NULL, NULL) && devid_out)
      {
        g_strstrip (devid_out);
        if (devid_out[0])
          page->gpu->device_id = devid_out;
        else
          g_free (devid_out);
      }
  }

  /* 17. Resizable BAR: find largest prefetchable BAR and compare vs VRAM */
  page->gpu->resizable_bar = FALSE;
  if (page->gpu->sysfs_path && page->gpu->vram_total > 0)
    {
      uint64_t largest_bar = 0;
      /* Check resource0..resource5 — GPU BARs are mapped as resource0..N */
      for (int i = 0; i <= 5; i++)
        {
          char name[16];
          g_snprintf (name, sizeof (name), "resource%d", i);
          char *resource_path = g_build_filename (page->gpu->sysfs_path,
            name, NULL);
          struct stat st;
          if (stat (resource_path, &st) == 0 && st.st_size > 0)
            {
              if ((uint64_t) st.st_size > largest_bar)
                largest_bar = (uint64_t) st.st_size;
            }
          g_free (resource_path);
        }
      if (largest_bar > 0)
        {
          double bar_gb = (double) largest_bar / (1024.0 * 1024.0 * 1024.0);
          double vram_gb = (double) page->gpu->vram_total /
            (1024.0 * 1024.0 * 1024.0);
          if (bar_gb >= 0.9 * vram_gb)
            page->gpu->resizable_bar = TRUE;
        }
    }
}

static void
populate_gpu_specs (HardwarePage *page)
{
  /* Clear existing children */
  GtkWidget *child = gtk_widget_get_first_child (page->gpu_specs_container);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_box_remove (GTK_BOX (page->gpu_specs_container), child);
      child = next;
    }

  /* Show Features header (always visible, even if no specs loaded) */
  GtkWidget *features_title = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Features"));
  gtk_widget_add_css_class (features_title, "dim-label");
  gtk_widget_set_halign (features_title, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom (features_title, 2);
  gtk_box_append (GTK_BOX (page->gpu_specs_container), features_title);

  if (!page->gpu || !page->gpu->gpu_specs) return;

  char *clean = g_strdup (page->gpu->gpu_specs);
  for (char *p = clean; *p; p++)
    if (*p == '\t') *p = ' ';

  char **specs = g_strsplit (clean, ", ", -1);
  g_free (clean);
  for (int i = 0; specs[i]; i++)
    {
      char *s = g_strstrip (specs[i]);
      if (!s[0]) continue;

      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_widget_set_margin_start (row, 8);
      GtkWidget *bullet = gtk_label_new ("•");
      gtk_widget_add_css_class (bullet, "dim-label");
      GtkWidget *text = gtk_label_new (s);
      gtk_widget_set_halign (text, GTK_ALIGN_START);
      gtk_label_set_wrap (GTK_LABEL (text), TRUE);
      gtk_box_append (GTK_BOX (row), bullet);
      gtk_box_append (GTK_BOX (row), text);
      gtk_box_append (GTK_BOX (page->gpu_specs_container), row);
    }
  g_strfreev (specs);
}

/* ---- Device Detection ---- */

/* ── Name normalisation ── */

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
  if (!type) return "Input Device";
  switch (type[0])
    {
    case 'c': return "Controller";
    case 'j': return "Joystick";
    case 'k': return "Keyboard";
    case 'm': return "Mouse";
    default:  return "Input Device";
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
      char *fallback = g_strdup_printf ("Unknown %s", sub);
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
              const char *override = lookup_controller_name (page, vendor, product);
              if (override)
                {
                  g_free (display_name);
                  display_name = g_strdup (override);
                }
              url = lookup_controller_url (page, vendor, product);
              category = lookup_controller_category (page, vendor, product);
            }
        }
      add_device (page, display_name, type ? type : "input", type_to_subtitle (type), devpath,
        vendor, product, url, category);
    }

  g_free (display_name);
  g_free (devpath);
}

static void
detect_devices (HardwarePage *page)
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
          g_dgettext (GETTEXT_PACKAGE, "BIOS:"),
          bios ? bios : "?");
        add_device (page, name, "motherboard", info, NULL, 0, 0, NULL, NULL);
        g_free (name);
        g_free (info);
      }
    else
      add_device (page, g_dgettext (GETTEXT_PACKAGE, "Unknown Motherboard"),
        "motherboard", NULL, NULL, 0, 0, NULL, NULL);

    g_free (vendor);
    g_free (model);
    g_free (bios);
  }

  /* GPU */
  detect_gpu_info (page);

  if (page->gpu && page->gpu->device_name)
    {
      char info[64];
      if (page->gpu->vram_total > 0)
        {
          double gb = page->gpu->vram_total / (1024.0 * 1024.0 * 1024.0);
          g_snprintf (info, sizeof (info), "%s (%.1f GB)",
            g_dgettext (GETTEXT_PACKAGE, "Video Card"), gb);
        }
      else
        g_snprintf (info, sizeof (info), "%s",
          g_dgettext (GETTEXT_PACKAGE, "Video Card"));
      add_device (page, page->gpu->device_name, "gpu", info, NULL, 0, 0, NULL, NULL);
    }
  else
    add_device (page, g_dgettext (GETTEXT_PACKAGE, "Unknown GPU"), "gpu", NULL, NULL, 0, 0, NULL, NULL);

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
                      g_dgettext (GETTEXT_PACKAGE, "Processor"), NULL, 0, 0, NULL, NULL);
                    break;
                  }
              }
          }
        fclose (f);
      }
    else
      add_device (page, g_dgettext (GETTEXT_PACKAGE, "Unknown CPU"), "cpu", NULL, NULL, 0, 0, NULL, NULL);
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
                  g_snprintf (total_str, sizeof (total_str), "%.0f GB Total",
                    total_mb / 1024.0);
                else if (total_mb >= 1024)
                  g_snprintf (total_str, sizeof (total_str), "%.1f GB Total",
                    total_mb / 1024.0);
                else
                  g_snprintf (total_str, sizeof (total_str), "%ld MB Total",
                    (long) total_mb);

                ram_str = g_strdup_printf ("%s (%dx %s - %s)", name, count, per_str, total_str);
              }
            else
              ram_str = g_strdup_printf ("%s %s", name, size0);

            add_device (page, ram_str, "ram", "Memory", NULL, 0, 0, NULL, NULL);
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
                          g_dgettext (GETTEXT_PACKAGE, "Memory"), NULL, 0, 0, NULL, NULL);
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
                      const char *override = lookup_controller_name (page, vendor, product);
                      if (override) { g_free (name); name = g_strdup (override); }
                      url = lookup_controller_url (page, vendor, product);
                      category = lookup_controller_category (page, vendor, product);
                    }
                  add_device (page, name, type ? type : "input", type_to_subtitle (type), devpath,
                    vendor, product, url, category);
                }
              else
                {
                  char *fallback = g_strdup_printf ("Unknown %s", type_to_subtitle (type));
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

/* ---- Controller Event Watch ---- */

static void
calibrate_axis (int fd, int axis, int *out_min, int *out_max)
{
  struct input_absinfo absinfo;
  if (ioctl (fd, EVIOCGABS (axis), &absinfo) >= 0)
    {
      *out_min = absinfo.minimum;
      *out_max = absinfo.maximum;
    }
  else
    {
      *out_min = 0;
      *out_max = 255;
    }
}

static gboolean
on_controller_event (gint fd, GIOCondition condition, gpointer user_data)
{
  (void) condition;
  HardwarePage *pg = user_data;
  struct input_event ev;
  ssize_t n;

  while ((n = read (fd, &ev, sizeof (ev))) == sizeof (ev))
    {
      if (ev.type == EV_KEY && ev.code <= KEY_MAX)
        {
          pg->ctrl_state->buttons[ev.code] = ev.value;
          int idx = -1;
          switch (ev.code)
            {
              /* Face buttons */
              case BTN_A:          idx = IND_A; break;
              case BTN_B:          idx = IND_B; break;
              case BTN_X:          idx = IND_X; break;
              case BTN_Y:          idx = IND_Y; break;
              /* Alternate face button mappings (some controllers use these) */
              case BTN_C:          idx = IND_X; break;
              case BTN_Z:          idx = IND_Y; break;
              case BTN_TRIGGER:    idx = IND_A; break;
              case BTN_THUMB:      idx = IND_B; break;
              case BTN_THUMB2:     idx = IND_X; break;
              case BTN_TOP:        idx = IND_Y; break;
              /* Bumpers / triggers via KEY codes */
              case BTN_TOP2:       idx = IND_L1; break;
              case BTN_PINKIE:     idx = IND_R1; break;
              case BTN_BASE:       idx = IND_L2; break;
              case BTN_BASE2:      idx = IND_R2; break;
              case BTN_BASE3:      idx = IND_SELECT; break;
              case BTN_BASE4:      idx = IND_START; break;
              case BTN_BASE5:      idx = IND_L3; break;
              case BTN_BASE6:      idx = IND_R3; break;
              case BTN_DEAD:       idx = IND_SELECT; break;
              /* D-pad */
              case BTN_DPAD_UP:    idx = IND_DPAD_UP; break;
              case BTN_DPAD_DOWN:  idx = IND_DPAD_DOWN; break;
              case BTN_DPAD_LEFT:  idx = IND_DPAD_LEFT; break;
              case BTN_DPAD_RIGHT: idx = IND_DPAD_RIGHT; break;
              /* Standard button names */
              case BTN_START:      idx = IND_START; break;
              case BTN_SELECT:     idx = IND_SELECT; break;
              case BTN_TL:         idx = IND_L1; break;
              case BTN_TR:         idx = IND_R1; break;
              case BTN_TL2:        idx = IND_L2; break;
              case BTN_TR2:        idx = IND_R2; break;
              /* Thumbstick clicks */
              case BTN_THUMBL:
                {
                  pg->ctrl_state->thumb_l_pressed = ev.value;
                  if (pg->analog_l_canvas)
                    gtk_widget_queue_draw (pg->analog_l_canvas);
                  idx = IND_L3;
                  break;
                }
              case BTN_THUMBR:
                {
                  pg->ctrl_state->thumb_r_pressed = ev.value;
                  if (pg->analog_r_canvas)
                    gtk_widget_queue_draw (pg->analog_r_canvas);
                  idx = IND_R3;
                  break;
                }
              case BTN_MODE:       idx = IND_GUIDE; break;
              /* Grip/paddle buttons (mapped to available indicators) */
              case BTN_GRIPL:      idx = IND_L1; break;
              case BTN_GRIPR:      idx = IND_R1; break;
              case BTN_GRIPL2:     idx = IND_L2; break;
              case BTN_GRIPR2:     idx = IND_R2; break;
              /* Extra / macro buttons (M1–M4 on many controllers) */
              case BTN_TRIGGER_HAPPY1: idx = IND_M1; break;
              case BTN_TRIGGER_HAPPY2: idx = IND_M2; break;
              case BTN_TRIGGER_HAPPY3: idx = IND_M3; break;
              case BTN_TRIGGER_HAPPY4: idx = IND_M4; break;
              /* D-pad via KEY_* codes (some controllers report these) */
              case KEY_UP:          idx = IND_DPAD_UP; break;
              case KEY_DOWN:        idx = IND_DPAD_DOWN; break;
              case KEY_LEFT:        idx = IND_DPAD_LEFT; break;
              case KEY_RIGHT:       idx = IND_DPAD_RIGHT; break;
              /* Map unknown BTN_ codes (0x120-0x2ff) to a generic indicator
                 so no input is silently dropped */
              default:
                if (ev.code >= BTN_JOYSTICK && ev.code < KEY_MAX)
                  {
                    /* Cycle through indicators for unmapped buttons */
                    static const int fallback_map[] = {
                      IND_A, IND_B, IND_X, IND_Y,
                      IND_L1, IND_R1, IND_L2, IND_R2,
                      IND_SELECT, IND_START, IND_GUIDE,
                      IND_L3, IND_R3
                    };
                    int map_idx = (ev.code - BTN_JOYSTICK) % G_N_ELEMENTS (fallback_map);
                    idx = fallback_map[map_idx];
                  }
                break;
            }
          if (idx >= 0 && pg->indicators[idx])
            {
              if (ev.value)
                gtk_widget_add_css_class (pg->indicators[idx], "ctrl-indicator-active");
              else
                gtk_widget_remove_css_class (pg->indicators[idx], "ctrl-indicator-active");
            }
        }
      else if (ev.type == EV_ABS)
        {
          switch (ev.code)
            {
              case ABS_X:  pg->ctrl_state->lx = ev.value; break;
              case ABS_Y:  pg->ctrl_state->ly = ev.value; break;
              case ABS_RX: pg->ctrl_state->rx = ev.value; break;
              case ABS_RY: pg->ctrl_state->ry = ev.value; break;
              /* D-pad as HAT axes (many controllers) */
              case ABS_HAT0X:
                {
                  int old_l = pg->ctrl_state->buttons[ABS_HAT0X];
                  int new_l = ev.value == -1;
                  int old_r = old_l == 1;
                  int new_r = ev.value == 1;
                  pg->ctrl_state->buttons[ABS_HAT0X] = ev.value;
                  if (old_l != new_l)
                    {
                      if (new_l) gtk_widget_add_css_class (pg->indicators[IND_DPAD_LEFT], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_LEFT], "ctrl-indicator-active");
                    }
                  if (old_r != new_r)
                    {
                      if (new_r) gtk_widget_add_css_class (pg->indicators[IND_DPAD_RIGHT], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_RIGHT], "ctrl-indicator-active");
                    }
                  break;
                }
              case ABS_HAT0Y:
                {
                  int old_u = pg->ctrl_state->buttons[ABS_HAT0Y];
                  int new_u = ev.value == -1;
                  int old_d = old_u == 1;
                  int new_d = ev.value == 1;
                  pg->ctrl_state->buttons[ABS_HAT0Y] = ev.value;
                  if (old_u != new_u)
                    {
                      if (new_u) gtk_widget_add_css_class (pg->indicators[IND_DPAD_UP], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_UP], "ctrl-indicator-active");
                    }
                  if (old_d != new_d)
                    {
                      if (new_d) gtk_widget_add_css_class (pg->indicators[IND_DPAD_DOWN], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_DPAD_DOWN], "ctrl-indicator-active");
                    }
                  break;
                }
              /* Analog triggers: use dynamic threshold based on axis range */
              case ABS_Z:
              case ABS_BRAKE:
                {
                  static int z_min = 0, z_max = 255;
                  static gboolean z_calibrated = FALSE;
                  if (!z_calibrated) calibrate_axis (fd, ABS_Z, &z_min, &z_max);
                  int threshold = z_min + (z_max - z_min) / 10;
                  int pressed = ev.value > threshold;
                  int old = pg->ctrl_state->buttons[ev.code];
                  pg->ctrl_state->buttons[ev.code] = pressed;
                  if (old != pressed)
                    {
                      if (pressed) gtk_widget_add_css_class (pg->indicators[IND_L2], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_L2], "ctrl-indicator-active");
                    }
                  break;
                }
              case ABS_RZ:
              case ABS_GAS:
                {
                  static int rz_min = 0, rz_max = 255;
                  static gboolean rz_calibrated = FALSE;
                  if (!rz_calibrated) calibrate_axis (fd, ABS_RZ, &rz_min, &rz_max);
                  int threshold = rz_min + (rz_max - rz_min) / 10;
                  int pressed = ev.value > threshold;
                  int old = pg->ctrl_state->buttons[ev.code];
                  pg->ctrl_state->buttons[ev.code] = pressed;
                  if (old != pressed)
                    {
                      if (pressed) gtk_widget_add_css_class (pg->indicators[IND_R2], "ctrl-indicator-active");
                      else gtk_widget_remove_css_class (pg->indicators[IND_R2], "ctrl-indicator-active");
                    }
                  break;
                }
            }
          char stick[64];
          g_snprintf (stick, sizeof (stick), "LX: %-4d  LY: %-4d",
            pg->ctrl_state->lx, pg->ctrl_state->ly);
          gtk_label_set_text (GTK_LABEL (pg->axis_lx), stick);
          g_snprintf (stick, sizeof (stick), "RX: %-4d  RY: %-4d",
            pg->ctrl_state->rx, pg->ctrl_state->ry);
          gtk_label_set_text (GTK_LABEL (pg->axis_rx), stick);
          if (pg->analog_l_canvas)
            gtk_widget_queue_draw (pg->analog_l_canvas);
          if (pg->analog_r_canvas)
            gtk_widget_queue_draw (pg->analog_r_canvas);
        }
    }

  /* Device disconnected or error: clean up and stop watching */
  if (n == 0 || (n < 0 && (errno == ENODEV || errno == ENXIO || errno == EIO)))
    {
      close_controller (pg);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/* ---- Controller UI ---- */

#define BASE_IND_SZ 40

static const char *indicator_labels_xbox[IND_COUNT] = {
  [IND_A]        = "A",
  [IND_B]        = "B",
  [IND_X]        = "X",
  [IND_Y]        = "Y",
  [IND_DPAD_UP]    = "\xe2\x86\x91",      /* ↑ */
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",      /* ↓ */
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",      /* ← */
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",      /* → */
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "LB",
  [IND_R1]        = "RB",
  [IND_L2]        = "LT",
  [IND_R2]        = "RT",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "LS",
  [IND_R3]        = "RS",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char *indicator_labels_ps[IND_COUNT] = {
  [IND_A]        = "\xc3\x97",            /* × */
  [IND_B]        = "\xe2\x97\x8b",        /* ○ */
  [IND_X]        = "\xe2\x96\xa1",        /* □ */
  [IND_Y]        = "\xe2\x96\xb3",        /* △ */
  [IND_DPAD_UP]    = "\xe2\x86\x91",      /* ↑ */
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",      /* ↓ */
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",      /* ← */
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",      /* → */
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "L1",
  [IND_R1]        = "R1",
  [IND_L2]        = "L2",
  [IND_R2]        = "R2",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "L3",
  [IND_R3]        = "R3",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char *indicator_labels_generic[IND_COUNT] = {
  [IND_A]        = "B1",
  [IND_B]        = "B2",
  [IND_X]        = "B3",
  [IND_Y]        = "B4",
  [IND_DPAD_UP]    = "\xe2\x86\x91",
  [IND_DPAD_DOWN]  = "\xe2\x86\x93",
  [IND_DPAD_LEFT]  = "\xe2\x86\x90",
  [IND_DPAD_RIGHT] = "\xe2\x86\x92",
  [IND_START]      = "St",
  [IND_SELECT]     = "Se",
  [IND_L1]        = "L1",
  [IND_R1]        = "R1",
  [IND_L2]        = "L2",
  [IND_R2]        = "R2",
  [IND_GUIDE]     = "Ho",
  [IND_L3]        = "L3",
  [IND_R3]        = "R3",
  [IND_M1]        = "M1",
  [IND_M2]        = "M2",
  [IND_M3]        = "M3",
  [IND_M4]        = "M4",
};

static const char **indicator_labels = indicator_labels_xbox;

static GtkWidget *
make_indicator (HardwarePage *page, int idx)
{
  GtkWidget *dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (dot, BASE_IND_SZ, BASE_IND_SZ);
  gtk_widget_add_css_class (dot, "ctrl-indicator");
  gtk_widget_set_halign (dot, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);

  if (indicator_labels[idx])
    {
      GtkWidget *lbl = gtk_label_new (indicator_labels[idx]);
      gtk_widget_add_css_class (lbl, "ctrl-label");
      gtk_widget_set_halign (lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (lbl, TRUE);
      gtk_box_append (GTK_BOX (dot), lbl);
    }

  page->indicators[idx] = dot;
  return dot;
}

static void
update_indicator_labels (HardwarePage *page, const char **labels)
{
  indicator_labels = labels;
  for (int i = 0; i < IND_COUNT; i++)
    {
      GtkWidget *ind = page->indicators[i];
      if (!ind || !labels[i]) continue;
      GtkWidget *child = gtk_widget_get_first_child (ind);
      if (child && GTK_IS_LABEL (child))
        gtk_label_set_text (GTK_LABEL (child), labels[i]);
    }
}

enum { STICK_LEFT, STICK_RIGHT };

static void
on_stick_draw (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data)
{
  (void) area;
  HardwarePage *page = user_data;
  if (!page->ctrl_state) return;

  int stick = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (area), "stick-id"));
  int sx = (stick == STICK_RIGHT) ? page->ctrl_state->rx : page->ctrl_state->lx;
  int sy = (stick == STICK_RIGHT) ? page->ctrl_state->ry : page->ctrl_state->ly;
  gboolean thumb_pressed = (stick == STICK_RIGHT)
    ? page->ctrl_state->thumb_r_pressed
    : page->ctrl_state->thumb_l_pressed;

  double cx = w / 2.0;
  double cy = h / 2.0;
  double radius = MIN (cx, cy) - 4;

  /* Background — accent color when thumbstick pressed */
  if (thumb_pressed)
    cairo_set_source_rgba (cr, 0.3, 0.6, 1.0, 0.35);
  else
    cairo_set_source_rgba (cr, 0.2, 0.2, 0.2, 0.6);
  cairo_arc (cr, cx, cy, radius, 0, 2 * G_PI);
  cairo_fill (cr);

  /* Border */
  if (thumb_pressed)
    cairo_set_source_rgba (cr, 0.3, 0.6, 1.0, 0.8);
  else
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.8);
  cairo_set_line_width (cr, 1.5);
  cairo_arc (cr, cx, cy, radius, 0, 2 * G_PI);
  cairo_stroke (cr);

  /* Crosshair */
  cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, 0.4);
  cairo_set_line_width (cr, 0.5);
  cairo_move_to (cr, cx - radius, cy);
  cairo_line_to (cr, cx + radius, cy);
  cairo_move_to (cr, cx, cy - radius);
  cairo_line_to (cr, cx, cy + radius);
  cairo_stroke (cr);

  /* Stick position dot */
  double nx = sx / 32767.0;
  double ny = sy / 32767.0;
  double dx = cx + nx * (radius - 6);
  double dy = cy + ny * (radius - 6);

  cairo_set_source_rgba (cr, 0.3, 0.7, 1.0, 0.9);
  cairo_arc (cr, dx, dy, 6, 0, 2 * G_PI);
  cairo_fill (cr);
}

static GtkWidget *
build_controller_test_layout_standard (HardwarePage *page)
{
  GtkWidget *test_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (test_box, 16);
  gtk_widget_set_margin_end (test_box, 16);
  gtk_widget_set_margin_top (test_box, 8);
  gtk_widget_set_margin_bottom (test_box, 8);

  /* Top row: LT  L1  ─  R1  RT */
  GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign (top_row, GTK_ALIGN_CENTER);

  page->indicators[IND_L2] = make_indicator (page, IND_L2);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L2]);

  page->indicators[IND_L1] = make_indicator (page, IND_L1);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L1]);

  GtkWidget *spacer1 = gtk_label_new ("      ");
  gtk_box_append (GTK_BOX (top_row), spacer1);

  page->indicators[IND_R1] = make_indicator (page, IND_R1);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R1]);

  page->indicators[IND_R2] = make_indicator (page, IND_R2);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R2]);

  gtk_box_append (GTK_BOX (test_box), top_row);

  /* Horizontal band: D-Pad  ─  Select/Home/Start  ─  Face buttons */
  GtkWidget *mid_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 60);
  gtk_widget_set_halign (mid_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (mid_row, 8);

  /* D-Pad 3×3 grid */
  GtkWidget *dpad_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dpad_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (dpad_grid), 2);

  page->indicators[IND_DPAD_UP] = make_indicator (page, IND_DPAD_UP);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_UP], 1, 0, 1, 1);

  page->indicators[IND_DPAD_LEFT] = make_indicator (page, IND_DPAD_LEFT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_LEFT], 0, 1, 1, 1);

  page->indicators[IND_DPAD_RIGHT] = make_indicator (page, IND_DPAD_RIGHT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_RIGHT], 2, 1, 1, 1);

  page->indicators[IND_DPAD_DOWN] = make_indicator (page, IND_DPAD_DOWN);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_DOWN], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (mid_row), dpad_grid);

  /* Center row: Select  Home  Start */
  GtkWidget *center_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign (center_row, GTK_ALIGN_CENTER);

  page->indicators[IND_SELECT] = make_indicator (page, IND_SELECT);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_SELECT]);

  page->indicators[IND_GUIDE] = make_indicator (page, IND_GUIDE);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_GUIDE]);

  page->indicators[IND_START] = make_indicator (page, IND_START);
  gtk_box_append (GTK_BOX (center_row), page->indicators[IND_START]);

  gtk_box_append (GTK_BOX (mid_row), center_row);

  /* Face buttons 3×3 grid */
  GtkWidget *face_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (face_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (face_grid), 2);

  page->indicators[IND_Y] = make_indicator (page, IND_Y);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_Y], 1, 0, 1, 1);

  page->indicators[IND_X] = make_indicator (page, IND_X);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_X], 0, 1, 1, 1);

  page->indicators[IND_B] = make_indicator (page, IND_B);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_B], 2, 1, 1, 1);

  page->indicators[IND_A] = make_indicator (page, IND_A);
  gtk_grid_attach (GTK_GRID (face_grid), page->indicators[IND_A], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (mid_row), face_grid);

  gtk_box_append (GTK_BOX (test_box), mid_row);

  /* Analog sticks row with LS/RS overlay labels */
  GtkWidget *sticks_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (sticks_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (sticks_row, 8);

  /* Left stick with "LS" overlay */
  GtkWidget *l_overlay = gtk_overlay_new ();
  page->analog_l_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_l_canvas, 100, 100);
  g_object_set_data (G_OBJECT (page->analog_l_canvas), "stick-id",
    GINT_TO_POINTER (STICK_LEFT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_l_canvas),
    on_stick_draw, page, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (l_overlay), page->analog_l_canvas);
  GtkWidget *ls_label = gtk_label_new ("LS");
  gtk_widget_set_halign (ls_label, GTK_ALIGN_START);
  gtk_widget_set_valign (ls_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (ls_label, 2);
  gtk_widget_set_margin_top (ls_label, 2);
  gtk_overlay_add_overlay (GTK_OVERLAY (l_overlay), ls_label);
  gtk_box_append (GTK_BOX (sticks_row), l_overlay);

  /* Right stick with "RS" overlay */
  GtkWidget *r_overlay = gtk_overlay_new ();
  page->analog_r_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_r_canvas, 100, 100);
  g_object_set_data (G_OBJECT (page->analog_r_canvas), "stick-id",
    GINT_TO_POINTER (STICK_RIGHT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_r_canvas),
    on_stick_draw, page, NULL);
  gtk_overlay_set_child (GTK_OVERLAY (r_overlay), page->analog_r_canvas);
  GtkWidget *rs_label = gtk_label_new ("RS");
  gtk_widget_set_halign (rs_label, GTK_ALIGN_END);
  gtk_widget_set_valign (rs_label, GTK_ALIGN_START);
  gtk_widget_set_margin_end (rs_label, 2);
  gtk_widget_set_margin_top (rs_label, 2);
  gtk_overlay_add_overlay (GTK_OVERLAY (r_overlay), rs_label);
  gtk_box_append (GTK_BOX (sticks_row), r_overlay);

  gtk_box_append (GTK_BOX (test_box), sticks_row);

  /* Extra buttons (M1–M4) under the sticks */
  GtkWidget *extra_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 20);
  gtk_widget_set_halign (extra_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (extra_row, 2);

  page->indicators[IND_M1] = make_indicator (page, IND_M1);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M1]);
  page->indicators[IND_M2] = make_indicator (page, IND_M2);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M2]);
  page->indicators[IND_M3] = make_indicator (page, IND_M3);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M3]);
  page->indicators[IND_M4] = make_indicator (page, IND_M4);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M4]);

  gtk_box_append (GTK_BOX (test_box), extra_row);

  /* Stick values */
  GtkWidget *vals_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (vals_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (vals_row, 4);
  page->axis_lx = gtk_label_new ("LX: 0  LY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_lx);
  page->axis_rx = gtk_label_new ("RX: 0  RY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_rx);
  gtk_box_append (GTK_BOX (test_box), vals_row);

  /* Hint */
  GtkWidget *hint = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Press any button on the controller"));
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (test_box), hint);

  return test_box;
}

static GtkWidget *
build_controller_test_layout_arcade (HardwarePage *page)
{
  GtkWidget *test_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (test_box, 16);
  gtk_widget_set_margin_end (test_box, 16);
  gtk_widget_set_margin_top (test_box, 8);
  gtk_widget_set_margin_bottom (test_box, 8);

  /* Top row: share, home, options, L3, R3 */
  GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (top_row, GTK_ALIGN_CENTER);

  page->indicators[IND_SELECT] = make_indicator (page, IND_SELECT);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_SELECT]);

  page->indicators[IND_GUIDE] = make_indicator (page, IND_GUIDE);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_GUIDE]);

  page->indicators[IND_START] = make_indicator (page, IND_START);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_START]);

  page->indicators[IND_L3] = make_indicator (page, IND_L3);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_L3]);

  page->indicators[IND_R3] = make_indicator (page, IND_R3);
  gtk_box_append (GTK_BOX (top_row), page->indicators[IND_R3]);

  gtk_box_append (GTK_BOX (test_box), top_row);

  /* Middle area: Joystick (left) + 2 rows of 4 buttons (right) */
  GtkWidget *mid_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 40);
  gtk_widget_set_halign (mid_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (mid_row, 8);

  /* Joystick (left side, large) */
  page->analog_l_canvas = gtk_drawing_area_new ();
  gtk_widget_set_size_request (page->analog_l_canvas, 120, 120);
  g_object_set_data (G_OBJECT (page->analog_l_canvas), "stick-id",
    GINT_TO_POINTER (STICK_LEFT));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->analog_l_canvas),
    on_stick_draw, page, NULL);
  gtk_box_append (GTK_BOX (mid_row), page->analog_l_canvas);

  /* Button grid (right side) - 2 rows x 4 cols with first col stagger */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign (btn_box, GTK_ALIGN_CENTER);

  /* Top row: L1, X, Y, R1 (with L1 slightly lower) */
  GtkWidget *row_top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  page->indicators[IND_L1] = make_indicator (page, IND_L1);
  gtk_widget_set_margin_top (page->indicators[IND_L1], 6);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_L1]);
  page->indicators[IND_X] = make_indicator (page, IND_X);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_X]);
  page->indicators[IND_Y] = make_indicator (page, IND_Y);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_Y]);
  page->indicators[IND_R1] = make_indicator (page, IND_R1);
  gtk_box_append (GTK_BOX (row_top), page->indicators[IND_R1]);
  gtk_box_append (GTK_BOX (btn_box), row_top);

  /* Bottom row: L2, A, B, R2 */
  GtkWidget *row_bot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  page->indicators[IND_L2] = make_indicator (page, IND_L2);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_L2]);
  page->indicators[IND_A] = make_indicator (page, IND_A);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_A]);
  page->indicators[IND_B] = make_indicator (page, IND_B);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_B]);
  page->indicators[IND_R2] = make_indicator (page, IND_R2);
  gtk_box_append (GTK_BOX (row_bot), page->indicators[IND_R2]);
  gtk_box_append (GTK_BOX (btn_box), row_bot);

  gtk_box_append (GTK_BOX (mid_row), btn_box);
  gtk_box_append (GTK_BOX (test_box), mid_row);

  /* D-Pad (bottom, smaller and less prominent) */
  GtkWidget *dpad_label = gtk_label_new ("D-Pad");
  gtk_widget_add_css_class (dpad_label, "dim-label");
  gtk_widget_set_halign (dpad_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (test_box), dpad_label);

  GtkWidget *dpad_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (dpad_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (dpad_grid), 2);
  gtk_widget_set_halign (dpad_grid, GTK_ALIGN_START);

  page->indicators[IND_DPAD_UP] = make_indicator (page, IND_DPAD_UP);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_UP], 1, 0, 1, 1);
  page->indicators[IND_DPAD_LEFT] = make_indicator (page, IND_DPAD_LEFT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_LEFT], 0, 1, 1, 1);
  page->indicators[IND_DPAD_RIGHT] = make_indicator (page, IND_DPAD_RIGHT);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_RIGHT], 2, 1, 1, 1);
  page->indicators[IND_DPAD_DOWN] = make_indicator (page, IND_DPAD_DOWN);
  gtk_grid_attach (GTK_GRID (dpad_grid), page->indicators[IND_DPAD_DOWN], 1, 2, 1, 1);

  gtk_box_append (GTK_BOX (test_box), dpad_grid);

  /* Extra buttons (M1–M4) */
  GtkWidget *extra_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (extra_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (extra_row, 4);

  page->indicators[IND_M1] = make_indicator (page, IND_M1);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M1]);
  page->indicators[IND_M2] = make_indicator (page, IND_M2);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M2]);
  page->indicators[IND_M3] = make_indicator (page, IND_M3);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M3]);
  page->indicators[IND_M4] = make_indicator (page, IND_M4);
  gtk_box_append (GTK_BOX (extra_row), page->indicators[IND_M4]);

  gtk_box_append (GTK_BOX (test_box), extra_row);

  /* Stick values */
  GtkWidget *vals_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_widget_set_halign (vals_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (vals_row, 4);
  page->axis_lx = gtk_label_new ("LX: 0  LY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_lx);
  page->axis_rx = gtk_label_new ("RX: 0  RY: 0");
  gtk_box_append (GTK_BOX (vals_row), page->axis_rx);
  gtk_box_append (GTK_BOX (test_box), vals_row);

  /* Hint */
  GtkWidget *hint = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Press any button on the controller"));
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (test_box), hint);

  return test_box;
}

static void on_openrgb_clicked (GtkButton *, gpointer);
static void on_controller_website_clicked (GtkButton *btn, gpointer user_data);

static void
build_controller_detail (HardwarePage *page)
{
  page->controller_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (page->controller_box, 16);
  gtk_widget_set_margin_end (page->controller_box, 16);
  gtk_widget_set_margin_top (page->controller_box, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("joystick-arcade-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->controller_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->controller_name_label, "title-1");
  gtk_widget_set_halign (page->controller_name_label, GTK_ALIGN_START);

  page->controller_info_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->controller_info_label, "dim-label");
  gtk_widget_set_halign (page->controller_info_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->controller_name_label);
  gtk_box_append (GTK_BOX (branding_text), page->controller_info_label);

  page->controller_website_btn = gtk_button_new_with_label (
    g_dgettext (GETTEXT_PACKAGE, "Official Website"));
  gtk_widget_set_valign (page->controller_website_btn, GTK_ALIGN_START);
  gtk_widget_set_margin_top (page->controller_website_btn, 4);
  gtk_widget_add_css_class (page->controller_website_btn, "text-button");
  gtk_widget_set_visible (page->controller_website_btn, FALSE);
  g_signal_connect (page->controller_website_btn, "clicked",
    G_CALLBACK (on_controller_website_clicked), page);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (branding_box), page->controller_website_btn);
  gtk_box_append (GTK_BOX (page->controller_box), branding_box);

  /* Stack switcher with two views */
  GtkWidget *stack = adw_view_stack_new ();
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_STACK (stack));
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);

  /* Settings page */
  GtkWidget *settings_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (settings_scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (settings_scroll), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (settings_scroll), TRUE);

  GtkWidget *settings_content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (settings_content, GTK_ALIGN_START);
  gtk_widget_set_margin_top (settings_content, 24);

  /* --- Settings group --- */
  AdwPreferencesGroup *settings_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (settings_group,
    g_dgettext (GETTEXT_PACKAGE, "Settings"));

  GtkWidget *settings_placeholder = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "No settings available yet."));
  gtk_widget_add_css_class (settings_placeholder, "dim-label");
  gtk_widget_set_halign (settings_placeholder, GTK_ALIGN_START);
  gtk_widget_set_hexpand (settings_placeholder, TRUE);
  adw_preferences_group_add (settings_group, settings_placeholder);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (settings_group));

  /* --- Specifications group --- */
  AdwPreferencesGroup *specs_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (specs_group,
    g_dgettext (GETTEXT_PACKAGE, "Specifications"));

  GtkWidget *specs_placeholder = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "No specification data available."));
  gtk_widget_add_css_class (specs_placeholder, "dim-label");
  gtk_widget_set_halign (specs_placeholder, GTK_ALIGN_START);
  gtk_widget_set_hexpand (specs_placeholder, TRUE);
  adw_preferences_group_add (specs_group, specs_placeholder);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (specs_group));

  /* --- Lighting Controls --- */
  AdwPreferencesGroup *lighting_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *lighting_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lighting_title = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Lighting Controls"));
  gtk_widget_add_css_class (lighting_title, "heading");
  gtk_widget_set_halign (lighting_title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (lighting_title, TRUE);
  gtk_box_append (GTK_BOX (lighting_header), lighting_title);

  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        g_dgettext (GETTEXT_PACKAGE, "Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_openrgb_clicked), NULL);
    }

  adw_preferences_group_add (lighting_group, lighting_header);

  GtkWidget *lighting_status = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "The app couldn't find any light source to manage."));
  gtk_widget_set_halign (lighting_status, GTK_ALIGN_START);
  gtk_widget_set_hexpand (lighting_status, TRUE);
  gtk_label_set_wrap (GTK_LABEL (lighting_status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (lighting_status), 60);
  adw_preferences_group_add (lighting_group, lighting_status);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (lighting_group));

  /* --- Extra Buttons group --- */
  page->extra_buttons_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (page->extra_buttons_group,
    g_dgettext (GETTEXT_PACKAGE, "Extra Buttons"));

  page->extra_buttons_none = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "No extra buttons detected. "
      "This controller does not have programmable buttons."));
  gtk_widget_add_css_class (page->extra_buttons_none, "dim-label");
  gtk_widget_set_halign (page->extra_buttons_none, GTK_ALIGN_START);
  gtk_widget_set_hexpand (page->extra_buttons_none, TRUE);
  adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);

  gtk_box_append (GTK_BOX (settings_content), GTK_WIDGET (page->extra_buttons_group));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (settings_scroll), settings_content);
  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack), settings_scroll, "settings",
    g_dgettext (GETTEXT_PACKAGE, "Settings"), "emblem-system-symbolic");

  /* Test page */
  page->controller_test_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (page->controller_test_stack),
    GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

  GtkWidget *test_page_standard = build_controller_test_layout_standard (page);

  /* Snapshot standard layout pointers before arcade overwrites them */
  memcpy (page->indicators_standard, page->indicators, sizeof (page->indicators_standard));
  page->analog_l_canvas_standard = page->analog_l_canvas;
  page->axis_lx_standard = page->axis_lx;
  page->axis_rx_standard = page->axis_rx;

  gtk_stack_add_named (GTK_STACK (page->controller_test_stack),
    test_page_standard, "controller");

  GtkWidget *test_page_arcade = build_controller_test_layout_arcade (page);

  /* Snapshot arcade layout pointers */
  memcpy (page->indicators_arcade, page->indicators, sizeof (page->indicators_arcade));
  page->analog_l_canvas_arcade = page->analog_l_canvas;
  page->axis_lx_arcade = page->axis_lx;
  page->axis_rx_arcade = page->axis_rx;

  /* Restore standard layout pointers as default */
  memcpy (page->indicators, page->indicators_standard, sizeof (page->indicators));
  page->analog_l_canvas = page->analog_l_canvas_standard;
  page->axis_lx = page->axis_lx_standard;
  page->axis_rx = page->axis_rx_standard;

  gtk_stack_add_named (GTK_STACK (page->controller_test_stack),
    test_page_arcade, "arcade");

  gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "controller");

  adw_view_stack_add_titled_with_icon (ADW_VIEW_STACK (stack),
    page->controller_test_stack, "test",
    g_dgettext (GETTEXT_PACKAGE, "Test"), "joystick-symbolic");

  gtk_box_append (GTK_BOX (page->controller_box), switcher);
  gtk_box_append (GTK_BOX (page->controller_box), stack);
  gtk_stack_add_named (page->detail_stack, page->controller_box, "controller");
}

/* ---- GPU Detail View ---- */

static void on_metric_dot_draw (GtkDrawingArea *, cairo_t *, int, int, gpointer);
static void on_monitor_graph_draw (GtkDrawingArea *, cairo_t *, int, int, gpointer);

static const char *
gpu_brand_resource (GpuInfo *gpu)
{
  if (!gpu || !gpu->vendor) return "/io/github/mayrinck/epinephrine/icons/gpu-generic.svg";
  if (strcmp (gpu->vendor, "AMD") == 0)
    return "/io/github/mayrinck/epinephrine/icons/gpu-amd.svg";
  if (strcmp (gpu->vendor, "NVIDIA") == 0)
    return "/io/github/mayrinck/epinephrine/icons/gpu-nvidia.svg";
  if (strcmp (gpu->vendor, "Intel") == 0)
    {
      if (gpu->device_name &&
          (strstr (gpu->device_name, "Arc") || strstr (gpu->device_name, "ARC")))
        return "/io/github/mayrinck/epinephrine/icons/gpu-intel-arc.png";
      if (gpu->device_name &&
          (strstr (gpu->device_name, "Xe") || strstr (gpu->device_name, "XE")))
        return "/io/github/mayrinck/epinephrine/icons/gpu-intel-xe.svg";
      return "/io/github/mayrinck/epinephrine/icons/gpu-intel.png";
    }
  if (strcmp (gpu->vendor, "Qualcomm") == 0)
    return "/io/github/mayrinck/epinephrine/icons/gpu-qualcomm.svg";
  if (strcmp (gpu->vendor, "Innosilicon") == 0)
    return "/io/github/mayrinck/epinephrine/icons/gpu-innosilicon.png";
  return "/io/github/mayrinck/epinephrine/icons/gpu-generic.png";
}

static void
on_perf_level_changed (GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  if (!page->gpu || !page->gpu->sysfs_path) return;
  guint selected = gtk_drop_down_get_selected (dd);
  const char *levels[] = { "auto", "low", "high", "manual" };
  if (selected >= G_N_ELEMENTS (levels)) return;

  write_epinephrine_gpu_config ("perf_level", levels[selected]);

  char *path = g_build_filename (page->gpu->sysfs_path,
    "power_dpm_force_performance_level", NULL);
  FILE *f = fopen (path, "w");
  if (f)
    {
      fprintf (f, "%s", levels[selected]);
      fclose (f);
    }
  else
    {
      /* Show error toast - find parent window */
      GtkWidget *win = gtk_widget_get_ancestor (GTK_WIDGET (dd), GTK_TYPE_WINDOW);
      if (ADW_IS_TOAST_OVERLAY (win))
        {
          AdwToast *toast = adw_toast_new (
            g_dgettext (GETTEXT_PACKAGE, "Permission denied. Run as root."));
          adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (win), toast);
        }
    }
  g_free (path);
}

static void
on_fan_switch_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  if (!page->gpu || !page->gpu->hwmon_path) return;
  gboolean active = gtk_switch_get_active (sw);

  gtk_widget_set_visible (page->fan_scale, active);

  char *path = g_build_filename (page->gpu->hwmon_path, "pwm1_enable", NULL);
  FILE *f = fopen (path, "w");
  if (f)
    {
      fprintf (f, "%d", active ? 1 : 2);
      fclose (f);
    }
  g_free (path);
}

static void
on_fan_scale_changed (GtkRange *range, gpointer user_data)
{
  HardwarePage *page = user_data;
  if (!page->gpu || !page->gpu->hwmon_path) return;
  int val = (int) gtk_range_get_value (range);

  char *path = g_build_filename (page->gpu->hwmon_path, "pwm1", NULL);
  FILE *f = fopen (path, "w");
  if (f)
    {
      fprintf (f, "%d", val);
      fclose (f);
    }
  g_free (path);
}

static void sync_spin_to_scale (GtkSpinButton *, gpointer);

static void
write_epinephrine_gpu_config (const char *key, const char *value)
{
  char *cfg_dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  g_mkdir_with_parents (cfg_dir, 0755);
  char *cfg_path = g_build_filename (cfg_dir, "epinephrine.conf", NULL);
  g_free (cfg_dir);
  GKeyFile *kf = g_key_file_new ();
  g_key_file_load_from_file (kf, cfg_path, G_KEY_FILE_NONE, NULL);
  g_key_file_set_string (kf, "gpu", key, value);
  char *content = g_key_file_to_data (kf, NULL, NULL);
  g_file_set_contents (cfg_path, content, -1, NULL);
  g_free (content);
  g_key_file_free (kf);
  g_free (cfg_path);
}

static void
on_anti_lag_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  (void) page;
  gboolean active = gtk_switch_get_active (sw);
  write_epinephrine_gpu_config ("anti_lag_enabled", active ? "true" : "false");
  const char *home = g_get_home_dir ();
  char *mesa_dir = g_build_filename (home, ".config", "MESA", NULL);
  g_mkdir_with_parents (mesa_dir, 0755);
  char *al_path = g_build_filename (mesa_dir, "anti_lag.conf", NULL);
  g_free (mesa_dir);
  char *al_content = g_strdup_printf ("[anti_lag]\nenabled = %s\n", active ? "true" : "false");
  g_file_set_contents (al_path, al_content, -1, NULL);
  g_free (al_content);
  g_free (al_path);
}

static void
write_chill_config (HardwarePage *page)
{
  gboolean active = gtk_switch_get_active (GTK_SWITCH (page->chill_switch));
  write_epinephrine_gpu_config ("chill_enabled", active ? "true" : "false");
  if (active)
    {
      int min_fps = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->chill_min_spin));
      int max_fps = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->chill_max_spin));
      char buf[16];
      g_snprintf (buf, sizeof (buf), "%d", min_fps);
      write_epinephrine_gpu_config ("chill_min_fps", buf);
      g_snprintf (buf, sizeof (buf), "%d", max_fps);
      write_epinephrine_gpu_config ("chill_max_fps", buf);
    }
}

static void
on_chill_fps_changed (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  HardwarePage *page = user_data;
  if (gtk_switch_get_active (GTK_SWITCH (page->chill_switch)))
    write_chill_config (page);
}

static void
on_chill_switch_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  gboolean active = gtk_switch_get_active (sw);
  gtk_widget_set_visible (page->chill_opts, active);
  write_chill_config (page);
}

static void
sync_scale_to_spin (GtkRange *range, gpointer user_data)
{
  GtkSpinButton *spin = GTK_SPIN_BUTTON (user_data);
  g_signal_handlers_block_by_func (spin, sync_spin_to_scale, range);
  gtk_spin_button_set_value (spin, gtk_range_get_value (range));
  g_signal_handlers_unblock_by_func (spin, sync_spin_to_scale, range);
}

static void
sync_spin_to_scale (GtkSpinButton *spin, gpointer user_data)
{
  GtkRange *range = GTK_RANGE (user_data);
  g_signal_handlers_block_by_func (range, sync_scale_to_spin, spin);
  gtk_range_set_value (range, gtk_spin_button_get_value (spin));
  g_signal_handlers_unblock_by_func (range, sync_scale_to_spin, spin);
}

/* ---- Motherboard detail ---- */

static void
update_lighting_section (GtkLabel *label)
{
  /* Detect ASUS Aura USB Motherboard controller (VID 0B05, PID 18F3) */
  char *result = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "/bin/sh", "-c", "lsusb -d 0b05:18f3 2>/dev/null | head -1", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &result, NULL, NULL, NULL);
  if (result) { g_strstrip (result); if (result[0] == '\0') { g_free (result); result = NULL; } }
  gboolean has_usb_aura = (result != NULL);
  g_free (result);

  if (has_usb_aura)
    gtk_label_set_text (label,
      g_dgettext (GETTEXT_PACKAGE, "ASUS Aura controller detected."));
  else
    gtk_label_set_text (label,
      g_dgettext (GETTEXT_PACKAGE,
        "The app couldn't find any light source to manage."));
}

/* ---- Motherboard Management helpers ---- */

static void on_smbus_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data);

static char *
read_file (const char *path)
{
  gchar *content = NULL;
  if (!g_file_get_contents (path, &content, NULL, NULL))
    return NULL;
  g_strstrip (content);
  if (content[0] == '\0') { g_free (content); return NULL; }
  return content;
}

/* ── Chipset detection ── */

typedef struct {
  guint16 vendor_id;
  const char *name;
} PciVendorEntry;

static const PciVendorEntry pci_vendors[] = {
  { 0x1022, "AMD" },
  { 0x8086, "Intel" },
  { 0x10DE, "NVIDIA" },
  { 0x1002, "ATI/AMD" },
  { 0x1B36, "Red Hat / QEMU" },
  { 0x15AD, "VMware" },
  { 0x1414, "Microsoft" },
  { 0x1AF4, "Red Hat / VirtIO" },
};

static const char *
pci_vendor_name (guint16 vid)
{
  for (size_t i = 0; i < G_N_ELEMENTS (pci_vendors); i++)
    if (pci_vendors[i].vendor_id == vid)
      return pci_vendors[i].name;
  return NULL;
}

static void
detect_chipset (HardwarePage *page)
{
  if (page->mobo_chipset_vendor)
    return;

  GDir *dir = g_dir_open ("/sys/bus/pci/devices", 0, NULL);
  if (!dir)
    return;

  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      char class_path[512];
      g_snprintf (class_path, sizeof (class_path),
        "/sys/bus/pci/devices/%s/class", entry);
      gchar *class = NULL;
      if (!g_file_get_contents (class_path, &class, NULL, NULL))
        continue;
      g_strstrip (class);
      if (g_strcmp0 (class, "0x060100") != 0)
        { g_free (class); continue; }
      g_free (class);

      char vendor_path[512], device_path[512];
      g_snprintf (vendor_path, sizeof (vendor_path),
        "/sys/bus/pci/devices/%s/vendor", entry);
      g_snprintf (device_path, sizeof (device_path),
        "/sys/bus/pci/devices/%s/device", entry);

      char *vendor_str = read_file (vendor_path);
      char *device_str = read_file (device_path);

      if (vendor_str)
        {
          guint16 vid = (guint16) g_ascii_strtoull (vendor_str, NULL, 16);
          const char *vname = pci_vendor_name (vid);
          page->mobo_chipset_vendor = g_strdup (vname ? vname : vendor_str);
          g_free (vendor_str);
        }
      if (device_str)
        {
          page->mobo_chipset_model = g_strdup (device_str);
          g_free (device_str);
        }
      break;
    }
  g_dir_close (dir);
}

static const char smbus_enable_script[] =
  "#!/bin/sh\n"
  "cp /etc/default/grub /etc/default/grub.bak.epi 2>/dev/null || true\n"
  "if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub 2>/dev/null; then\n"
  "  if grep -q 'acpi_enforce_resources=lax' /etc/default/grub 2>/dev/null; then\n"
  "    rm -f /etc/default/grub.bak.epi; exit 0\n"
  "  fi\n"
  "  sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=\"\\(.*\\)\"/GRUB_CMDLINE_LINUX_DEFAULT=\"\\1 acpi_enforce_resources=lax\"/' /etc/default/grub\n"
  "else\n"
  "  echo 'GRUB_CMDLINE_LINUX_DEFAULT=\"quiet splash acpi_enforce_resources=lax\"' >> /etc/default/grub\n"
  "fi\n"
  "r=0\n"
  "if command -v update-grub >/dev/null 2>&1; then update-grub; r=$?\n"
  "elif command -v grub2-mkconfig >/dev/null 2>&1; then grub2-mkconfig -o /boot/grub2/grub.cfg; r=$?\n"
  "elif command -v grub-mkconfig >/dev/null 2>&1; then grub-mkconfig -o /boot/grub/grub.cfg; r=$?\n"
  "else r=1; fi\n"
  "if [ \"$r\" -ne 0 ]; then\n"
  "  cp /etc/default/grub.bak.epi /etc/default/grub 2>/dev/null || true\n"
  "fi\n"
  "rm -f /etc/default/grub.bak.epi\n"
  "exit $r\n";

static const char smbus_disable_script[] =
  "#!/bin/sh\n"
  "cp /etc/default/grub /etc/default/grub.bak.epi 2>/dev/null || true\n"
  "sed -i 's/[[:space:]]*acpi_enforce_resources=lax//g' /etc/default/grub\n"
  "sed -i 's/^acpi_enforce_resources=lax[[:space:]]*//g' /etc/default/grub\n"
  "r=0\n"
  "if command -v update-grub >/dev/null 2>&1; then update-grub; r=$?\n"
  "elif command -v grub2-mkconfig >/dev/null 2>&1; then grub2-mkconfig -o /boot/grub2/grub.cfg; r=$?\n"
  "elif command -v grub-mkconfig >/dev/null 2>&1; then grub-mkconfig -o /boot/grub/grub.cfg; r=$?\n"
  "else r=1; fi\n"
  "if [ \"$r\" -ne 0 ]; then\n"
  "  cp /etc/default/grub.bak.epi /etc/default/grub 2>/dev/null || true\n"
  "fi\n"
  "rm -f /etc/default/grub.bak.epi\n"
  "exit $r\n";

static gboolean
run_smbus_pkexec (gboolean enable, char **out_error)
{
  int fd;
  char tmp[] = "/tmp/epi-smbus-XXXXXX";
  fd = g_mkstemp (tmp);
  if (fd < 0) { *out_error = g_strdup ("Failed to create temp file"); return FALSE; }

  const char *script = enable ? smbus_enable_script : smbus_disable_script;
  gsize len = strlen (script);
  if (write (fd, script, len) != (ssize_t) len)
    {
      close (fd); unlink (tmp);
      *out_error = g_strdup ("Failed to write temp script");
      return FALSE;
    }
  close (fd);

  char *cmd = g_strdup_printf ("bash '%s'", tmp);
  int exit_code = -1;
  GError *err = NULL;
  gboolean ok = epi_privilege_exec_sync (cmd, &exit_code, NULL, &err);

  unlink (tmp);
  g_free (cmd);

  if (ok && exit_code == 0) return TRUE;

  *out_error = err ? g_strdup (err->message)
                   : g_strdup_printf ("Script exited with code %d", exit_code);
  if (err) g_error_free (err);
  return FALSE;
}

static gboolean
grub_has_smbus_flag (void)
{
  char *content = read_file ("/etc/default/grub");
  if (!content) return FALSE;
  gboolean found = (strstr (content, "acpi_enforce_resources=lax") != NULL);
  g_free (content);
  return found;
}

static gboolean
cmdline_has_smbus_flag (void)
{
  char *content = read_file ("/proc/cmdline");
  if (!content) return FALSE;
  gboolean found = (strstr (content, "acpi_enforce_resources=lax") != NULL);
  g_free (content);
  return found;
}

static void
refresh_smbus_status (HardwarePage *page)
{
  gboolean has_grub = g_file_test ("/etc/default/grub", G_FILE_TEST_EXISTS);
  if (!has_grub)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), FALSE);
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
        g_dgettext (GETTEXT_PACKAGE, "GRUB not detected"));
      return;
    }

  if (!g_find_program_in_path ("pkexec"))
    {
      gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), FALSE);
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
        g_dgettext (GETTEXT_PACKAGE, "pkexec not available"));
      return;
    }

  gboolean persistent = grub_has_smbus_flag ();
  gboolean active_now = cmdline_has_smbus_flag ();

  g_signal_handlers_block_by_func (page->mobo_smbus_toggle,
    on_smbus_toggled, page);
  gtk_switch_set_active (GTK_SWITCH (page->mobo_smbus_toggle), persistent);
  g_signal_handlers_unblock_by_func (page->mobo_smbus_toggle,
    on_smbus_toggled, page);
  gtk_widget_set_sensitive (GTK_WIDGET (page->mobo_smbus_toggle), TRUE);

  if (persistent && active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      g_dgettext (GETTEXT_PACKAGE, "Active (current boot)"));
  else if (persistent && !active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      g_dgettext (GETTEXT_PACKAGE, "Active (after reboot)"));
  else if (!persistent && active_now)
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      g_dgettext (GETTEXT_PACKAGE, "Active (temporary, will be lost on reboot)"));
  else
    gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
      g_dgettext (GETTEXT_PACKAGE, "Inactive"));
}

static void
on_smbus_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  gboolean enable = gtk_switch_get_active (GTK_SWITCH (sw));

  gtk_widget_set_sensitive (GTK_WIDGET (sw), FALSE);
  gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc),
    g_dgettext (GETTEXT_PACKAGE, "Applying..."));

  char *error = NULL;
  gboolean ok = run_smbus_pkexec (enable, &error);

  if (!ok)
    {
      g_signal_handlers_block_by_func (sw, on_smbus_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (sw), !enable);
      g_signal_handlers_unblock_by_func (sw, on_smbus_toggled, page);
      char buf[256];
      g_snprintf (buf, sizeof (buf), "%s: %s",
        g_dgettext (GETTEXT_PACKAGE, "Error"), error ? error : "?");
      gtk_label_set_text (GTK_LABEL (page->mobo_smbus_desc), buf);
    }

  g_free (error);
  refresh_smbus_status (page);
}

static void
build_motherboard_detail (HardwarePage *page)
{
  page->mobo_detail_box = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (page->mobo_detail_box),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (page->mobo_detail_box), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (page->mobo_detail_box), TRUE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (content, GTK_ALIGN_START);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("motherboard-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->mobo_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->mobo_name_label, "title-1");
  gtk_widget_set_halign (page->mobo_name_label, GTK_ALIGN_START);

  page->mobo_bios_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->mobo_bios_label, "dim-label");
  gtk_widget_set_halign (page->mobo_bios_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->mobo_name_label);
  gtk_box_append (GTK_BOX (branding_text), page->mobo_bios_label);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (content), branding_box);

  /* ======= General Information ======= */
  AdwPreferencesGroup *gen_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (gen_group,
    g_dgettext (GETTEXT_PACKAGE, "General Information"));

  page->mobo_vendor_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_vendor_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_vendor_label, TRUE);
  AdwActionRow *vendor_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vendor_row), g_dgettext (GETTEXT_PACKAGE, "Vendor"));
  adw_action_row_add_suffix (vendor_row, page->mobo_vendor_label);
  adw_preferences_group_add (gen_group, GTK_WIDGET (vendor_row));

  page->mobo_model_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_model_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_model_label, TRUE);
  AdwActionRow *model_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (model_row), g_dgettext (GETTEXT_PACKAGE, "Model"));
  adw_action_row_add_suffix (model_row, page->mobo_model_label);
  adw_preferences_group_add (gen_group, GTK_WIDGET (model_row));

  page->mobo_revision_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_revision_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_revision_label, TRUE);
  AdwActionRow *revision_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (revision_row), g_dgettext (GETTEXT_PACKAGE, "Revision"));
  adw_action_row_add_suffix (revision_row, page->mobo_revision_label);
  adw_preferences_group_add (gen_group, GTK_WIDGET (revision_row));

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (gen_group));

  /* ======= BIOS Information ======= */
  AdwPreferencesGroup *bios_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (bios_group,
    g_dgettext (GETTEXT_PACKAGE, "BIOS Information"));

  page->mobo_bios_manufacturer_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_bios_manufacturer_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_bios_manufacturer_label, TRUE);
  AdwActionRow *bios_mfg_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_mfg_row), g_dgettext (GETTEXT_PACKAGE, "Manufacturer"));
  adw_action_row_add_suffix (bios_mfg_row, page->mobo_bios_manufacturer_label);
  adw_preferences_group_add (bios_group, GTK_WIDGET (bios_mfg_row));

  page->mobo_bios_detail_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_bios_detail_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_bios_detail_label, TRUE);
  AdwActionRow *bios_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_row), g_dgettext (GETTEXT_PACKAGE, "Version"));
  adw_action_row_add_suffix (bios_row, page->mobo_bios_detail_label);
  adw_preferences_group_add (bios_group, GTK_WIDGET (bios_row));

  page->mobo_bios_date_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_bios_date_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_bios_date_label, TRUE);
  AdwActionRow *bios_date_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bios_date_row), g_dgettext (GETTEXT_PACKAGE, "Date"));
  adw_action_row_add_suffix (bios_date_row, page->mobo_bios_date_label);
  adw_preferences_group_add (bios_group, GTK_WIDGET (bios_date_row));

  page->mobo_bios_romsize_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_bios_romsize_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_bios_romsize_label, TRUE);
  AdwActionRow *romsize_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (romsize_row), g_dgettext (GETTEXT_PACKAGE, "ROM Size"));
  adw_action_row_add_suffix (romsize_row, page->mobo_bios_romsize_label);
  adw_preferences_group_add (bios_group, GTK_WIDGET (romsize_row));

  page->mobo_bios_efi_pk_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_bios_efi_pk_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_bios_efi_pk_label, TRUE);
  gtk_label_set_selectable (GTK_LABEL (page->mobo_bios_efi_pk_label), TRUE);
  AdwActionRow *efi_pk_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (efi_pk_row), g_dgettext (GETTEXT_PACKAGE, "EFI PK"));
  adw_action_row_add_suffix (efi_pk_row, page->mobo_bios_efi_pk_label);
  adw_preferences_group_add (bios_group, GTK_WIDGET (efi_pk_row));

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (bios_group));

  /* ======= Chipset ======= */
  AdwPreferencesGroup *chipset_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (chipset_group,
    g_dgettext (GETTEXT_PACKAGE, "Chipset"));

  page->mobo_chipset_vendor_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_chipset_vendor_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_chipset_vendor_label, TRUE);
  AdwActionRow *cs_vendor_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cs_vendor_row), g_dgettext (GETTEXT_PACKAGE, "Provider"));
  adw_action_row_add_suffix (cs_vendor_row, page->mobo_chipset_vendor_label);
  adw_preferences_group_add (chipset_group, GTK_WIDGET (cs_vendor_row));

  page->mobo_chipset_model_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->mobo_chipset_model_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->mobo_chipset_model_label, TRUE);
  AdwActionRow *cs_model_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cs_model_row), g_dgettext (GETTEXT_PACKAGE, "Model"));
  adw_action_row_add_suffix (cs_model_row, page->mobo_chipset_model_label);
  adw_preferences_group_add (chipset_group, GTK_WIDGET (cs_model_row));

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (chipset_group));

  /* ===== Motherboard Management ===== */
  AdwPreferencesGroup *mgt_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (mgt_group,
    g_dgettext (GETTEXT_PACKAGE, "Motherboard Management"));

  page->mobo_smbus_toggle = gtk_switch_new ();
  page->mobo_smbus_desc = gtk_label_new ("");
  gtk_widget_set_halign (page->mobo_smbus_desc, GTK_ALIGN_END);
  AdwActionRow *smbus_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (smbus_row), g_dgettext (GETTEXT_PACKAGE, "Unlock SMBus"));
  adw_action_row_set_subtitle (smbus_row,
    g_dgettext (GETTEXT_PACKAGE,
      "Unlocks the SMBus controller for direct memory access by software like OpenRGB."));
  gtk_widget_set_valign (page->mobo_smbus_toggle, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (smbus_row, page->mobo_smbus_desc);
  adw_action_row_add_suffix (smbus_row, page->mobo_smbus_toggle);
  adw_preferences_group_add (mgt_group, GTK_WIDGET (smbus_row));

  g_signal_connect (page->mobo_smbus_toggle, "notify::active",
    G_CALLBACK (on_smbus_toggled), page);

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (mgt_group));

  /* ====== Lighting Controls ====== */
  AdwPreferencesGroup *lighting_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  GtkWidget *mobo_lighting_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (mobo_lighting_header, 8);
  gtk_widget_set_margin_end (mobo_lighting_header, 8);
  gtk_widget_set_margin_top (mobo_lighting_header, 4);
  GtkWidget *mobo_lighting_title = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Lighting Controls"));
  gtk_widget_add_css_class (mobo_lighting_title, "heading");
  gtk_widget_set_halign (mobo_lighting_title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (mobo_lighting_title, TRUE);
  gtk_box_append (GTK_BOX (mobo_lighting_header), mobo_lighting_title);

  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        g_dgettext (GETTEXT_PACKAGE, "Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (mobo_lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_openrgb_clicked), NULL);
    }

  adw_preferences_group_add (lighting_group, mobo_lighting_header);

  page->mobo_lighting_status = gtk_label_new ("");
  gtk_widget_set_halign (page->mobo_lighting_status, GTK_ALIGN_START);
  gtk_widget_set_hexpand (page->mobo_lighting_status, TRUE);
  gtk_label_set_wrap (GTK_LABEL (page->mobo_lighting_status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (page->mobo_lighting_status), 60);
  adw_preferences_group_add (lighting_group, page->mobo_lighting_status);

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (lighting_group));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (page->mobo_detail_box), content);
  gtk_stack_add_named (page->detail_stack, page->mobo_detail_box, "motherboard");
}

/* ---- RAM detail page ---- */

static void
build_rams_detail (HardwarePage *page)
{
  page->ram_detail_box = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (page->ram_detail_box),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (
    GTK_SCROLLED_WINDOW (page->ram_detail_box), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (
    GTK_SCROLLED_WINDOW (page->ram_detail_box), TRUE);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (content, GTK_ALIGN_START);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);

  /* Branding */
  GtkWidget *branding_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);

  GtkWidget *brand_icon = gtk_image_new_from_icon_name ("memory-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (brand_icon), 64);
  gtk_widget_set_valign (brand_icon, GTK_ALIGN_START);

  page->ram_name_label = gtk_label_new ("");
  gtk_widget_add_css_class (page->ram_name_label, "title-1");
  gtk_widget_set_halign (page->ram_name_label, GTK_ALIGN_START);

  GtkWidget *branding_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (branding_text, TRUE);
  gtk_box_append (GTK_BOX (branding_text), page->ram_name_label);

  gtk_box_append (GTK_BOX (branding_box), brand_icon);
  gtk_box_append (GTK_BOX (branding_box), branding_text);
  gtk_box_append (GTK_BOX (content), branding_box);

  /* ====== Memory Usage Group ====== */
  AdwPreferencesGroup *usage_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (usage_group,
    g_dgettext (GETTEXT_PACKAGE, "Memory Usage"));

  page->ram_total_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->ram_total_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->ram_total_label, TRUE);
  AdwActionRow *total_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (total_row), g_dgettext (GETTEXT_PACKAGE, "Total Memory"));
  adw_action_row_add_suffix (total_row, page->ram_total_label);
  adw_preferences_group_add (usage_group, GTK_WIDGET (total_row));

  page->ram_used_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->ram_used_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->ram_used_label, TRUE);
  AdwActionRow *used_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (used_row), g_dgettext (GETTEXT_PACKAGE, "Usage"));
  adw_action_row_add_suffix (used_row, page->ram_used_label);
  adw_preferences_group_add (usage_group, GTK_WIDGET (used_row));

  page->ram_tech_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->ram_tech_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->ram_tech_label, TRUE);
  AdwActionRow *tech_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (tech_row), g_dgettext (GETTEXT_PACKAGE, "Technology"));
  adw_action_row_add_suffix (tech_row, page->ram_tech_label);
  adw_preferences_group_add (usage_group, GTK_WIDGET (tech_row));

  page->ram_freq_label = gtk_label_new ("—");
  gtk_widget_set_halign (page->ram_freq_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand (page->ram_freq_label, TRUE);
  AdwActionRow *freq_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (freq_row), g_dgettext (GETTEXT_PACKAGE, "RAM Frequency"));
  adw_action_row_add_suffix (freq_row, page->ram_freq_label);
  adw_preferences_group_add (usage_group, GTK_WIDGET (freq_row));

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (usage_group));

  /* ====== Lighting Controls Group ====== */
  AdwPreferencesGroup *lighting_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  GtkWidget *ram_lighting_header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (ram_lighting_header, 8);
  gtk_widget_set_margin_end (ram_lighting_header, 8);
  gtk_widget_set_margin_top (ram_lighting_header, 4);
  GtkWidget *ram_lighting_title = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Lighting Controls"));
  gtk_widget_add_css_class (ram_lighting_title, "heading");
  gtk_widget_set_halign (ram_lighting_title, GTK_ALIGN_START);
  gtk_widget_set_hexpand (ram_lighting_title, TRUE);
  gtk_box_append (GTK_BOX (ram_lighting_header), ram_lighting_title);

  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        g_dgettext (GETTEXT_PACKAGE, "Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (ram_lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_openrgb_clicked), NULL);
    }

  adw_preferences_group_add (lighting_group, ram_lighting_header);

  page->ram_lighting_status = gtk_label_new ("");
  gtk_widget_set_halign (page->ram_lighting_status, GTK_ALIGN_START);
  gtk_widget_set_hexpand (page->ram_lighting_status, TRUE);
  gtk_label_set_wrap (GTK_LABEL (page->ram_lighting_status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (page->ram_lighting_status), 60);
  adw_preferences_group_add (lighting_group, page->ram_lighting_status);

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (lighting_group));

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (page->ram_detail_box), content);
  gtk_stack_add_named (page->detail_stack, page->ram_detail_box, "ram");
}

/* ---- OpenRGB ---- */

static void
on_openrgb_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  (void) user_data;
  g_spawn_async (NULL, (char *[]) { "openrgb", NULL }, NULL,
    G_SPAWN_DEFAULT | G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void
on_controller_website_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  if (!page->controller_url) return;
  GError *err = NULL;
  g_app_info_launch_default_for_uri (page->controller_url, NULL, &err);
  if (err)
    {
      g_error_free (err);
      dashboard_show_toast ("Could not open URL");
    }
}

/* ---- GPU detail page ---- */

static void
load_gpu_config (HardwarePage *page)
{
  if (!page->gpu) return;

  char *cfg_dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  char *cfg_path = g_build_filename (cfg_dir, "epinephrine.conf", NULL);
  g_free (cfg_dir);

  GKeyFile *kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, cfg_path, G_KEY_FILE_NONE, NULL))
    {
      g_key_file_free (kf);
      g_free (cfg_path);
      return;
    }

  /* Helper: get string with default */
  #define CFG_STR(key, def) \
    ({ char *v = g_key_file_get_string (kf, "gpu", (key), NULL); \
       v ? v : g_strdup (def); })
  #define CFG_BOOL(key) \
    g_key_file_get_boolean (kf, "gpu", (key), NULL)
  #define CFG_INT(key, def) \
    g_key_file_get_integer (kf, "gpu", (key), NULL)

  /* Performance Mode */
  if (page->perf_level_dropdown)
    {
      char *perf = CFG_STR ("perf_level", "auto");
      const char *levels[] = { "auto", "low", "high", "manual" };
      guint idx = 0;
      for (guint i = 0; i < G_N_ELEMENTS (levels); i++)
        if (g_strcmp0 (perf, levels[i]) == 0) { idx = i; break; }
      g_signal_handlers_block_by_func (page->perf_level_dropdown,
        on_perf_level_changed, page);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->perf_level_dropdown), idx);
      g_signal_handlers_unblock_by_func (page->perf_level_dropdown,
        on_perf_level_changed, page);
      g_free (perf);
    }

  /* Power Profile */
  if (page->power_profile_dd)
    {
      char *prof = CFG_STR ("power_profile", "auto");
      const char *profiles[] = { "auto", "3d_full_screen", "power_saving", "video", "vr", "compute" };
      guint idx = 0;
      for (guint i = 0; i < G_N_ELEMENTS (profiles); i++)
        if (g_strcmp0 (prof, profiles[i]) == 0) { idx = i; break; }
      g_signal_handlers_block_by_func (page->power_profile_dd,
        on_power_profile_changed, page);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->power_profile_dd), idx);
      g_signal_handlers_unblock_by_func (page->power_profile_dd,
        on_power_profile_changed, page);
      g_free (prof);
    }

  /* Power Limit */
  if (page->power_limit_spin)
    {
      int pl = CFG_INT ("power_limit", 0);
      if (pl > 0)
        {
          g_signal_handlers_block_by_func (page->power_limit_spin,
            on_power_limit_changed, page);
          gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->power_limit_spin), pl);
          g_signal_handlers_unblock_by_func (page->power_limit_spin,
            on_power_limit_changed, page);
        }
    }

  /* FRTC */
  if (page->frtc_switch)
    {
      gboolean frtc_on = CFG_BOOL ("frtc_enabled");
      g_signal_handlers_block_by_func (page->frtc_switch,
        on_frtc_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->frtc_switch), frtc_on);
      gtk_widget_set_visible (page->frtc_opts, frtc_on);
      g_signal_handlers_unblock_by_func (page->frtc_switch,
        on_frtc_toggled, page);

      int fps = CFG_INT ("frtc_fps", 60);
      if (fps > 0)
        {
          g_signal_handlers_block_by_func (page->frtc_spin,
            on_frtc_fps_changed, page);
          gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->frtc_spin), fps);
          g_signal_handlers_unblock_by_func (page->frtc_spin,
            on_frtc_fps_changed, page);
        }
    }

  /* Radeon Chill */
  if (page->chill_switch)
    {
      gboolean chill_on = CFG_BOOL ("chill_enabled");
      g_signal_handlers_block_by_func (page->chill_switch,
        on_chill_switch_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->chill_switch), chill_on);
      gtk_widget_set_visible (page->chill_opts, chill_on);
      g_signal_handlers_unblock_by_func (page->chill_switch,
        on_chill_switch_toggled, page);

      int min_fps = CFG_INT ("chill_min_fps", 30);
      int max_fps = CFG_INT ("chill_max_fps", 60);
      g_signal_handlers_block_by_func (page->chill_min_spin,
        on_chill_fps_changed, page);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->chill_min_spin), min_fps);
      g_signal_handlers_unblock_by_func (page->chill_min_spin,
        on_chill_fps_changed, page);
      g_signal_handlers_block_by_func (page->chill_max_spin,
        on_chill_fps_changed, page);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->chill_max_spin), max_fps);
      g_signal_handlers_unblock_by_func (page->chill_max_spin,
        on_chill_fps_changed, page);
    }

  /* Anti-Lag */
  if (page->anti_lag_switch)
    {
      gboolean al_on = CFG_BOOL ("anti_lag_enabled");
      g_signal_handlers_block_by_func (page->anti_lag_switch,
        on_anti_lag_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->anti_lag_switch), al_on);
      g_signal_handlers_unblock_by_func (page->anti_lag_switch,
        on_anti_lag_toggled, page);
    }

  /* VRS */
  if (page->vrs_switch)
    {
      gboolean vrs_on = CFG_BOOL ("vrs_enabled");
      g_signal_handlers_block_by_func (page->vrs_switch,
        on_vrs_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->vrs_switch), vrs_on);
      gtk_widget_set_visible (page->vrs_opts, vrs_on);
      g_signal_handlers_unblock_by_func (page->vrs_switch,
        on_vrs_toggled, page);

      char *rate = CFG_STR ("vrs_rate", "2x2");
      guint rate_idx = (g_strcmp0 (rate, "4x4") == 0) ? 1 : 0;
      g_signal_handlers_block_by_func (page->vrs_dd,
        on_vrs_toggled, page);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (page->vrs_dd), rate_idx);
      g_signal_handlers_unblock_by_func (page->vrs_dd,
        on_vrs_toggled, page);
      g_free (rate);
    }

  /* Zero RPM Fan */
  if (page->zero_rpm_switch)
    {
      gboolean zr_on = CFG_BOOL ("zero_rpm_enabled");
      g_signal_handlers_block_by_func (page->zero_rpm_switch,
        on_zero_rpm_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->zero_rpm_switch), zr_on);
      gtk_widget_set_visible (page->zero_rpm_opts, zr_on);
      g_signal_handlers_unblock_by_func (page->zero_rpm_switch,
        on_zero_rpm_toggled, page);

      int zr_temp = CFG_INT ("zero_rpm_temp", 60);
      if (zr_temp > 0)
        {
          g_signal_handlers_block_by_func (page->zero_rpm_temp_spin,
            on_zero_rpm_temp_changed, page);
          gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->zero_rpm_temp_spin), zr_temp);
          g_signal_handlers_unblock_by_func (page->zero_rpm_temp_spin,
            on_zero_rpm_temp_changed, page);
        }
    }

  /* NVIDIA Power Limit */
  if (page->nvidia_power_spin)
    {
      int np = CFG_INT ("nvidia_power_limit", 0);
      if (np > 0)
        {
          g_signal_handlers_block_by_func (page->nvidia_power_spin,
            on_nvidia_power_limit_changed, page);
          gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->nvidia_power_spin), np);
          g_signal_handlers_unblock_by_func (page->nvidia_power_spin,
            on_nvidia_power_limit_changed, page);
        }
    }

  /* NVIDIA Fan */
  if (page->nvidia_fan_switch)
    {
      gboolean nf_on = CFG_BOOL ("nvidia_fan_enabled");
      g_signal_handlers_block_by_func (page->nvidia_fan_switch,
        on_nvidia_fan_toggled, page);
      gtk_switch_set_active (GTK_SWITCH (page->nvidia_fan_switch), nf_on);
      gtk_widget_set_visible (page->nvidia_fan_scale, nf_on);
      g_signal_handlers_unblock_by_func (page->nvidia_fan_switch,
        on_nvidia_fan_toggled, page);

      int nf_speed = CFG_INT ("nvidia_fan_speed", 50);
      g_signal_handlers_block_by_func (page->nvidia_fan_scale,
        on_nvidia_fan_speed_changed, page);
      gtk_range_set_value (GTK_RANGE (page->nvidia_fan_scale), nf_speed);
      g_signal_handlers_unblock_by_func (page->nvidia_fan_scale,
        on_nvidia_fan_speed_changed, page);
    }

  #undef CFG_STR
  #undef CFG_BOOL
  #undef CFG_INT

  g_key_file_free (kf);
  g_free (cfg_path);
}

static void
build_gpu_detail (HardwarePage *page)
{
  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-hardware.ui");

  page->gpu_detail_box = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_detail_page"));
  page->gpu_brand_icon = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_brand_icon"));
  page->gpu_name_label = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_name_label"));
  page->gpu_vram_label = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_vram_label"));
  page->gpu_release_label = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_release_label"));
  page->gpu_specs_btn = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_specs_btn"));
  page->monitor_panel = GTK_WIDGET (gtk_builder_get_object (builder, "monitor_panel"));
  page->monitor_graph = GTK_WIDGET (gtk_builder_get_object (builder, "monitor_graph"));

  /* Notice shown when a live data source (e.g. nvidia-smi) is missing */
  page->monitor_notice = gtk_label_new ("");
  gtk_widget_add_css_class (page->monitor_notice, "dim-label");
  gtk_widget_set_halign (page->monitor_notice, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (page->monitor_notice), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (page->monitor_notice), 60);
  gtk_widget_set_visible (page->monitor_notice, FALSE);
  gtk_box_prepend (GTK_BOX (page->monitor_panel), page->monitor_notice);
  page->gpu_specs_container = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_specs_container"));
  page->gpu_mgt_group = ADW_PREFERENCES_GROUP (gtk_builder_get_object (builder, "gpu_mgt_group"));
  page->gpu_lighting_status = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_lighting_status"));

  page->gpu_vendor_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_vendor_row"));
  page->gpu_swdriver_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_swdriver_row"));
  page->gpu_mesa_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_mesa_row"));
  page->gpu_opengl_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_opengl_row"));
  page->gpu_vk_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_vk_row"));
  page->gpu_vram_specs_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_vram_specs_row"));
  page->gpu_resizable_bar_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_resizable_bar_row"));
  page->gpu_deviceid_row = GTK_WIDGET (gtk_builder_get_object (builder, "gpu_deviceid_row"));

  /* Connect graph drawing function */
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->monitor_graph),
    on_monitor_graph_draw, page, NULL);

  /* Build monitor metric labels */
  GtkWidget *monitor_left = GTK_WIDGET (gtk_builder_get_object (builder, "monitor_left"));

  static const char *metric_names[] = {
    "Temperature", "GPU Usage", "Core Clock", "Memory Clock",
    "VRAM Used", "Power", "Voltage", "Fan Speed"
  };
  for (int i = 0; i < 8; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_widget_set_margin_top (row, 1);
      gtk_widget_set_margin_bottom (row, 1);

      GtkWidget *dot = gtk_drawing_area_new ();
      gtk_widget_set_size_request (dot, 8, 8);
      gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot),
        on_metric_dot_draw, GINT_TO_POINTER (i), NULL);

      GtkWidget *lbl = gtk_label_new (metric_names[i]);
      gtk_widget_set_halign (lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand (lbl, TRUE);
      gtk_widget_add_css_class (lbl, "dim-label");

      GtkWidget *val = gtk_label_new ("—");
      gtk_widget_set_halign (val, GTK_ALIGN_END);

      gtk_box_append (GTK_BOX (row), dot);
      gtk_box_append (GTK_BOX (row), lbl);
      gtk_box_append (GTK_BOX (row), val);
      gtk_box_append (GTK_BOX (monitor_left), row);

      page->monitor_labels[i] = lbl;
      page->monitor_values[i] = val;
      page->monitor_avail[i] = FALSE;
    }

  /* Conditionally add OpenRGB button to lighting header */
  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *lighting_header = GTK_WIDGET (gtk_builder_get_object (builder, "lighting_header"));
      GtkWidget *openrgb_btn = gtk_button_new_with_label (
        g_dgettext (GETTEXT_PACKAGE, "Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked",
        G_CALLBACK (on_openrgb_clicked), NULL);
    }

  g_object_ref_sink (page->gpu_detail_box);
  g_object_unref (builder);

  /* ======= GPU Management Group (rows added programmatically) ======= */

  /* 1 - Performance Level */
  {
    page->perf_level_dropdown = gtk_drop_down_new (
      G_LIST_MODEL (gtk_string_list_new (
        (const char *[]){ "Auto", "Low", "High", "Manual", NULL })), NULL);
    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), g_dgettext (GETTEXT_PACKAGE, "Performance Mode"));
    adw_action_row_set_subtitle (row, g_dgettext (GETTEXT_PACKAGE,
      "Forces the GPU to a specific performance level: auto, low power, high performance, or manual."));
    adw_action_row_add_suffix (row, page->perf_level_dropdown);
    g_object_set_data (G_OBJECT (row), "power-id", "perf-level");
    g_object_set_data (G_OBJECT (row), "power-vendor", "AMD");
    adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (row));
  }

  /* 2 - Power Profile Mode */
  page->power_profile_dd = gtk_drop_down_new (
    G_LIST_MODEL (gtk_string_list_new (
      (const char *[]){ "Auto", "3D_FULL_SCREEN", "POWER_SAVING", "VIDEO", "VR", "COMPUTE", NULL })), NULL);
  AdwActionRow *pp_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pp_row), g_dgettext (GETTEXT_PACKAGE, "Power Profile"));
  adw_action_row_set_subtitle (pp_row, g_dgettext (GETTEXT_PACKAGE,
    "Selects the clock and power switching heuristic for the GPU."));
  adw_action_row_add_suffix (pp_row, page->power_profile_dd);
  g_object_set_data (G_OBJECT (pp_row), "power-id", "power-profile");
  g_object_set_data (G_OBJECT (pp_row), "power-vendor", "AMD");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (pp_row));

  /* 3 - Power Limit */
  {
    page->power_limit_scale = gtk_scale_new_with_range (
      GTK_ORIENTATION_HORIZONTAL, 0, 500, 1);
    gtk_widget_set_hexpand (page->power_limit_scale, TRUE);
    gtk_scale_set_draw_value (GTK_SCALE (page->power_limit_scale), FALSE);
    page->power_limit_spin = gtk_spin_button_new_with_range (0, 500, 1);
    gtk_spin_button_set_snap_to_ticks (GTK_SPIN_BUTTON (page->power_limit_spin), FALSE);
    gtk_widget_set_size_request (page->power_limit_spin, 100, -1);
    GtkWidget *pl_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (pl_hbox), page->power_limit_scale);
    gtk_box_append (GTK_BOX (pl_hbox), page->power_limit_spin);
    AdwActionRow *pl_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pl_row), g_dgettext (GETTEXT_PACKAGE, "Power Limit (max)"));
    adw_action_row_set_subtitle (pl_row, g_dgettext (GETTEXT_PACKAGE,
      "Sets the maximum power consumption of the GPU in watts."));
    adw_action_row_add_suffix (pl_row, pl_hbox);
    g_object_set_data (G_OBJECT (pl_row), "power-id", "power-limit");
    g_object_set_data (G_OBJECT (pl_row), "power-vendor", "AMD");
    adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (pl_row));
  }

  /* 4 - Frame Rate Target Control */
  page->frtc_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->frtc_switch, GTK_ALIGN_CENTER);
  page->frtc_spin = gtk_spin_button_new_with_range (30, 360, 1);
  gtk_widget_set_size_request (page->frtc_spin, 100, -1);
  page->frtc_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 30, 360, 1);
  gtk_widget_set_hexpand (page->frtc_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (page->frtc_scale), FALSE);
  page->frtc_opts = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *frtc_cap_lbl = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "FPS Limit"));
  gtk_box_append (GTK_BOX (page->frtc_opts), frtc_cap_lbl);
  gtk_box_append (GTK_BOX (page->frtc_opts), page->frtc_scale);
  gtk_box_append (GTK_BOX (page->frtc_opts), page->frtc_spin);
  gtk_widget_set_margin_start (page->frtc_opts, 16);
  AdwActionRow *frtc_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (frtc_row), g_dgettext (GETTEXT_PACKAGE, "Frame Rate Target Control"));
  adw_action_row_set_subtitle (frtc_row, g_dgettext (GETTEXT_PACKAGE,
    "Sets a maximum frame rate cap for all 3D applications via MangoHud."));
  adw_action_row_add_suffix (frtc_row, page->frtc_switch);
  g_object_set_data (G_OBJECT (frtc_row), "power-id", "frtc");
  /* frtc: no power-vendor — generic (MangoHud), works with any GPU */
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (frtc_row));

  /* 5 - Radeon Chill */
  page->chill_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->chill_switch, GTK_ALIGN_CENTER);
  page->chill_min_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 30, 300, 1);
  gtk_widget_set_hexpand (page->chill_min_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (page->chill_min_scale), FALSE);
  page->chill_min_spin = gtk_spin_button_new_with_range (30, 300, 1);
  gtk_widget_set_size_request (page->chill_min_spin, 100, -1);
  page->chill_max_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 30, 300, 1);
  gtk_widget_set_hexpand (page->chill_max_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (page->chill_max_scale), FALSE);
  page->chill_max_spin = gtk_spin_button_new_with_range (30, 300, 1);
  gtk_widget_set_size_request (page->chill_max_spin, 100, -1);
  page->chill_opts = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *min_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *min_lbl = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Min FPS"));
  gtk_box_append (GTK_BOX (min_row), min_lbl);
  gtk_box_append (GTK_BOX (min_row), page->chill_min_scale);
  gtk_box_append (GTK_BOX (min_row), page->chill_min_spin);
  gtk_box_append (GTK_BOX (page->chill_opts), min_row);
  GtkWidget *max_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *max_lbl = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Max FPS"));
  gtk_box_append (GTK_BOX (max_row), max_lbl);
  gtk_box_append (GTK_BOX (max_row), page->chill_max_scale);
  gtk_box_append (GTK_BOX (max_row), page->chill_max_spin);
  gtk_box_append (GTK_BOX (page->chill_opts), max_row);
  gtk_widget_set_margin_start (page->chill_opts, 16);
  AdwActionRow *chill_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (chill_row), g_dgettext (GETTEXT_PACKAGE, "Radeon Chill"));
  adw_action_row_set_subtitle (chill_row, g_dgettext (GETTEXT_PACKAGE,
    "Dynamically regulates your game's frame rate based on your movements."));
  adw_action_row_add_suffix (chill_row, page->chill_switch);
  g_object_set_data (G_OBJECT (chill_row), "power-id", "chill");
  g_object_set_data (G_OBJECT (chill_row), "power-vendor", "AMD");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (chill_row));

  /* 6 - Anti-Lag */
  page->anti_lag_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->anti_lag_switch, GTK_ALIGN_CENTER);
  AdwActionRow *al_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (al_row), g_dgettext (GETTEXT_PACKAGE, "Radeon Anti-Lag"));
  adw_action_row_set_subtitle (al_row, g_dgettext (GETTEXT_PACKAGE,
    "Reduces input lag by pacing GPU frames."));
  adw_action_row_add_suffix (al_row, page->anti_lag_switch);
  g_object_set_data (G_OBJECT (al_row), "power-id", "anti-lag");
  g_object_set_data (G_OBJECT (al_row), "power-vendor", "AMD");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (al_row));

  /* 7 - Variable Rate Shading */
  page->vrs_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->vrs_switch, GTK_ALIGN_CENTER);
  GtkStringList *vrs_model = gtk_string_list_new (
    (const char *[]){ "2x2", "4x4", NULL });
  page->vrs_dd = gtk_drop_down_new (G_LIST_MODEL (vrs_model), NULL);
  page->vrs_opts = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *vrs_rate_lbl = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Shading Rate"));
  gtk_box_append (GTK_BOX (page->vrs_opts), vrs_rate_lbl);
  gtk_box_append (GTK_BOX (page->vrs_opts), page->vrs_dd);
  gtk_widget_set_margin_start (page->vrs_opts, 16);
  AdwActionRow *vrs_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vrs_row), g_dgettext (GETTEXT_PACKAGE, "Variable Rate Shading"));
  adw_action_row_set_subtitle (vrs_row, g_dgettext (GETTEXT_PACKAGE,
    "Reduces shading rate to boost performance (RDNA2+ GPU required)."));
  adw_action_row_add_suffix (vrs_row, page->vrs_switch);
  g_object_set_data (G_OBJECT (vrs_row), "power-id", "vrs");
  /* vrs: no power-vendor — cross-vendor (AMD RDNA2+, Intel Arc+) */
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (vrs_row));

  /* 8 - Fan Control */
  page->fan_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->fan_switch, GTK_ALIGN_CENTER);
  page->fan_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
  gtk_widget_set_size_request (page->fan_scale, 200, -1);
  AdwActionRow *fan_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (fan_row), g_dgettext (GETTEXT_PACKAGE, "Fan Control"));
  adw_action_row_set_subtitle (fan_row, g_dgettext (GETTEXT_PACKAGE,
    "Enables manual fan speed control (PWM). Toggle on to set speed."));
  adw_action_row_add_suffix (fan_row, page->fan_switch);
  g_object_set_data (G_OBJECT (fan_row), "power-id", "fan");
  /* fan: no power-vendor — cross-vendor (AMD + Intel) */
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (fan_row));

  /* 9 - Zero RPM Fan Mode */
  page->zero_rpm_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->zero_rpm_switch, GTK_ALIGN_CENTER);
  page->zero_rpm_temp_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 30, 90, 1);
  gtk_widget_set_hexpand (page->zero_rpm_temp_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (page->zero_rpm_temp_scale), FALSE);
  page->zero_rpm_temp_spin = gtk_spin_button_new_with_range (30, 90, 1);
  gtk_widget_set_size_request (page->zero_rpm_temp_spin, 100, -1);
  page->zero_rpm_opts = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *zr_temp_lbl = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Stop Temp"));
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), zr_temp_lbl);
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), page->zero_rpm_temp_scale);
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), page->zero_rpm_temp_spin);
  gtk_widget_set_margin_start (page->zero_rpm_opts, 16);
  AdwActionRow *zr_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (zr_row), g_dgettext (GETTEXT_PACKAGE, "Zero RPM Fan"));
  adw_action_row_set_subtitle (zr_row, g_dgettext (GETTEXT_PACKAGE,
    "Allows the GPU fan to stop completely when below the temperature threshold."));
  adw_action_row_add_suffix (zr_row, page->zero_rpm_switch);
  g_object_set_data (G_OBJECT (zr_row), "power-id", "zero-rpm");
  g_object_set_data (G_OBJECT (zr_row), "power-vendor", "AMD");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (zr_row));

  /* 10 - NVIDIA Power Limit */
  page->nvidia_power_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 50, 500, 1);
  gtk_widget_set_hexpand (page->nvidia_power_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (page->nvidia_power_scale), FALSE);
  page->nvidia_power_spin = gtk_spin_button_new_with_range (50, 500, 1);
  gtk_widget_set_size_request (page->nvidia_power_spin, 100, -1);
  GtkWidget *np_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (np_hbox), page->nvidia_power_scale);
  gtk_box_append (GTK_BOX (np_hbox), page->nvidia_power_spin);
  AdwActionRow *np_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (np_row), g_dgettext (GETTEXT_PACKAGE, "NVIDIA Power Limit"));
  adw_action_row_set_subtitle (np_row, g_dgettext (GETTEXT_PACKAGE,
    "Sets maximum power in watts for the NVIDIA GPU (via nvidia-smi)."));
  adw_action_row_add_suffix (np_row, np_hbox);
  g_object_set_data (G_OBJECT (np_row), "power-id", "nvidia-power");
  g_object_set_data (G_OBJECT (np_row), "power-vendor", "NVIDIA");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (np_row));

  /* 11 - NVIDIA Fan Control */
  page->nvidia_fan_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->nvidia_fan_switch, GTK_ALIGN_CENTER);
  page->nvidia_fan_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_widget_set_size_request (page->nvidia_fan_scale, 200, -1);
  AdwActionRow *nf_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (nf_row), g_dgettext (GETTEXT_PACKAGE, "NVIDIA Fan Speed"));
  adw_action_row_set_subtitle (nf_row, g_dgettext (GETTEXT_PACKAGE,
    "Controls NVIDIA GPU fan speed. Requires CoolBits enabled in nvidia-settings."));
  adw_action_row_add_suffix (nf_row, page->nvidia_fan_switch);
  g_object_set_data (G_OBJECT (nf_row), "power-id", "nvidia-fan");
  g_object_set_data (G_OBJECT (nf_row), "power-vendor", "NVIDIA");
  adw_preferences_group_add (page->gpu_mgt_group, GTK_WIDGET (nf_row));

  /* UI file handles specs_group, specs_container and lighting_group */
}

static void
on_gpu_specs_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  if (!page->gpu) return;

  const char *url = page->gpu->gpu_specs_url;
  if (!url && page->gpu->vendor)
    {
      if (strcmp (page->gpu->vendor, "AMD") == 0)
        url = "https://www.amd.com/en/products/specifications/graphics";
      else if (strcmp (page->gpu->vendor, "NVIDIA") == 0)
        url = "https://www.nvidia.com/en-us/geforce/graphics-cards";
      else if (strcmp (page->gpu->vendor, "Intel") == 0)
        url = "https://www.intel.com/content/www/us/en/products/specifications/graphics.html";
    }

  if (url)
    {
      GError *err = NULL;
      g_app_info_launch_default_for_uri (url, NULL, &err);
      if (err)
        {
          g_warning ("Failed to open URL: %s", err->message);
          g_error_free (err);
        }
    }
}

/* ---- Device Selection ---- */

static void start_monitor (HardwarePage *page);

static void
close_controller (HardwarePage *page)
{
  if (page->controller_watch_id)
    {
      g_source_remove (page->controller_watch_id);
      page->controller_watch_id = 0;
    }
  if (page->controller_fd >= 0)
    {
      close (page->controller_fd);
      page->controller_fd = -1;
    }
  for (int i = 0; i < IND_COUNT; i++)
    if (page->indicators[i])
      gtk_widget_remove_css_class (page->indicators[i], "ctrl-indicator-active");
  g_clear_pointer (&page->ctrl_state, g_free);
}

/* ---- New Feature Callbacks ---- */


static void on_metric_dot_draw (GtkDrawingArea *, cairo_t *, int, int, gpointer);
static void on_monitor_graph_draw (GtkDrawingArea *, cairo_t *, int, int, gpointer);

static const GdkRGBA metric_colors[8] = {
  { 0.902, 0.380, 0.000, 1.0 },   /* Temp (orange) */
  { 0.200, 0.820, 0.478, 1.0 },   /* Usage (green) */
  { 0.208, 0.518, 0.894, 1.0 },   /* Core Clock (blue) */
  { 0.384, 0.627, 0.918, 1.0 },   /* Mem Clock (light blue) */
  { 0.753, 0.380, 0.824, 1.0 },   /* VRAM (purple) */
  { 0.965, 0.827, 0.176, 1.0 },   /* Power (yellow) */
  { 0.604, 0.600, 0.588, 1.0 },   /* Voltage (gray) */
  { 0.929, 0.204, 0.231, 1.0 },   /* Fan (red) */
};

static void
on_metric_dot_draw (GtkDrawingArea *area, cairo_t *cr,
    int width, int height, gpointer user_data)
{
  (void) area;
  int idx = GPOINTER_TO_INT (user_data);
  if (idx < 0 || idx >= 8) return;
  double cx = width / 2.0;
  double cy = height / 2.0;
  double r = MIN (width, height) / 2.0 - 1;
  if (r < 1) r = 1;
  gdk_cairo_set_source_rgba (cr, &metric_colors[idx]);
  cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
  cairo_fill (cr);
}

static void
on_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
    int width, int height, gpointer user_data)
{
  (void) area;
  HardwarePage *page = user_data;
  int margin_l = 4, margin_r = 8, margin_t = 8, margin_b = 4;
  int plot_x = margin_l;
  int plot_y = margin_t;
  int plot_w = width - margin_l - margin_r;
  int plot_h = height - margin_t - margin_b;
  if (plot_w < 20 || plot_h < 20) return;

  if (page->monitor_history_count <= 0) return;

  int n = page->monitor_history_count;
  int max_pts = MIN (n, 120);

  /* Grid lines */
  cairo_set_source_rgba (cr, 1, 1, 1, 0.06);
  cairo_set_line_width (cr, 1);
  for (int i = 1; i < 5; i++)
    {
      double y = plot_y + plot_h * i / 5.0;
      cairo_move_to (cr, plot_x, y);
      cairo_line_to (cr, plot_x + plot_w, y);
      cairo_stroke (cr);
    }

  /* Grid labels */
  cairo_set_source_rgba (cr, 1, 1, 1, 0.2);
  cairo_select_font_face (cr, "Share Tech", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 8);
  for (int i = 0; i <= 5; i++)
    {
      char buf[8];
      g_snprintf (buf, sizeof (buf), "%d%%", 100 - i * 20);
      cairo_text_extents_t te;
      cairo_text_extents (cr, buf, &te);
      double y = plot_y + plot_h * i / 5.0 - te.height / 2;
      cairo_move_to (cr, plot_x + 2, y + te.height);
      cairo_show_text (cr, buf);
    }

  /* Traces with Catmull-Rom → Cubic Bezier easing */
  for (int m = 0; m < 8; m++)
    {
      if (!page->monitor_avail[m]) continue;

      cairo_set_source_rgba (cr,
        metric_colors[m].red, metric_colors[m].green,
        metric_colors[m].blue, 0.85);
      cairo_set_line_width (cr, 1.5);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

      if (max_pts < 2) continue;

      /* Collect points */
      double px[120], py[120];
      for (int i = 0; i < max_pts; i++)
        {
          int idx = (n - max_pts + i) % 120;
          float val = page->monitor_history[m][idx];
          px[i] = plot_x + (i + 0.5) * plot_w / max_pts;
          py[i] = plot_y + plot_h * (1.0 - CLAMP (val, 0, 1));
        }

      /* Monotone cubic interpolation (Fritsch-Carlson) — no overshoot */
      if (max_pts < 2) continue;

      /* Compute secant slopes */
      double delta[120];
      for (int i = 0; i < max_pts - 1; i++)
        delta[i] = (py[i + 1] - py[i]) / (px[i + 1] - px[i]);

      /* Tangents (m_i) — Fritsch-Carlson monotonicity constraints */
      double m[120];
      m[0] = delta[0];
      m[max_pts - 1] = delta[max_pts - 2];
      for (int i = 1; i < max_pts - 1; i++)
        {
          if (delta[i - 1] * delta[i] <= 0)
            m[i] = 0;
          else
            {
              double w1 = 2 * (px[i + 1] - px[i]) + (px[i] - px[i - 1]);
              double w2 = (px[i + 1] - px[i]) + 2 * (px[i] - px[i - 1]);
              m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
            }
        }

      /* Draw each segment subdivided for smoothness */
      int subdivisions = 8;
      cairo_move_to (cr, px[0], py[0]);
      for (int i = 0; i < max_pts - 1; i++)
        {
          double x0 = px[i], y0 = py[i];
          double x1 = px[i + 1], y1 = py[i + 1];
          double m0 = m[i], m1 = m[i + 1];
          double dx = x1 - x0;

          for (int s = 1; s <= subdivisions; s++)
            {
              double t = (double)s / subdivisions;
              double t2 = t * t, t3 = t2 * t;
              double h00 =  2 * t3 - 3 * t2 + 1;
              double h10 =      t3 - 2 * t2 + t;
              double h01 = -2 * t3 + 3 * t2;
              double h11 =      t3 -     t2;
              double x = x0 + t * dx;
              double y = h00 * y0 + h10 * m0 * dx + h01 * y1 + h11 * m1 * dx;
              cairo_line_to (cr, x, y);
            }
        }
      cairo_stroke (cr);
    }
}

static gboolean
monitor_poll_cb (gpointer user_data)
{
  HardwarePage *page = user_data;
  if (!page->gpu) return G_SOURCE_CONTINUE;

  float vals[8] = { 0 };
  gboolean ok[8] = { FALSE };

  /* NVIDIA path: use nvidia-smi */
  if (page->gpu->has_nvidia_smi)
    {
      gboolean nv_ok = nvidia_smi_query (vals, ok,
        page->gpu->nvidia_gpu_index);
      /* nvidia-smi is present but returned no usable data */
      if (!nv_ok && page->monitor_notice)
        {
          gtk_label_set_text (GTK_LABEL (page->monitor_notice),
            g_dgettext (GETTEXT_PACKAGE,
              "nvidia-smi could not provide real-time data for this GPU."));
          gtk_widget_set_visible (page->monitor_notice, TRUE);
        }
    }
  else if (g_strcmp0 (page->gpu->vendor, "NVIDIA") == 0)
    {
      /* The proprietary NVIDIA driver exposes no hwmon/sysfs metrics;
         nvidia-smi is required for live monitoring. */
      if (page->monitor_notice)
        {
          gtk_label_set_text (GTK_LABEL (page->monitor_notice),
            g_dgettext (GETTEXT_PACKAGE,
              "Real-time monitoring requires nvidia-smi, which was not found on "
              "this system. Install the NVIDIA driver tools to enable live GPU "
              "metrics."));
          gtk_widget_set_visible (page->monitor_notice, TRUE);
        }
    }
  else if (g_strcmp0 (page->gpu->kernel_driver, "xe") == 0)
    {
      /* Intel Xe driver path (Arc B580, Battlemage) */
      const char *hwmon = page->gpu->hwmon_path;
      const char *sysfs = page->gpu->sysfs_path;
      const char *tfile = page->gpu->hwmon_temp_file;

      /* 0 - Temperature (millidegrees → °C) */
      if (hwmon && tfile)
        {
          char *p = g_build_filename (hwmon, tfile, NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[0] = raw / 1000.0f; ok[0] = TRUE; }
        }

      /* 1 - GPU Usage (percent) — Xe doesn't have device_busy_percent */
      /* Could use debugfs rps_boost, skip for now */

      /* 2 - Core Clock (MHz) — from tile0/gt0/freq0/cur_freq */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "tile0", "gt0", "freq0", "cur_freq", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[2] = raw; ok[2] = TRUE; }
        }

      /* 3 - Memory Clock — not available in Xe sysfs */

      /* 4 - VRAM Used (bytes → GB) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "mem_info_vram_used", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[4] = raw / (1024.0f * 1024.0f * 1024.0f); ok[4] = TRUE; }
        }

      /* 5 - Power (calculated via energy_input delta) */
      if (hwmon)
        {
          uint64_t now_ns = g_get_monotonic_time () * 1000;
          char *p = NULL;
          /* Try energy1_input (card) then energy2_input (pkg) */
          p = g_build_filename (hwmon, "energy1_input", NULL);
          uint64_t energy = read_sysfs_u64 (p);
          g_free (p);
          if (energy == 0)
            {
              p = g_build_filename (hwmon, "energy2_input", NULL);
              energy = read_sysfs_u64 (p);
              g_free (p);
            }
          if (energy > 0 && page->gpu->power_last_energy > 0)
            {
              uint64_t de = energy - page->gpu->power_last_energy;
              uint64_t dt = now_ns - page->gpu->power_last_time_ns;
              if (dt > 0)
                { vals[5] = (float) de * 1000.0f / dt; ok[5] = TRUE; }
            }
          page->gpu->power_last_energy = energy;
          page->gpu->power_last_time_ns = now_ns;
        }

      /* 6 - Voltage (millivolts) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "in1_input", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[6] = raw / 1000.0f; ok[6] = TRUE; }
        }

      /* 7 - Fan Speed (RPM) — show 0 if sensor exists */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "fan1_input", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (exists)
            { vals[7] = raw; ok[7] = TRUE; }
        }
    }
  else
    {
      /* AMD / other sysfs/hwmon path */
      const char *hwmon = page->gpu->hwmon_path;
      const char *sysfs = page->gpu->sysfs_path;

      /* 0 - Temperature (millidegrees → °C) */
      if (hwmon)
        {
          const char *tf = page->gpu->hwmon_temp_file ? page->gpu->hwmon_temp_file : "temp1_input";
          char *p = g_build_filename (hwmon, tf, NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[0] = raw / 1000.0f; ok[0] = TRUE; }
        }

      /* 1 - GPU Usage (percent) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "device_busy_percent", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0 || exists)
            { vals[1] = raw; ok[1] = TRUE; }
        }

      /* 2 - Core Clock (MHz) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "freq0_input", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[2] = raw / 1000000.0f; ok[2] = TRUE; }
        }

      /* 3 - Memory Clock (MHz) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "freq1_input", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[3] = raw / 1000000.0f; ok[3] = TRUE; }
        }

      /* 4 - VRAM Used (bytes → GB) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "mem_info_vram_used", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[4] = raw / (1024.0f * 1024.0f * 1024.0f); ok[4] = TRUE; }
        }

      /* 5 - Power (microwatts → W) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "power1_average", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[5] = raw / 1000000.0f; ok[5] = TRUE; }
        }

      /* 6 - Voltage (millivolts) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "in0_input", NULL);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[6] = raw / 1000.0f; ok[6] = TRUE; }
        }

      /* 7 - Fan Speed (RPM) — show 0 if sensor exists */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "fan1_input", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = read_sysfs_u64 (p);
          g_free (p);
          if (exists)
            { vals[7] = raw; ok[7] = TRUE; }
        }
    }

  /* Update labels and history */
  static const char *fmt8[] = {
    "%.0f °C",       /* Temp */
    "%.0f %%",       /* Usage */
    "%.0f MHz",      /* Core Clock */
    "%.0f MHz",      /* Mem Clock */
    "%.2f GB",       /* VRAM */
    "%.1f W",        /* Power */
    "%.3f V",        /* Voltage */
    "%.0f RPM",      /* Fan */
  };

  static const char *na[] = {
    "— °C", "— %", "— MHz", "— MHz",
    "— GB", "— W", "— V", "— RPM",
  };

  static const char *nvidia_fmt8[] = {
    "%.0f °C",       /* Temp */
    "%.0f %%",       /* Usage */
    "%.0f MHz",      /* Core Clock */
    "%.0f MHz",      /* Mem Clock */
    "%.2f GB",       /* VRAM */
    "%.1f W",        /* Power */
    "—",             /* Voltage — not available */
    "%.0f %%",       /* Fan (percentage on NVIDIA) */
  };

  /* Update labels every 800ms (every 8th call at 100ms interval) */
  if (page->monitor_history_count % 8 == 0)
    {
      gboolean any_data = FALSE;
      for (int i = 0; i < 8; i++)
        {
          page->monitor_avail[i] = ok[i];
          if (ok[i])
            {
              any_data = TRUE;
              char buf[32];
              const char *f = page->gpu->has_nvidia_smi ? nvidia_fmt8[i] : fmt8[i];
              g_snprintf (buf, sizeof (buf), f, vals[i]);
              gtk_label_set_text (GTK_LABEL (page->monitor_values[i]), buf);
            }
          else
            gtk_label_set_text (GTK_LABEL (page->monitor_values[i]), na[i]);
        }
      if (any_data && page->monitor_notice)
        gtk_widget_set_visible (page->monitor_notice, FALSE);
    }

  /* Store normalized values for graph */
  static const float norm_max[] = {
    100.0f,    /* Temp: 0-100 °C */
    100.0f,    /* Usage: 0-100 % */
    3000.0f,   /* Core Clock: 0-3000 MHz */
    3000.0f,   /* Mem Clock: 0-3000 MHz */
    32.0f,     /* VRAM: 0-32 GB */
    400.0f,    /* Power: 0-400 W */
    1.5f,      /* Voltage: 0-1.5 V */
    100.0f,    /* Fan: 0-100 % (nvidia-smi) */
  };

  int n = page->monitor_history_count;
  for (int i = 0; i < 8; i++)
    page->monitor_history[i][n % 120] = ok[i]
      ? CLAMP (vals[i] / norm_max[i], 0, 1) : 0;
  page->monitor_history_count = n + 1;

  gtk_widget_queue_draw (page->monitor_graph);
  return G_SOURCE_CONTINUE;
}

static void
start_monitor (HardwarePage *page)
{
  page->monitor_history_count = 0;
  memset (page->monitor_history, 0, sizeof (page->monitor_history));
  if (page->monitor_timer_id) return;
  page->monitor_timer_id = g_timeout_add (100, monitor_poll_cb, page);
  monitor_poll_cb (page);
}

static gboolean
on_ram_monitor_tick (gpointer user_data)
{
  HardwarePage *page = user_data;
  FILE *f = fopen ("/proc/meminfo", "r");
  if (!f) return G_SOURCE_CONTINUE;

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

  char total_str[64], used_str[64];
  double total_gb = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
  if (total_gb >= 1.0)
    g_snprintf (total_str, sizeof (total_str), "%.2f GB", total_gb);
  else
    g_snprintf (total_str, sizeof (total_str), "%.0f MB",
      page->ram_total_bytes / (1024.0 * 1024.0));
  gtk_label_set_text (GTK_LABEL (page->ram_total_label), total_str);

  double used_gb = page->ram_used_bytes / (1024.0 * 1024.0 * 1024.0);
  double total_disp = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
  double used_pct = page->ram_total_bytes > 0
    ? (page->ram_used_bytes * 100.0 / page->ram_total_bytes) : 0.0;
  if (used_gb >= 1.0)
    g_snprintf (used_str, sizeof (used_str), "%.2f GB / %.2f GB (%.0f%%)",
      used_gb, total_disp, used_pct);
  else
    g_snprintf (used_str, sizeof (used_str), "%.0f MB / %.2f GB (%.0f%%)",
      page->ram_used_bytes / (1024.0 * 1024.0), total_disp, used_pct);
  gtk_label_set_text (GTK_LABEL (page->ram_used_label), used_str);

  if (page->ram_freq_mhz > 0)
    {
      char freq_str[32];
      g_snprintf (freq_str, sizeof (freq_str), "%ld MHz", (long) page->ram_freq_mhz);
      gtk_label_set_text (GTK_LABEL (page->ram_freq_label), freq_str);
    }
  if (page->ram_technology)
    gtk_label_set_text (GTK_LABEL (page->ram_tech_label), page->ram_technology);

  return G_SOURCE_CONTINUE;
}

static void
on_power_profile_changed (GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  HardwarePage *page = user_data;
  if (!page->gpu || !page->gpu->sysfs_path) return;
  guint idx = gtk_drop_down_get_selected (dd);
  char *profile_names[] = { "auto", "3d_full_screen", "power_saving", "video", "vr", "compute", NULL };
  if (idx < G_N_ELEMENTS (profile_names))
    write_epinephrine_gpu_config ("power_profile", profile_names[idx]);
  char *path = g_build_filename (page->gpu->sysfs_path,
    "pp_power_profile_mode", NULL);
  char *val = g_strdup_printf ("%d", idx);
  char *cmd = g_strdup_printf ("printf '%%s' '%s' > '%s'", val, path);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
  g_free (path);
  g_free (val);
}

static void
on_frtc_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) sw;
  (void) pspec;
  gboolean active = gtk_switch_get_active (GTK_SWITCH (page->frtc_switch));
  gtk_widget_set_visible (page->frtc_opts, active);
  write_epinephrine_gpu_config ("frtc_enabled", active ? "true" : "false");
  const char *home = g_get_home_dir ();
  char *conf_path = g_build_filename (home, ".config", "MangoHud",
    "MangoHud.conf", NULL);
  if (active)
    {
      int fps = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->frtc_spin));
      char fps_buf[16];
      g_snprintf (fps_buf, sizeof (fps_buf), "%d", fps);
      write_epinephrine_gpu_config ("frtc_fps", fps_buf);
      char *content = g_strdup_printf ("fps_limit=%d\n", fps);
      g_file_set_contents (conf_path, content, -1, NULL);
      g_free (content);
    }
  else
    {
      /* Remove the fps_limit line from the config file */
      char *content = NULL;
      if (g_file_get_contents (conf_path, &content, NULL, NULL))
        {
          char **lines = g_strsplit (content, "\n", -1);
          GString *new_content = g_string_new ("");
          for (int i = 0; lines[i]; i++)
            if (!g_str_has_prefix (lines[i], "fps_limit"))
              {
                g_string_append (new_content, lines[i]);
                g_string_append_c (new_content, '\n');
              }
          g_file_set_contents (conf_path, new_content->str, -1, NULL);
          g_string_free (new_content, TRUE);
          g_strfreev (lines);
        }
      g_free (content);
    }
  g_free (conf_path);
}

static void
on_frtc_fps_changed (GtkSpinButton *spin, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) spin;
  if (!gtk_switch_get_active (GTK_SWITCH (page->frtc_switch))) return;
  int fps = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->frtc_spin));
  char fps_buf[16];
  g_snprintf (fps_buf, sizeof (fps_buf), "%d", fps);
  write_epinephrine_gpu_config ("frtc_fps", fps_buf);
  const char *home = g_get_home_dir ();
  char *conf_path = g_build_filename (home, ".config", "MangoHud",
    "MangoHud.conf", NULL);
  char *content = g_strdup_printf ("fps_limit=%d\n", fps);
  g_file_set_contents (conf_path, content, -1, NULL);
  g_free (content);
  g_free (conf_path);
}

static void
on_power_limit_changed (GtkWidget *widget, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) widget;
  if (!page->gpu || !page->gpu->hwmon_path) return;
  double watts = gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->power_limit_spin));
  char buf[16];
  g_snprintf (buf, sizeof (buf), "%.0f", watts);
  write_epinephrine_gpu_config ("power_limit", buf);
  uint64_t uw = (uint64_t)(watts * 1000000.0);
  char *path = g_build_filename (page->gpu->hwmon_path, "power1_cap", NULL);
  if (access (path, F_OK) != 0)
    {
      g_free (path);
      path = g_build_filename (page->gpu->sysfs_path, "power1_cap", NULL);
    }
  char *val = g_strdup_printf ("%lu", (unsigned long) uw);
  char *cmd = g_strdup_printf ("printf '%%s' '%s' > '%s'", val, path);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
  g_free (path);
  g_free (val);
}

static void
on_zero_rpm_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) sw;
  (void) pspec;
  if (!page->gpu || !page->gpu->hwmon_path) return;
  gboolean active = gtk_switch_get_active (GTK_SWITCH (page->zero_rpm_switch));
  gtk_widget_set_visible (page->zero_rpm_opts, active);
  write_epinephrine_gpu_config ("zero_rpm_enabled", active ? "true" : "false");
  char *path = g_build_filename (page->gpu->hwmon_path,
    "fan_zero_rpm_enable", NULL);
  char *val = g_strdup (active ? "1" : "0");
  char *cmd = g_strdup_printf ("printf '%%s' '%s' > '%s'", val, path);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
  g_free (path);
  g_free (val);
}

static void
on_zero_rpm_temp_changed (GtkWidget *widget, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) widget;
  if (!page->gpu || !page->gpu->hwmon_path) return;
  int temp = (int) gtk_spin_button_get_value (
    GTK_SPIN_BUTTON (page->zero_rpm_temp_spin));
  char temp_buf[16];
  g_snprintf (temp_buf, sizeof (temp_buf), "%d", temp);
  write_epinephrine_gpu_config ("zero_rpm_temp", temp_buf);
  char *path = g_build_filename (page->gpu->hwmon_path,
    "fan_zero_rpm_stop_temperature", NULL);
  char *val = g_strdup_printf ("%d", temp);
  char *cmd = g_strdup_printf ("printf '%%s' '%s' > '%s'", val, path);
  epi_privilege_exec_async (cmd);
  g_free (cmd);
  g_free (path);
  g_free (val);
}

static void
on_vrs_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) sw;
  (void) pspec;
  gboolean active = gtk_switch_get_active (GTK_SWITCH (page->vrs_switch));
  gtk_widget_set_visible (page->vrs_opts, active);
  char *cfg_dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  g_mkdir_with_parents (cfg_dir, 0755);
  char *cfg_path = g_build_filename (cfg_dir, "epinephrine.conf", NULL);
  g_free (cfg_dir);
  GKeyFile *kf = g_key_file_new ();
  g_key_file_load_from_file (kf, cfg_path, G_KEY_FILE_NONE, NULL);
  g_key_file_set_boolean (kf, "gpu", "vrs_enabled", active);
  const char *rate = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->vrs_dd)) == 1
    ? "4x4" : "2x2";
  g_key_file_set_string (kf, "gpu", "vrs_rate", rate);
  char *content = g_key_file_to_data (kf, NULL, NULL);
  g_file_set_contents (cfg_path, content, -1, NULL);
  g_free (content);
  g_key_file_free (kf);
  g_free (cfg_path);
}

/* ---- NVIDIA Callbacks ---- */

static void
on_nvidia_power_limit_changed (GtkWidget *widget, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) widget;
  if (!page->gpu || !page->gpu->has_nvidia_smi) return;
  double watts = gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->nvidia_power_spin));
  char buf[16];
  g_snprintf (buf, sizeof (buf), "%.0f", watts);
  write_epinephrine_gpu_config ("nvidia_power_limit", buf);
  char *args = g_strdup_printf ("-pl %d", (int)watts);
  nvidia_smi_manage (args);
  g_free (args);
}

static void
on_nvidia_fan_toggled (GtkSwitch *sw, GParamSpec *pspec, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) sw;
  (void) pspec;
  if (!page->gpu || !page->gpu->has_nvidia_smi) return;
  gboolean active = gtk_switch_get_active (GTK_SWITCH (page->nvidia_fan_switch));
  gtk_widget_set_visible (page->nvidia_fan_scale, active);
  write_epinephrine_gpu_config ("nvidia_fan_enabled", active ? "true" : "false");
  if (active)
    {
      int speed = (int) gtk_range_get_value (GTK_RANGE (page->nvidia_fan_scale));
      char *args = g_strdup_printf ("-g %d -f %d",
        page->gpu->nvidia_gpu_index, speed);
      nvidia_smi_manage (args);
      g_free (args);
    }
  else
    {
      /* Reset to auto fan control */
      char *args = g_strdup_printf ("-g %d -f -1",
        page->gpu->nvidia_gpu_index);
      nvidia_smi_manage (args);
      g_free (args);
    }
}

static void
on_nvidia_fan_speed_changed (GtkRange *range, gpointer user_data)
{
  HardwarePage *page = user_data;
  (void) range;
  if (!page->gpu || !page->gpu->has_nvidia_smi) return;
  if (!gtk_switch_get_active (GTK_SWITCH (page->nvidia_fan_switch))) return;
  int speed = (int) gtk_range_get_value (GTK_RANGE (page->nvidia_fan_scale));
  char speed_buf[16];
  g_snprintf (speed_buf, sizeof (speed_buf), "%d", speed);
  write_epinephrine_gpu_config ("nvidia_fan_speed", speed_buf);
  char *args = g_strdup_printf ("-g %d -f %d",
    page->gpu->nvidia_gpu_index, speed);
  nvidia_smi_manage (args);
  g_free (args);
}

/* ---- Device List Management ---- */

static void
populate_device_list (HardwarePage *page)
{
  for (int i = 0; i < page->n_devices; i++)
    {
      const char *icon_name = NULL;
      const char *type = page->device_types[i];
      if (g_strcmp0 (type, "motherboard") == 0)
        icon_name = "motherboard-symbolic";
      else if (g_strcmp0 (type, "cpu") == 0)
        icon_name = "cpu-symbolic";
      else if (g_strcmp0 (type, "gpu") == 0)
        icon_name = "pci-card-symbolic";
      else if (g_strcmp0 (type, "ram") == 0)
        icon_name = "memory-symbolic";
      else if (g_strcmp0 (type, "keyboard") == 0)
        icon_name = "keyboard-symbolic";
      else if (g_strcmp0 (type, "mouse") == 0)
        icon_name = "mouse-symbolic";
      else if (g_strcmp0 (type, "joystick") == 0)
        icon_name = "joystick-symbolic";
      else if (g_strcmp0 (type, "controller") == 0)
        icon_name = "gamepad-symbolic";
      else if (g_strcmp0 (type, "mic") == 0 || g_strcmp0 (type, "microphone") == 0)
        icon_name = "mic-symbolic";
      else if (g_strcmp0 (type, "headset") == 0 || g_strcmp0 (type, "headphone") == 0)
        icon_name = "headset-symbolic";
      else
        icon_name = "chip-symbolic";

      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_start (row, 8);
      gtk_widget_set_margin_end (row, 8);
      gtk_widget_set_margin_top (row, 4);
      gtk_widget_set_margin_bottom (row, 4);

      if (icon_name)
        {
          GtkWidget *ico = gtk_image_new_from_icon_name (icon_name);
          gtk_image_set_pixel_size (GTK_IMAGE (ico), 24);
          gtk_box_append (GTK_BOX (row), ico);
        }

      GtkWidget *label_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *title = gtk_label_new (page->device_names[i]);
      gtk_widget_set_halign (title, GTK_ALIGN_START);
      gtk_widget_add_css_class (title, "body");
      gtk_box_append (GTK_BOX (label_box), title);

      if (page->device_info[i] && page->device_info[i][0])
        {
          GtkWidget *sub = gtk_label_new (page->device_info[i]);
          gtk_widget_set_halign (sub, GTK_ALIGN_START);
          gtk_widget_add_css_class (sub, "dim-label");
          gtk_box_append (GTK_BOX (label_box), sub);
        }

      gtk_box_append (GTK_BOX (row), label_box);
      gtk_list_box_insert (GTK_LIST_BOX (page->device_list), row, -1);
    }
}

static void
clear_device_list (HardwarePage *page)
{
  close_controller (page);
  gtk_list_box_remove_all (GTK_LIST_BOX (page->device_list));
  for (int i = 0; i < page->n_devices; i++)
    {
      g_free (page->device_names[i]);
      g_free (page->device_types[i]);
      g_free (page->device_info[i]);
      g_free (page->device_paths[i]);
      g_free (page->device_urls[i]);
      g_free (page->device_categories[i]);
    }
  page->n_devices = 0;
  g_free (page->device_names);  page->device_names = NULL;
  g_free (page->device_types);  page->device_types = NULL;
  g_free (page->device_info);   page->device_info = NULL;
  g_free (page->device_paths);  page->device_paths = NULL;
  g_free (page->device_vendor); page->device_vendor = NULL;
  g_free (page->device_product); page->device_product = NULL;
  g_free (page->device_urls);   page->device_urls = NULL;
  g_free (page->device_categories); page->device_categories = NULL;
  g_free (page->controller_url); page->controller_url = NULL;
  g_free (page->mobo_vendor); page->mobo_vendor = NULL;
  g_free (page->mobo_model);  page->mobo_model = NULL;
  g_free (page->mobo_bios_version); page->mobo_bios_version = NULL;
  g_free (page->mobo_bios_vendor); page->mobo_bios_vendor = NULL;
  g_free (page->mobo_revision); page->mobo_revision = NULL;
  g_free (page->mobo_bios_date); page->mobo_bios_date = NULL;
  g_free (page->mobo_bios_romsize); page->mobo_bios_romsize = NULL;
  g_free (page->mobo_bios_efi_pk); page->mobo_bios_efi_pk = NULL;
  g_free (page->mobo_chipset_vendor); page->mobo_chipset_vendor = NULL;
  g_free (page->mobo_chipset_model); page->mobo_chipset_model = NULL;
  page->mobo_detected = FALSE;

  g_strfreev (page->ram_dimm_names); page->ram_dimm_names = NULL;
  page->n_ram_dimms = 0;
  page->ram_total_bytes = 0;
  page->ram_used_bytes = 0;
  if (page->ram_monitor_timer_id > 0)
    {
      g_source_remove (page->ram_monitor_timer_id);
      page->ram_monitor_timer_id = 0;
    }

  if (page->monitor_timer_id > 0)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }

  if (page->gpu)
    {
      g_free (page->gpu->vendor);
      g_free (page->gpu->device_name);
      g_free (page->gpu->kernel_driver);
      g_free (page->gpu->driver_name);
      g_free (page->gpu->driver_info);
      g_free (page->gpu->vulkan_version);
      g_free (page->gpu->sysfs_path);
      g_free (page->gpu->hwmon_path);
      g_free (page->gpu->hwmon_temp_file);
      g_free (page->gpu->gpu_specs);
      g_free (page->gpu->gpu_specs_url);
      g_free (page->gpu->gpu_release_date);
      g_free (page->gpu);
      page->gpu = NULL;
    }
  if (page->power_profile_names)
    {
      g_strfreev (page->power_profile_names);
      page->power_profile_names = NULL;
    }
  page->n_power_profiles = 0;
}

static gboolean
reload_devices_done (gpointer user_data)
{
  HardwarePage *page = user_data;
  detect_devices (page);
  populate_gpu_specs (page);
  populate_device_list (page);
  gtk_widget_set_sensitive (page->reload_btn, TRUE);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (
    GTK_LIST_BOX (page->device_list), 0);
  if (first)
    gtk_list_box_select_row (GTK_LIST_BOX (page->device_list), first);
  else
    gtk_stack_set_visible_child_name (page->detail_stack, "default");
  return G_SOURCE_REMOVE;
}

static void
update_extra_buttons_section (HardwarePage *page)
{
  /* Remove all previous rows from the group (except "none" label which we
     will remove and re-add as needed). We unparent every child first. */
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (page->extra_buttons_group));
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_widget_unparent (child);
      child = next;
    }

  if (page->controller_fd < 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
      return;
    }

  unsigned long key_bits[KEY_MAX / (8 * sizeof (unsigned long)) + 1] = {0};
  if (ioctl (page->controller_fd, EVIOCGBIT (EV_KEY, sizeof (key_bits)), key_bits) < 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
      return;
    }

  static const struct { int code; const char *label; } btns[] = {
    { BTN_TRIGGER_HAPPY1, "M1" },
    { BTN_TRIGGER_HAPPY2, "M2" },
    { BTN_TRIGGER_HAPPY3, "M3" },
    { BTN_TRIGGER_HAPPY4, "M4" },
  };
  int found = 0;

  for (int i = 0; i < 4; i++)
    {
      if (TEST_BIT (btns[i].code, key_bits))
        {
          found++;
          GtkWidget *row = adw_action_row_new ();
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), btns[i].label);
          char subtitle[128];
          g_snprintf (subtitle, sizeof (subtitle), "%s (code 0x%x)",
            g_dgettext (GETTEXT_PACKAGE, "Detected"), btns[i].code);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
          adw_preferences_group_add (page->extra_buttons_group,
            GTK_WIDGET (row));
        }
    }

  if (found == 0)
    {
      adw_preferences_group_add (page->extra_buttons_group, page->extra_buttons_none);
      gtk_widget_set_visible (page->extra_buttons_none, TRUE);
    }
}

static void
on_reload_devices (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  HardwarePage *page = user_data;
  gtk_widget_set_sensitive (page->reload_btn, FALSE);
  gtk_stack_set_visible_child_name (page->detail_stack, "loading");
  clear_device_list (page);
  g_timeout_add (1000, reload_devices_done, page);
}

static void
on_device_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  HardwarePage *page = user_data;
  if (!row) return;
  int idx = gtk_list_box_row_get_index (row);
  if (idx < 0 || idx >= page->n_devices) return;

  close_controller (page);

  /* Stop any active monitors */
  if (page->monitor_timer_id > 0)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  if (page->ram_monitor_timer_id > 0)
    {
      g_source_remove (page->ram_monitor_timer_id);
      page->ram_monitor_timer_id = 0;
    }

  const char *type = page->device_types[idx];
  const char *path = page->device_paths[idx];

  if (g_strcmp0 (type, "motherboard") == 0 && page->mobo_detected
      && page->mobo_vendor && page->mobo_model)
    {
      gtk_stack_set_visible_child_name (page->detail_stack, "motherboard");

      /* Name */
      char *full = g_strdup_printf ("%s %s", page->mobo_vendor, page->mobo_model);
      gtk_label_set_text (GTK_LABEL (page->mobo_name_label), full);
      g_free (full);

      /* BIOS info */
      char *bios_str = g_strdup_printf ("%s %s",
        g_dgettext (GETTEXT_PACKAGE, "BIOS Version:"),
        page->mobo_bios_version);
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_label), bios_str);
      g_free (bios_str);

      /* General Information */
      gtk_label_set_text (GTK_LABEL (page->mobo_vendor_label), page->mobo_vendor ? page->mobo_vendor : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_model_label), page->mobo_model ? page->mobo_model : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_revision_label), page->mobo_revision ? page->mobo_revision : "—");

      /* BIOS Information */
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_manufacturer_label), page->mobo_bios_vendor ? page->mobo_bios_vendor : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_detail_label), page->mobo_bios_version ? page->mobo_bios_version : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_date_label), page->mobo_bios_date ? page->mobo_bios_date : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_romsize_label), page->mobo_bios_romsize ? page->mobo_bios_romsize : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_bios_efi_pk_label), page->mobo_bios_efi_pk ? page->mobo_bios_efi_pk : "—");

      /* Chipset */
      detect_chipset (page);
      gtk_label_set_text (GTK_LABEL (page->mobo_chipset_vendor_label), page->mobo_chipset_vendor ? page->mobo_chipset_vendor : "—");
      gtk_label_set_text (GTK_LABEL (page->mobo_chipset_model_label), page->mobo_chipset_model ? page->mobo_chipset_model : "—");

      /* Lighting Controls */
      update_lighting_section (GTK_LABEL (page->mobo_lighting_status));

      /* Refresh SMBus status */
      refresh_smbus_status (page);
    }
  else if (g_strcmp0 (type, "ram") == 0)
    {
      gtk_stack_set_visible_child_name (page->detail_stack, "ram");

      /* Name */
      gtk_label_set_text (GTK_LABEL (page->ram_name_label),
        page->device_names[idx]);

      /* Total Memory */
      char total_str[64];
      double total_gb = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
      if (total_gb >= 1.0)
        g_snprintf (total_str, sizeof (total_str), "%.2f GB", total_gb);
      else
        g_snprintf (total_str, sizeof (total_str), "%.0f MB",
          page->ram_total_bytes / (1024.0 * 1024.0));
      gtk_label_set_text (GTK_LABEL (page->ram_total_label), total_str);

      /* Usage */
      char used_str[64];
      double used_gb = page->ram_used_bytes / (1024.0 * 1024.0 * 1024.0);
      double total_disp = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
      double used_pct = page->ram_total_bytes > 0
        ? (page->ram_used_bytes * 100.0 / page->ram_total_bytes) : 0.0;
      if (used_gb >= 1.0)
        g_snprintf (used_str, sizeof (used_str), "%.2f GB / %.2f GB (%.0f%%)",
          used_gb, total_disp, used_pct);
      else
        g_snprintf (used_str, sizeof (used_str), "%.0f MB / %.2f GB (%.0f%%)",
          page->ram_used_bytes / (1024.0 * 1024.0), total_disp, used_pct);
      gtk_label_set_text (GTK_LABEL (page->ram_used_label), used_str);

      /* Technology */
      gtk_label_set_text (GTK_LABEL (page->ram_tech_label),
        page->ram_technology ? page->ram_technology : "—");

      /* RAM Frequency */
      if (page->ram_freq_mhz > 0)
        {
          char freq_str[32];
          g_snprintf (freq_str, sizeof (freq_str), "%ld MHz", (long) page->ram_freq_mhz);
          gtk_label_set_text (GTK_LABEL (page->ram_freq_label), freq_str);
        }
      else
        gtk_label_set_text (GTK_LABEL (page->ram_freq_label), "—");

      /* Start RAM usage monitor */
      if (page->ram_monitor_timer_id == 0)
        {
          page->ram_monitor_timer_id = g_timeout_add (1000, on_ram_monitor_tick, page);
          on_ram_monitor_tick (page);
        }
    }
  else if (g_strcmp0 (type, "gpu") == 0 && page->gpu)
    {
      gtk_stack_set_visible_child_name (page->detail_stack, "gpu");

      /* Brand icon */
      const char *brand_res = gpu_brand_resource (page->gpu);
      if (brand_res)
        gtk_image_set_from_resource (GTK_IMAGE (page->gpu_brand_icon), brand_res);
      else
        {
          gtk_image_set_from_icon_name (GTK_IMAGE (page->gpu_brand_icon),
            "video-card-symbolic");
          gtk_image_set_pixel_size (GTK_IMAGE (page->gpu_brand_icon), 64);
        }

      /* Name */
      gtk_label_set_text (GTK_LABEL (page->gpu_name_label),
        page->gpu->device_name ? page->gpu->device_name : "?");

      /* VRAM info */
      if (page->gpu->vram_total > 0)
        {
          char vram_str[64];
          double gb = page->gpu->vram_total / (1024.0 * 1024.0 * 1024.0);
          g_snprintf (vram_str, sizeof (vram_str), "VRAM: %.1f GB", gb);
          gtk_label_set_text (GTK_LABEL (page->gpu_vram_label), vram_str);

          /* If VRAM came from GPU_INDEX.md, show info icon with tooltip */
          GtkWidget *old_info = g_object_get_data (G_OBJECT (page->gpu_vram_label), "index-info-icon");
          if (old_info) gtk_widget_unparent (old_info);

          if (page->gpu->vram_from_index)
            {
              GtkWidget *vram_row_box = gtk_widget_get_parent (page->gpu_vram_label);
              GtkWidget *info_icon = gtk_image_new_from_icon_name ("dialog-information-symbolic");
              gtk_image_set_pixel_size (GTK_IMAGE (info_icon), 14);
              gtk_widget_set_valign (info_icon, GTK_ALIGN_CENTER);
              gtk_widget_set_margin_start (info_icon, 4);
              gtk_widget_add_css_class (info_icon, "dim-label");
              gtk_widget_set_tooltip_text (GTK_WIDGET (info_icon),
                "This information may be inaccurate or outdated");
              gtk_box_append (GTK_BOX (vram_row_box), info_icon);
              g_object_set_data (G_OBJECT (page->gpu_vram_label), "index-info-icon", info_icon);
            }
        }
      else
        {
          gtk_label_set_text (GTK_LABEL (page->gpu_vram_label), "");
          GtkWidget *old_info = g_object_get_data (G_OBJECT (page->gpu_vram_label), "index-info-icon");
          if (old_info) gtk_widget_unparent (old_info);
        }

      /* Release date */
      if (page->gpu->gpu_release_date && page->gpu->gpu_release_date[0]
          && page->gpu->gpu_release_date[0] != '?')
        {
          char date_str[64];
          g_snprintf (date_str, sizeof (date_str),
            g_dgettext (GETTEXT_PACKAGE, "Release: %s"),
            page->gpu->gpu_release_date);
          gtk_label_set_text (GTK_LABEL (page->gpu_release_label), date_str);
        }
      else
        gtk_label_set_text (GTK_LABEL (page->gpu_release_label), "");

      /* Driver row */
      /* Populate CPU-X style spec rows */
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_vendor_row),
          page->gpu->vendor ? page->gpu->vendor : "—");
      }
      {
        char driver_str[128];
        if (g_strcmp0 (page->gpu->kernel_driver, "xe") == 0)
          g_snprintf (driver_str, sizeof (driver_str), "%s",
            g_dgettext (GETTEXT_PACKAGE, "Intel Open Graphics Driver (Xe)"));
        else if (g_strcmp0 (page->gpu->kernel_driver, "amdgpu") == 0)
          g_snprintf (driver_str, sizeof (driver_str), "%s",
            g_dgettext (GETTEXT_PACKAGE, "AMD Open Graphics Driver (amdgpu)"));
        else if (page->gpu->kernel_driver && page->gpu->driver_info)
          g_snprintf (driver_str, sizeof (driver_str), "%s (%s)",
            page->gpu->kernel_driver, page->gpu->driver_info);
        else if (page->gpu->kernel_driver)
          g_snprintf (driver_str, sizeof (driver_str), "%s", page->gpu->kernel_driver);
        else if (page->gpu->driver_info)
          g_snprintf (driver_str, sizeof (driver_str), "%s", page->gpu->driver_info);
        else
          g_snprintf (driver_str, sizeof (driver_str), "—");
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_swdriver_row), driver_str);
      }
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_mesa_row),
          page->gpu->mesa_version ? page->gpu->mesa_version : "—");
      }
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_opengl_row),
          page->gpu->opengl_version ? page->gpu->opengl_version : "—");
      }
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_vk_row),
          page->gpu->vulkan_version ? page->gpu->vulkan_version : "—");
      }
      /* VRAM row (sysfs or GPU_INDEX.md fallback) */
      {
        /* Remove any previous info icon suffix */
        GtkWidget *old_icon = g_object_get_data (G_OBJECT (page->gpu_vram_specs_row),
          "vram-info-icon");
        if (old_icon) gtk_widget_unparent (old_icon);

        if (page->gpu->vram_total > 0)
          {
            char vram_str[64];
            double gb = page->gpu->vram_total / (1024.0 * 1024.0 * 1024.0);
            g_snprintf (vram_str, sizeof (vram_str), "%.1f GB", gb);
            adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_vram_specs_row),
              vram_str);

            /* If VRAM came from GPU_INDEX.md, show info icon */
            if (page->gpu->vram_from_index)
              {
                GtkWidget *info_icon = gtk_image_new_from_icon_name (
                  "dialog-information-symbolic");
                gtk_image_set_pixel_size (GTK_IMAGE (info_icon), 14);
                gtk_widget_set_valign (info_icon, GTK_ALIGN_CENTER);
                gtk_widget_set_margin_start (info_icon, 4);
                gtk_widget_add_css_class (info_icon, "dim-label");
                gtk_widget_set_tooltip_text (GTK_WIDGET (info_icon),
                  g_dgettext (GETTEXT_PACKAGE,
                    "This information may be inaccurate or outdated"));
                adw_action_row_add_suffix (ADW_ACTION_ROW (page->gpu_vram_specs_row),
                  info_icon);
                g_object_set_data (G_OBJECT (page->gpu_vram_specs_row),
                  "vram-info-icon", info_icon);
              }
          }
        else
          {
            adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_vram_specs_row),
              "—");
          }
      }
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_resizable_bar_row),
          page->gpu->resizable_bar ?
            g_dgettext (GETTEXT_PACKAGE, "Yes") :
            g_dgettext (GETTEXT_PACKAGE, "No"));
      }
      {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_deviceid_row),
          page->gpu->device_id ? page->gpu->device_id : "—");
      }

      /* Populate specs from GPU_INDEX.md */
      populate_gpu_specs (page);

      /* Radeon Chill — show/hide chill options */
      if (page->gpu->has_radeon_chill)
        {
          gtk_widget_set_visible (page->chill_opts,
            gtk_switch_get_active (GTK_SWITCH (page->chill_switch)));
        }

      /* Anti-Lag */
      {
        GtkWidget *row = NULL;
        GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (page->gpu_mgt_group));
        while (child)
          {
            if (g_strcmp0 (g_object_get_data (G_OBJECT (child), "power-id"), "anti-lag") == 0)
              { row = child; break; }
            child = gtk_widget_get_next_sibling (child);
          }
        if (row)
          gtk_widget_set_visible (row, page->gpu->has_anti_lag);
      }

      /* Performance level — just update current value, visibility is handled by loop below */
      if (page->gpu->has_perf_level)
        {
          char *perf_path = g_build_filename (page->gpu->sysfs_path,
            "power_dpm_force_performance_level", NULL);
          char cur[16] = {0};
          FILE *f = fopen (perf_path, "r");
          if (f && fgets (cur, sizeof (cur), f))
            {
              g_strstrip (cur);
              const char *levels[] = { "auto", "low", "high", "manual" };
              for (guint i = 0; i < G_N_ELEMENTS (levels); i++)
                {
                  if (strcmp (cur, levels[i]) == 0)
                    {
                      g_signal_handlers_block_by_func (page->perf_level_dropdown,
                        on_perf_level_changed, page);
                      gtk_drop_down_set_selected (
                        GTK_DROP_DOWN (page->perf_level_dropdown), i);
                      g_signal_handlers_unblock_by_func (page->perf_level_dropdown,
                        on_perf_level_changed, page);
                      break;
                    }
                }
              fclose (f);
            }
          else if (f) fclose (f);
          g_free (perf_path);
        }

/* Fan control */
      if (page->gpu->has_fan_control)
        {
          gtk_widget_set_visible (page->fan_scale, FALSE);
          char *pwm_path = g_build_filename (page->gpu->hwmon_path, "pwm1_enable", NULL);
          char cur[4] = {0};
          FILE *f = fopen (pwm_path, "r");
          if (f && fgets (cur, sizeof (cur), f))
            {
              g_strstrip (cur);
              int val = atoi (cur);
              g_signal_handlers_block_by_func (page->fan_switch,
                on_fan_switch_toggled, page);
              gtk_switch_set_active (GTK_SWITCH (page->fan_switch), val == 1);
              g_signal_handlers_unblock_by_func (page->fan_switch,
                on_fan_switch_toggled, page);
              gtk_widget_set_visible (page->fan_scale, val == 1);
              fclose (f);
            }
          else if (f) fclose (f);
          g_free (pwm_path);

          char *fan_val_path = g_build_filename (page->gpu->hwmon_path, "pwm1", NULL);
          f = fopen (fan_val_path, "r");
          if (f)
            {
              char val_str[8] = {0};
              if (fgets (val_str, sizeof (val_str), f))
                {
                  g_signal_handlers_block_by_func (page->fan_scale,
                    on_fan_scale_changed, page);
                  gtk_range_set_value (GTK_RANGE (page->fan_scale), atoi (val_str));
                  g_signal_handlers_unblock_by_func (page->fan_scale,
                    on_fan_scale_changed, page);
                }
              fclose (f);
            }
          g_free (fan_val_path);
        }
      else
        {
          gtk_widget_set_visible (page->fan_switch, FALSE);
          gtk_widget_set_visible (page->fan_scale, FALSE);
        }

      /* Hide/show each management row based on capability */
      {
        GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (page->gpu_mgt_group));
        while (child)
          {
            const char *pid = g_object_get_data (G_OBJECT (child), "power-id");
            if (pid)
              {
                gboolean vis = TRUE;

                /* Vendor-gated: if power-vendor is set, it must match GPU vendor */
                const char *pv = g_object_get_data (G_OBJECT (child), "power-vendor");
                if (pv && page->gpu->vendor)
                  vis = (g_strcmp0 (pv, page->gpu->vendor) == 0);

                if (vis)
                  {
                    if (g_strcmp0 (pid, "perf-level") == 0) vis = page->gpu->has_perf_level;
                    else if (g_strcmp0 (pid, "power-profile") == 0) vis = page->gpu->has_power_profile;
                    else if (g_strcmp0 (pid, "power-limit") == 0) vis = page->gpu->has_power_limit;
                    else if (g_strcmp0 (pid, "frtc") == 0) vis = page->gpu->has_frtc;
                    else if (g_strcmp0 (pid, "chill") == 0) vis = page->gpu->has_radeon_chill;
                    else if (g_strcmp0 (pid, "anti-lag") == 0) vis = page->gpu->has_anti_lag;
                    else if (g_strcmp0 (pid, "vrs") == 0) vis = page->gpu->has_vrs;
                    else if (g_strcmp0 (pid, "fan") == 0) vis = page->gpu->has_fan_control;
                    else if (g_strcmp0 (pid, "zero-rpm") == 0) vis = page->gpu->has_zero_rpm_fan;
                    else if (g_strcmp0 (pid, "nvidia-power") == 0) vis = page->gpu->has_nvidia_smi;
                    else if (g_strcmp0 (pid, "nvidia-fan") == 0) vis = page->gpu->has_nvidia_smi;
                  }
                gtk_widget_set_visible (child, vis);
              }
            child = gtk_widget_get_next_sibling (child);
          }
      }

      start_monitor (page);

      /* Update lighting controls status */
      update_lighting_section (GTK_LABEL (page->gpu_lighting_status));
    }
  else if ((g_strcmp0 (type, "controller") == 0 || g_strcmp0 (type, "joystick") == 0) && path && path[0])
    {
      gtk_stack_set_visible_child_name (page->detail_stack, "controller");

      /* Switch layout based on category */
      const char *cat = page->device_categories[idx];
      if (cat && g_strcmp0 (cat, "Arcade") == 0)
        {
          gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "arcade");
          memcpy (page->indicators, page->indicators_arcade, sizeof (page->indicators));
          page->analog_l_canvas = page->analog_l_canvas_arcade;
          page->axis_lx = page->axis_lx_arcade;
          page->axis_rx = page->axis_rx_arcade;
        }
      else
        {
          gtk_stack_set_visible_child_name (GTK_STACK (page->controller_test_stack), "controller");
          memcpy (page->indicators, page->indicators_standard, sizeof (page->indicators));
          page->analog_l_canvas = page->analog_l_canvas_standard;
          page->axis_lx = page->axis_lx_standard;
          page->axis_rx = page->axis_rx_standard;
        }
      gtk_label_set_text (GTK_LABEL (page->controller_name_label),
        page->device_names[idx]);
      gtk_label_set_text (GTK_LABEL (page->controller_info_label),
        page->device_info[idx] && page->device_info[idx][0]
          ? page->device_info[idx] : "");

      /* Show/hide official website button */
      g_free (page->controller_url);
      page->controller_url = page->device_urls[idx]
        ? g_strdup (page->device_urls[idx]) : NULL;
      gtk_widget_set_visible (page->controller_website_btn,
        page->controller_url != NULL);

      /* Detect manufacturer for button labels */
      const char *name = page->device_names[idx];
      const char **labels = indicator_labels_xbox;
      if (strstr (name, "PlayStation") || strstr (name, "PS") ||
          strstr (name, "DUALSHOCK") || strstr (name, "DualSense") ||
          strstr (name, "PS4") || strstr (name, "PS5"))
        labels = indicator_labels_ps;
      else if (strstr (name, "Nintendo") || strstr (name, "Switch") ||
               strstr (name, "Pro Controller") || strstr (name, "Joy-Con") ||
               strstr (name, "Steam Controller"))
        labels = indicator_labels_generic;
      update_indicator_labels (page, labels);

      page->controller_fd = open (path, O_RDONLY | O_NONBLOCK);
      if (page->controller_fd >= 0)
        {
          page->ctrl_state = g_new0 (ControllerState, 1);
          page->controller_watch_id = g_unix_fd_add (page->controller_fd,
            G_IO_IN, on_controller_event, page);
        }
      else
        g_warning ("Failed to open controller device %s: %s",
          path, g_strerror (errno));

      update_extra_buttons_section (page);
    }
  else if (g_strcmp0 (type, "cpu") == 0 && page->cpu_page)
    gtk_stack_set_visible_child_name (page->detail_stack, "cpu");
  else if (g_strcmp0 (type, "keyboard") == 0 && page->keyboard_page)
    gtk_stack_set_visible_child_name (page->detail_stack, "keyboard");
  else if (g_strcmp0 (type, "mouse") == 0 && page->mouse_page)
    {
      mouse_page_show_device (page->mouse_page,
        page->device_vendor[idx], page->device_product[idx]);
      gtk_stack_set_visible_child_name (page->detail_stack, "mouse");
    }
  else
    gtk_stack_set_visible_child_name (page->detail_stack, "default");
}

/* ---- Page Creation ---- */

HardwarePage *
hardware_page_new (void)
{
  HardwarePage *page = g_new0 (HardwarePage, 1);
  page->controller_fd = -1;

  page->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  /* Left panel: device list */
  GtkWidget *left_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (left_panel, 320, -1);
  gtk_widget_set_hexpand (left_panel, FALSE);
  gtk_widget_add_css_class (left_panel, "device-list-panel");

  GtkWidget *header_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start (header_box, 8);
  gtk_widget_set_margin_end (header_box, 8);
  gtk_widget_set_margin_top (header_box, 8);
  gtk_widget_set_margin_bottom (header_box, 8);

  GtkWidget *header = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Devices"));
  gtk_widget_add_css_class (header, "heading");
  gtk_widget_set_hexpand (header, TRUE);
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  page->reload_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_valign (page->reload_btn, GTK_ALIGN_CENTER);
  gtk_button_set_has_frame (GTK_BUTTON (page->reload_btn), FALSE);
  gtk_widget_add_css_class (page->reload_btn, "flat");
  gtk_box_append (GTK_BOX (header_box), header);
  gtk_box_append (GTK_BOX (header_box), page->reload_btn);
  gtk_box_append (GTK_BOX (left_panel), header_box);

  page->device_list = gtk_list_box_new ();
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (page->device_list), TRUE);
  gtk_widget_set_hexpand (page->device_list, TRUE);
  gtk_widget_set_vexpand (page->device_list, TRUE);

  GtkWidget *left_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (left_scroll), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (left_scroll), page->device_list);
  gtk_box_append (GTK_BOX (left_panel), left_scroll);
  gtk_box_append (GTK_BOX (page->box), left_panel);

  /* Separator */
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
  gtk_box_append (GTK_BOX (page->box), sep);

  /* Right panel: detail stack */
  GtkWidget *right_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (right_panel, TRUE);
  gtk_widget_set_vexpand (right_panel, TRUE);

  page->detail_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (page->detail_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand (GTK_WIDGET (page->detail_stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (page->detail_stack), TRUE);

  /* Loading page */
  GtkWidget *loading_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (spinner, 48, 48);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (spinner, TRUE);
  gtk_widget_set_vexpand (spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (spinner));
  gtk_box_append (GTK_BOX (loading_page), spinner);
  gtk_stack_add_named (page->detail_stack, loading_page, "loading");

  /* Default page */
  GtkWidget *default_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *default_label = gtk_label_new (
    g_dgettext (GETTEXT_PACKAGE, "Select a device to view details"));
  gtk_widget_set_halign (default_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (default_label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (default_label, TRUE);
  gtk_widget_set_vexpand (default_label, TRUE);
  gtk_box_append (GTK_BOX (default_page), default_label);
  gtk_stack_add_named (page->detail_stack, default_page, "default");

  /* Controller detail page */
  build_controller_detail (page);

  /* Motherboard detail page */
  build_motherboard_detail (page);

  /* RAM detail page */
  build_rams_detail (page);

  /* GPU detail page */
  build_gpu_detail (page);
  gtk_stack_add_named (page->detail_stack, page->gpu_detail_box, "gpu");

  /* CPU, Keyboard, Mouse sub-pages */
  page->cpu_page = cpu_page_new ();
  if (page->cpu_page)
    gtk_stack_add_named (page->detail_stack, page->cpu_page->box, "cpu");

  page->keyboard_page = keyboard_page_new ();
  if (page->keyboard_page)
    gtk_stack_add_named (page->detail_stack, page->keyboard_page->box, "keyboard");

  page->mouse_page = mouse_page_new ();
  if (page->mouse_page)
    gtk_stack_add_named (page->detail_stack, page->mouse_page->box, "mouse");

  gtk_box_append (GTK_BOX (right_panel), GTK_WIDGET (page->detail_stack));
  gtk_box_append (GTK_BOX (page->box), right_panel);

  /* Detect devices */
  detect_devices (page);

  /* Load controller index from JOY_INDEX.md */
  load_controller_index (page);

  /* Initialize USB ID database: system file + async internet refresh */
  page->usb_ids = usb_ids_new ();
  usb_ids_load (page->usb_ids);
  usb_ids_refresh_async (page->usb_ids);

  /* Populate GPU specs from GPU_INDEX.md */
  populate_gpu_specs (page);

  /* Populate list */
  populate_device_list (page);

  /* Connect signals */
  g_signal_connect (page->reload_btn, "clicked",
    G_CALLBACK (on_reload_devices), page);
  g_signal_connect (page->device_list, "row-selected",
    G_CALLBACK (on_device_selected), page);

  /* Load saved GPU config after signals are connected */
  load_gpu_config (page);

  /* Pre-select first device */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (
    GTK_LIST_BOX (page->device_list), 0);
  if (first)
    gtk_list_box_select_row (GTK_LIST_BOX (page->device_list), first);

  /* Connect GPU management signals */
  if (page->gpu)
    {
      g_signal_connect (page->perf_level_dropdown, "notify::selected",
        G_CALLBACK (on_perf_level_changed), page);
      g_signal_connect (page->fan_switch, "notify::active",
        G_CALLBACK (on_fan_switch_toggled), page);
      g_signal_connect (page->fan_scale, "value-changed",
        G_CALLBACK (on_fan_scale_changed), page);

      /* Radeon Chill */
      g_signal_connect (page->chill_switch, "notify::active",
        G_CALLBACK (on_chill_switch_toggled), page);
      g_signal_connect (page->chill_min_scale, "value-changed",
        G_CALLBACK (sync_scale_to_spin), page->chill_min_spin);
      g_signal_connect (page->chill_min_spin, "value-changed",
        G_CALLBACK (sync_spin_to_scale), page->chill_min_scale);
      g_signal_connect (page->chill_max_scale, "value-changed",
        G_CALLBACK (sync_scale_to_spin), page->chill_max_spin);
      g_signal_connect (page->chill_max_spin, "value-changed",
        G_CALLBACK (sync_spin_to_scale), page->chill_max_scale);
      g_signal_connect (page->chill_min_spin, "value-changed",
        G_CALLBACK (on_chill_fps_changed), page);
      g_signal_connect (page->chill_max_spin, "value-changed",
        G_CALLBACK (on_chill_fps_changed), page);

      /* Anti-Lag */
      g_signal_connect (page->anti_lag_switch, "notify::active",
        G_CALLBACK (on_anti_lag_toggled), page);

      /* Specs button */
      g_signal_connect (page->gpu_specs_btn, "clicked",
        G_CALLBACK (on_gpu_specs_clicked), page);

      /* Power Profile Mode */
      g_signal_connect (page->power_profile_dd, "notify::selected",
        G_CALLBACK (on_power_profile_changed), page);

      /* Frame Rate Target Control */
      g_signal_connect (page->frtc_switch, "notify::active",
        G_CALLBACK (on_frtc_toggled), page);
      g_signal_connect (page->frtc_spin, "value-changed",
        G_CALLBACK (on_frtc_fps_changed), page);
      g_signal_connect (page->frtc_scale, "value-changed",
        G_CALLBACK (sync_scale_to_spin), page->frtc_spin);
      g_signal_connect (page->frtc_spin, "value-changed",
        G_CALLBACK (sync_spin_to_scale), page->frtc_scale);

      /* Power Limit */
      g_signal_connect (page->power_limit_spin, "value-changed",
        G_CALLBACK (on_power_limit_changed), page);
      g_signal_connect (page->power_limit_scale, "value-changed",
        G_CALLBACK (sync_scale_to_spin), page->power_limit_spin);
      g_signal_connect (page->power_limit_spin, "value-changed",
        G_CALLBACK (sync_spin_to_scale), page->power_limit_scale);

      /* Zero RPM Fan */
      g_signal_connect (page->zero_rpm_switch, "notify::active",
        G_CALLBACK (on_zero_rpm_toggled), page);
      g_signal_connect (page->zero_rpm_temp_spin, "value-changed",
        G_CALLBACK (on_zero_rpm_temp_changed), page);
      g_signal_connect (page->zero_rpm_temp_scale, "value-changed",
        G_CALLBACK (sync_scale_to_spin), page->zero_rpm_temp_spin);
      g_signal_connect (page->zero_rpm_temp_spin, "value-changed",
        G_CALLBACK (sync_spin_to_scale), page->zero_rpm_temp_scale);

      /* Variable Rate Shading */
      g_signal_connect (page->vrs_switch, "notify::active",
        G_CALLBACK (on_vrs_toggled), page);
    }

  /* NVIDIA signal connections (outside gpu check, widgets always exist) */
  g_signal_connect (page->nvidia_power_spin, "value-changed",
    G_CALLBACK (on_nvidia_power_limit_changed), page);
  g_signal_connect (page->nvidia_power_scale, "value-changed",
    G_CALLBACK (sync_scale_to_spin), page->nvidia_power_spin);
  g_signal_connect (page->nvidia_power_spin, "value-changed",
    G_CALLBACK (sync_spin_to_scale), page->nvidia_power_scale);
  g_signal_connect (page->nvidia_fan_switch, "notify::active",
    G_CALLBACK (on_nvidia_fan_toggled), page);
  g_signal_connect (page->nvidia_fan_scale, "value-changed",
    G_CALLBACK (on_nvidia_fan_speed_changed), page);

  return page;
}
