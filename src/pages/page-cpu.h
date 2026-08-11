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
  guint graph_timer_id;
  float monitor_history[CPU_MON_METRICS][CPU_MON_HISTORY];
  int monitor_history_count;

  /* CPU identification (static) */
  char *model_name;
  char *vendor_id;
  char *socket;
  char *architecture;
  char *bogomips;
  char *microcode;
  char *flags;
  int family;
  int model;
  int stepping;
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

  /* Vulnerabilities */
  char *vulnerabilities[CPU_MAX_VULNS];
  int n_vulns;

  /* Specification rows (AdwActionRow — set via subtitle) */
  AdwActionRow *socket_row;
  AdwActionRow *arch_row;
  AdwActionRow *family_row;
  AdwActionRow *cores_row;
  AdwActionRow *bogomips_row;
  AdwActionRow *microcode_row;
  AdwActionRow *base_freq_row;
  AdwActionRow *boost_freq_row;
  AdwActionRow *flags_row;

  /* Cache rows */
  AdwActionRow *l1d_row;
  AdwActionRow *l1i_row;
  AdwActionRow *l2_row;
  AdwActionRow *l3_row;

  /* Vulnerabilities group */
  AdwPreferencesGroup *vulns_group;

  /* CPU control */
  GtkWidget *governor_dropdown;
  AdwActionRow *governor_row;
  AdwActionRow *cur_freq_row;
  GtkWidget *min_freq_spin;
  GtkWidget *max_freq_spin;

  /* Dynamic governor list */
  char *available_governors[CPU_MAX_GOVERNORS];
  int n_governors;

  /* /proc/stat previous values for usage calculation */
  uint64_t prev_idle;
  uint64_t prev_total;
} CpuPage;

CpuPage *cpu_page_new (void);
void     cpu_monitor_start  (CpuPage *page);
void     cpu_monitor_stop   (CpuPage *page);
void     cpu_monitor_restart (CpuPage *page);

G_END_DECLS
