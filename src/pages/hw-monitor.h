#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Start the GPU metric poller (idempotent). */
void hw_monitor_start (HardwarePage *page);

/* Stop and restart the GPU metric poller (for live speed changes). */
void hw_monitor_restart (HardwarePage *page);

/* Start / stop / restart the RAM monitor graph poller (speed aware). */
void hw_ram_monitor_start    (HardwarePage *page);
void hw_ram_monitor_stop     (HardwarePage *page);
void hw_ram_monitor_restart  (HardwarePage *page);

/* Draw callbacks used by the GPU/RAM detail page monitor widgets. */
void hw_monitor_metric_dot_draw (GtkDrawingArea *area, cairo_t *cr,
                                 int width, int height, gpointer user_data);
void hw_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer user_data);
void hw_ram_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
                                int width, int height, gpointer user_data);

G_END_DECLS
