#define _GNU_SOURCE
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "pages/hw-input-classify.h"
static void write_file(const char *dir,const char *name,const char *value) {
  g_autofree char *path=g_build_filename(dir,name,NULL);g_assert_true(g_file_set_contents(path,value,-1,NULL));
}
static void test_classification(void) {
  g_autofree char *tmp=g_dir_make_tmp("epi-input-XXXXXX",NULL);g_assert_nonnull(tmp);
  g_autofree char *usb=g_build_filename(tmp,"usb",NULL),*kbd=g_build_filename(usb,"usb:1.0",NULL),*mouse=g_build_filename(usb,"usb:1.1",NULL);
  g_autofree char *kbd_input=g_build_filename(kbd,"hid","input",NULL),*mouse_input=g_build_filename(mouse,"hid","input",NULL),*sys=g_build_filename(tmp,"class",NULL);
  g_mkdir_with_parents(kbd_input,0700);g_mkdir_with_parents(mouse_input,0700);g_mkdir_with_parents(sys,0700);
  write_file(usb,"idVendor","05ac\n");write_file(kbd,"bInterfaceNumber","00\n");write_file(kbd,"bInterfaceClass","03\n");write_file(kbd,"bInterfaceSubClass","01\n");write_file(kbd,"bInterfaceProtocol","01\n");
  write_file(kbd_input,"name","SONiX AKL982 Pro\n");write_file(mouse_input,"name","SONiX AKL982 Pro\n");
  g_autofree char *e1=g_build_filename(sys,"event1",NULL),*e2=g_build_filename(sys,"event2",NULL),*d1=g_build_filename(e1,"device",NULL),*d2=g_build_filename(e2,"device",NULL);
  g_mkdir_with_parents(e1,0700);g_mkdir_with_parents(e2,0700);g_assert_cmpint(symlink(kbd_input,d1),==,0);g_assert_cmpint(symlink(mouse_input,d2),==,0);
  g_assert_true(hw_input_is_keyboard_aux_mouse(sys,"event2"));
  write_file(mouse_input,"name","Real mouse with same receiver\n");g_assert_false(hw_input_is_keyboard_aux_mouse(sys,"event2"));
  write_file(mouse_input,"name","SONiX AKL982 Pro\n");write_file(kbd,"bInterfaceProtocol","02\n");g_assert_false(hw_input_is_keyboard_aux_mouse(sys,"event2"));
  g_assert_false(hw_input_is_keyboard_aux_mouse(sys,"event99"));
  g_autofree char *quoted=g_shell_quote(tmp), *command=g_strdup_printf("rm -rf -- %s",quoted);g_spawn_command_line_sync(command,NULL,NULL,NULL,NULL);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/hardware/composite-input",test_classification);return g_test_run();}
