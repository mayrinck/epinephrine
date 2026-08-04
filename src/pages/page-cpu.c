#include "page-cpu.h"
#include "page-dashboard.h"
#include "epi-privilege.h"
#include "../epi-i18n.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ---- Metric colors (same palette style as GPU monitor) ---- */

static const GdkRGBA cpu_metric_colors[CPU_MON_METRICS] = {
  { 0.902, 0.380, 0.000, 1.0 },   /* Temperature (orange) */
  { 0.200, 0.820, 0.478, 1.0 },   /* Usage (green)        */
  { 0.208, 0.518, 0.894, 1.0 },   /* Core Speed (blue)    */
  { 0.604, 0.600, 0.588, 1.0 },   /* Voltage (gray)       */
};

static const char *cpu_metric_names[CPU_MON_METRICS] = {
  "Temperature", "CPU Usage", "Core Speed", "Voltage"
};

static const char *cpu_metric_units[CPU_MON_METRICS] = {
  "°C", "%", "MHz", "V"
};

/* Normalization max values for the graph (0.0 – 1.0 range) */
static const float cpu_norm_max[CPU_MON_METRICS] = {
  100.0f,  /* Temperature: 0–100 °C */
  100.0f,  /* Usage: 0–100 % */
  5000.0f, /* Speed: 0–5000 MHz */
  1.5f,    /* Voltage: 0–1.5 V */
};

/* ---- Graph draw callback (monotone cubic, same as GPU) ---- */

static void
cpu_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
                        int width, int height, gpointer user_data)
{
  CpuPage *page = user_data;
  (void) area;

  /* Background */
  cairo_set_source_rgb (cr, 0.12, 0.12, 0.14);
  cairo_paint (cr);

  /* Grid: 5 horizontal lines */
  cairo_set_source_rgba (cr, 0.4, 0.4, 0.4, 0.5);
  cairo_set_line_width (cr, 0.5);
  cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, 8);

  for (int i = 0; i <= 4; i++)
    {
      double y = height - 4.0 - (height - 12.0) * i / 4.0;
      cairo_move_to (cr, 30, y);
      cairo_line_to (cr, width - 4, y);
      cairo_stroke (cr);

      char lbl[8];
      g_snprintf (lbl, sizeof (lbl), "%d%%", i * 25);
      cairo_move_to (cr, 2, y + 3);
      cairo_show_text (cr, lbl);
    }

  /* Draw traces */
  int n = page->monitor_history_count;
  int pts = n < CPU_MON_HISTORY ? n : CPU_MON_HISTORY;
  if (pts < 2) return;

  double plot_w = width - 34.0;
  double plot_h = height - 12.0;
  double x0 = 30.0;
  double y0 = 6.0;

  for (int m = 0; m < CPU_MON_METRICS; m++)
    {
      const GdkRGBA *c = &cpu_metric_colors[m];
      cairo_set_source_rgba (cr, c->red, c->green, c->blue, 0.9);
      cairo_set_line_width (cr, 1.2);

      int start = (n - pts);
      for (int i = 0; i < pts - 1; i++)
        {
          int idx0 = (start + i) % CPU_MON_HISTORY;
          int idx1 = (start + i + 1) % CPU_MON_HISTORY;

          float v0 = CLAMP (page->monitor_history[m][idx0], 0.0f, 1.0f);
          float v1 = CLAMP (page->monitor_history[m][idx1], 0.0f, 1.0f);

          double px0 = x0 + plot_w * i / (pts - 1);
          double px1 = x0 + plot_w * (i + 1) / (pts - 1);
          double py0 = y0 + plot_h * (1.0 - v0);
          double py1 = y0 + plot_h * (1.0 - v1);

          /* Monotone cubic (Fritsch-Carlson) */
          double dx = (px1 - px0) / 8.0;
          double dy0, dy1;

          if (i > 0)
            {
              int idx_prev = (start + i - 1) % CPU_MON_HISTORY;
              float vp = CLAMP (page->monitor_history[m][idx_prev], 0.0f, 1.0f);
              dy0 = (v1 - vp) / 2.0 * plot_h;
            }
          else
            dy0 = (v1 - v0) * plot_h;

          if (i < pts - 2)
            {
              int idx_next = (start + i + 2) % CPU_MON_HISTORY;
              float vn = CLAMP (page->monitor_history[m][idx_next], 0.0f, 1.0f);
              dy1 = (vn - v0) / 2.0 * plot_h;
            }
          else
            dy1 = (v1 - v0) * plot_h;

          /* Hermite basis */
          cairo_move_to (cr, px0, py0);
          for (int s = 1; s <= 8; s++)
            {
              double t = s / 8.0;
              double t2 = t * t;
              double t3 = t2 * t;
              double h00 = 2*t3 - 3*t2 + 1;
              double h10 = t3 - 2*t2 + t;
              double h01 = -2*t3 + 3*t2;
              double h11 = t3 - t2;
              double y = h00*py0 + h10*dx*dy0 + h01*py1 + h11*dx*dy1;
              cairo_line_to (cr, px0 + dx * s, y);
            }
          cairo_stroke (cr);
        }
    }
}

/* ---- Metric dot draw callback ---- */

static void
cpu_metric_dot_draw (GtkDrawingArea *area, cairo_t *cr,
                     int w, int h, gpointer user_data)
{
  (void) area; (void) w; (void) h;
  int idx = GPOINTER_TO_INT (user_data);
  const GdkRGBA *c = &cpu_metric_colors[idx];
  cairo_arc (cr, 5, 5, 4, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, c->red, c->green, c->blue, 1.0);
  cairo_fill (cr);
}

/* ---- Read a sysfs/proc file as uint64 ---- */

static uint64_t
read_sysfs_u64 (const char *path)
{
  if (!path) return 0;
  FILE *f = fopen (path, "r");
  if (!f) return 0;
  char buf[64] = {0};
  if (fgets (buf, sizeof (buf), f))
    {
      fclose (f);
      return strtoull (buf, NULL, 10);
    }
  fclose (f);
  return 0;
}

/* ---- Read first line of a file (trimmed) ---- */

static gboolean
read_file_line (const char *path, char *buf, size_t len)
{
  if (!path || !buf) return FALSE;
  FILE *f = fopen (path, "r");
  if (!f) return FALSE;
  gboolean ok = fgets (buf, len, f) != NULL;
  fclose (f);
  if (ok) g_strstrip (buf);
  return ok;
}

/* ---- /proc/stat CPU usage calculation ---- */

static float
read_cpu_usage (CpuPage *page)
{
  FILE *f = fopen ("/proc/stat", "r");
  if (!f) return 0.0f;

  char line[256];
  if (!fgets (line, sizeof (line), f))
    { fclose (f); return 0.0f; }
  fclose (f);

  /* First line: "cpu  user nice system idle iowait irq softirq steal" */
  unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
  if (sscanf (line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
              &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
    return 0.0f;

  uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
  uint64_t idle_all = idle + iowait;

  uint64_t d_total = total - page->prev_total;
  uint64_t d_idle  = idle_all - page->prev_idle;

  page->prev_total = total;
  page->prev_idle  = idle_all;

  if (d_total == 0) return 0.0f;
  return (float)(d_total - d_idle) / (float)d_total * 100.0f;
}

/* ---- Temperature from thermal zones ---- */

static float
read_cpu_temp (void)
{
  char path[256];
  /* Try known thermal zone patterns */
  for (int tz = 0; tz < 20; tz++)
    {
      g_snprintf (path, sizeof (path),
        "/sys/class/thermal/thermal_zone%d/temp", tz);
      uint64_t raw = read_sysfs_u64 (path);
      if (raw > 0 && raw < 200000)
        return raw / 1000.0f;
    }

  /* Fallback: coretemp hwmon */
  for (int hw = 0; hw < 10; hw++)
    {
      g_snprintf (path, sizeof (path),
        "/sys/class/hwmon/hwmon%d/temp1_input", hw);
      uint64_t raw = read_sysfs_u64 (path);
      if (raw > 0 && raw < 200000)
        return raw / 1000.0f;
    }

  return 0.0f;
}

/* ---- Voltage from hwmon ---- */

static float
read_cpu_voltage (void)
{
  char path[256];
  for (int hw = 0; hw < 10; hw++)
    {
      g_snprintf (path, sizeof (path),
        "/sys/class/hwmon/hwmon%d/in0_input", hw);
      uint64_t raw = read_sysfs_u64 (path);
      if (raw > 0 && raw < 20000)
        return raw / 1000.0f;
    }
  return 0.0f;
}

/* ---- Current core frequency (MHz) ---- */

static float
read_cpu_freq_mhz (void)
{
  uint64_t khz = read_sysfs_u64 (
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
  if (khz > 0) return khz / 1000.0f;

  /* Fallback: /proc/cpuinfo */
  FILE *f = fopen ("/proc/cpuinfo", "r");
  if (!f) return 0.0f;
  char line[256];
  while (fgets (line, sizeof (line), f))
    {
      if (g_str_has_prefix (line, "cpu MHz"))
        {
          float mhz = 0;
          if (sscanf (line, "cpu MHz : %f", &mhz) == 1 && mhz > 0)
            { fclose (f); return mhz; }
        }
    }
  fclose (f);
  return 0.0f;
}

/* ---- Polling callback (every 1s) ---- */

static gboolean
cpu_monitor_poll_cb (gpointer user_data)
{
  CpuPage *page = user_data;
  if (!page) return G_SOURCE_REMOVE;

  int n = page->monitor_history_count;

  /* Read live values */
  float temp  = read_cpu_temp ();
  float usage = read_cpu_usage (page);
  float freq  = read_cpu_freq_mhz ();
  float volt  = read_cpu_voltage ();

  float vals[CPU_MON_METRICS] = { temp, usage, freq, volt };
  gboolean ok[CPU_MON_METRICS] = {
    temp > 0,
    n > 0,        /* first call has no valid delta */
    freq > 0,
    volt > 0
  };

  /* Store normalized history */
  for (int m = 0; m < CPU_MON_METRICS; m++)
    page->monitor_history[m][n % CPU_MON_HISTORY] =
      ok[m] ? CLAMP (vals[m] / cpu_norm_max[m], 0.0f, 1.0f) : 0.0f;

  page->monitor_history_count++;

  /* Update labels */
  for (int m = 0; m < CPU_MON_METRICS; m++)
    {
      if (ok[m])
        {
          char buf[32];
          if (m == 3) /* Voltage */
            g_snprintf (buf, sizeof (buf), "%.3f %s", vals[m], cpu_metric_units[m]);
          else if (m == 2) /* Frequency */
            g_snprintf (buf, sizeof (buf), "%.0f %s", vals[m], cpu_metric_units[m]);
          else
            g_snprintf (buf, sizeof (buf), "%.1f %s", vals[m], cpu_metric_units[m]);
          gtk_label_set_text (GTK_LABEL (page->monitor_values[m]), buf);
        }
      else
        gtk_label_set_text (GTK_LABEL (page->monitor_values[m]), "N/A");
    }

  /* Update current frequency label too */
  if (page->cur_freq_label && freq > 0)
    {
      char freq_str[64];
      if (freq >= 1000.0f)
        g_snprintf (freq_str, sizeof (freq_str), "%.2f GHz", freq / 1000.0f);
      else
        g_snprintf (freq_str, sizeof (freq_str), "%.0f MHz", freq);
      gtk_label_set_text (GTK_LABEL (page->cur_freq_label), freq_str);
    }

  gtk_widget_queue_draw (page->monitor_graph);
  return G_SOURCE_CONTINUE;
}

/* ---- Start / stop monitoring ---- */

void
cpu_monitor_start (CpuPage *page)
{
  if (!page) return;
  page->monitor_history_count = 0;
  memset (page->monitor_history, 0, sizeof (page->monitor_history));
  memset (&page->prev_idle, 0, sizeof (page->prev_idle) + sizeof (page->prev_total));
  if (page->monitor_timer_id) return;
  page->monitor_timer_id = g_timeout_add (1000, cpu_monitor_poll_cb, page);
  cpu_monitor_poll_cb (page);
}

void
cpu_monitor_stop (CpuPage *page)
{
  if (!page || !page->monitor_timer_id) return;
  g_source_remove (page->monitor_timer_id);
  page->monitor_timer_id = 0;
}

/* ---- Codename detection (comprehensive AMD + Intel) ---- */

static char *
detect_codename (const char *model_name)
{
  if (!model_name) return NULL;
  char *lower = g_ascii_strdown (model_name, -1);
  char *codename = NULL;

  /* AMD Ryzen Desktop */
  if (strstr (lower, "ryzen"))
    {
      if (strstr (lower, "9 ") || strstr (lower, "9950") ||
          strstr (lower, "9900") || strstr (lower, "9800"))
        codename = g_strdup ("Granite Ridge");
      else if (strstr (lower, "7 ") || strstr (lower, "7950") ||
               strstr (lower, "7900") || strstr (lower, "7800") ||
               strstr (lower, "7700") || strstr (lower, "7600"))
        codename = g_strdup ("Raphael");
      else if (strstr (lower, "5 ") || strstr (lower, "5950") ||
               strstr (lower, "5900") || strstr (lower, "5800") ||
               strstr (lower, "5700") || strstr (lower, "5600"))
        codename = g_strdup ("Vermeer");
      else if (strstr (lower, "3 ") || strstr (lower, "3950") ||
               strstr (lower, "3900") || strstr (lower, "3800") ||
               strstr (lower, "3700") || strstr (lower, "3600"))
        codename = g_strdup ("Matisse");
      else if (strstr (lower, "2 ") || strstr (lower, "2700") ||
               strstr (lower, "2600"))
        codename = g_strdup ("Pinnacle Ridge");
      else if (strstr (lower, "1 ") || strstr (lower, "1800") ||
               strstr (lower, "1700") || strstr (lower, "1600"))
        codename = g_strdup ("Summit Ridge");
      /* AMD Mobile */
      else if (strstr (lower, "ai 9") || strstr (lower, "ai 7"))
        codename = g_strdup ("Strix Point");
      else if (strstr (lower, "7 7840") || strstr (lower, "7 7840HS") ||
               strstr (lower, "7 8840") || strstr (lower, "9 8945"))
        codename = g_strdup ("Hawk Point");
      else if (strstr (lower, "7 7700") || strstr (lower, "7 7840") ||
               strstr (lower, "7 7940") || strstr (lower, "9 8945"))
        codename = g_strdup ("Phoenix");
      else if (strstr (lower, "6 6600") || strstr (lower, "6 6800") ||
               strstr (lower, "6 6900"))
        codename = g_strdup ("Rembrandt");
      else if (strstr (lower, "5 5600") || strstr (lower, "5 5800") ||
               strstr (lower, "5 5900"))
        codename = g_strdup ("Cezanne");
      else if (strstr (lower, "4 4600") || strstr (lower, "4 4700") ||
               strstr (lower, "4 4800"))
        codename = g_strdup ("Renoir");
      else if (strstr (lower, "3 3200") || strstr (lower, "3 3400") ||
               strstr (lower, "3 3500"))
        codename = g_strdup ("Picasso");
      /* AMD HEDT / Server */
      else if (strstr (lower, "threadripper"))
        {
          if (strstr (lower, "79") || strstr (lower, "78"))
            codename = g_strdup ("Storm Peak");
          else if (strstr (lower, "39") || strstr (lower, "38"))
            codename = g_strdup ("Castle Peak");
          else if (strstr (lower, "29") || strstr (lower, "28"))
            codename = g_strdup ("Castle Peak");
          else if (strstr (lower, "19") || strstr (lower, "18"))
            codename = g_strdup ("Colfax");
        }
      else if (strstr (lower, "epyc"))
        {
          if (strstr (lower, "97") || strstr (lower, "96") || strstr (lower, "95") ||
              strstr (lower, "94") || strstr (lower, "93") || strstr (lower, "92"))
            codename = g_strdup ("Genoa");
          else if (strstr (lower, "77") || strstr (lower, "76") || strstr (lower, "75"))
            codename = g_strdup ("Milan");
          else if (strstr (lower, "73") || strstr (lower, "72"))
            codename = g_strdup ("Milan");
          else if (strstr (lower, "70") || strstr (lower, "60"))
            codename = g_strdup ("Rome");
          else if (strstr (lower, "3") || strstr (lower, "4") || strstr (lower, "5"))
            codename = g_strdup ("Naples");
        }
    }
  /* Intel */
  else if (strstr (lower, "intel") || strstr (lower, "core"))
    {
      /* Arrow Lake */
      if (strstr (lower, "ultra 9") || strstr (lower, "ultra 7") ||
          strstr (lower, "ultra 5"))
        {
          if (strstr (lower, "2") || strstr (lower, "k"))
            codename = g_strdup ("Arrow Lake");
          else
            codename = g_strdup ("Lunar Lake");
        }
      /* Raptor Lake (13th/14th gen) */
      else if (strstr (lower, "i9-13") || strstr (lower, "i7-13") ||
               strstr (lower, "i5-13") || strstr (lower, "i3-13") ||
               strstr (lower, "i9-14") || strstr (lower, "i7-14") ||
               strstr (lower, "i5-14") || strstr (lower, "i3-14"))
        codename = g_strdup ("Raptor Lake");
      /* Alder Lake (12th gen) */
      else if (strstr (lower, "i9-12") || strstr (lower, "i7-12") ||
               strstr (lower, "i5-12") || strstr (lower, "i3-12"))
        codename = g_strdup ("Alder Lake");
      /* Rocket Lake (11th gen) */
      else if (strstr (lower, "i9-11") || strstr (lower, "i7-11") ||
               strstr (lower, "i5-11") || strstr (lower, "i3-11"))
        codename = g_strdup ("Rocket Lake");
      /* Comet Lake (10th gen) */
      else if (strstr (lower, "i9-10") || strstr (lower, "i7-10") ||
               strstr (lower, "i5-10") || strstr (lower, "i3-10"))
        codename = g_strdup ("Comet Lake");
      /* Ice Lake (10th gen mobile) */
      else if (strstr (lower, "i7-106") || strstr (lower, "i5-103"))
        codename = g_strdup ("Ice Lake");
      /* Coffee Lake (8th/9th gen) */
      else if (strstr (lower, "i9-9") || strstr (lower, "i7-9") ||
               strstr (lower, "i5-9") || strstr (lower, "i3-9") ||
               strstr (lower, "i7-8") || strstr (lower, "i5-8") ||
               strstr (lower, "i3-8"))
        codename = g_strdup ("Coffee Lake");
      /* Kaby Lake (7th gen) */
      else if (strstr (lower, "i7-7") || strstr (lower, "i5-7") ||
               strstr (lower, "i3-7"))
        codename = g_strdup ("Kaby Lake");
      /* Skylake (6th gen) */
      else if (strstr (lower, "i7-6") || strstr (lower, "i5-6") ||
               strstr (lower, "i3-6"))
        codename = g_strdup ("Skylake");
      /* Xeon */
      else if (strstr (lower, "xeon"))
        {
          if (strstr (lower, "w9-35") || strstr (lower, "w7-34") ||
              strstr (lower, "w5-34") || strstr (lower, "w3-34"))
            codename = g_strdup ("Sapphire Rapids");
          else if (strstr (lower, "w9-24") || strstr (lower, "w7-24") ||
                   strstr (lower, "w5-24") || strstr (lower, "w3-24"))
            codename = g_strdup ("Sapphire Rapids");
          else if (strstr (lower, "e5-26") || strstr (lower, "e5-16") ||
                   strstr (lower, "e5-46"))
            codename = g_strdup ("Haswell");
          else if (strstr (lower, "e3-12"))
            codename = g_strdup ("Skylake");
        }
      /* Fallback for Intel with any number */
      else if (strstr (lower, "i9") || strstr (lower, "i7") ||
               strstr (lower, "i5") || strstr (lower, "i3"))
        codename = g_strdup ("Intel (Unknown Gen)");
    }

  g_free (lower);
  return codename;
}

/* ---- Collect CPU vulnerabilities from sysfs ---- */

static void
collect_vulnerabilities (CpuPage *page)
{
  char path[256];
  page->n_vulns = 0;

  /* Known vulnerability names from /sys/devices/system/cpu/vulnerabilities/ */
  static const char *vuln_names[] = {
    "itlb_multihit", "l1tf", "mds", "meltdown", "mmio_stale_data",
    "retbleed", "spec_store_bypass", "spectre_v1", "spectre_v2",
    "srbds", "tsx_async_abort", "reg_file_data_sampling", NULL
  };

  for (int i = 0; vuln_names[i] && page->n_vulns < CPU_MAX_VULNS; i++)
    {
      g_snprintf (path, sizeof (path),
        "/sys/devices/system/cpu/vulnerabilities/%s", vuln_names[i]);
      FILE *f = fopen (path, "r");
      if (f)
        {
          char line[256];
          if (fgets (line, sizeof (line), f))
            {
              g_strstrip (line);
              if (strlen (line) > 0)
                page->vulnerabilities[page->n_vulns++] =
                  g_strdup_printf ("%s: %s", vuln_names[i], line);
            }
          fclose (f);
        }
    }
}

/* ---- Static data collection ---- */

static void
collect_cpu_info (CpuPage *page)
{
  FILE *f;
  char buf[1024];

  /* ---- /proc/cpuinfo ---- */
  f = fopen ("/proc/cpuinfo", "r");
  if (f)
    {
      page->n_cores = 0;
      while (fgets (buf, sizeof (buf), f))
        {
          if (g_str_has_prefix (buf, "processor"))
            page->n_cores++;
          else if (g_str_has_prefix (buf, "model name"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->model_name = g_strdup (colon);
                }
            }
          else if (g_str_has_prefix (buf, "vendor_id"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->vendor_id = g_strdup (colon);
                }
            }
          else if (g_str_has_prefix (buf, "cpu family"))
            {
              char *colon = strchr (buf, ':');
              if (colon) page->family = atoi (colon + 2);
            }
          else if (g_str_has_prefix (buf, "model\t"))
            {
              char *colon = strchr (buf, ':');
              if (colon) page->model = atoi (colon + 2);
            }
          else if (g_str_has_prefix (buf, "stepping"))
            {
              char *colon = strchr (buf, ':');
              if (colon) page->stepping = atoi (colon + 2);
            }
          else if (g_str_has_prefix (buf, "bogomips"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->bogomips = g_strdup (colon);
                }
            }
          else if (g_str_has_prefix (buf, "microcode"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->microcode = g_strdup (colon);
                }
            }
          else if (g_str_has_prefix (buf, "bugs"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->bugs_str = g_strdup (colon);
                }
            }
          else if (g_str_has_prefix (buf, "flags\t"))
            {
              char *colon = strchr (buf, ':');
              if (colon)
                {
                  colon += 2;
                  g_strstrip (colon);
                  page->flags = g_strdup (colon);
                }
            }
        }
      fclose (f);
    }

  /* ---- lscpu ---- */
  FILE *lscpu = popen ("lscpu 2>/dev/null", "r");
  if (lscpu)
    {
      while (fgets (buf, sizeof (buf), lscpu))
        {
          if (g_str_has_prefix (buf, "Architecture:"))
            {
              char *val = strchr (buf, ':');
              if (val) { val++; g_strstrip (val); page->architecture = g_strdup (val); }
            }
          else if (g_str_has_prefix (buf, "Socket(s):"))
            {
              char *val = strchr (buf, ':');
              if (val) page->socket = g_strdup (val + 2);
              if (page->socket) g_strstrip (page->socket);
            }
          else if (g_str_has_prefix (buf, "Core(s) per socket:"))
            {
              char *val = strchr (buf, ':');
              if (val) page->cores_physical = atoi (val + 2);
            }
          else if (g_str_has_prefix (buf, "Thread(s) per core:"))
            {
              char *val = strchr (buf, ':');
              if (val)
                {
                  int tpc = atoi (val + 2);
                  if (tpc > 0 && page->cores_physical > 0)
                    page->threads_total = page->cores_physical * tpc;
                }
            }
          else if (g_str_has_prefix (buf, "CPU MHz:"))
            {
              char *val = strchr (buf, ':');
              if (val && !page->base_freq)
                {
                  val++; g_strstrip (val);
                  page->base_freq = g_strdup_printf ("%s MHz", val);
                }
            }
          else if (g_str_has_prefix (buf, "CPU max MHz:"))
            {
              char *val = strchr (buf, ':');
              if (val && !page->boost_freq)
                {
                  val++; g_strstrip (val);
                  /* Remove trailing .000 */
                  char *dot = strstr (val, ".000");
                  if (dot) *dot = '\0';
                  page->boost_freq = g_strdup_printf ("%s MHz", val);
                }
            }
          else if (g_str_has_prefix (buf, "CPU min MHz:"))
            {
              /* Just skip — we don't display it */
            }
          else if (g_str_has_prefix (buf, "L1d cache:"))
            {
              char *val = strchr (buf, ':');
              if (val) { val++; g_strstrip (val); page->l1d_cache = g_strdup (val); }
            }
          else if (g_str_has_prefix (buf, "L1i cache:"))
            {
              char *val = strchr (buf, ':');
              if (val) { val++; g_strstrip (val); page->l1i_cache = g_strdup (val); }
            }
          else if (g_str_has_prefix (buf, "L2 cache:"))
            {
              char *val = strchr (buf, ':');
              if (val) { val++; g_strstrip (val); page->l2_cache = g_strdup (val); }
            }
          else if (g_str_has_prefix (buf, "L3 cache:"))
            {
              char *val = strchr (buf, ':');
              if (val) { val++; g_strstrip (val); page->l3_cache = g_strdup (val); }
            }
        }
      pclose (lscpu);
    }

  /* Thread fallback */
  if (page->threads_total < 1)
    page->threads_total = page->n_cores;

  /* ---- Cache from sysfs (fallback if lscpu missed it) ---- */
  if (!page->l1d_cache)
    {
      uint64_t sz = read_sysfs_u64 (
        "/sys/devices/system/cpu/cpu0/cache/index0/size");
      if (sz > 0) page->l1d_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }
  if (!page->l2_cache)
    {
      uint64_t sz = read_sysfs_u64 (
        "/sys/devices/system/cpu/cpu0/cache/index2/size");
      if (sz > 0) page->l2_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }
  if (!page->l3_cache)
    {
      uint64_t sz = read_sysfs_u64 (
        "/sys/devices/system/cpu/cpu0/cache/index3/size");
      if (sz > 0) page->l3_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }

  /* ---- Base/Boost from sysfs fallback ---- */
  if (!page->base_freq)
    {
      uint64_t khz = read_sysfs_u64 (
        "/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
      if (khz > 0)
        page->base_freq = g_strdup_printf ("%.0f MHz", khz / 1000.0);
    }
  if (!page->boost_freq)
    {
      uint64_t khz = read_sysfs_u64 (
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
      if (khz > 0)
        page->boost_freq = g_strdup_printf ("%.0f MHz", khz / 1000.0);
    }

  /* ---- Available governors ---- */
  {
    FILE *gf = fopen ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", "r");
    if (gf)
      {
        char gbuf[256] = {0};
        if (fgets (gbuf, sizeof (gbuf), gf))
          {
            char *tok = strtok (gbuf, " \t\n");
            while (tok && page->n_governors < CPU_MAX_GOVERNORS)
              {
                page->available_governors[page->n_governors++] = g_strdup (tok);
                tok = strtok (NULL, " \t\n");
              }
          }
        fclose (gf);
      }
  }

  /* ---- Min/Max freq from sysfs ---- */
  {
    uint64_t min_khz = read_sysfs_u64 (
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
    uint64_t max_khz = read_sysfs_u64 (
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    page->min_freq_hw = g_strdup_printf ("%lu", (unsigned long)min_khz);
    page->max_freq_hw = g_strdup_printf ("%lu", (unsigned long)max_khz);
  }

  /* ---- Vulnerabilities ---- */
  collect_vulnerabilities (page);
}

/* ---- Populate UI labels with collected data ---- */

static void
populate_cpu_labels (CpuPage *page)
{
  char buf[1024];

  /* Branding */
  if (page->model_name)
    gtk_label_set_text (GTK_LABEL (page->cpu_name_label), page->model_name);

  if (page->vendor_id)
    gtk_label_set_text (GTK_LABEL (page->cpu_vendor_label), page->vendor_id);

  /* Codename: comprehensive detection */
  if (page->model_name)
    {
      char *codename = detect_codename (page->model_name);
      if (codename)
        {
          gtk_label_set_text (GTK_LABEL (page->cpu_codename_label), codename);
          g_free (codename);
        }
    }

  /* Socket */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-socket-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->socket ? page->socket : "—");
  }

  /* Architecture */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-arch-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->architecture ? page->architecture : "—");
  }

  /* Family/Model/Stepping */
  {
    char fms[64];
    g_snprintf (fms, sizeof (fms), "0x%X / 0x%X / %d",
                page->family, page->model, page->stepping);
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-family-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), fms);
  }

  /* Cores/Threads */
  {
    char ct[64];
    g_snprintf (ct, sizeof (ct), "%d / %d",
                page->cores_physical > 0 ? page->cores_physical : page->n_cores,
                page->threads_total);
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-cores-threads-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), ct);
  }

  /* BogoMIPS */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-bogomips-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->bogomips ? page->bogomips : "—");
  }

  /* Microcode */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-microcode-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->microcode ? page->microcode : "—");
  }

  /* Base frequency */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-base-freq-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->base_freq ? page->base_freq : "—");
  }

  /* Boost frequency */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-boost-freq-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->boost_freq ? page->boost_freq : "—");
  }

  /* Flags */
  {
    GtkWidget *w = g_object_get_data (G_OBJECT (page->box), "cpu-flags-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->flags ? page->flags : "—");
  }

  /* Caches */
  {
    GtkWidget *w;
    w = g_object_get_data (G_OBJECT (page->box), "cpu-l1d-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->l1d_cache ? page->l1d_cache : "—");
    w = g_object_get_data (G_OBJECT (page->box), "cpu-l1i-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->l1i_cache ? page->l1i_cache : "—");
    w = g_object_get_data (G_OBJECT (page->box), "cpu-l2-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->l2_cache ? page->l2_cache : "—");
    w = g_object_get_data (G_OBJECT (page->box), "cpu-l3-label");
    if (w) gtk_label_set_text (GTK_LABEL (w), page->l3_cache ? page->l3_cache : "—");
  }

  /* Vulnerabilities: add rows to the security section */
  {
    GtkWidget *vulns_group = GTK_WIDGET (g_object_get_data (G_OBJECT (page->box), "cpu-vulns-group"));
    if (vulns_group)
      {
        for (int v = 0; v < page->n_vulns; v++)
          {
            AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
            /* Split "name: status" for title/subtitle */
            char *colon = strchr (page->vulnerabilities[v], ':');
            if (colon)
              {
                char name[128] = {0};
                size_t nlen = colon - page->vulnerabilities[v];
                if (nlen > sizeof (name) - 1) nlen = sizeof (name) - 1;
                memcpy (name, page->vulnerabilities[v], nlen);
                adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
                adw_action_row_set_subtitle (row, colon + 2);
              }
            else
              {
                adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                  page->vulnerabilities[v]);
              }
            adw_preferences_group_add (ADW_PREFERENCES_GROUP (vulns_group),
              GTK_WIDGET (row));
          }

        if (page->n_vulns == 0)
          {
            AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
              tr ("No known vulnerabilities detected"));
            adw_action_row_add_suffix (row, NULL);
            adw_preferences_group_add (ADW_PREFERENCES_GROUP (vulns_group),
              GTK_WIDGET (row));
          }
      }
  }

  /* Governor */
  if (page->governor_current_label)
    {
      if (read_file_line ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                          buf, sizeof (buf)))
        gtk_label_set_text (GTK_LABEL (page->governor_current_label), buf);
    }

  /* Current frequency */
  if (page->cur_freq_label)
    {
      float mhz = read_cpu_freq_mhz ();
      if (mhz > 0)
        {
          char freq_str[64];
          if (mhz >= 1000.0f)
            g_snprintf (freq_str, sizeof (freq_str), "%.2f GHz", mhz / 1000.0f);
          else
            g_snprintf (freq_str, sizeof (freq_str), "%.0f MHz", mhz);
          gtk_label_set_text (GTK_LABEL (page->cur_freq_label), freq_str);
        }
    }

  /* Auto-populate min/max freq spin buttons */
  if (page->min_freq_spin && page->min_freq_hw)
    {
      int min_val = atoi (page->min_freq_hw);
      if (min_val > 0)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->min_freq_spin), min_val);
    }
  if (page->max_freq_spin && page->max_freq_hw)
    {
      int max_val = atoi (page->max_freq_hw);
      if (max_val > 0)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (page->max_freq_spin), max_val);
    }
}

/* ---- Save handler ---- */

static void
on_cpu_save (GtkButton *btn, CpuPage *page)
{
  (void) btn;

  const char *governor = NULL;
  {
    guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->governor_dropdown));
    if (sel < (guint) page->n_governors)
      governor = page->available_governors[sel];
  }

  if (governor && page->n_cores > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%s' '%s' > \"$d/scaling_governor\" 2>/dev/null; done",
        governor);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }

  int min_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->min_freq_spin));
  int max_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->max_freq_spin));

  if (min_freq > 0 && page->n_cores > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%d' %d > \"$d/scaling_min_freq\" 2>/dev/null; done",
        min_freq);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }

  if (max_freq > 0 && page->n_cores > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%d' %d > \"$d/scaling_max_freq\" 2>/dev/null; done",
        max_freq);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }

  dashboard_show_toast (tr ("CPU settings applied"));
}

/* ---- OpenRGB handler ---- */

static void
on_cpu_openrgb_clicked (GtkButton *btn, CpuPage *page)
{
  (void) btn; (void) page;
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

/* ---- Helper to store a widget reference on page->box ---- */

static void
store_widget_ref (CpuPage *page, GtkBuilder *builder,
                  const char *ui_id, const char *data_key)
{
  GtkWidget *w = GTK_WIDGET (gtk_builder_get_object (builder, ui_id));
  if (w) g_object_set_data (G_OBJECT (page->box), data_key, w);
}

/* ---- Constructor ---- */

CpuPage *
cpu_page_new (void)
{
  CpuPage *page = g_new0 (CpuPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-cpu.ui");

  page->box = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-page"));

  /* Branding */
  page->cpu_brand_icon   = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_brand_icon"));
  page->cpu_name_label   = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_name_label"));
  page->cpu_vendor_label = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_vendor_label"));
  page->cpu_codename_label = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_codename_label"));

  /* Monitor */
  page->monitor_graph = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_monitor_graph"));
  GtkWidget *monitor_left = GTK_WIDGET (gtk_builder_get_object (builder, "cpu_monitor_left"));

  /* Connect graph draw function */
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->monitor_graph),
    cpu_monitor_graph_draw, page, NULL);

  /* Build monitor metric legend rows */
  static const char *default_labels[CPU_MON_METRICS] = { "—", "—", "—", "—" };
  for (int i = 0; i < CPU_MON_METRICS; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_start (row, 8);
      gtk_widget_set_margin_end (row, 8);

      /* Colored dot */
      GtkWidget *dot = gtk_drawing_area_new ();
      gtk_widget_set_size_request (dot, 10, 10);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot),
        cpu_metric_dot_draw, GINT_TO_POINTER (i), NULL);
      gtk_box_append (GTK_BOX (row), dot);

      /* Metric name */
      page->monitor_labels[i] = gtk_label_new (cpu_metric_names[i]);
      gtk_widget_set_halign (page->monitor_labels[i], GTK_ALIGN_START);
      gtk_widget_set_hexpand (page->monitor_labels[i], TRUE);
      gtk_box_append (GTK_BOX (row), page->monitor_labels[i]);

      /* Metric value */
      page->monitor_values[i] = gtk_label_new (default_labels[i]);
      gtk_widget_set_halign (page->monitor_values[i], GTK_ALIGN_END);
      gtk_label_set_width_chars (GTK_LABEL (page->monitor_values[i]), 12);
      gtk_box_append (GTK_BOX (row), page->monitor_values[i]);

      /* Units */
      GtkWidget *unit_lbl = gtk_label_new (cpu_metric_units[i]);
      gtk_widget_set_halign (unit_lbl, GTK_ALIGN_START);
      gtk_label_set_width_chars (GTK_LABEL (unit_lbl), 4);
      gtk_box_append (GTK_BOX (row), unit_lbl);

      gtk_box_append (GTK_BOX (monitor_left), row);
    }

  /* CPU control */
  page->governor_dropdown = GTK_WIDGET (gtk_builder_get_object (builder, "governor-dropdown"));
  page->governor_current_label = GTK_WIDGET (gtk_builder_get_object (builder, "governor-current-label"));
  page->min_freq_spin = GTK_WIDGET (gtk_builder_get_object (builder, "min-freq-spin"));
  page->max_freq_spin = GTK_WIDGET (gtk_builder_get_object (builder, "max-freq-spin"));
  page->cur_freq_label = GTK_WIDGET (gtk_builder_get_object (builder, "cur-freq-label"));
  page->save_btn = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-save-btn"));

  /* Store all spec/cache label references on page->box */
  store_widget_ref (page, builder, "cpu-socket-label",        "cpu-socket-label");
  store_widget_ref (page, builder, "cpu-arch-label",          "cpu-arch-label");
  store_widget_ref (page, builder, "cpu-family-label",        "cpu-family-label");
  store_widget_ref (page, builder, "cpu-cores-threads-label", "cpu-cores-threads-label");
  store_widget_ref (page, builder, "cpu-bogomips-label",      "cpu-bogomips-label");
  store_widget_ref (page, builder, "cpu-microcode-label",     "cpu-microcode-label");
  store_widget_ref (page, builder, "cpu-base-freq-label",     "cpu-base-freq-label");
  store_widget_ref (page, builder, "cpu-boost-freq-label",    "cpu-boost-freq-label");
  store_widget_ref (page, builder, "cpu-flags-label",         "cpu-flags-label");
  store_widget_ref (page, builder, "cpu-l1d-label",           "cpu-l1d-label");
  store_widget_ref (page, builder, "cpu-l1i-label",           "cpu-l1i-label");
  store_widget_ref (page, builder, "cpu-l2-label",            "cpu-l2-label");
  store_widget_ref (page, builder, "cpu-l3-label",            "cpu-l3-label");

  /* Collect static data first (governors, freq, vulnerabilities) */
  collect_cpu_info (page);

  /* Dynamic governor dropdown */
  {
    GtkStringList *gov_list = GTK_STRING_LIST (
      gtk_builder_get_object (builder, "governor-stringlist"));

    /* Clear default items */
    guint n_items = g_list_model_get_n_items (G_LIST_MODEL (gov_list));
    for (guint i = 0; i < n_items; i++)
      gtk_string_list_remove (gov_list, 0);

    /* Add detected governors */
    for (int i = 0; i < page->n_governors; i++)
      gtk_string_list_append (gov_list, page->available_governors[i]);

    /* Select current governor */
    char cur_gov[64] = {0};
    if (read_file_line ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                        cur_gov, sizeof (cur_gov)))
      {
        for (int i = 0; i < page->n_governors; i++)
          {
            if (g_strcmp0 (cur_gov, page->available_governors[i]) == 0)
              {
                gtk_drop_down_set_selected (GTK_DROP_DOWN (page->governor_dropdown), i);
                break;
              }
          }
      }
  }

  /* Lighting */
  page->lighting_status = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-lighting-status"));
  if (page->lighting_status)
    gtk_label_set_text (GTK_LABEL (page->lighting_status),
      tr ("The app couldn't find any light source to manage."));

  GtkWidget *cpu_lighting_header = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-lighting-header"));
  if (g_find_program_in_path ("openrgb"))
    {
      GtkWidget *openrgb_btn = gtk_button_new_with_label (tr ("Edit on OpenRGB"));
      gtk_widget_add_css_class (openrgb_btn, "accent");
      gtk_widget_set_valign (openrgb_btn, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (cpu_lighting_header), openrgb_btn);
      g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (on_cpu_openrgb_clicked), page);
    }

  g_signal_connect (page->save_btn, "clicked", G_CALLBACK (on_cpu_save), page);

  g_object_ref_sink (page->box);

  /* Store vulns group reference for dynamic row addition */
  {
    GtkWidget *vulns_group = GTK_WIDGET (gtk_builder_get_object (builder, "cpu-vulns-section"));
    if (vulns_group)
      g_object_set_data (G_OBJECT (page->box), "cpu-vulns-group", vulns_group);
  }

  /* Populate all labels */
  populate_cpu_labels (page);

  g_object_unref (builder);

  return page;
}
