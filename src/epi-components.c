#include "epi-components.h"
#include "epi-privilege.h"
#include <string.h>

typedef struct {const char *flatpak, *binary, *alternate, *package, *appimage;} Definition;
static const Definition definitions[EPI_COMPONENT_COUNT] = {
 {"com.valvesoftware.Steam","steam",NULL,"steam",NULL},
 {"com.heroicgameslauncher.hgl","heroic","heroic-games-launcher","heroic-games-launcher","heroic"},
 {"net.lutris.Lutris","lutris",NULL,"lutris",NULL},
 {"com.usebottles.bottles","bottles",NULL,"bottles",NULL},
 {"org.openrgb.OpenRGB","openrgb",NULL,"openrgb","openrgb"},
 {NULL,"mangohud","mangoapp","mangohud",NULL},
 {NULL,"gamescope",NULL,"gamescope",NULL},
 {NULL,"gamemoderun","gamemoded","gamemode",NULL},
};
const char *epi_component_flatpak_id(EpiComponent c) {return c>=0 && c<EPI_COMPONENT_COUNT ? definitions[c].flatpak:NULL;}
static gboolean command_ok(const char *const *args) {
  gint status=-1;return g_spawn_sync(NULL,(char **)args,NULL,G_SPAWN_SEARCH_PATH|G_SPAWN_STDOUT_TO_DEV_NULL|G_SPAWN_STDERR_TO_DEV_NULL,NULL,NULL,NULL,NULL,&status,NULL) && g_spawn_check_wait_status(status,NULL);
}
static gboolean executable(const char *name){g_autofree char *p=name?g_find_program_in_path(name):NULL;return p!=NULL;}
static gboolean appimage_found(const char *name) {
  if(!name)return FALSE;
  const char *home=g_get_home_dir();
  g_autofree char *apps=g_build_filename(home,"Applications",NULL);
  g_autofree char *bin=g_build_filename(home,".local","bin",NULL);
  const char *dirs[]={home,apps,bin,"/opt","/usr/local/bin",NULL};
  for(guint i=0;dirs[i];i++) {
    g_autoptr(GDir) d=g_dir_open(dirs[i],0,NULL);if(!d)continue;
    const char *entry;
    while((entry=g_dir_read_name(d))) {
      g_autofree char *lower=g_ascii_strdown(entry,-1);
      if(!g_str_has_prefix(lower,name)||!g_str_has_suffix(lower,".appimage"))continue;
      g_autofree char *path=g_build_filename(dirs[i],entry,NULL);
      if(g_file_test(path,G_FILE_TEST_IS_EXECUTABLE) && g_file_test(path,G_FILE_TEST_IS_REGULAR))return TRUE;
    }
  }
  return FALSE;
}
static gboolean flatpak_remote_exists(const char *scope) {
  g_autofree char *out=NULL;
  gint status=-1;
  if(!g_spawn_sync(NULL,(char *[]){"flatpak",(char *)scope,"remotes","--columns=name",NULL},NULL,
     G_SPAWN_SEARCH_PATH|G_SPAWN_STDERR_TO_DEV_NULL,NULL,NULL,&out,NULL,&status,NULL) ||
     !g_spawn_check_wait_status(status,NULL) || !out)return FALSE;
  g_auto(GStrv) lines=g_strsplit(out,"\n",-1);
  for(guint i=0;lines[i];i++)if(g_str_equal(g_strstrip(lines[i]),"flathub"))return TRUE;
  return FALSE;
}
static gboolean deb_installed(const char *package) {
  g_autofree char *out=NULL;gint status=-1;
  if(!g_spawn_sync(NULL,(char *[]){"dpkg-query","-W","-f=${Status}",(char *)package,NULL},NULL,
      G_SPAWN_SEARCH_PATH|G_SPAWN_STDERR_TO_DEV_NULL,NULL,NULL,&out,NULL,&status,NULL) ||
      !g_spawn_check_wait_status(status,NULL))return FALSE;
  return out && g_str_equal(g_strstrip(out),"install ok installed");
}
gboolean epi_component_installed(EpiComponent c) {
  if(c<0 || c>=EPI_COMPONENT_COUNT)return FALSE;
  const Definition *d=&definitions[c];
  if(executable(d->binary)||executable(d->alternate)||appimage_found(d->appimage))return TRUE;
  if(d->flatpak && executable("flatpak") && command_ok((const char *const[]){"flatpak","info",d->flatpak,NULL}))return TRUE;
  if(d->flatpak) {
    g_autofree char *export_name=g_build_filename(g_get_user_data_dir(),"flatpak","exports","bin",d->flatpak,NULL);
    if(g_file_test(export_name,G_FILE_TEST_IS_EXECUTABLE))return TRUE;
    g_autofree char *system_name=g_build_filename("/var/lib/flatpak/exports/bin",d->flatpak,NULL);
    if(g_file_test(system_name,G_FILE_TEST_IS_EXECUTABLE))return TRUE;
  }
  if(executable("snap") && command_ok((const char *const[]){"snap","list",d->package,NULL}))return TRUE;
  if(c==EPI_COMPONENT_HEROIC && executable("rpm") && command_ok((const char *const[]){"rpm","-q","heroic",NULL}))return TRUE;
  if(executable("rpm") && command_ok((const char *const[]){"rpm","-q",d->package,NULL}))return TRUE;
  if(executable("dpkg-query") && deb_installed(d->package))return TRUE;
  if(executable("pacman") && command_ok((const char *const[]){"pacman","-Q",d->package,NULL}))return TRUE;
  if(executable("xbps-query") && command_ok((const char *const[]){"xbps-query",d->package,NULL}))return TRUE;
  return FALSE;
}
static const char *package_manager(void) {
  g_autofree char *contents=NULL;
  if(g_file_get_contents("/etc/os-release",&contents,NULL,NULL)) {
    g_autofree char *s=g_ascii_strdown(contents,-1);
    if((strstr(s,"id=fedora")||strstr(s,"id_like=\"fedora")||strstr(s,"id_like=fedora")||strstr(s,"id=rhel")) && executable("dnf"))return "dnf";
    if((strstr(s,"id=debian")||strstr(s,"ubuntu")||strstr(s,"id_like=debian")) && executable("apt"))return "apt";
    if((strstr(s,"id=arch")||strstr(s,"id_like=arch")) && executable("pacman"))return "pacman";
    if(strstr(s,"suse") && executable("zypper"))return "zypper";
    if(strstr(s,"id=void") && executable("xbps-install"))return "xbps";
  }
  if(executable("dnf"))return "dnf";
  if(executable("apt"))return "apt";
  if(executable("pacman"))return "pacman";
  if(executable("zypper"))return "zypper";
  if(executable("xbps-install"))return "xbps";
  return NULL;
}
char *epi_component_native_command(EpiComponent c,const char *manager) {
  if(c<0||c>=EPI_COMPONENT_COUNT||!manager)return NULL;
  /* GUI applications are installed via Flatpak to avoid distro-specific names
   * and unavailable third-party repositories. */
  if(definitions[c].flatpak)return NULL;
  const char *pkg=definitions[c].package;
  if(g_str_equal(manager,"apt"))return g_strdup_printf("apt install -y %s",pkg);
  if(g_str_equal(manager,"dnf"))return g_strdup_printf("dnf install -y %s",pkg);
  if(g_str_equal(manager,"pacman"))return g_strdup_printf("pacman -S --noconfirm %s",pkg);
  if(g_str_equal(manager,"zypper"))return g_strdup_printf("zypper install -y %s",pkg);
  if(g_str_equal(manager,"xbps"))return g_strdup_printf("xbps-install -y %s",pkg);
  return NULL;
}
static void install_worker(GTask *task,gpointer source,gpointer task_data,GCancellable *cancel) {
  (void)source;EpiComponent c=GPOINTER_TO_INT(task_data);const Definition *d=&definitions[c];
  g_autoptr(GError) err=NULL;g_autofree char *output=NULL;int code=-1;
  if(d->flatpak) {
    if(!executable("flatpak")) {g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"Flatpak is not installed. Install Flatpak and configure Flathub, or use the application's official installation instructions.");return;}
    const char *scope=flatpak_remote_exists("--user") ? "--user" :
                      flatpak_remote_exists("--system") ? "--system" : NULL;
    if(!scope) {g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,"Flathub is not configured. Add the Flathub remote in your software center or follow the official Flatpak setup instructions.");return;}
    g_autoptr(GSubprocess) p=g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_MERGE,&err,"flatpak","install",scope,"--noninteractive","flathub",d->flatpak,NULL);
    if(!p || !g_subprocess_communicate_utf8(p,NULL,cancel,&output,NULL,&err)) {g_task_return_error(task,g_steal_pointer(&err));return;}
    code=g_subprocess_get_if_exited(p)?g_subprocess_get_exit_status(p):-1;
  } else if (!epi_privilege_is_available() && executable("pkcon")) {
    /* PackageKit is an unprivileged client and asks Polkit itself. This also
     * works for a development build without trusting an executable from it. */
    g_autoptr(GSubprocess) p=g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_MERGE,
      &err,"pkcon","--noninteractive","--plain","install",d->package,NULL);
    if(!p || !g_subprocess_communicate_utf8(p,NULL,cancel,&output,NULL,&err)) {
      g_task_return_error(task,g_steal_pointer(&err));return;
    }
    code=g_subprocess_get_if_exited(p)?g_subprocess_get_exit_status(p):-1;
  } else {
    g_autofree char *command=epi_component_native_command(c,package_manager());
    if(!command){g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"No supported package manager was found; install the component with your distribution's package manager.");return;}
    if(!epi_privilege_exec_sync(command,&code,&output,&err)){g_task_return_error(task,g_steal_pointer(&err));return;}

  }
  if(code!=0) {g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"Installation failed (exit %d):\n%s",code,output&&*output?output:"No diagnostic output.");return;}
  if(!epi_component_installed(c)) {g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"The installer completed but the component could not be detected. Check the installation and restart the application.");return;}
  g_task_return_boolean(task,TRUE);
}
void epi_component_install_async(EpiComponent c,GCancellable *cancel,GAsyncReadyCallback callback,gpointer data) {
  GTask *task=g_task_new(NULL,cancel,callback,data);
  if(c<0 || c>=EPI_COMPONENT_COUNT) {g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Unknown component");g_object_unref(task);return;}
  g_task_set_task_data(task,GINT_TO_POINTER(c),NULL);g_task_run_in_thread(task,install_worker);g_object_unref(task);
}
gboolean epi_component_install_finish(GAsyncResult *result,GError **error){return g_task_propagate_boolean(G_TASK(result),error);}
