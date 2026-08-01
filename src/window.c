#include "window.h"
#include "epi-i18n.h"
#include "pages/page-dashboard.h"
#include "pages/page-games.h"
#include "pages/page-gamescope.h"
#include "pages/page-gamemode.h"
#include "pages/page-utilities.h"
#include "pages/page-settings.h"
#include "pages/page-hardware.h"
#include "epi-privilege.h"

typedef struct
{
  GtkWindow       *win;

  AdwViewStack    *view_stack;
  AdwToastOverlay *toast_overlay;

  DashboardPage   *dash_page;
  GtkWidget       *games_page;
  HardwarePage    *hw_page;
  UtilidadesPage  *util_page;
  SettingsPage    *settings_page;
} EpinephrineWindow;

static EpinephrineWindow *g_epi = NULL;

static void
on_window_destroy (void)
{
  epi_privilege_shutdown ();
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

void
dashboard_navigate_to_games (void)
{
  if (g_epi)
    adw_view_stack_set_visible_child_name (g_epi->view_stack, "games");
}

void
epinephrine_retranslate (void)
{
  if (g_epi)
    epi_i18n_apply_widgets (GTK_WIDGET (g_epi->win));
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
  epi->hw_page       = hardware_page_new ();
  epi->util_page     = utilidades_page_new ();
  epi->settings_page = settings_page_new ();

  /* Overview tab */
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->dash_page->box, "dashboard",
    epi_i18n_get ("Overview"), "computer-symbolic");

  /* Games tab */
  epi->games_page = games_page_new (epi->dash_page);
  refresh_games_grid (epi->dash_page);
  games_page_register_actions (G_ACTION_MAP (epi->win), epi->dash_page);
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->games_page, "games",
    epi_i18n_get ("Game Library"), "input-gaming-symbolic");

  /* Hardware tab */
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->hw_page->box, "hardware",
    epi_i18n_get ("Hardware Control"), "chip-symbolic");

  /* utilities tab */
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->util_page->box, "utilities",
    epi_i18n_get ("Utilities"), "pocket-knife-symbolic");

  /* Settings tab */
  adw_view_stack_add_titled_with_icon (epi->view_stack,
    epi->settings_page->box, "settings",
    epi_i18n_get ("Settings"), "emblem-system-symbolic");

  adw_view_stack_set_visible_child_name (epi->view_stack, "dashboard");

  epi_i18n_apply_widgets (GTK_WIDGET (epi->win));

  g_object_unref (builder);

  gtk_window_set_application (GTK_WINDOW (epi->win), GTK_APPLICATION (app));

  /* Shutdown privilege helper and reset globals on window close */
  g_signal_connect (epi->win, "destroy", G_CALLBACK (on_window_destroy), NULL);

  return GTK_WINDOW (epi->win);
}
