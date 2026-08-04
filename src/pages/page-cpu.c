#include "page-cpu.h"
#include "page-dashboard.h"
#include "epi-privilege.h"
#include "../epi-i18n.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ---- Metric colors ---- */

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

static const float cpu_norm_max[CPU_MON_METRICS] = {
  100.0f,  /* Temperature */
  100.0f,  /* Usage */
  5000.0f, /* Speed */
  1.5f,    /* Voltage */
};

/* ---- Graph draw callback ---- */

static void
cpu_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
                        int width, int height, gpointer user_data)
{
  CpuPage *page = user_data;
  (void) area;

  /* Transparent background — no fill */

  /* Grid lines + percentage labels */
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.2);
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

  /* Draw traces — always use full history size for fixed-width slots */
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
      cairo_set_line_width (cr, 1.5);

      int start = (n - pts);
      for (int i = 0; i < pts - 1; i++)
        {
          int idx0 = (start + i) % CPU_MON_HISTORY;
          int idx1 = (start + i + 1) % CPU_MON_HISTORY;

          float v0 = CLAMP (page->monitor_history[m][idx0], 0.0f, 1.0f);
          float v1 = CLAMP (page->monitor_history[m][idx1], 0.0f, 1.0f);

          /* Fixed-width slots: divide by (HISTORY-1) so each point has constant spacing */
          double px0 = x0 + plot_w * i / (CPU_MON_HISTORY - 1);
          double px1 = x0 + plot_w * (i + 1) / (CPU_MON_HISTORY - 1);
          double py0 = y0 + plot_h * (1.0 - v0);
          double py1 = y0 + plot_h * (1.0 - v1);

          /* Monotone cubic */
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
  for (int tz = 0; tz < 20; tz++)
    {
      g_snprintf (path, sizeof (path),
        "/sys/class/thermal/thermal_zone%d/temp", tz);
      uint64_t raw = read_sysfs_u64 (path);
      if (raw > 0 && raw < 200000)
        return raw / 1000.0f;
    }

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

  float temp  = read_cpu_temp ();
  float usage = read_cpu_usage (page);
  float freq  = read_cpu_freq_mhz ();
  float volt  = read_cpu_voltage ();

  float vals[CPU_MON_METRICS] = { temp, usage, freq, volt };
  gboolean ok[CPU_MON_METRICS] = {
    temp > 0,
    n > 0,
    freq > 0,
    volt > 0
  };

  for (int m = 0; m < CPU_MON_METRICS; m++)
    page->monitor_history[m][n % CPU_MON_HISTORY] =
      ok[m] ? CLAMP (vals[m] / cpu_norm_max[m], 0.0f, 1.0f) : 0.0f;

  page->monitor_history_count++;

  /* Update legend labels */
  for (int m = 0; m < CPU_MON_METRICS; m++)
    {
      if (ok[m])
        {
          char buf[32];
          if (m == 3)
            g_snprintf (buf, sizeof (buf), "%.3f %s", vals[m], cpu_metric_units[m]);
          else if (m == 2)
            g_snprintf (buf, sizeof (buf), "%.0f %s", vals[m], cpu_metric_units[m]);
          else
            g_snprintf (buf, sizeof (buf), "%.1f %s", vals[m], cpu_metric_units[m]);
          gtk_label_set_text (GTK_LABEL (page->monitor_values[m]), buf);
        }
      else
        gtk_label_set_text (GTK_LABEL (page->monitor_values[m]), "N/A");
    }

  /* Update current frequency row */
  if (page->cur_freq_row && freq > 0)
    {
      char freq_str[64];
      if (freq >= 1000.0f)
        g_snprintf (freq_str, sizeof (freq_str), "%.2f GHz", freq / 1000.0f);
      else
        g_snprintf (freq_str, sizeof (freq_str), "%.0f MHz", freq);
      adw_action_row_set_subtitle (page->cur_freq_row, freq_str);
    }

  return G_SOURCE_CONTINUE;
}

/* ---- Graph redraw callback (150ms) ---- */

static gboolean
cpu_graph_redraw_cb (gpointer user_data)
{
  CpuPage *page = user_data;
  if (!page) return G_SOURCE_REMOVE;
  gtk_widget_queue_draw (page->monitor_graph);
  return G_SOURCE_CONTINUE;
}

/* ---- Start / stop monitoring ---- */

void
cpu_monitor_start (CpuPage *page)
{
  if (!page) return;
  /* If both timers already running, nothing to do */
  if (page->monitor_timer_id && page->graph_timer_id) return;
  page->monitor_history_count = 0;
  memset (page->monitor_history, 0, sizeof (page->monitor_history));
  memset (&page->prev_idle, 0, sizeof (page->prev_idle) + sizeof (page->prev_total));
  /* Clean up stale timers before creating new ones */
  if (page->monitor_timer_id)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  if (page->graph_timer_id)
    {
      g_source_remove (page->graph_timer_id);
      page->graph_timer_id = 0;
    }
  /* Data poll every 1s (ALWAYS 1s, never changes) */
  page->monitor_timer_id = g_timeout_add (1000, cpu_monitor_poll_cb, page);
  /* Graph redraw at configurable interval */
  extern int g_graph_speed_ms;
  page->graph_timer_id = g_timeout_add (g_graph_speed_ms, cpu_graph_redraw_cb, page);
  cpu_monitor_poll_cb (page);
}

void
cpu_monitor_stop (CpuPage *page)
{
  if (!page) return;
  if (page->monitor_timer_id)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  if (page->graph_timer_id)
    {
      g_source_remove (page->graph_timer_id);
      page->graph_timer_id = 0;
    }
}

void
cpu_monitor_restart (CpuPage *page)
{
  if (!page) return;
  cpu_monitor_stop (page);
  cpu_monitor_start (page);
}

/* ---- Codename detection ---- */

static char *
detect_codename (const char *model_name)
{
  if (!model_name) return NULL;
  char *lower = g_ascii_strdown (model_name, -1);
  char *codename = NULL;

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
      else if (strstr (lower, "ai 9") || strstr (lower, "ai 7"))
        codename = g_strdup ("Strix Point");
      else if (strstr (lower, "7 7840") || strstr (lower, "7 8840") ||
               strstr (lower, "9 8945"))
        codename = g_strdup ("Hawk Point");
      else if (strstr (lower, "6 6600") || strstr (lower, "6 6800") ||
               strstr (lower, "6 6900"))
        codename = g_strdup ("Rembrandt");
      else if (strstr (lower, "5 5600") || strstr (lower, "5 5800"))
        codename = g_strdup ("Cezanne");
      else if (strstr (lower, "4 4600") || strstr (lower, "4 4800"))
        codename = g_strdup ("Renoir");
      else if (strstr (lower, "threadripper"))
        {
          if (strstr (lower, "79") || strstr (lower, "78"))
            codename = g_strdup ("Storm Peak");
          else if (strstr (lower, "39") || strstr (lower, "29"))
            codename = g_strdup ("Castle Peak");
          else
            codename = g_strdup ("Castle Peak");
        }
      else if (strstr (lower, "epyc"))
        {
          if (strstr (lower, "97") || strstr (lower, "96") || strstr (lower, "95"))
            codename = g_strdup ("Genoa");
          else if (strstr (lower, "77") || strstr (lower, "76"))
            codename = g_strdup ("Milan");
          else if (strstr (lower, "70") || strstr (lower, "60"))
            codename = g_strdup ("Rome");
          else
            codename = g_strdup ("EPYC");
        }
    }
  else if (strstr (lower, "intel") || strstr (lower, "core"))
    {
      if (strstr (lower, "ultra 9") || strstr (lower, "ultra 7") ||
          strstr (lower, "ultra 5"))
        codename = g_strdup ("Arrow Lake");
      else if (strstr (lower, "i9-13") || strstr (lower, "i7-13") ||
               strstr (lower, "i5-13") || strstr (lower, "i9-14") ||
               strstr (lower, "i7-14") || strstr (lower, "i5-14"))
        codename = g_strdup ("Raptor Lake");
      else if (strstr (lower, "i9-12") || strstr (lower, "i7-12") ||
               strstr (lower, "i5-12") || strstr (lower, "i3-12"))
        codename = g_strdup ("Alder Lake");
      else if (strstr (lower, "i9-11") || strstr (lower, "i7-11") ||
               strstr (lower, "i5-11"))
        codename = g_strdup ("Rocket Lake");
      else if (strstr (lower, "i9-10") || strstr (lower, "i7-10") ||
               strstr (lower, "i5-10") || strstr (lower, "i3-10"))
        codename = g_strdup ("Comet Lake");
      else if (strstr (lower, "i9-9") || strstr (lower, "i7-9") ||
               strstr (lower, "i5-9") || strstr (lower, "i3-9") ||
               strstr (lower, "i7-8") || strstr (lower, "i5-8") ||
               strstr (lower, "i3-8"))
        codename = g_strdup ("Coffee Lake");
      else if (strstr (lower, "i7-7") || strstr (lower, "i5-7") ||
               strstr (lower, "i3-7"))
        codename = g_strdup ("Kaby Lake");
      else if (strstr (lower, "i7-6") || strstr (lower, "i5-6") ||
               strstr (lower, "i3-6"))
        codename = g_strdup ("Skylake");
      else if (strstr (lower, "xeon"))
        codename = g_strdup ("Xeon");
      else
        codename = g_strdup ("Intel");
    }

  g_free (lower);
  return codename;
}

/* ---- Collect CPU vulnerabilities ---- */

static void
collect_vulnerabilities (CpuPage *page)
{
  char path[256];
  page->n_vulns = 0;

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
              if (colon) { colon += 2; g_strstrip (colon); page->model_name = g_strdup (colon); }
            }
          else if (g_str_has_prefix (buf, "vendor_id"))
            {
              char *colon = strchr (buf, ':');
              if (colon) { colon += 2; g_strstrip (colon); page->vendor_id = g_strdup (colon); }
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
              if (colon) { colon += 2; g_strstrip (colon); page->bogomips = g_strdup (colon); }
            }
          else if (g_str_has_prefix (buf, "microcode"))
            {
              char *colon = strchr (buf, ':');
              if (colon) { colon += 2; g_strstrip (colon); page->microcode = g_strdup (colon); }
            }
          else if (g_str_has_prefix (buf, "flags\t"))
            {
              char *colon = strchr (buf, ':');
              if (colon) { colon += 2; g_strstrip (colon); page->flags = g_strdup (colon); }
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
              if (val) { page->socket = g_strdup (val + 2); g_strstrip (page->socket); }
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
                { val++; g_strstrip (val); page->base_freq = g_strdup (val); }
            }
          else if (g_str_has_prefix (buf, "CPU max MHz:"))
            {
              char *val = strchr (buf, ':');
              if (val && !page->boost_freq)
                {
                  val++; g_strstrip (val);
                  char *dot = strstr (val, ".000");
                  if (dot) *dot = '\0';
                  page->boost_freq = g_strdup (val);
                }
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

  if (page->threads_total < 1)
    page->threads_total = page->n_cores;

  /* ---- Cache from sysfs fallback ---- */
  if (!page->l1d_cache)
    {
      uint64_t sz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cache/index0/size");
      if (sz > 0) page->l1d_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }
  if (!page->l2_cache)
    {
      uint64_t sz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cache/index2/size");
      if (sz > 0) page->l2_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }
  if (!page->l3_cache)
    {
      uint64_t sz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cache/index3/size");
      if (sz > 0) page->l3_cache = g_strdup_printf ("%lu KB", (unsigned long)sz);
    }

  /* ---- Base/Boost from sysfs fallback ---- */
  if (!page->base_freq)
    {
      uint64_t khz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
      if (khz > 0) page->base_freq = g_strdup_printf ("%.0f MHz", khz / 1000.0);
    }
  if (!page->boost_freq)
    {
      uint64_t khz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
      if (khz > 0) page->boost_freq = g_strdup_printf ("%.0f MHz", khz / 1000.0);
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
    uint64_t min_khz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
    uint64_t max_khz = read_sysfs_u64 ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    page->min_freq_hw = g_strdup_printf ("%lu", (unsigned long)min_khz);
    page->max_freq_hw = g_strdup_printf ("%lu", (unsigned long)max_khz);
  }

  /* ---- Vulnerabilities ---- */
  collect_vulnerabilities (page);
}

/* ---- Forward declarations ---- */
static void update_governor_subtitle (CpuPage *page, const char *gov_name);

/* ---- Populate all row subtitles ---- */

static void
populate_cpu_labels (CpuPage *page)
{
  /* Branding */
  if (page->model_name)
    gtk_label_set_text (GTK_LABEL (page->cpu_name_label), page->model_name);
  if (page->vendor_id)
    gtk_label_set_text (GTK_LABEL (page->cpu_vendor_label), page->vendor_id);

  /* Codename */
  if (page->model_name)
    {
      char *codename = detect_codename (page->model_name);
      if (codename)
        {
          gtk_label_set_text (GTK_LABEL (page->cpu_codename_label), codename);
          g_free (codename);
        }
    }

  /* Specifications — all use adw_action_row_set_subtitle */
  if (page->socket_row)
    adw_action_row_set_subtitle (page->socket_row, page->socket ? page->socket : "—");

  if (page->arch_row)
    adw_action_row_set_subtitle (page->arch_row, page->architecture ? page->architecture : "—");

  if (page->family_row)
    {
      char fms[64];
      g_snprintf (fms, sizeof (fms), "0x%X / 0x%X / %d",
                  page->family, page->model, page->stepping);
      adw_action_row_set_subtitle (page->family_row, fms);
    }

  if (page->cores_row)
    {
      char ct[64];
      g_snprintf (ct, sizeof (ct), "%d / %d",
                  page->cores_physical > 0 ? page->cores_physical : page->n_cores,
                  page->threads_total);
      adw_action_row_set_subtitle (page->cores_row, ct);
    }

  if (page->bogomips_row)
    adw_action_row_set_subtitle (page->bogomips_row, page->bogomips ? page->bogomips : "—");

  if (page->microcode_row)
    adw_action_row_set_subtitle (page->microcode_row, page->microcode ? page->microcode : "—");

  if (page->base_freq_row)
    adw_action_row_set_subtitle (page->base_freq_row, page->base_freq ? page->base_freq : "—");

  if (page->boost_freq_row)
    adw_action_row_set_subtitle (page->boost_freq_row, page->boost_freq ? page->boost_freq : "—");

  if (page->flags_row)
    adw_action_row_set_subtitle (page->flags_row, page->flags ? page->flags : "—");

  /* Caches */
  if (page->l1d_row)
    adw_action_row_set_subtitle (page->l1d_row, page->l1d_cache ? page->l1d_cache : "—");
  if (page->l1i_row)
    adw_action_row_set_subtitle (page->l1i_row, page->l1i_cache ? page->l1i_cache : "—");
  if (page->l2_row)
    adw_action_row_set_subtitle (page->l2_row, page->l2_cache ? page->l2_cache : "—");
  if (page->l3_row)
    adw_action_row_set_subtitle (page->l3_row, page->l3_cache ? page->l3_cache : "—");

  /* Vulnerabilities */
  if (page->vulns_group)
    {
      for (int v = 0; v < page->n_vulns; v++)
        {
          AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
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
          adw_preferences_group_add (page->vulns_group, GTK_WIDGET (row));
        }

      if (page->n_vulns == 0)
        {
          AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
            tr ("No known vulnerabilities detected"));
          adw_preferences_group_add (page->vulns_group, GTK_WIDGET (row));
        }
    }

  /* Governor — subtitle shows current state */
  if (page->governor_row)
    {
      char gov[64] = {0};
      if (read_file_line ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                          gov, sizeof (gov)))
        update_governor_subtitle (page, gov);
      else
        update_governor_subtitle (page, NULL);
    }

  /* Current frequency */
  if (page->cur_freq_row)
    {
      float mhz = read_cpu_freq_mhz ();
      if (mhz > 0)
        {
          char freq_str[64];
          if (mhz >= 1000.0f)
            g_snprintf (freq_str, sizeof (freq_str), "%.2f GHz", mhz / 1000.0f);
          else
            g_snprintf (freq_str, sizeof (freq_str), "%.0f MHz", mhz);
          adw_action_row_set_subtitle (page->cur_freq_row, freq_str);
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

/* ---- Update governor subtitle ---- */

static void
update_governor_subtitle (CpuPage *page, const char *gov_name)
{
  if (!page->governor_row) return;

  if (gov_name && gov_name[0])
    {
      char *sub = g_strdup_printf (
        "Define how the processes will be managed by the system. Current state: %s",
        gov_name);
      adw_action_row_set_subtitle (page->governor_row, sub);
      g_free (sub);
    }
  else
    {
      adw_action_row_set_subtitle (page->governor_row,
        "Define how the processes will be managed by the system.");
    }
}

/* ---- Apply governor on change ---- */

static void
apply_governor (CpuPage *page)
{
  const char *governor = NULL;
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (page->governor_dropdown));
  if (sel < (guint) page->n_governors)
    governor = page->available_governors[sel];

  if (governor && page->n_cores > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%s' '%s' > \"$d/scaling_governor\" 2>/dev/null; done",
        governor);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }

  update_governor_subtitle (page, governor);
}

static void
on_governor_changed (GtkDropDown *dd, GParamSpec *pspec, CpuPage *page)
{
  (void) dd; (void) pspec;
  apply_governor (page);
}

/* ---- Apply frequency limits on change ---- */

static void
apply_freq_limits (CpuPage *page)
{
  if (page->n_cores <= 0) return;

  int min_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->min_freq_spin));
  int max_freq = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (page->max_freq_spin));

  if (min_freq > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%d' %d > \"$d/scaling_min_freq\" 2>/dev/null; done",
        min_freq);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }

  if (max_freq > 0)
    {
      char *cmd = g_strdup_printf (
        "for d in /sys/devices/system/cpu/cpu*/cpufreq; do "
        "printf '%%d' %d > \"$d/scaling_max_freq\" 2>/dev/null; done",
        max_freq);
      epi_privilege_exec_async (cmd);
      g_free (cmd);
    }
}

static void
on_min_freq_changed (GtkSpinButton *spin, CpuPage *page)
{
  (void) spin;
  apply_freq_limits (page);
}

static void
on_max_freq_changed (GtkSpinButton *spin, CpuPage *page)
{
  (void) spin;
  apply_freq_limits (page);
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

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (page->monitor_graph),
    cpu_monitor_graph_draw, page, NULL);

  /* Build monitor legend rows */
  static const char *default_labels[CPU_MON_METRICS] = { "—", "—", "—", "—" };
  for (int i = 0; i < CPU_MON_METRICS; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_margin_start (row, 8);
      gtk_widget_set_margin_end (row, 8);

      GtkWidget *dot = gtk_drawing_area_new ();
      gtk_widget_set_size_request (dot, 10, 10);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot),
        cpu_metric_dot_draw, GINT_TO_POINTER (i), NULL);
      gtk_box_append (GTK_BOX (row), dot);

      page->monitor_labels[i] = gtk_label_new (cpu_metric_names[i]);
      gtk_widget_set_halign (page->monitor_labels[i], GTK_ALIGN_START);
      gtk_widget_set_hexpand (page->monitor_labels[i], TRUE);
      gtk_box_append (GTK_BOX (row), page->monitor_labels[i]);

      page->monitor_values[i] = gtk_label_new (default_labels[i]);
      gtk_widget_set_halign (page->monitor_values[i], GTK_ALIGN_END);
      gtk_label_set_width_chars (GTK_LABEL (page->monitor_values[i]), 12);
      gtk_box_append (GTK_BOX (row), page->monitor_values[i]);

      GtkWidget *unit_lbl = gtk_label_new (cpu_metric_units[i]);
      gtk_widget_set_halign (unit_lbl, GTK_ALIGN_START);
      gtk_label_set_width_chars (GTK_LABEL (unit_lbl), 4);
      gtk_box_append (GTK_BOX (row), unit_lbl);

      gtk_box_append (GTK_BOX (monitor_left), row);
    }

  /* Specifications rows */
  page->socket_row    = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-socket-row"));
  page->arch_row      = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-arch-row"));
  page->family_row    = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-family-row"));
  page->cores_row     = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-cores-row"));
  page->bogomips_row  = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-bogomips-row"));
  page->microcode_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-microcode-row"));
  page->base_freq_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-base-freq-row"));
  page->boost_freq_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-boost-freq-row"));
  page->flags_row     = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-flags-row"));

  /* Cache rows */
  page->l1d_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-l1d-row"));
  page->l1i_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-l1i-row"));
  page->l2_row  = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-l2-row"));
  page->l3_row  = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cpu-l3-row"));

  /* Vulnerabilities group */
  page->vulns_group = ADW_PREFERENCES_GROUP (gtk_builder_get_object (builder, "cpu-vulns-section"));

  /* CPU control */
  page->governor_dropdown = GTK_WIDGET (gtk_builder_get_object (builder, "governor-dropdown"));
  page->governor_row      = ADW_ACTION_ROW (gtk_builder_get_object (builder, "governor-row"));
  page->min_freq_spin        = GTK_WIDGET (gtk_builder_get_object (builder, "min-freq-spin"));
  page->max_freq_spin        = GTK_WIDGET (gtk_builder_get_object (builder, "max-freq-spin"));
  page->cur_freq_row         = ADW_ACTION_ROW (gtk_builder_get_object (builder, "cur-freq-row"));

  /* Collect static data first */
  collect_cpu_info (page);

  /* Dynamic governor dropdown */
  {
    GtkStringList *gov_list = GTK_STRING_LIST (
      gtk_builder_get_object (builder, "governor-stringlist"));

    guint n_items = g_list_model_get_n_items (G_LIST_MODEL (gov_list));
    for (guint i = 0; i < n_items; i++)
      gtk_string_list_remove (gov_list, 0);

    for (int i = 0; i < page->n_governors; i++)
      gtk_string_list_append (gov_list, page->available_governors[i]);

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

  /* Connect on-change signals — apply immediately, no save button */
  g_signal_connect (page->governor_dropdown, "notify::selected",
    G_CALLBACK (on_governor_changed), page);
  g_signal_connect (page->min_freq_spin, "value-changed",
    G_CALLBACK (on_min_freq_changed), page);
  g_signal_connect (page->max_freq_spin, "value-changed",
    G_CALLBACK (on_max_freq_changed), page);

  g_object_ref_sink (page->box);

  /* Populate all labels */
  populate_cpu_labels (page);

  g_object_unref (builder);

  return page;
}
