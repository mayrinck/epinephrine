#define _GNU_SOURCE
#include "page-hardware.h"
#include "hw-gpu.h"
#include "hw-detail.h"
#include "hw-gpu-detail.h"
#include "hw-controller.h"
#include "hw-devices.h"
#include "hw-device-list.h"
#include "usb-ids.h"
#include "../epi-i18n.h"
#include "page-settings.h"

#define tr(x) epi_i18n_get (x)

/* Current accent color for gradient divider (RGB 0-1) */
static double g_divider_r = 0.75, g_divider_g = 0.15, g_divider_b = 0.15;
static GtkWidget *g_divider_widget = NULL;

static void
parse_hex_color (const char *hex, double *r, double *g, double *b)
{
  if (!hex || hex[0] != '#' || strlen (hex) != 7)
    return;
  long val = strtol (hex + 1, NULL, 16);
  *r = ((val >> 16) & 0xFF) / 255.0;
  *g = ((val >> 8) & 0xFF) / 255.0;
  *b = (val & 0xFF) / 255.0;
}

void
hw_update_accent_color (int choice)
{
  static const char *colors[] = {
    NULL,        /* Follow GPU  */
    "#c01c28",   /* AMD Red     */
    "#0071c5",   /* Intel Blue  */
    "#76B900",   /* NVIDIA Green */
    "#ff8307",   /* TUX Orange  */
  };

  if (choice < 0 || choice > 4)
    return;

  /* Resolve "Follow GPU" to detected GPU brand */
  if (choice == 0)
    choice = (int) settings_detect_gpu_accent ();

  const char *hex = colors[choice];
  if (!hex)
    return;

  parse_hex_color (hex, &g_divider_r, &g_divider_g, &g_divider_b);

  /* Redraw the divider if it exists */
  if (g_divider_widget)
    gtk_widget_queue_draw (g_divider_widget);
}

/* ---- Page Creation ---- */

void
initial_hardware_scan (HardwarePage *page)
{
  if (!page || page->hw_scanned)
    return;

  page->hw_scanned = TRUE;

  /* Detect devices */
  hw_devices_detect (page);

  /* Load controller index from JOY_INDEX.md */
  hw_controller_load_index (page);

  /* Initialize USB ID database: system file + async internet refresh */
  page->usb_ids = usb_ids_new ();
  usb_ids_load (page->usb_ids);
  usb_ids_refresh_async (page->usb_ids);

  /* Populate GPU specs from GPU_INDEX.md */
  hw_gpu_populate_specs (page);

  /* Populate list */
  hw_device_list_populate (page);

  /* Pre-select first device */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (
    GTK_LIST_BOX (page->device_list), 0);
  if (first)
    gtk_list_box_select_row (GTK_LIST_BOX (page->device_list), first);

  /* Load saved GPU config after signals are connected */
  hw_gpu_detail_load_config (page);

  /* Connect GPU management signals */
  if (page->gpu)
    hw_gpu_detail_connect_signals (page);

  /* NVIDIA signal connections (outside gpu check, widgets always exist) */
  hw_gpu_detail_connect_nvidia_signals (page);
}

/* ─────────────── Gradient Divider ─────────────── */

static void
gradient_divider_draw_func (GtkDrawingArea *area, cairo_t *cr,
  int width, int height, gpointer user_data)
{
  (void) area;
  (void) user_data;

  /* Use the current accent color */
  double r = g_divider_r;
  double g = g_divider_g;
  double b = g_divider_b;

  /* Draw vertical gradient line: transparent at top/bottom, accent color in middle */
  cairo_pattern_t *gradient = cairo_pattern_create_linear (0, 0, 0, height);
  cairo_pattern_add_color_stop_rgba (gradient, 0.0, r, g, b, 0.0);
  cairo_pattern_add_color_stop_rgba (gradient, 0.12, r, g, b, 0.7);
  cairo_pattern_add_color_stop_rgba (gradient, 0.5, r, g, b, 1.0);
  cairo_pattern_add_color_stop_rgba (gradient, 0.88, r, g, b, 0.7);
  cairo_pattern_add_color_stop_rgba (gradient, 1.0, r, g, b, 0.0);

  cairo_set_source (cr, gradient);
  cairo_rectangle (cr, (width - 1) / 2.0, 0, 1, height);
  cairo_fill (cr);
  cairo_pattern_destroy (gradient);
}

HardwarePage *
hardware_page_new (void)
{
  HardwarePage *page = g_new0 (HardwarePage, 1);
  page->controller_fd = -1;
  page->ram_stick_rows = g_ptr_array_new ();

  page->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  /* Left panel: device list */
  GtkWidget *left_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (left_panel, 320, -1);
  gtk_widget_set_hexpand (left_panel, FALSE);
  gtk_widget_add_css_class (left_panel, "device-list-panel");

  GtkWidget *header_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_start (header_box, 8);
  gtk_widget_set_margin_end (header_box, 8);
  gtk_widget_set_margin_top (header_box, 8);
  gtk_widget_set_margin_bottom (header_box, 8);

  GtkWidget *header = gtk_label_new (tr ("Devices"));
  gtk_widget_add_css_class (header, "heading");
  gtk_widget_set_hexpand (header, TRUE);
  gtk_widget_set_halign (header, GTK_ALIGN_START);
  page->reload_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_valign (page->reload_btn, GTK_ALIGN_CENTER);
  gtk_button_set_has_frame (GTK_BUTTON (page->reload_btn), FALSE);
  gtk_widget_add_css_class (page->reload_btn, "flat");
  gtk_box_append (GTK_BOX (header_box), header);
  gtk_box_append (GTK_BOX (header_box), page->reload_btn);
  gtk_box_append (GTK_BOX (left_panel), header_box);

  page->device_list = gtk_list_box_new ();
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (page->device_list), TRUE);
  gtk_widget_set_hexpand (page->device_list, TRUE);
  gtk_widget_set_vexpand (page->device_list, TRUE);

  GtkWidget *left_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (left_scroll), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (left_scroll), page->device_list);
  gtk_box_append (GTK_BOX (left_panel), left_scroll);
  gtk_box_append (GTK_BOX (page->box), left_panel);

  /* Gradient divider: accent color fading to transparent at top/bottom */
  GtkWidget *divider = gtk_drawing_area_new ();
  g_divider_widget = divider;
  gtk_widget_set_size_request (divider, 1, -1);
  gtk_widget_set_hexpand (divider, FALSE);
  gtk_widget_set_vexpand (divider, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (divider),
    gradient_divider_draw_func, NULL, NULL);

  /* Set initial accent color from saved settings */
  AccentChoice accent = settings_load_accent ();
  hw_update_accent_color (accent);

  gtk_box_append (GTK_BOX (page->box), divider);

  /* Right panel: detail stack */
  GtkWidget *right_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (right_panel, TRUE);
  gtk_widget_set_vexpand (right_panel, TRUE);

  page->detail_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (page->detail_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_hexpand (GTK_WIDGET (page->detail_stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (page->detail_stack), TRUE);

  /* Loading page */
  GtkWidget *loading_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (spinner, 48, 48);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (spinner, TRUE);
  gtk_widget_set_vexpand (spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (spinner));
  gtk_box_append (GTK_BOX (loading_page), spinner);
  gtk_stack_add_named (page->detail_stack, loading_page, "loading");

  /* Default page */
  GtkWidget *default_page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *default_label = gtk_label_new (
    tr ("Select a device to view details"));
  gtk_widget_set_halign (default_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (default_label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (default_label, TRUE);
  gtk_widget_set_vexpand (default_label, TRUE);
  gtk_box_append (GTK_BOX (default_page), default_label);
  gtk_stack_add_named (page->detail_stack, default_page, "default");

  /* Controller detail page */
  hw_controller_build_detail (page);

  /* Motherboard detail page */
  hw_detail_build_motherboard (page);

  /* RAM detail page */
  hw_detail_build_rams (page);

  /* GPU detail page */
  hw_gpu_detail_build (page);
  gtk_stack_add_named (page->detail_stack, page->gpu_detail_box, "gpu");

  /* CPU, Keyboard, Mouse sub-pages */
  page->cpu_page = cpu_page_new ();
  if (page->cpu_page)
    {
      gtk_stack_add_named (page->detail_stack, page->cpu_page->box, "cpu");
      cpu_monitor_start (page->cpu_page);
    }

  page->keyboard_page = keyboard_page_new ();
  if (page->keyboard_page)
    gtk_stack_add_named (page->detail_stack, page->keyboard_page->box, "keyboard");

  page->mouse_page = mouse_page_new ();
  if (page->mouse_page)
    gtk_stack_add_named (page->detail_stack, page->mouse_page->box, "mouse");

  gtk_box_append (GTK_BOX (right_panel), GTK_WIDGET (page->detail_stack));
  gtk_box_append (GTK_BOX (page->box), right_panel);

  /* Connect signals early so the UI is interactive during scan */
  g_signal_connect (page->reload_btn, "clicked",
    G_CALLBACK (hw_device_list_on_reload), page);
  g_signal_connect (page->device_list, "row-selected",
    G_CALLBACK (hw_device_list_on_selected), page);

  return page;
}
