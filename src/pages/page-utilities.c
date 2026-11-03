#include "page-utilities.h"
#include "hw-gpu-detail.h"
#include "../epi-components.h"
#include "../window.h"
#include "epi-i18n.h"

#define tr(x) epi_i18n_get (x)

static void
utility_install_done (GObject *source, GAsyncResult *result, gpointer data)
{
  (void) source;
  GtkWidget *button = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = epi_component_install_finish (result, &error);
  gtk_widget_set_sensitive (button, !ok);
  if (ok)
    {
      gtk_button_set_label (GTK_BUTTON (button), tr ("Installed — restart the application"));
      epinephrine_show_toast (tr ("Component installed; restart the application to open its settings."));
    }
  else
    dashboard_show_error_toast (tr ("Could not install component"), error->message);
  g_object_unref (button);
}

static void
on_install_clicked (GtkButton *btn, gpointer data)
{
  EpiComponent component = GPOINTER_TO_INT (data);
  gtk_widget_set_sensitive (GTK_WIDGET (btn), FALSE);
  epi_component_install_async (component, NULL, utility_install_done, g_object_ref (btn));
}

static GtkWidget *
create_empty_state (const char *icon_name, const char *title, const char *pkg_name)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_vexpand (box, TRUE);

  GtkWidget *icon = gtk_image_new_from_icon_name (icon_name);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 64);
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *title_label = gtk_label_new (title);
  gtk_widget_add_css_class (title_label, "title-2");
  gtk_box_append (GTK_BOX (box), title_label);

  char *desc = g_strdup_printf (tr ("%s is not installed on your system."), title);
  GtkWidget *desc_label = gtk_label_new (desc);
  g_free (desc);
  gtk_label_set_wrap (GTK_LABEL (desc_label), TRUE);
  gtk_widget_set_halign (desc_label, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity (desc_label, 0.7);
  gtk_box_append (GTK_BOX (box), desc_label);

  EpiComponent component = g_str_equal (pkg_name, "gamescope") ? EPI_COMPONENT_GAMESCOPE :
                           g_str_equal (pkg_name, "mangohud") ? EPI_COMPONENT_MANGOHUD : EPI_COMPONENT_GAMEMODE;
  GtkWidget *btn = gtk_button_new_with_label (tr ("Install"));
  gtk_widget_add_css_class (btn, "suggested-action");
  gtk_widget_set_halign (btn, GTK_ALIGN_CENTER);
  g_signal_connect (btn, "clicked", G_CALLBACK (on_install_clicked), GINT_TO_POINTER (component));
  gtk_box_append (GTK_BOX (box), btn);

  return box;
}

/* RGB Controller placeholder: warning text plus an OpenRGB launcher button.
   The actual lighting editor will live here once implemented. */
static GtkWidget *
create_rgb_controller_page (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_vexpand (box, TRUE);

  GtkWidget *icon = gtk_image_new_from_icon_name ("color-select-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 64);
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *title = gtk_label_new (tr ("RGB Controller"));
  gtk_widget_add_css_class (title, "title-2");
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *warn = gtk_label_new (tr ("This feature is under construction."));
  gtk_widget_set_halign (warn, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity (warn, 0.7);
  gtk_label_set_wrap (GTK_LABEL (warn), TRUE);
  gtk_box_append (GTK_BOX (box), warn);

  GtkWidget *openrgb_btn = gtk_button_new_with_label (tr ("Open OpenRGB"));
  gtk_widget_add_css_class (openrgb_btn, "accent");
  gtk_widget_set_halign (openrgb_btn, GTK_ALIGN_CENTER);
  g_signal_connect (openrgb_btn, "clicked", G_CALLBACK (hw_openrgb_launch), NULL);
  gtk_box_append (GTK_BOX (box), openrgb_btn);

  return box;
}

UtilidadesPage *
utilidades_page_new (void)
{
  UtilidadesPage *page = g_new0 (UtilidadesPage, 1);

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-utilities.ui");

  page->box     = GTK_WIDGET (gtk_builder_get_object (builder, "utilidades-page"));
  page->sub_stack = ADW_VIEW_STACK (gtk_builder_get_object (builder, "util-sub-stack"));

  gboolean has_gamescope = epi_component_installed (EPI_COMPONENT_GAMESCOPE);
  gboolean has_gamemode  = epi_component_installed (EPI_COMPONENT_GAMEMODE);
  gboolean has_mangohud  = epi_component_installed (EPI_COMPONENT_MANGOHUD);

  if (has_gamescope)
    {
      page->gamescope_page = gamescope_page_new ();
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->gamescope_page->box, "gamescope",
        "Gamescope", "gamescope-symbolic");
    }
  else
    {
      page->gamescope_empty = create_empty_state ("gamescope-symbolic", "Gamescope", "gamescope");
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->gamescope_empty, "gamescope",
        "Gamescope", "gamescope-symbolic");
    }

  if (has_gamemode)
    {
      page->gamemode_page = gamemode_page_new ();
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->gamemode_page->box, "gamemode",
        "GameMode", "addon-symbolic");
    }
  else
    {
      page->gamemode_empty = create_empty_state ("addon-symbolic", "GameMode", "gamemode");
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->gamemode_empty, "gamemode",
        "GameMode", "addon-symbolic");
    }

  if (has_mangohud)
    {
      page->mangohud_page = mangohud_page_new ();
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->mangohud_page->box, "mangohud",
        "MangoHud", "speedometer-symbolic");
    }
  else
    {
      page->mangohud_empty = create_empty_state ("speedometer-symbolic", "MangoHud", "mangohud");
      adw_view_stack_add_titled_with_icon (page->sub_stack,
        page->mangohud_empty, "mangohud",
        "MangoHud", "speedometer-symbolic");
    }

  page->rgb_controller_page = create_rgb_controller_page ();
  adw_view_stack_add_titled_with_icon (page->sub_stack,
    page->rgb_controller_page, "rgb-controller",
    tr ("RGB Controller"), "color-select-symbolic");

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
