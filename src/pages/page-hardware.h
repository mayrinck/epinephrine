#pragma once

#include <adwaita.h>
#include "page-cpu.h"
#include "page-keyboard.h"
#include "page-mouse.h"

G_BEGIN_DECLS

typedef struct _ControllerState ControllerState;
typedef struct _UsbIds UsbIds;

typedef struct
{
  char *vendor;
  char *device_name;
  char *kernel_driver;
  char *driver_name;
  char *driver_info;
  char *vulkan_version;
  uint64_t vram_total;
  uint64_t vram_used;
  char *sysfs_path;
  char *hwmon_path;
  char *hwmon_temp_file;
  uint64_t power_last_energy;
  uint64_t power_last_time_ns;
  char *gpu_specs;
  char *gpu_specs_url;
  char *gpu_release_date;
  char *opengl_version;
  char *mesa_version;
  char *device_id;
  gboolean resizable_bar;
  gboolean has_perf_level;
  gboolean has_fan_control;
  gboolean has_radeon_chill;
  gboolean has_anti_lag;
  gboolean has_power_profile;
  gboolean has_power_limit;
  gboolean has_zero_rpm_fan;
  gboolean has_vrs;
  gboolean has_nvidia_smi;
  gboolean vram_from_index;
  gint nvidia_gpu_index;
  double nvidia_power_min;
  double nvidia_power_max;
} GpuInfo;

typedef struct
{
  GtkWidget *box;
  GtkWidget *device_list;
  GtkWidget *reload_btn;
  GtkStack *detail_stack;

  char **device_names;
  char **device_types;
  char **device_info;
  char **device_paths;
  uint16_t *device_vendor;
  uint16_t *device_product;
  char **device_urls;
  char **device_categories;
  int n_devices;

  GpuInfo *gpu;

  /* === GPU detail (AdwPreferencesPage) === */
  GtkWidget *gpu_brand_icon;
  GtkWidget *gpu_name_label;
  GtkWidget *gpu_vram_label;
  GtkWidget *gpu_release_label;
  GtkWidget *gpu_specs_btn;
  GtkWidget *gpu_specs_container;
  GtkWidget *gpu_detail_box;

  /* Motherboard detail boxes */
  GtkWidget *mobo_detail_box;

  /* RAM detail boxes */
  GtkWidget *ram_detail_box;

  GtkWidget *perf_level_dropdown;
  GtkWidget *fan_switch;
  GtkWidget *fan_scale;

  /* Section 3 spec rows (AdwActionRow) */
  GtkWidget *gpu_vk_row;
  GtkWidget *gpu_vram_specs_row;

  /* Radeon Chill */
  GtkWidget *chill_switch;
  GtkWidget *chill_opts;
  GtkWidget *chill_min_scale;
  GtkWidget *chill_min_spin;
  GtkWidget *chill_max_scale;
  GtkWidget *chill_max_spin;

  /* Anti-Lag */
  GtkWidget *anti_lag_switch;

  /* Power Profile Mode */
  GtkWidget *power_profile_dd;
  char **power_profile_names;
  int n_power_profiles;

  /* Power Limit */
  GtkWidget *power_limit_scale;
  GtkWidget *power_limit_spin;
  uint64_t power_limit_default;

  /* Zero RPM Fan */
  GtkWidget *zero_rpm_switch;
  GtkWidget *zero_rpm_opts;
  GtkWidget *zero_rpm_temp_scale;
  GtkWidget *zero_rpm_temp_spin;

  /* Variable Rate Shading */
  GtkWidget *vrs_switch;
  GtkWidget *vrs_opts;
  GtkWidget *vrs_dd;

  /* GPU Monitor */
  guint monitor_timer_id;
  guint monitor_data_timer_id;
  GtkWidget *monitor_panel;
  GtkWidget *monitor_graph;
  GtkWidget *monitor_notice;
  GtkWidget *monitor_labels[8];
  GtkWidget *monitor_values[8];
  gboolean monitor_avail[8];
  float monitor_history[8][120];
  int monitor_history_count;
  guint monitor_graph_width;

  /* NVIDIA management */
  GtkWidget *nvidia_power_scale;
  GtkWidget *nvidia_power_spin;
  GtkWidget *nvidia_fan_switch;
  GtkWidget *nvidia_fan_scale;

  /* GPU Management AdwPreferencesGroup */
  AdwPreferencesGroup *gpu_mgt_group;
  GtkWidget *gpu_specs_group;

  /* New CPU-X style rows for Section 3 */
  GtkWidget *gpu_vendor_row;
  GtkWidget *gpu_swdriver_row;
  GtkWidget *gpu_mesa_row;
  GtkWidget *gpu_opengl_row;
  GtkWidget *gpu_deviceid_row;
  GtkWidget *gpu_resizable_bar_row;

  /* Controller test */
  GtkWidget *controller_box;
  GtkWidget *controller_test_stack;
  GtkWidget *controller_name_label;
  GtkWidget *controller_info_label;
  GtkWidget *controller_website_btn;
  char *controller_url;
  GtkWidget *axis_lx, *axis_rx;
  GtkWidget *overlay;
  GtkWidget *svg_img;
  GtkWidget *indicators[24];
  GtkWidget *indicators_standard[24];
  GtkWidget *indicators_arcade[24];
  GtkWidget *analog_l_canvas;
  GtkWidget *analog_r_canvas;
  int controller_fd;
  guint controller_watch_id;
  ControllerState *ctrl_state;

  /* Per-layout controller widget storage (for conflict-free layout switching) */
  GtkWidget *analog_l_canvas_standard;
  GtkWidget *analog_l_canvas_arcade;
  GtkWidget *axis_lx_standard;
  GtkWidget *axis_rx_standard;
  GtkWidget *axis_lx_arcade;
  GtkWidget *axis_rx_arcade;

  /* Extra buttons settings group */
  AdwPreferencesGroup *extra_buttons_group;
  GtkWidget *extra_buttons_none;

  /* Motherboard info */
  char *mobo_vendor;
  char *mobo_model;
  char *mobo_bios_version;
  char *mobo_bios_vendor;
  gboolean mobo_detected;
  char *mobo_revision;
  char *mobo_bios_date;
  char *mobo_bios_romsize;
  char *mobo_bios_efi_pk;
  char *mobo_chipset_vendor;
  char *mobo_chipset_model;

  /* Motherboard detail */
  GtkWidget *mobo_name_label;
  GtkWidget *mobo_bios_label;
  GtkWidget *mobo_vendor_label;
  GtkWidget *mobo_model_label;
  GtkWidget *mobo_revision_label;
  GtkWidget *mobo_bios_manufacturer_label;
  GtkWidget *mobo_bios_detail_label;
  GtkWidget *mobo_bios_date_label;
  GtkWidget *mobo_bios_romsize_label;
  GtkWidget *mobo_bios_efi_pk_label;
  GtkWidget *mobo_chipset_vendor_label;
  GtkWidget *mobo_chipset_model_label;
  GtkWidget *mobo_smbus_toggle;
  GtkWidget *mobo_smbus_desc;
  GtkWidget *mobo_lighting_status;

  /* RAM metadata */
  uint64_t ram_total_bytes;
  uint64_t ram_used_bytes;
  char **ram_dimm_names;
  int n_ram_dimms;
  char *ram_technology;
  uint64_t ram_freq_mhz;

  /* RAM detail (AdwPreferencesPage) */
  GtkWidget *ram_name_label;
  GtkWidget *ram_total_label;
  GtkWidget *ram_used_label;
  GtkWidget *ram_freq_label;
  GtkWidget *ram_tech_label;
  GtkWidget *ram_lighting_status;
  guint ram_monitor_timer_id;

  /* CPU, Keyboard, Mouse sub-pages */
  CpuPage *cpu_page;
  KeyboardPage *keyboard_page;
  MousePage *mouse_page;

  /* Lighting detection */
  GtkWidget *gpu_lighting_status;

  /* Controller index loaded from JOY_INDEX.md */
  GHashTable *controller_index;
  struct _UsbIds *usb_ids;
} HardwarePage;

HardwarePage *hardware_page_new (void);

G_END_DECLS