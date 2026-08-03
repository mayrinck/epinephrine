#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-gpu.h"
#include "epi-privilege.h"
#include "../epi-i18n.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define tr(x) epi_i18n_get (x)

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

/* ---- Sysfs helpers ---- */

uint64_t
hw_gpu_read_sysfs_u64 (const char *path)
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

/* ---- NVIDIA Helpers ---- */

static gboolean
nvidia_smi_available (void)
{
  return g_find_program_in_path ("nvidia-smi") != NULL;
}

/* Parse a line from "nvidia-smi --query-gpu=... --format=csv,noheader,nounits"
   Returns 8 parsed values (temp, usage, core_clock, mem_clock, vram_mib, power_w, fan_pct), with *ok set */
gboolean
hw_gpu_nvidia_query (float vals[8], gboolean ok[8], int gpu_index)
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
void
hw_gpu_nvidia_manage (const char *args)
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

void
hw_gpu_detect (HardwarePage *page)
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
      uint64_t sysfs_vram = hw_gpu_read_sysfs_u64 (vram_path);
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
          uint64_t ar = hw_gpu_read_sysfs_u64 (ar_path);
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
      page->gpu->vram_used = hw_gpu_read_sysfs_u64 (vram_used_path);
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

void
hw_gpu_populate_specs (HardwarePage *page)
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
  GtkWidget *features_title = gtk_label_new (tr ("Features"));
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

const char *
hw_gpu_brand_resource (const GpuInfo *gpu)
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
