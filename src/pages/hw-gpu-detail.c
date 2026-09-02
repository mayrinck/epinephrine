#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-gpu-detail.h"
#include "hw-gpu.h"
#include "hw-monitor.h"
#include "hw-detail.h"
#include "../epi-privilege.h"
#include "../epi-i18n.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define tr(x) epi_i18n_get (x)

/* ---- Row widget pointers for visibility filtering ----
   AdwPreferencesGroup wraps children in an internal GtkBox, so iterating
   gtk_widget_get_first_child() on the group never reaches the rows.
   We store direct pointers to the row widgets during build and iterate
   those during show. */

#define MAX_GPU_MGT_ROWS 16
static GtkWidget *gpu_mgt_rows[MAX_GPU_MGT_ROWS];
static int n_gpu_mgt_rows = 0;

/* ---- GPU config persistence ---- */

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

/* ---- OpenRGB ---- */

void
hw_openrgb_launch (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  (void) user_data;
  g_spawn_async (NULL, (char *[]) { "openrgb", NULL }, NULL,
    G_SPAWN_DEFAULT | G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

/* ---- Scale ↔ spin sync helpers ---- */

static void sync_spin_to_scale (GtkSpinButton *, gpointer);

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

/* ---- AMD / generic management callbacks ---- */

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
            tr ("Permission denied. Run as root."));
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
  hw_gpu_nvidia_manage (args);
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
      hw_gpu_nvidia_manage (args);
      g_free (args);
    }
  else
    {
      /* Reset to auto fan control */
      char *args = g_strdup_printf ("-g %d -f -1",
        page->gpu->nvidia_gpu_index);
      hw_gpu_nvidia_manage (args);
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
  hw_gpu_nvidia_manage (args);
  g_free (args);
}

/* ---- GPU management feature filter ----
   Each management row is tagged with a power-id and resolved through this
   table. A feature only shows when (1) its name-based vendor rule matches
   (AMD/Radeon/RDNA => AMD only, NVIDIA/GeForce => NVIDIA only), (2) its
   explicit vendor tag matches the GPU vendor, and (3) the card/driver is
   actually capable of it. */

typedef gboolean (*GpuFeatureCheck) (const GpuInfo *);

typedef struct
{
  const char *id;
  const char *name;
  const char *vendor;
  GpuFeatureCheck check;
} GpuMgtFeature;

static gboolean
gpu_feat_perf_level (const GpuInfo *gpu)
{
  return gpu->has_perf_level;
}

static gboolean
gpu_feat_power_profile (const GpuInfo *gpu)
{
  return gpu->has_power_profile;
}

static gboolean
gpu_feat_power_limit (const GpuInfo *gpu)
{
  return gpu->has_power_limit;
}

static gboolean
gpu_feat_chill (const GpuInfo *gpu)
{
  return gpu->has_radeon_chill;
}

static gboolean
gpu_feat_anti_lag (const GpuInfo *gpu)
{
  return gpu->has_anti_lag;
}

static gboolean
gpu_feat_vrs (const GpuInfo *gpu)
{
  return gpu->has_vrs;
}

static gboolean
gpu_feat_fan (const GpuInfo *gpu)
{
  return gpu->has_fan_control;
}

static gboolean
gpu_feat_zero_rpm (const GpuInfo *gpu)
{
  return gpu->has_zero_rpm_fan;
}

static gboolean
gpu_feat_nvidia (const GpuInfo *gpu)
{
  return gpu->has_nvidia_smi;
}

static const GpuMgtFeature gpu_mgt_features[] = {
  { "perf-level",    "Performance Mode",            "AMD",    gpu_feat_perf_level },
  { "power-profile", "Power Profile",               "AMD",    gpu_feat_power_profile },
  { "power-limit",   "Power Limit (max)",           "AMD",    gpu_feat_power_limit },
  { "chill",         "Radeon Chill",                "AMD",    gpu_feat_chill },
  { "anti-lag",      "Radeon Anti-Lag",             "AMD",    gpu_feat_anti_lag },
  { "vrs",           "Variable Rate Shading",       NULL,     gpu_feat_vrs },
  { "fan",           "Fan Control",                 NULL,     gpu_feat_fan },
  { "zero-rpm",      "Zero RPM Fan",                "AMD",    gpu_feat_zero_rpm },
  { "nvidia-power",  "NVIDIA Power Limit",          "NVIDIA", gpu_feat_nvidia },
  { "nvidia-fan",    "NVIDIA Fan Speed",            "NVIDIA", gpu_feat_nvidia },
};

static const GpuMgtFeature *
gpu_mgt_feature_lookup (const char *id)
{
  for (guint i = 0; i < G_N_ELEMENTS (gpu_mgt_features); i++)
    if (g_strcmp0 (gpu_mgt_features[i].id, id) == 0)
      return &gpu_mgt_features[i];
  return NULL;
}

static gboolean
gpu_mgt_feature_visible (const HardwarePage *page, const GpuMgtFeature *f)
{
  const GpuInfo *gpu = page->gpu;
  const char *vendor = gpu ? gpu->vendor : NULL;

  /* 1) Name-based vendor rule (hard): AMD/Radeon/RDNA => AMD only,
        NVIDIA/GeForce => NVIDIA only */
  char *lower = g_ascii_strdown (f->name, -1);
  if (strstr (lower, "amd") || strstr (lower, "radeon") || strstr (lower, "rdna"))
    {
      g_free (lower);
      return g_strcmp0 (vendor, "AMD") == 0 && f->check (gpu);
    }
  if (strstr (lower, "nvidia") || strstr (lower, "geforce"))
    {
      g_free (lower);
      return g_strcmp0 (vendor, "NVIDIA") == 0 && f->check (gpu);
    }
  g_free (lower);

  /* 2) Explicit vendor tag: strict match (hidden when the GPU vendor is
        unknown as well) */
  if (f->vendor && g_strcmp0 (f->vendor, vendor) != 0)
    return FALSE;

  /* 3) Capability / driver support */
  return f->check (gpu);
}

/* ---- GPU detail page ---- */

void
hw_gpu_detail_build (HardwarePage *page)
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
  page->gpu_mgt_group = ADW_EXPANDER_ROW (gtk_builder_get_object (builder, "gpu-mgt-toggle"));

  n_gpu_mgt_rows = 0;

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
    hw_monitor_graph_draw, page, NULL);
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
        hw_monitor_metric_dot_draw, GINT_TO_POINTER (i), NULL);

      GtkWidget *lbl = gtk_label_new (tr (metric_names[i]));
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

  g_object_ref_sink (page->gpu_detail_box);
  g_object_unref (builder);

  /* ======= GPU Management Group (rows added programmatically) ======= */

  /* 1 - Performance Level */
  {
    page->perf_level_dropdown = gtk_drop_down_new (
      G_LIST_MODEL (gtk_string_list_new (
        (const char *[]){ tr ("Auto"), tr ("Low"), tr ("High"), tr ("Manual"), NULL })), NULL);
    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), tr ("Performance Mode"));
    adw_action_row_set_subtitle (row, tr (
      "Forces the GPU to a specific performance level: auto, low power, high performance, or manual."));
    adw_action_row_add_suffix (row, page->perf_level_dropdown);
    g_object_set_data (G_OBJECT (row), "power-id", "perf-level");
    adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (row));
    gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (row);
  }

  /* 2 - Power Profile Mode */
  page->power_profile_dd = gtk_drop_down_new (
    G_LIST_MODEL (gtk_string_list_new (
      (const char *[]){ tr ("Auto"), tr ("3D_FULL_SCREEN"), tr ("POWER_SAVING"), tr ("VIDEO"), tr ("VR"), tr ("COMPUTE"), NULL })), NULL);
  AdwActionRow *pp_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pp_row), tr ("Power Profile"));
  adw_action_row_set_subtitle (pp_row, tr (
    "Selects the clock and power switching heuristic for the GPU."));
  adw_action_row_add_suffix (pp_row, page->power_profile_dd);
  g_object_set_data (G_OBJECT (pp_row), "power-id", "power-profile");
  adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (pp_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (pp_row);

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
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pl_row), tr ("Power Limit (max)"));
    adw_action_row_set_subtitle (pl_row, tr (
      "Sets the maximum power consumption of the GPU in watts."));
    adw_action_row_add_suffix (pl_row, pl_hbox);
    g_object_set_data (G_OBJECT (pl_row), "power-id", "power-limit");
    adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (pl_row));
    gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (pl_row);
  }

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
  GtkWidget *min_lbl = gtk_label_new (tr ("Min FPS"));
  gtk_box_append (GTK_BOX (min_row), min_lbl);
  gtk_box_append (GTK_BOX (min_row), page->chill_min_scale);
  gtk_box_append (GTK_BOX (min_row), page->chill_min_spin);
  gtk_box_append (GTK_BOX (page->chill_opts), min_row);
  GtkWidget *max_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *max_lbl = gtk_label_new (tr ("Max FPS"));
  gtk_box_append (GTK_BOX (max_row), max_lbl);
  gtk_box_append (GTK_BOX (max_row), page->chill_max_scale);
  gtk_box_append (GTK_BOX (max_row), page->chill_max_spin);
  gtk_box_append (GTK_BOX (page->chill_opts), max_row);
  gtk_widget_set_margin_start (page->chill_opts, 16);
  AdwActionRow *chill_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (chill_row), tr ("Radeon Chill"));
  adw_action_row_set_subtitle (chill_row, tr (
    "Dynamically regulates your game's frame rate based on your movements."));
  adw_action_row_add_suffix (chill_row, page->chill_switch);
  g_object_set_data (G_OBJECT (chill_row), "power-id", "chill");
  adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (chill_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (chill_row);

  /* 6 - Anti-Lag */
  page->anti_lag_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->anti_lag_switch, GTK_ALIGN_CENTER);
  AdwActionRow *al_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (al_row), tr ("Radeon Anti-Lag"));
  adw_action_row_set_subtitle (al_row, tr (
    "Reduces input lag by pacing GPU frames."));
adw_action_row_add_suffix (al_row, page->anti_lag_switch);
  g_object_set_data (G_OBJECT (al_row), "power-id", "anti-lag");
  adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (al_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (al_row);

  /* 7 - Variable Rate Shading */
  page->vrs_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->vrs_switch, GTK_ALIGN_CENTER);
  GtkStringList *vrs_model = gtk_string_list_new (
    (const char *[]){ "2x2", "4x4", NULL });
  page->vrs_dd = gtk_drop_down_new (G_LIST_MODEL (vrs_model), NULL);
  page->vrs_opts = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *vrs_rate_lbl = gtk_label_new (
    tr ("Shading Rate"));
  gtk_box_append (GTK_BOX (page->vrs_opts), vrs_rate_lbl);
  gtk_box_append (GTK_BOX (page->vrs_opts), page->vrs_dd);
  gtk_widget_set_margin_start (page->vrs_opts, 16);
  AdwActionRow *vrs_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vrs_row), tr ("Variable Rate Shading"));
  adw_action_row_set_subtitle (vrs_row, tr (
    "Reduces shading rate to boost performance (AMD RDNA2+ or Intel Arc GPUs)."));
  adw_action_row_add_suffix (vrs_row, page->vrs_switch);
  g_object_set_data (G_OBJECT (vrs_row), "power-id", "vrs");
    adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (vrs_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (vrs_row);

  /* 8 - Fan Control */
  page->fan_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->fan_switch, GTK_ALIGN_CENTER);
  page->fan_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
  gtk_widget_set_size_request (page->fan_scale, 200, -1);
  AdwActionRow *fan_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (fan_row), tr ("Fan Control"));
  adw_action_row_set_subtitle (fan_row, tr (
    "Enables manual fan speed control (PWM). Toggle on to set speed."));
  adw_action_row_add_suffix (fan_row, page->fan_switch);
  g_object_set_data (G_OBJECT (fan_row), "power-id", "fan");
  adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (fan_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (fan_row);

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
  GtkWidget *zr_temp_lbl = gtk_label_new (tr ("Stop Temp"));
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), zr_temp_lbl);
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), page->zero_rpm_temp_scale);
  gtk_box_append (GTK_BOX (page->zero_rpm_opts), page->zero_rpm_temp_spin);
  gtk_widget_set_margin_start (page->zero_rpm_opts, 16);
  AdwActionRow *zr_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (zr_row), tr ("Zero RPM Fan"));
  adw_action_row_set_subtitle (zr_row, tr (
    "Allows the GPU fan to stop completely when below the temperature threshold."));
  adw_action_row_add_suffix (zr_row, page->zero_rpm_switch);
  g_object_set_data (G_OBJECT (zr_row), "power-id", "zero-rpm");
  adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (zr_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (zr_row);

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
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (np_row), tr ("NVIDIA Power Limit"));
  adw_action_row_set_subtitle (np_row, tr (
    "Sets maximum power in watts for the NVIDIA GPU (via nvidia-smi)."));
  adw_action_row_add_suffix (np_row, np_hbox);
  g_object_set_data (G_OBJECT (np_row), "power-id", "nvidia-power");
    adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (np_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (np_row);

  /* 11 - NVIDIA Fan Control */
  page->nvidia_fan_switch = gtk_switch_new ();
  gtk_widget_set_valign (page->nvidia_fan_switch, GTK_ALIGN_CENTER);
  page->nvidia_fan_scale = gtk_scale_new_with_range (
    GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_widget_set_size_request (page->nvidia_fan_scale, 200, -1);
  AdwActionRow *nf_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (nf_row), tr ("NVIDIA Fan Speed"));
  adw_action_row_set_subtitle (nf_row, tr (
    "Controls NVIDIA GPU fan speed. Requires CoolBits enabled in nvidia-settings."));
  adw_action_row_add_suffix (nf_row, page->nvidia_fan_switch);
  g_object_set_data (G_OBJECT (nf_row), "power-id", "nvidia-fan");
    adw_expander_row_add_row (page->gpu_mgt_group, GTK_WIDGET (nf_row));
  gpu_mgt_rows[n_gpu_mgt_rows++] = GTK_WIDGET (nf_row);

  /* UI file handles specs_group, specs_container and lighting_group */
}

void
hw_gpu_detail_load_config (HardwarePage *page)
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

void
hw_gpu_detail_show (HardwarePage *page)
{
  if (!page->gpu) return;

  gtk_stack_set_visible_child_name (page->detail_stack, "gpu");

  /* Brand icon */
  const char *brand_res = hw_gpu_brand_resource (page->gpu);
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
      g_snprintf (vram_str, sizeof (vram_str), tr ("VRAM: %.1f GB"), gb);
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
            tr ("This information may be inaccurate or outdated"));
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
        tr ("Release: %s"),
        page->gpu->gpu_release_date);
      gtk_label_set_text (GTK_LABEL (page->gpu_release_label), date_str);
    }
  else
    gtk_label_set_text (GTK_LABEL (page->gpu_release_label), "");

  /* Populate CPU-X style spec rows */
  {
    adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_vendor_row),
      page->gpu->vendor ? page->gpu->vendor : "—");
  }
  {
    char driver_str[128];
    if (g_strcmp0 (page->gpu->kernel_driver, "xe") == 0)
      g_snprintf (driver_str, sizeof (driver_str), "%s",
        tr ("Intel Open Graphics Driver (Xe)"));
    else if (g_strcmp0 (page->gpu->kernel_driver, "amdgpu") == 0)
      g_snprintf (driver_str, sizeof (driver_str), "%s",
        tr ("AMD Open Graphics Driver (amdgpu)"));
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
              tr (
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
        tr ("Yes") :
        tr ("No"));
  }
  {
    adw_action_row_set_subtitle (ADW_ACTION_ROW (page->gpu_deviceid_row),
      page->gpu->device_id ? page->gpu->device_id : "—");
  }

  /* Populate specs from GPU_INDEX.md */
  hw_gpu_populate_specs (page);

  /* Radeon Chill — show/hide chill options */
  if (page->gpu->has_radeon_chill)
    {
      gtk_widget_set_visible (page->chill_opts,
        gtk_switch_get_active (GTK_SWITCH (page->chill_switch)));
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

  /* Hide/show each management row based on vendor rules and capability */
  for (int i = 0; i < n_gpu_mgt_rows; i++)
    {
      const char *pid = g_object_get_data (G_OBJECT (gpu_mgt_rows[i]), "power-id");
      if (pid)
        {
          const GpuMgtFeature *f = gpu_mgt_feature_lookup (pid);
          gtk_widget_set_visible (gpu_mgt_rows[i],
            f ? gpu_mgt_feature_visible (page, f) : TRUE);
        }
    }

  hw_monitor_start (page);
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

void
hw_gpu_detail_connect_signals (HardwarePage *page)
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

void
hw_gpu_detail_connect_nvidia_signals (HardwarePage *page)
{
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
}
