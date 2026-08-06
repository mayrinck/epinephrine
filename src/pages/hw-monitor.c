#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-monitor.h"
#include "hw-gpu.h"
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

  /* Update labels every 1s */
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
  /* Data poll every 1s (ALWAYS 1s, never changes) */
  page->monitor_data_timer_id = g_timeout_add (1000, monitor_poll_cb, page);
  /* Graph redraw at configurable speed */
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
  /* Clear history on explicit restart (e.g. graph speed change) */
  page->monitor_history_count = 0;
  memset (page->monitor_history, 0, sizeof (page->monitor_history));
  hw_monitor_start (page);
}

gboolean
hw_ram_monitor_tick (gpointer user_data)
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
