#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-monitor.h"
#include "hw-detail.h"
#include "hw-gpu.h"
#include "page-settings.h"
#include "../epi-i18n.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ---- Live GPU / RAM monitoring ---- */

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

void
hw_monitor_metric_dot_draw (GtkDrawingArea *area, cairo_t *cr,
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

void
hw_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
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

      /* Fixed-width slots: divide by (120-1) so each point has constant spacing */
      double px[120], py[120];
      for (int i = 0; i < max_pts; i++)
        {
          int idx = (n - max_pts + i) % 120;
          float val = page->monitor_history[m][idx];
          px[i] = plot_x + i * plot_w / 119.0;
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
      gboolean nv_ok = hw_gpu_nvidia_query (vals, ok,
        page->gpu->nvidia_gpu_index);
      /* nvidia-smi is present but returned no usable data */
      if (!nv_ok && page->monitor_notice)
        {
          gtk_label_set_text (GTK_LABEL (page->monitor_notice),
            tr (
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
            tr (
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
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
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
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[2] = raw; ok[2] = TRUE; }
        }

      /* 3 - Memory Clock — not available in Xe sysfs */

      /* 4 - VRAM Used (bytes → GB) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "mem_info_vram_used", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
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
          uint64_t energy = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (energy == 0)
            {
              p = g_build_filename (hwmon, "energy2_input", NULL);
              energy = hw_gpu_read_sysfs_u64 (p);
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
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[6] = raw / 1000.0f; ok[6] = TRUE; }
        }

      /* 7 - Fan Speed (RPM) — show 0 if sensor exists */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "fan1_input", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
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
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[0] = raw / 1000.0f; ok[0] = TRUE; }
        }

      /* 1 - GPU Usage (percent) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "device_busy_percent", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0 || exists)
            { vals[1] = raw; ok[1] = TRUE; }
        }

      /* 2 - Core Clock (MHz) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "freq0_input", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[2] = raw / 1000000.0f; ok[2] = TRUE; }
        }

      /* 3 - Memory Clock (MHz) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "freq1_input", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[3] = raw / 1000000.0f; ok[3] = TRUE; }
        }

      /* 4 - VRAM Used (bytes → GB) */
      if (sysfs)
        {
          char *p = g_build_filename (sysfs, "mem_info_vram_used", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[4] = raw / (1024.0f * 1024.0f * 1024.0f); ok[4] = TRUE; }
        }

      /* 5 - Power (microwatts → W) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "power1_average", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[5] = raw / 1000000.0f; ok[5] = TRUE; }
        }

      /* 6 - Voltage (millivolts) */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "in0_input", NULL);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (raw > 0)
            { vals[6] = raw / 1000.0f; ok[6] = TRUE; }
        }

      /* 7 - Fan Speed (RPM) — show 0 if sensor exists */
      if (hwmon)
        {
          char *p = g_build_filename (hwmon, "fan1_input", NULL);
          gboolean exists = (access (p, F_OK) == 0);
          uint64_t raw = hw_gpu_read_sysfs_u64 (p);
          g_free (p);
          if (exists)
            { vals[7] = raw; ok[7] = TRUE; }
        }
    }

  /* Update labels with value + 10s rolling average */
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

  {
    gboolean any_data = FALSE;
    for (int i = 0; i < 8; i++)
      {
        page->monitor_avail[i] = ok[i];
        if (ok[i])
          {
            any_data = TRUE;

            /* Store raw value in circular buffer for average */
            int idx = page->monitor_avg_idx[i];
            page->monitor_avg_buf[i][idx % 400] = vals[i];
            page->monitor_avg_idx[i] = idx + 1;
            if (page->monitor_avg_count[i] < 400)
              page->monitor_avg_count[i]++;

            /* Compute rolling average */
            double sum = 0;
            int cnt = page->monitor_avg_count[i];
            for (int j = 0; j < cnt; j++)
              sum += page->monitor_avg_buf[i][j];
            double avg = sum / cnt;

            char buf[64];
            const char *f = page->gpu->has_nvidia_smi ? nvidia_fmt8[i] : fmt8[i];
            char val_str[32];
            g_snprintf (val_str, sizeof (val_str), f, vals[i]);
            char avg_str[32];
            g_snprintf (avg_str, sizeof (avg_str), f, avg);
            g_snprintf (buf, sizeof (buf), "%s (avg: %s)", val_str, avg_str);
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

  return G_SOURCE_CONTINUE;
}

/* ---- Graph redraw callback (configurable speed) ---- */

static gboolean
gpu_graph_redraw_cb (gpointer user_data)
{
  HardwarePage *page = user_data;
  if (!page) return G_SOURCE_REMOVE;
  gtk_widget_queue_draw (page->monitor_graph);
  return G_SOURCE_CONTINUE;
}

void
hw_monitor_start (HardwarePage *page)
{
  if (!page) return;
  /* If both timers already running, nothing to do */
  if (page->monitor_data_timer_id && page->monitor_timer_id) return;
  /* Clean up stale timers before creating new ones */
  if (page->monitor_data_timer_id)
    {
      g_source_remove (page->monitor_data_timer_id);
      page->monitor_data_timer_id = 0;
    }
  if (page->monitor_timer_id)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  extern int g_graph_speed_ms;
  /* Data poll at configured graph speed */
  page->monitor_data_timer_id = g_timeout_add (g_graph_speed_ms, monitor_poll_cb, page);
  /* Graph redraw at same interval — keeps animation smooth */
  page->monitor_timer_id = g_timeout_add (g_graph_speed_ms, gpu_graph_redraw_cb, page);
  monitor_poll_cb (page);
}

void
hw_monitor_restart (HardwarePage *page)
{
  if (!page) return;
  if (page->monitor_data_timer_id)
    {
      g_source_remove (page->monitor_data_timer_id);
      page->monitor_data_timer_id = 0;
    }
  if (page->monitor_timer_id)
    {
      g_source_remove (page->monitor_timer_id);
      page->monitor_timer_id = 0;
    }
  hw_monitor_start (page);
}

/* ---- RAM monitor graph ---- */

void
hw_ram_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
    int width, int height, gpointer user_data)
{
  HardwarePage *page = user_data;
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

  int n = page->ram_monitor_history_count;
  if (n <= 0) return;
  int pts = MIN (n, RAM_MON_HISTORY);
  if (pts < 2) return;

  static const GdkRGBA colors[RAM_MON_METRICS] = {
    { 0.200, 0.820, 0.478, 0.9 },  /* RAM usage (green)      */
    { 0.965, 0.827, 0.176, 0.9 },  /* Swap usage (yellow)    */
  };

  double plot_w = width - 34.0;
  double plot_h = height - 12.0;
  double x0 = 30.0;
  double y0 = 6.0;

  for (int m = 0; m < RAM_MON_METRICS; m++)
    {
      if (!page->ram_monitor_avail[m]) continue;

      const GdkRGBA *c = &colors[m];
      cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha);
      cairo_set_line_width (cr, 1.5);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

      int start = n - pts;
      for (int i = 0; i < pts - 1; i++)
        {
          int idx0 = (start + i) % RAM_MON_HISTORY;
          int idx1 = (start + i + 1) % RAM_MON_HISTORY;

          float v0 = CLAMP (page->ram_monitor_history[m][idx0], 0.0f, 1.0f);
          float v1 = CLAMP (page->ram_monitor_history[m][idx1], 0.0f, 1.0f);

          /* Fixed-width slots: divide by (HISTORY-1) so each point has constant spacing */
          double px0 = x0 + plot_w * i / (RAM_MON_HISTORY - 1);
          double px1 = x0 + plot_w * (i + 1) / (RAM_MON_HISTORY - 1);
          double py0 = y0 + plot_h * (1.0 - v0);
          double py1 = y0 + plot_h * (1.0 - v1);

          /* Hermite interpolation */
          double dx = (px1 - px0) / 8.0;
          double dy0, dy1;

          if (i > 0)
            {
              int idx_prev = (start + i - 1) % RAM_MON_HISTORY;
              float vp = CLAMP (page->ram_monitor_history[m][idx_prev], 0.0f, 1.0f);
              dy0 = (v1 - vp) / 2.0 * plot_h;
            }
          else
            dy0 = (v1 - v0) * plot_h;

          if (i < pts - 2)
            {
              int idx_next = (start + i + 2) % RAM_MON_HISTORY;
              float vn = CLAMP (page->ram_monitor_history[m][idx_next], 0.0f, 1.0f);
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

static gboolean
ram_data_poll_cb (gpointer user_data)
{
  HardwarePage *page = user_data;
  if (!page) return G_SOURCE_REMOVE;

  FILE *f = fopen ("/proc/meminfo", "r");
  if (!f) return G_SOURCE_CONTINUE;

  char buf[256];
  uint64_t mem_total_kb = 0, mem_avail_kb = 0;
  uint64_t swap_total_kb = 0, swap_free_kb = 0;
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
      else if (g_str_has_prefix (buf, "SwapTotal:"))
        {
          char *v = strchr (buf, ':') + 1;
          while (*v == ' ') v++;
          swap_total_kb = strtoull (v, NULL, 10);
        }
      else if (g_str_has_prefix (buf, "SwapFree:"))
        {
          char *v = strchr (buf, ':') + 1;
          while (*v == ' ') v++;
          swap_free_kb = strtoull (v, NULL, 10);
        }
    }
  fclose (f);

  uint64_t swap_used_kb = swap_total_kb > swap_free_kb
    ? swap_total_kb - swap_free_kb : 0;

  page->ram_total_bytes = mem_total_kb * 1024;
  page->ram_used_bytes = (mem_total_kb - mem_avail_kb) * 1024;

  /* Refresh static info labels */
  char *total_str = ram_size_mb_str (page->ram_total_bytes >> 20);
  gtk_label_set_text (GTK_LABEL (page->ram_total_label), total_str);
  g_free (total_str);

  if (page->ram_freq_mhz > 0)
    {
      char freq_str[32];
      g_snprintf (freq_str, sizeof (freq_str), "%ld MHz", (long) page->ram_freq_mhz);
      gtk_label_set_text (GTK_LABEL (page->ram_freq_label), freq_str);
    }

  /* Metrics: 0 = RAM usage (% of total), 1 = swap usage (% of SwapTotal) */
  float vals[RAM_MON_METRICS] = { 0.0f, 0.0f };
  gboolean ok[RAM_MON_METRICS] = { FALSE, FALSE };

  /* Raw GB values for legend display */
  double ram_used_gb = 0, ram_total_gb = 0;
  double swap_used_gb = 0, swap_total_gb = 0;

  if (page->ram_total_bytes > 0)
    {
      vals[0] = (float) (page->ram_used_bytes * 100.0 / page->ram_total_bytes);
      ram_used_gb = page->ram_used_bytes / (1024.0 * 1024.0 * 1024.0);
      ram_total_gb = page->ram_total_bytes / (1024.0 * 1024.0 * 1024.0);
      ok[0] = TRUE;
    }
  if (swap_total_kb > 0)
    {
      vals[1] = (float) (swap_used_kb * 100.0 / swap_total_kb);
      swap_used_gb = swap_used_kb / (1024.0 * 1024.0);
      swap_total_gb = swap_total_kb / (1024.0 * 1024.0);
      ok[1] = TRUE;
    }

  int n = page->ram_monitor_history_count;
  for (int m = 0; m < RAM_MON_METRICS; m++)
    {
      page->ram_monitor_avail[m] = ok[m];
      if (ok[m])
        {
          page->ram_monitor_history[m][n % RAM_MON_HISTORY] =
            CLAMP (vals[m] / 100.0f, 0.0f, 1.0f);

          char bufv[96];
          if (m == 0)
            g_snprintf (bufv, sizeof (bufv), "%.1f / %.1f GB (%.0f%%)",
              ram_used_gb, ram_total_gb, vals[m]);
          else
            g_snprintf (bufv, sizeof (bufv), "%.1f / %.1f GB (%.0f%%)",
              swap_used_gb, swap_total_gb, vals[m]);
          gtk_label_set_text (GTK_LABEL (page->ram_monitor_values[m]), bufv);
        }
      else
        gtk_label_set_text (GTK_LABEL (page->ram_monitor_values[m]), "N/A");
    }
  page->ram_monitor_history_count = n + 1;

  return G_SOURCE_CONTINUE;
}

static gboolean
ram_graph_redraw_cb (gpointer user_data)
{
  HardwarePage *page = user_data;
  if (!page) return G_SOURCE_REMOVE;
  gtk_widget_queue_draw (page->ram_graph);
  return G_SOURCE_CONTINUE;
}

void
hw_ram_monitor_start (HardwarePage *page)
{
  if (!page) return;
  /* If both timers already running, nothing to do */
  if (page->ram_data_timer_id && page->ram_graph_timer_id) return;
  /* Clean up stale timers before creating new ones */
  if (page->ram_data_timer_id)
    {
      g_source_remove (page->ram_data_timer_id);
      page->ram_data_timer_id = 0;
    }
  if (page->ram_graph_timer_id)
    {
      g_source_remove (page->ram_graph_timer_id);
      page->ram_graph_timer_id = 0;
    }
  extern int g_graph_speed_ms;
  /* Data poll at configured graph speed */
  page->ram_data_timer_id = g_timeout_add (g_graph_speed_ms, ram_data_poll_cb, page);
  /* Graph redraw at same interval — keeps animation smooth */
  page->ram_graph_timer_id = g_timeout_add (g_graph_speed_ms, ram_graph_redraw_cb, page);
  ram_data_poll_cb (page);
}

void
hw_ram_monitor_stop (HardwarePage *page)
{
  if (!page) return;
  if (page->ram_data_timer_id)
    {
      g_source_remove (page->ram_data_timer_id);
      page->ram_data_timer_id = 0;
    }
  if (page->ram_graph_timer_id)
    {
      g_source_remove (page->ram_graph_timer_id);
      page->ram_graph_timer_id = 0;
    }
}

void
hw_ram_monitor_restart (HardwarePage *page)
{
  if (!page) return;
  /* Only restart when the RAM monitor is currently active */
  if (!page->ram_data_timer_id && !page->ram_graph_timer_id)
    return;
  hw_ram_monitor_stop (page);
  hw_ram_monitor_start (page);
}
