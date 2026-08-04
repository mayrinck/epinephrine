#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define CPU_MON_METRICS 4
#define CPU_MON_HISTORY 120
#define CPU_MAX_GOVERNORS 16
#define CPU_MAX_VULNS 12

typedef struct
{
  /* Box */
  GtkWidget *box;

  /* Branding */
  GtkWidget *cpu_brand_icon;
  GtkWidget *cpu_name_label;
  GtkWidget *cpu_vendor_label;
  GtkWidget *cpu_codename_label;

  /* Monitor */
  GtkWidget *monitor_graph;
  GtkWidget *monitor_labels[CPU_MON_METRICS];
  GtkWidget *monitor_values[CPU_MON_METRICS];
  guint monitor_timer_id;
  float monitor_history[CPU_MON_METRICS][CPU_MON_HISTORY];
  int monitor_history_count;

  /* CPU identification (static) */
  char *model_name;
  char *vendor_id;
  char *socket;
  char *architecture;
  char *bogomips;
  char *microcode;
  char *bugs_str;
  char *vulnerabilities[CPU_MAX_VULNS];
  int n_vulns;
  int family;
  int model;
  int stepping;
  char *flags;
  int cores_physical;
  int threads_total;
  int n_cores;

  /* Frequency (static from sysfs/lscpu) */
  char *base_freq;
  char *boost_freq;
  char *min_freq_hw;
  char *max_freq_hw;

  /* Cache sizes (static) */
  char *l1d_cache;
  char *l1i_cache;
  char *l2_cache;
  char *l3_cache;

  /* CPU control */
  GtkWidget *governor_dropdown;
  GtkWidget *governor_current_label;
  GtkWidget *min_freq_spin;
  GtkWidget *max_freq_spin;
  GtkWidget *cur_freq_label;
  GtkWidget *save_btn;
  GtkWidget *lighting_status;

  /* Dynamic governor list */
  char *available_governors[CPU_MAX_GOVERNORS];
  int n_governors;

  /* /proc/stat previous values for usage calculation */
  uint64_t prev_idle;
  uint64_t prev_total;
} CpuPage;

CpuPage *cpu_page_new (void);
void     cpu_monitor_start (CpuPage *page);
void     cpu_monitor_stop  (CpuPage *page);

G_END_DECLS
