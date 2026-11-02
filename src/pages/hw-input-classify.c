#define _GNU_SOURCE
#include "hw-input-classify.h"
#include <stdlib.h>
#include <string.h>

static char *read_text(const char *path) {
  char *text=NULL;
  if (!g_file_get_contents(path,&text,NULL,NULL)) return NULL;
  return g_strstrip(text);
}
static char *usb_parent(const char *event_device) {
  char *path=realpath(event_device,NULL);
  while(path && strcmp(path,"/")!=0) {
    g_autofree char *vendor=g_build_filename(path,"idVendor",NULL);
    if(g_file_test(vendor,G_FILE_TEST_EXISTS))return path;
    char *parent=g_path_get_dirname(path);g_free(path);path=parent;
  }
  g_free(path);return NULL;
}
static gboolean is_boot_keyboard_interface(const char *device_path,const char *usb) {
  g_autofree char *path=realpath(device_path,NULL);
  if(!path)return FALSE;
  while(g_str_has_prefix(path,usb) && strcmp(path,usb)!=0) {
    g_autofree char *number_path=g_build_filename(path,"bInterfaceNumber",NULL);
    g_autofree char *class_path=g_build_filename(path,"bInterfaceClass",NULL);
    g_autofree char *subclass_path=g_build_filename(path,"bInterfaceSubClass",NULL);
    g_autofree char *protocol_path=g_build_filename(path,"bInterfaceProtocol",NULL);
    g_autofree char *num=read_text(number_path), *cls=read_text(class_path);
    g_autofree char *sub=read_text(subclass_path), *proto=read_text(protocol_path);
    if(num && cls && sub && proto)
      return g_ascii_strcasecmp(num,"00")==0 && g_ascii_strcasecmp(cls,"03")==0 &&
             g_ascii_strcasecmp(sub,"01")==0 && g_ascii_strcasecmp(proto,"01")==0;
    char *parent=g_path_get_dirname(path);g_free(g_steal_pointer(&path));path=parent;
  }
  return FALSE;
}
gboolean hw_input_is_keyboard_aux_mouse(const char *sys_input_dir,const char *event_name) {
  g_autofree char *current=g_build_filename(sys_input_dir,event_name,"device",NULL);
  g_autofree char *usb=usb_parent(current);
  g_autofree char *name_path=g_build_filename(current,"name",NULL);
  g_autofree char *name=read_text(name_path);
  if(!usb || !name || !*name)return FALSE;
  GDir *dir=g_dir_open(sys_input_dir,0,NULL);if(!dir)return FALSE;
  gboolean found=FALSE;const char *entry;
  while((entry=g_dir_read_name(dir))) {
    if(!g_str_has_prefix(entry,"event") || g_str_equal(entry,event_name))continue;
    g_autofree char *other=g_build_filename(sys_input_dir,entry,"device",NULL);
    g_autofree char *other_usb=usb_parent(other);
    if(g_strcmp0(usb,other_usb)!=0)continue;
    g_autofree char *other_name_path=g_build_filename(other,"name",NULL);
    g_autofree char *other_name=read_text(other_name_path);
    if(g_strcmp0(name,other_name)==0 && is_boot_keyboard_interface(other,usb)) {found=TRUE;break;}
  }
  g_dir_close(dir);return found;
}
