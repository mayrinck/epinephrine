#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "epi-components.h"
static char *fakebin;
static void write_script(const char *name,const char *body) {
  g_autofree char *path=g_build_filename(fakebin,name,NULL);
  g_assert_true(g_file_set_contents(path,body,-1,NULL));g_assert_cmpint(g_chmod(path,0700),==,0);
}
static void test_ids_and_plans(void) {
  g_assert_cmpstr(epi_component_flatpak_id(EPI_COMPONENT_HEROIC),==,"com.heroicgameslauncher.hgl");
  g_assert_cmpstr(epi_component_flatpak_id(EPI_COMPONENT_BOTTLES),==,"com.usebottles.bottles");
  const char *managers[]={"apt","dnf","pacman","zypper","xbps"};
  const char *prefixes[]={"apt install -y ","dnf install -y ","pacman -S --noconfirm ","zypper install -y ","xbps-install -y "};
  const EpiComponent tools[]={EPI_COMPONENT_MANGOHUD,EPI_COMPONENT_GAMESCOPE,EPI_COMPONENT_GAMEMODE};
  const char *packages[]={"mangohud","gamescope","gamemode"};
  for(guint i=0;i<G_N_ELEMENTS(managers);i++) for(guint j=0;j<G_N_ELEMENTS(tools);j++) {
    g_autofree char *cmd=epi_component_native_command(tools[j],managers[i]);
    g_autofree char *expected=g_strconcat(prefixes[i],packages[j],NULL);
    g_assert_cmpstr(cmd,==,expected);
  }
  g_assert_null(epi_component_native_command(EPI_COMPONENT_HEROIC,"dnf"));
  g_assert_null(epi_component_native_command(EPI_COMPONENT_GAMESCOPE,"invalid"));
}
static void test_detection(void) {
  write_script("flatpak","#!/bin/sh\n[ \"$1\" = info ] && [ \"$2\" = com.heroicgameslauncher.hgl ]\n");
  g_assert_true(epi_component_installed(EPI_COMPONENT_HEROIC));
  write_script("gamescope","#!/bin/sh\nexit 0\n");g_assert_true(epi_component_installed(EPI_COMPONENT_GAMESCOPE));
  g_autofree char *path=g_build_filename(fakebin,"gamescope",NULL);g_unlink(path);
  g_assert_false(epi_component_installed(EPI_COMPONENT_GAMESCOPE));
}
static void test_native_binaries(void) {
  const char *names[]={"steam","heroic","lutris","bottles","openrgb","mangohud","gamescope","gamemoderun"};
  for(int i=0;i<EPI_COMPONENT_COUNT;i++) {
    write_script(names[i],"#!/bin/sh\nexit 0\n");
    g_assert_true(epi_component_installed((EpiComponent)i));
    g_autofree char *path=g_build_filename(fakebin,names[i],NULL);g_unlink(path);
  }
}
typedef struct {GMainLoop *loop;gboolean result;GError *error;} Result;
static void finished(GObject *source,GAsyncResult *r,gpointer user) {
  (void)source;Result *v=user;v->result=epi_component_install_finish(r,&v->error);g_main_loop_quit(v->loop);
}
static void test_install_failure(void) {
  write_script("flatpak","#!/bin/sh\nif [ \"$2\" = remotes ]; then echo flathub; exit 0; fi\nif [ \"$1\" = install ]; then echo repository-unavailable; exit 17; fi\nexit 1\n");
  Result r={0};r.loop=g_main_loop_new(NULL,FALSE);epi_component_install_async(EPI_COMPONENT_HEROIC,NULL,finished,&r);g_main_loop_run(r.loop);
  g_assert_false(r.result);g_assert_error(r.error,G_IO_ERROR,G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(r.error->message,"repository-unavailable"));g_assert_nonnull(strstr(r.error->message,"17"));
  g_clear_error(&r.error);g_main_loop_unref(r.loop);
}
static void test_install_success(void) {
  g_autofree char *state=g_build_filename(fakebin,"installed",NULL);
  g_autofree char *script=g_strdup_printf("#!/bin/sh\nif [ \"$2\" = remotes ]; then echo flathub; exit 0; fi\nif [ \"$1\" = install ]; then : > '%s'; exit 0; fi\nif [ \"$1\" = info ] && [ \"$2\" = com.heroicgameslauncher.hgl ]; then test -f '%s'; exit $?; fi\nexit 1\n",state,state);
  write_script("flatpak",script);
  Result r={0};r.loop=g_main_loop_new(NULL,FALSE);epi_component_install_async(EPI_COMPONENT_HEROIC,NULL,finished,&r);g_main_loop_run(r.loop);
  g_assert_no_error(r.error);g_assert_true(r.result);g_assert_true(epi_component_installed(EPI_COMPONENT_HEROIC));
  g_main_loop_unref(r.loop);g_unlink(state);
}

static void test_packagekit_success(void) {
  g_autofree char *binary=g_build_filename(fakebin,"gamescope",NULL);
  g_autofree char *script=g_strdup_printf("#!/bin/sh\nif [ \"$3\" = install ] && [ \"$4\" = gamescope ]; then printf '#!/bin/sh\\nexit 0\\n' > '%s'; /bin/chmod 755 '%s'; exit 0; fi\nexit 9\n",binary,binary);
  write_script("pkcon",script);
  Result r={0};r.loop=g_main_loop_new(NULL,FALSE);epi_component_install_async(EPI_COMPONENT_GAMESCOPE,NULL,finished,&r);g_main_loop_run(r.loop);
  g_assert_no_error(r.error);g_assert_true(r.result);g_assert_true(epi_component_installed(EPI_COMPONENT_GAMESCOPE));
  g_main_loop_unref(r.loop);g_unlink(binary);g_autofree char *pkcon=g_build_filename(fakebin,"pkcon",NULL);g_unlink(pkcon);
}

int main(int argc,char **argv) {
  g_autofree char *tmp=g_dir_make_tmp("epi-components-XXXXXX",NULL);g_assert_nonnull(tmp);
  fakebin=g_build_filename(tmp,"bin",NULL);g_mkdir_with_parents(fakebin,0700);
  g_setenv("PATH",fakebin,TRUE);g_setenv("XDG_DATA_HOME",tmp,TRUE);g_setenv("HOME",tmp,TRUE);
  g_test_init(&argc,&argv,NULL);g_test_add_func("/components/plans",test_ids_and_plans);g_test_add_func("/components/detection",test_detection);g_test_add_func("/components/native-binaries",test_native_binaries);g_test_add_func("/components/install-failure",test_install_failure);g_test_add_func("/components/install-success",test_install_success);g_test_add_func("/components/packagekit",test_packagekit_success);
  int result=g_test_run();g_autofree char *flat=g_build_filename(fakebin,"flatpak",NULL);g_unlink(flat);g_rmdir(fakebin);g_rmdir(tmp);g_free(fakebin);return result;
}
