#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef struct
{
  GtkWidget   *box;

  /* Toggle visibility */
  AdwSwitchRow *show_fps;
  AdwSwitchRow *show_frame_timing;
  AdwSwitchRow *show_cpu;
  AdwSwitchRow *show_cpu_temp;
  AdwSwitchRow *show_gpu;
  AdwSwitchRow *show_gpu_temp;
  AdwSwitchRow *show_vram;
  AdwSwitchRow *show_ram;
  AdwSwitchRow *show_procmem;
  AdwSwitchRow *show_io;
  AdwSwitchRow *show_power;
  AdwSwitchRow *show_engine;
  AdwSwitchRow *show_vulkan_driver;
  AdwSwitchRow *show_wine;
  AdwSwitchRow *show_histogram;

  /* Position */
  AdwComboRow *position;
  AdwSpinRow *offset_x;
  AdwSpinRow *offset_y;
  AdwSpinRow *font_size;

  /* Appearance */
  AdwSpinRow *background_alpha;
  AdwSpinRow *table_columns;

  /* Performance */
  AdwSpinRow *fps_limit;
  AdwEntryRow   *custom_text_center;
  AdwEntryRow   *custom_text_left;
  AdwEntryRow   *custom_text_right;

  /* Bottom bar */
  GtkButton    *save_button;
  GtkButton    *reset_button;
} MangoHudPage;

MangoHudPage *mangohud_page_new (void);

G_END_DECLS
