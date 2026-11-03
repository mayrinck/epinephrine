#include <adwaita.h>
#include "pages/page-mouse.h"
#include "epi-i18n.h"
#include "pages/hw-keyboard-index.h"
#include "pages/mouse-settings.h"
int main(int argc,char **argv) {
  (void)argc;(void)argv;g_setenv("GSETTINGS_BACKEND","memory",TRUE);g_setenv("XDG_CURRENT_DESKTOP","GNOME",TRUE);
  if(!gtk_init_check()) return 77;
  adw_init();epi_i18n_set_language("en_US");
  const char *icons[] = {"brand-default.svg", "brand-blue.svg", "brand-green.svg", "brand-orange.svg"};
  for(guint i=0;i<G_N_ELEMENTS(icons);i++) {
    g_autofree char *icon_path=g_strconcat("/io/github/mayrinck/epinephrine/icons/",icons[i],NULL);
    g_autoptr(GError) error=NULL;
    g_autoptr(GdkPixbuf) icon=gdk_pixbuf_new_from_resource_at_scale(icon_path,128,128,TRUE,&error);
    g_assert_no_error(error);g_assert_nonnull(icon);
  }
  g_autofree char *keyboard=hw_keyboard_index_lookup(0x05ac,0x024f,"SONiX AKL982 Pro");
  g_assert_cmpstr(keyboard,==,"AJAZZ AKP815");
  g_assert_null(hw_keyboard_index_lookup(0x05ac,0x024f,"Apple Keyboard"));
  g_assert_null(hw_keyboard_index_lookup(0x1234,0x024f,"SONiX AKL982 Pro"));
  MousePage *p=mouse_page_new();g_assert_nonnull(p);g_assert_nonnull(p->scroll_factor);
  g_autoptr(GSettings) settings = mouse_settings_open("GNOME");
  g_assert_cmpint(gtk_widget_get_sensitive(p->speed_scale), ==, settings != NULL);g_assert_false(gtk_widget_get_sensitive(p->scroll_factor));
  mouse_page_show_device(p,0x046d,0xc246,NULL);g_assert_true(gtk_widget_get_visible(p->ratbag_editor_btn));
  mouse_page_show_device(p,0x1234,0xabcd,"Example mouse");g_assert_false(gtk_widget_get_visible(p->ratbag_editor_btn));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(p->mouse_name_label)),==,"Example mouse");
  g_object_unref(p->box);return 0;
}
