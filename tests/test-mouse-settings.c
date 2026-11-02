#include <gio/gio.h>
#include "pages/mouse-settings.h"
static void roundtrip(void) {
  g_autoptr(GSettings) settings=mouse_settings_open("ubuntu:GNOME");
  if(!settings){g_test_skip("GNOME schema is not installed");return;}
  g_autoptr(GError) err=NULL;
  g_assert_true(mouse_settings_write(settings,0.4,"flat",TRUE,&err));g_assert_no_error(err);
  g_assert_cmpfloat_with_epsilon(g_settings_get_double(settings,"speed"),0.4,0.0001);
  g_autofree char *profile=g_settings_get_string(settings,"accel-profile");g_assert_cmpstr(profile,==,"flat");
  g_assert_true(g_settings_get_boolean(settings,"natural-scroll"));
  g_assert_false(mouse_settings_write(settings,2.0,"flat",FALSE,&err));g_assert_error(err,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT);g_clear_error(&err);
  g_assert_cmpfloat_with_epsilon(g_settings_get_double(settings,"speed"),0.4,0.0001);
  g_assert_true(g_settings_get_boolean(settings,"natural-scroll"));
  g_assert_false(mouse_settings_write(settings,0,"none",FALSE,&err));g_assert_error(err,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null(mouse_settings_open("KDE"));g_assert_null(mouse_settings_open("NOTGNOME"));g_assert_null(mouse_settings_open(NULL));
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/mouse/settings/roundtrip",roundtrip);return g_test_run();}
