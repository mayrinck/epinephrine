#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  GtkWidget *box;
  GtkWidget *governor_dropdown;
  GtkWidget *governor_current_label;
  GtkWidget *min_freq_spin;
  GtkWidget *max_freq_spin;
  GtkWidget *cur_freq_label;
  GtkWidget *cpu_cores_label;
  GtkWidget *cpu_arch_label;
  GtkWidget *save_btn;
  GtkWidget *lighting_status;

  int n_cores;
} CpuPage;

CpuPage *cpu_page_new (void);

G_END_DECLS