#pragma once

#include "page-hardware.h"

G_BEGIN_DECLS

/* Start the 500 ms GPU metric poller (idempotent). */
void hw_monitor_start (HardwarePage *page);

/* 1 s RAM usage poller (timer callback). */
gboolean hw_ram_monitor_tick (gpointer user_data);

/* Draw callbacks used by the GPU detail page monitor widgets. */
void hw_monitor_metric_dot_draw (GtkDrawingArea *area, cairo_t *cr,
                                 int width, int height, gpointer user_data);
void hw_monitor_graph_draw (GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer user_data);

G_END_DECLS
