#include "window.h"
#include "epi-i18n.h"
#include "pages/page-dashboard.h"
#include "pages/page-games.h"
#include "pages/page-gamescope.h"
#include "pages/page-gamemode.h"
#include "pages/page-utilities.h"
#include "pages/page-settings.h"
#include "pages/page-hardware.h"
#include "pages/hw-monitor.h"
#include "pages/page-cpu.h"
#include "epi-privilege.h"

typedef struct
{
  GtkWindow       *win;

  AdwViewStack    *view_stack;
  AdwToastOverlay *toast_overlay;

  const char *tab_keys[5];

  DashboardPage   *dash_page;
  GtkWidget       *games_page;
  HardwarePage    *hw_page;
  UtilidadesPage  *util_page;
  SettingsPage    *settings_page;

  /* Current error details for toast action */
  char *current_error_details;
} EpinephrineWindow;

static EpinephrineWindow *g_epi = NULL;

static void
on_window_destroy (void)
{
  epi_privilege_shutdown ();
  g_clear_pointer (&g_epi->current_error_details, g_free);
  g_epi = NULL;
}

void
epinephrine_show_toast (const char *message)
{
  if (!g_epi || !g_epi->toast_overlay) return;
  AdwToast *toast = adw_toast_new (message);
  adw_toast_set_timeout (toast, 3);
  adw_toast_overlay_add_toast (g_epi->toast_overlay, toast);
}

/* ─────────────── Error Details Overlay ─────────────── */

static void
on_copy_error_clicked (GtkButton *btn, gpointer user_data)
{
  GtkLabel *label = GTK_LABEL (user_data);
  const char *text = gtk_label_get_text (label);
  GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (btn));
  gdk_clipboard_set_text (clipboard, text);

  AdwToast *toast = adw_toast_new (epi_i18n_get ("Error copied to clipboard"));
  adw_toast_set_timeout (toast, 2);
  adw_toast_overlay_add_toast (g_epi->toast_overlay, toast);
}

static void
on_error_overlay_close_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  GtkWidget *overlay = GTK_WIDGET (user_data);
  gtk_widget_set_visible (overlay, FALSE);
}

static void
show_error_overlay (const char *details)
{
  if (!g_epi || !g_epi->win) return;

  /* Remove existing overlay if any */
  GtkWidget *old = gtk_window_get_child (g_epi->win);
  if (!old) return;

  /* Find or create overlay container */
  GtkWidget *overlay = g_object_get_data (G_OBJECT (old), "error-overlay");
  if (overlay)
    gtk_widget_set_visible (overlay, TRUE);
  else
    {
      overlay = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      g_object_set_data (G_OBJECT (old), "error-overlay", overlay);
      gtk_widget_set_hexpand (overlay, TRUE);
      gtk_widget_set_vexpand (overlay, TRUE);
      gtk_widget_set_valign (overlay, GTK_ALIGN_FILL);
      gtk_widget_set_halign (overlay, GTK_ALIGN_FILL);
      gtk_widget_set_visible (overlay, FALSE);

      /* Dark semi-transparent background */
      GtkCssProvider *css = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (css,
        ".error-overlay-bg { background-color: rgba(0,0,0,0.7); }");
      gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (css);
      gtk_widget_add_css_class (overlay, "error-overlay-bg");

      /* Card container */
      GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
      gtk_widget_set_margin_top (card, 32);
      gtk_widget_set_margin_bottom (card, 32);
      gtk_widget_set_margin_start (card, 32);
      gtk_widget_set_margin_end (card, 32);
      gtk_widget_set_halign (card, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (card, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand (card, TRUE);
      gtk_widget_set_vexpand (card, TRUE);
      gtk_box_append (GTK_BOX (overlay), card);

      /* Title */
      GtkWidget *title = gtk_label_new (epi_i18n_get ("Error Details"));
      gtk_widget_add_css_class (title, "title-1");
      gtk_label_set_xalign (GTK_LABEL (title), 0.5);
      gtk_box_append (GTK_BOX (card), title);

      /* Scrollable details */
      GtkWidget *scroll = gtk_scrolled_window_new ();
      gtk_widget_set_vexpand (scroll, TRUE);
      gtk_widget_set_hexpand (scroll, TRUE);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

      GtkWidget *label = gtk_label_new (details ? details : epi_i18n_get ("No details available."));
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_selectable (GTK_LABEL (label), TRUE);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), label);
      gtk_box_append (GTK_BOX (card), scroll);

      /* Button row */
      GtkWidget *btn_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_halign (btn_row, GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (card), btn_row);

      /* Copy button */
      GtkWidget *copy_btn = gtk_button_new_with_label (epi_i18n_get ("Copy Error"));
      g_signal_connect (copy_btn, "clicked", G_CALLBACK (on_copy_error_clicked), label);
      gtk_box_append (GTK_BOX (btn_row), copy_btn);

      /* OK button */
      GtkWidget *ok_btn = gtk_button_new_with_label (epi_i18n_get ("OK"));
      gtk_widget_add_css_class (ok_btn, "suggested-action");
      g_signal_connect (ok_btn, "clicked", G_CALLBACK (on_error_overlay_close_clicked), overlay);
      gtk_box_append (GTK_BOX (btn_row), ok_btn);

      gtk_widget_set_visible (overlay, TRUE);
    }
}

/* Action handler for toast "Details" button */
static void
on_show_error_details (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action;
  (void) parameter;
  (void) user_data;

  if (g_epi && g_epi->current_error_details)
    {
      show_error_overlay (g_epi->current_error_details);
      g_clear_pointer (&g_epi->current_error_details, g_free);
    }
}

static GActionEntry win_entries[] = {
  { "show-error-details", on_show_error_details, NULL, NULL, NULL },
};

void
dashboard_show_error_toast (const char *short_msg, const char *details)
{
  if (!g_epi || !g_epi->toast_overlay) return;

  /* Store details for when user clicks "Details" */
  g_free (g_epi->current_error_details);
  g_epi->current_error_details = g_strdup (details ? details : short_msg);

  /* Show toast with "Details" button */
  AdwToast *toast = adw_toast_new (short_msg);
  adw_toast_set_timeout (toast, 8);
  adw_toast_set_button_label (toast, epi_i18n_get ("Details"));
  adw_toast_set_action_name (toast, "win.show-error-details");
  adw_toast_overlay_add_toast (g_epi->toast_overlay, toast);
}

void
dashboard_navigate_to_games (void)
{
  if (g_epi)
    adw_view_stack_set_visible_child_name (g_epi->view_stack, "games");
}

void
dashboard_navigate_to_hardware (void)
{
  if (g_epi)
    adw_view_stack_set_visible_child_name (g_epi->view_stack, "hardware");
}

void
epinephrine_retranslate (void)
{
  if (!g_epi)
    return;

  epi_i18n_apply_widgets (GTK_WIDGET (g_epi->win));

  /* Tab titles are stored in the view stack pages, not in widgets. */
  GtkSelectionModel *pages = adw_view_stack_get_pages (g_epi->view_stack);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (pages));
  for (guint i = 0; i < n && i < G_N_ELEMENTS (g_epi->tab_keys); i++)
    {
      AdwViewStackPage *page =
        ADW_VIEW_STACK_PAGE (g_list_model_get_item (G_LIST_MODEL (pages), i));
      if (page)
        {
          adw_view_stack_page_set_title (page,
            epi_i18n_get (g_epi->tab_keys[i]));
          g_object_unref (page);
        }
    }
}

void
eph_restart_monitors (void)
{
  if (!g_epi || !g_epi->hw_page)
    return;

  hw_monitor_restart (g_epi->hw_page);
  hw_ram_monitor_restart (g_epi->hw_page);

  if (g_epi->hw_page->cpu_page)
    cpu_monitor_restart (g_epi->hw_page->cpu_page);
}

/* ─────────────── Tab Switch Handler ─────────────── */

static void
on_view_stack_visible_child_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  AdwViewStack *stack = ADW_VIEW_STACK (obj);
  const char *name = adw_view_stack_get_visible_child_name (stack);
  if (name && g_strcmp0 (name, "hardware") == 0)
    {
      /* Trigger hardware scan when user navigates to Hardware Control tab */
      if (g_epi && g_epi->hw_page)
        initial_hardware_scan (g_epi->hw_page);
    }
}

GtkWindow *
epinephrine_window_new (EpinephrineApplication *app)
{
  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/window.ui");

  EpinephrineWindow *epi = g_new0 (EpinephrineWindow, 1);
  g_epi = epi;

  epi->win = GTK_WINDOW (gtk_builder_get_object (builder, "window"));
  epi->view_stack = ADW_VIEW_STACK (gtk_builder_get_object (builder, "view-stack"));
  epi->toast_overlay = ADW_TOAST_OVERLAY (gtk_builder_get_object (builder, "toast-overlay"));

  epi->dash_page     = dashboard_page_new ();

  /* Load graph speed BEFORE creating hardware page so monitors start with correct speed */
  extern int g_graph_speed_ms;
  g_graph_speed_ms = settings_load_graph_speed ();

  epi->hw_page       = hardware_page_new ();
  epi->util_page     = utilidades_page_new ();
  epi->settings_page = settings_page_new ();

  /* Overview tab */
  epi->tab_keys[0] = "Overview";
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->dash_page->box, "dashboard",
    epi_i18n_get ("Overview"), "computer-symbolic");

  /* Games tab */
  epi->tab_keys[1] = "Game Library";
  epi->games_page = games_page_new (epi->dash_page);
  refresh_games_grid (epi->dash_page);
  games_page_register_actions (G_ACTION_MAP (epi->win), epi->dash_page);
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->games_page, "games",
    epi_i18n_get ("Game Library"), "input-gaming-symbolic");

  /* Hardware tab */
  epi->tab_keys[2] = "Hardware Control";
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->hw_page->box, "hardware",
    epi_i18n_get ("Hardware Control"), "chip-symbolic");

  /* utilities tab */
  epi->tab_keys[3] = "Utilities";
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->util_page->box, "utilities",
    epi_i18n_get ("Utilities"), "pocket-knife-symbolic");

  /* Settings tab */
  epi->tab_keys[4] = "Settings";
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->settings_page->box, "settings",
    epi_i18n_get ("Settings"), "emblem-system-symbolic");

  adw_view_stack_set_visible_child_name (epi->view_stack, "dashboard");

  epi_i18n_apply_widgets (GTK_WIDGET (epi->win));

  g_object_unref (builder);

  gtk_window_set_application (GTK_WINDOW (epi->win), GTK_APPLICATION (app));

  /* Shutdown privilege helper and reset globals on window close */
  g_signal_connect (epi->win, "destroy", G_CALLBACK (on_window_destroy), NULL);

  /* Register error details action for toast button */
  g_action_map_add_action_entries (G_ACTION_MAP (epi->win),
    win_entries, G_N_ELEMENTS (win_entries), NULL);

  /* Trigger hardware scan when user navigates to Hardware Control tab */
  g_signal_connect (epi->view_stack, "notify::visible-child",
    G_CALLBACK (on_view_stack_visible_child_notify), NULL);

  return GTK_WINDOW (epi->win);
}
