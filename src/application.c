#include "application.h"
#include "window.h"
#include "pages/page-settings.h"
#include "epi-privilege.h"
#include "epi-i18n.h"

#include <fontconfig/fontconfig.h>

struct _EpinephrineApplication
{
  AdwApplication parent_instance;
};

G_DEFINE_TYPE (EpinephrineApplication, epinephrine_application, ADW_TYPE_APPLICATION)

static const char *EPI_BASE_CSS =
  "*, window, label, button, entry, row, list, box {\n"
  "  font-family: 'Share Tech';\n"
  "}\n"
  "button.text-button:not(.window-controls):hover {\n"
  "  box-shadow: 0 0 8px 2px alpha(@accent_color, 0.4);\n"
  "}\n"
  "button.suggested-action:not(.window-controls):hover {\n"
  "  box-shadow: 0 0 14px 4px alpha(@accent_color, 0.5);\n"
  "}\n"
  "button.flat.accent-btn {\n"
  "  color: alpha(@accent_color, 0.85);\n"
  "  font-weight: bold;\n"
  "}\n"
  "button.flat.accent-btn:hover {\n"
  "  background-color: alpha(@accent_color, 0.12);\n"
  "  box-shadow: 0 0 8px 2px alpha(@accent_color, 0.3);\n"
  "}\n"
  "button.destructive-action:not(.window-controls):hover {\n"
  "  box-shadow: 0 0 14px 4px alpha(@destructive_color, 0.5);\n"
  "}\n"
  "row:selected {\n"
  "  background-color: alpha(@accent_color, 0.15);\n"
  "}\n"
  "headerbar {\n"
  "  border-bottom: 1px solid alpha(@accent_color, 0.2);\n"
  "}\n"
  ".dashboard-card {\n"
  "  background-color: rgba(255, 255, 255, 0.06);\n"
  "  border-radius: 12px;\n"
  "  border: 1px solid rgba(255, 255, 255, 0.12);\n"
  "  padding: 16px;\n"
  "  transition: border-color 200ms ease;\n"
  "}\n"
  ".dashboard-card:hover {\n"
  "  border-color: rgba(255, 255, 255, 0.2);\n"
  "}\n"
  ".dashboard-card .header {\n"
  "  font-size: 1.1em;\n"
  "  font-weight: bold;\n"
  "}\n"
  "flowboxchild {\n"
  "  border: none;\n"
  "  box-shadow: none;\n"
  "  background: none;\n"
  "  padding: 0;\n"
  "}\n"
  ".games-grid {\n"
  "  padding: 0;\n"
  "  margin: 0;\n"
  "}\n"
  ".game-card {\n"
  "  background-color: rgba(255, 255, 255, 0.06);\n"
  "  border-radius: 12px;\n"
  "  border: 1px solid rgba(255, 255, 255, 0.1);\n"
  "  padding: 16px;\n"
  "  min-width: 180px;\n"
  "  transition: all 200ms ease;\n"
  "}\n"
  ".game-card:hover {\n"
  "  background-color: rgba(255, 255, 255, 0.10);\n"
  "  border-color: alpha(@accent_color, 0.3);\n"
  "}\n"
  ".game-card .game-icon {\n"
  "  font-size: 32px;\n"
  "  margin-bottom: 8px;\n"
  "}\n"
  ".game-card .game-name {\n"
  "  font-weight: bold;\n"
  "  margin-bottom: 4px;\n"
  "}\n"
  ".game-card .game-stats {\n"
  "  font-size: 0.85em;\n"
  "  opacity: 0.7;\n"
  "}\n"
  ".game-card .play-now-btn {\n"
  "  border-radius: 8px;\n"
  "  padding: 4px 12px;\n"
  "  font-size: 0.85em;\n"
  "  margin-top: 8px;\n"
  "}\n"
  ".game-card.game-selected {\n"
  "  border-color: @accent_color;\n"
  "  box-shadow: 0 0 12px 2px alpha(@accent_color, 0.45);\n"
  "}\n"
  ".game-select-check {\n"
  "  margin: 6px;\n"
  "}\n"
  ".category-chip {\n"
  "  border-radius: 16px;\n"
  "  padding: 4px 14px;\n"
  "  font-size: 0.9em;\n"
  "  border: 1px solid rgba(255, 255, 255, 0.12);\n"
  "}\n"
  ".category-chip:checked {\n"
  "  background-color: alpha(@accent_color, 0.2);\n"
  "  border-color: @accent_color;\n"
  "  color: @accent_color;\n"
  "  font-weight: bold;\n"
  "}\n"
  ".game-shortcut {\n"
  "  background-color: rgba(255, 255, 255, 0.06);\n"
  "  border-radius: 12px;\n"
  "  border: 1px solid rgba(255, 255, 255, 0.1);\n"
  "  padding: 16px;\n"
  "  transition: all 200ms ease;\n"
  "}\n"
  ".game-shortcut:hover {\n"
  "  background-color: rgba(255, 255, 255, 0.10);\n"
  "  border-color: alpha(@accent_color, 0.3);\n"
  "}\n"
  ".game-shortcut .game-icon {\n"
  "  font-size: 32px;\n"
  "  margin-bottom: 8px;\n"
  "}\n"
  ".game-shortcut .game-name {\n"
  "  font-weight: bold;\n"
  "  margin-bottom: 4px;\n"
  "}\n"
  ".game-shortcut .game-stats {\n"
  "  font-size: 0.85em;\n"
  "  opacity: 0.7;\n"
  "}\n"
  ".game-search-bar {\n"
  "  min-width: 280px;\n"
  "  border-radius: 8px;\n"
  "  background-color: rgba(255, 255, 255, 0.06);\n"
  "  border: 1px solid rgba(255, 255, 255, 0.1);\n"
  "  padding: 4px 8px;\n"
  "}\n"
  ".game-search-bar:focus-within {\n"
  "  border-color: alpha(@accent_color, 0.5);\n"
  "  box-shadow: 0 0 8px 2px alpha(@accent_color, 0.2);\n"
  "}\n"
  ".import-launcher-row {\n"
  "  padding: 12px;\n"
  "  border-radius: 8px;\n"
  "  background-color: rgba(255, 255, 255, 0.04);\n"
  "  margin-bottom: 4px;\n"
  "}\n"
  ".import-launcher-row:hover {\n"
  "  background-color: rgba(255, 255, 255, 0.08);\n"
  "}\n"
  ".import-launcher-row.detected {\n"
  "  border-left: 3px solid alpha(@accent_color, 0.8);\n"
  "}\n"
  ".import-launcher-row.not-detected {\n"
  "  opacity: 0.5;\n"
  "}\n"
  ".section-header {\n"
  "  border-radius: 8px;\n"
  "  padding: 8px 12px;\n"
  "  margin-top: 4px;\n"
  "  margin-bottom: 2px;\n"
  "  background-color: rgba(255, 255, 255, 0.04);\n"
  "  transition: background-color 200ms ease;\n"
  "}\n"
  ".section-header:hover {\n"
  "  background-color: rgba(255, 255, 255, 0.08);\n"
  "}\n"
  ".section-header .section-arrow {\n"
  "  transition: transform 200ms ease;\n"
  "}\n"
  ".section-header.collapsed .section-arrow {\n"
  "  transform: rotate(-90deg);\n"
  "}\n"
  ".game-running {\n"
  "  background-color: alpha(@accent_color, 0.15);\n"
  "  border-color: alpha(@accent_color, 0.4);\n"
  "}\n"
  ".game-running .play-now-btn {\n"
  "  background-color: alpha(@destructive_color, 0.8);\n"
  "  color: white;\n"
  "}\n"
  ".status-ok {\n"
  "  color: #2ec27e;\n"
  "}\n"
  ".status-warn {\n"
  "  color: #e5a11c;\n"
  "}\n"
  ".dashboard-card row {\n"
  "  background: transparent;\n"
  "  border: none;\n"
  "  padding: 4px 0;\n"
  "  min-height: 0;\n"
  "}\n"
  ".dashboard-card row:hover {\n"
  "  background: transparent;\n"
  "}\n"
  ".dashboard-card-group {\n"
  "}\n"
  ".ctrl-indicator {\n"
  "  border-radius: 20px;\n"
  "  background: none;\n"
  "  border: 2px solid rgba(255, 255, 255, 0.15);\n"
  "  transition: all 80ms ease;\n"
  "}\n"
  ".ctrl-indicator-active {\n"
  "  background-color: alpha(@accent_color, 0.6);\n"
  "  border-color: @accent_color;\n"
  "  box-shadow: 0 0 12px 4px alpha(@accent_color, 0.5);\n"
  "}\n"
  ".ctrl-label {\n"
  "  font-size: 12px;\n"
  "  font-weight: bold;\n"
  "  color: rgba(255, 255, 255, 0.7);\n"
  "}\n"
  ".ctrl-indicator-active .ctrl-label {\n"
  "  color: white;\n"
  "}\n"
  ".device-list-panel {\n"
  "  min-width: 240px;\n"
  "}\n"
  ".accent-label {\n"
  "  color: @accent_color;\n"
  "}\n"
  ".gaming-score-card {\n"
  "  background-color: rgba(255, 255, 255, 0.04);\n"
  "  border-radius: 12px;\n"
  "  border: 1px solid rgba(255, 255, 255, 0.08);\n"
  "  padding: 24px 28px;\n"
  "}\n"
  ".gaming-score-value {\n"
  "  font-size: 2.5em;\n"
  "  font-weight: bold;\n"
  "  color: @accent_color;\n"
  "}\n"
  ".gaming-score-tier {\n"
  "  font-size: 11px;\n"
  "  font-weight: 600;\n"
  "  padding: 2px 8px;\n"
  "  border-radius: 4px;\n"
  "  border: 1px solid transparent;\n"
  "  letter-spacing: 0.5px;\n"
  "}\n"
  ".gaming-score-tier.tier-ruim {\n"
  "  color: #e01b24;\n"
  "  border-color: #e01b24;\n"
  "  background-color: rgba(224, 27, 36, 0.10);\n"
  "}\n"
  ".gaming-score-tier.tier-regular {\n"
  "  color: #ff7800;\n"
  "  border-color: #ff7800;\n"
  "  background-color: rgba(255, 120, 0, 0.10);\n"
  "}\n"
  ".gaming-score-tier.tier-boa {\n"
  "  color: #f5c211;\n"
  "  border-color: #f5c211;\n"
  "  background-color: rgba(245, 194, 17, 0.10);\n"
  "}\n"
  ".gaming-score-tier.tier-muito-boa {\n"
  "  color: #73d216;\n"
  "  border-color: #73d216;\n"
  "  background-color: rgba(115, 210, 22, 0.10);\n"
  "}\n"
  ".gaming-score-tier.tier-ideal {\n"
  "  color: #2ec27e;\n"
  "  border-color: #2ec27e;\n"
  "  background-color: rgba(46, 194, 126, 0.10);\n"
  "}\n"
  ".score-bar-low progress {\n"
  "  background-color: #e01b24;\n"
  "}\n"
  ".score-bar-mid progress {\n"
  "  background-color: #ff8c00;\n"
  "}\n"
  ".score-bar-good progress {\n"
  "  background-color: #2ec27e;\n"
  "}\n"
  ".status-icon-ok {\n"
  "  color: #2ec27e;\n"
  "}\n"
  ".status-icon-error {\n"
  "  color: #e01b24;\n"
  "}\n"
  ".status-icon-info {\n"
  "  color: rgba(255, 255, 255, 0.7);\n"
  "}\n";

static GtkCssProvider *g_base_provider = NULL;

static void
epinephrine_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (epinephrine_application_parent_class)->startup (app);

  /* Register custom font with fontconfig */
  FcInit ();

  g_autoptr(GBytes) font_data = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/fonts/ShareTech-Regular.ttf",
    G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!font_data)
    g_warning ("Failed to lookup font resource");
  else
    {
      g_autofree char *cache_dir = g_build_filename (
        g_get_user_cache_dir (), "epinephrine", "fonts", NULL);
      g_mkdir_with_parents (cache_dir, 0755);
      g_autofree char *font_path = g_build_filename (
        cache_dir, "ShareTech-Regular.ttf", NULL);

      gsize size;
      const void *data = g_bytes_get_data (font_data, &size);
      if (!g_file_set_contents (font_path, data, size, NULL))
        g_warning ("Failed to write font to %s", font_path);
      else if (!FcConfigAppFontAddFile (FcConfigGetCurrent (),
                                        (const FcChar8 *)font_path))
        g_warning ("Fontconfig failed to register %s", font_path);
    }
}

static void
epinephrine_application_activate (GApplication *app)
{
  EpinephrineApplication *epi_app = EPINEPHRINE_APPLICATION (app);

  /* Apply saved language preference before building the UI so every widget
     is created already translated. */
  char *lang = settings_load_language ();
  epi_i18n_set_language (lang);
  g_free (lang);

  GtkWindow *win = epinephrine_window_new (epi_app);
  gtk_window_present (win);

  /* Request the persistent admin session once, at startup. All features
     that need root consume this single session for the app lifetime. */
  static gboolean admin_notified = FALSE;
  if (!epi_privilege_is_available () && !admin_notified)
    {
      GError *perr = NULL;
      if (!epi_privilege_init (&perr))
        {
          admin_notified = TRUE;
          char *msg = g_strdup_printf (
            "%s\n%s",
            epi_i18n_get ("Administrator session is unavailable. Features that need admin access won't work."),
            perr && perr->message ? perr->message : "");
          epinephrine_show_toast (msg);
          g_free (msg);
          g_clear_error (&perr);
        }
    }

  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (win));

  /* Register custom symbolic icons from resources */
  if (display)
    {
      GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
      gtk_icon_theme_add_resource_path (theme,
        "/io/github/mayrinck/epinephrine/icons-sym");
    }

  if (display && !g_base_provider)
    {
      g_base_provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (g_base_provider, EPI_BASE_CSS);
      gtk_style_context_add_provider_for_display (
        display,
        GTK_STYLE_PROVIDER (g_base_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

  /* Set default application font */
  GtkSettings *settings = gtk_settings_get_default ();
  g_object_set (settings, "gtk-font-name", "Share Tech 11", NULL);

  /* Apply saved accent/theme — display is now available */
  AccentChoice saved = settings_load_accent ();
  settings_apply_accent (saved);
  gboolean theme_on = settings_load_full_theme ();
  settings_apply_full_theme (saved, theme_on);
}

static void
epinephrine_application_class_init (EpinephrineApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->startup = epinephrine_application_startup;
  app_class->activate = epinephrine_application_activate;
}

static void
epinephrine_application_init (EpinephrineApplication *self)
{
  (void) self;
}

EpinephrineApplication *
epinephrine_application_new (void)
{
  return g_object_new (EPINEPHRINE_TYPE_APPLICATION,
                       "application-id", "io.github.mayrinck.epinephrine",
                       "flags", G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
