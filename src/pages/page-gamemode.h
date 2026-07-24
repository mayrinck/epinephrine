#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  GtkWidget   *box;

  AdwComboRow *cpu_governor;
  AdwComboRow *ioprio;
  AdwComboRow *gpu_governor;
  AdwSwitchRow *apply_gpu_governor;

  GtkSpinButton *renice;
  GtkSpinButton *cpu_min_freq;
  GtkSpinButton *cpu_max_freq;
  GtkSpinButton *cpu_perf_pct;
  GtkSpinButton *scheduler_priority;
  GtkSpinButton *io_weight;

  GtkEntry     *script_start;
  GtkEntry     *script_end;

  AdwSwitchRow *tweak_disable_animations;
  AdwSwitchRow *tweak_disable_notifications;
  AdwSwitchRow *tweak_suspend_compositor;
  AdwSwitchRow *tweak_swappiness;
  AdwSwitchRow *tweak_nmi_watchdog;
  AdwSwitchRow *tweak_usb_autosuspend;
  AdwSwitchRow *tweak_timer_khz;
  AdwSwitchRow *tweak_disable_wakeup;
  AdwSwitchRow *tweak_dirty_ratio;
  AdwSwitchRow *tweak_kill_trackers;
  AdwSwitchRow *tweak_sched_rt;
  AdwSwitchRow *tweak_stop_services;

  GtkButton    *save_config_button;
  GtkButton    *reset_config_button;
} GameModePage;

GameModePage *gamemode_page_new (void);

G_END_DECLS
