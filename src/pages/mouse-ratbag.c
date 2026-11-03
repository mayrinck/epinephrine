#include "mouse-ratbag.h"
#include "mouse-ratbag-client.h"
#include "../epi-i18n.h"
#include <glib/gstdio.h>

#define BUS_NAME "org.freedesktop.ratbag1"
#define ROOT_PATH "/org/freedesktop/ratbag1"
#define IFACE(x) BUS_NAME "." x
#define tr(x) epi_i18n_get(x)

typedef struct {
  char *key, *path, *iface, *property;
  GVariant *original;
  GtkWidget *entry;
} Field;
typedef struct {
  GtkWidget *window, *status, *fields, *profiles, *copy_name;
  GDBusConnection *bus;
  char *device, *model;
  GPtrArray *profile_paths, *items;
  guint signal;
  gboolean loading, tainted;
} Editor;

static void field_free (gpointer p) {
  Field *f=p; g_free(f->key); g_free(f->path); g_free(f->iface);
  g_free(f->property); g_variant_unref(f->original); g_free(f);
}
static void editor_free (gpointer p) {
  Editor *e=p;
  if(e->signal) g_dbus_connection_signal_unsubscribe(e->bus,e->signal);
  g_clear_object(&e->bus); g_free(e->device); g_free(e->model);
  g_ptr_array_unref(e->profile_paths); g_ptr_array_unref(e->items); g_free(e);
}
static void status (Editor *e, const char *msg) { gtk_label_set_text(GTK_LABEL(e->status),msg); }
static GVariant *prop (Editor *e,const char *path,const char *iface,const char *name,GError **error) {
  return mouse_ratbag_get(e->bus,path,iface,name,error);
}
static gboolean set_prop (Editor *e,Field *f,GVariant *v,GError **error) {
  return mouse_ratbag_set(e->bus,f->path,f->iface,f->property,v,error);
}
typedef struct { const char *label; guint type, value; } ButtonPreset;
static const ButtonPreset presets[] = {
  {"Disabled",0,0}, {"Left click",1,1}, {"Right click",1,2}, {"Middle click",1,3},
  {"Back",1,4}, {"Forward",1,5}, {"Button 6",1,6}, {"Button 7",1,7},
  {"Button 8",1,8}, {"Button 9",1,9},
  {"DPI up",2,1073741833}, {"DPI down",2,1073741834},
  {"Cycle DPI",2,1073741831}, {"DPI shift",2,1073741835},
  {"Default DPI",2,1073741836}, {"Cycle profile",2,1073741837},
  {"Previous profile",2,1073741838}
};
static void preset_changed(GObject *obj,GParamSpec *spec,gpointer data) {
  (void)spec; Field *f=data; guint n=gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
  GArray *indices=g_object_get_data(obj,"preset-indices");
  if(!n || !indices || n>indices->len)return;
  const ButtonPreset *p=&presets[g_array_index(indices,guint,n-1)];
  g_autoptr(GVariant) v=g_variant_ref_sink(g_variant_new("(uv)",p->type,g_variant_new_uint32(p->value)));
  g_autofree char *text=g_variant_print(v,TRUE);gtk_editable_set_text(GTK_EDITABLE(f->entry),text);
}
static GtkWidget *preset_dropdown(Editor *e,Field *f) {
  g_autoptr(GVariant) types=prop(e,f->path,IFACE("Button"),"ActionTypes",NULL);
  if(!types || !g_variant_is_of_type(types,G_VARIANT_TYPE("au")))return NULL;
  gboolean supported[3]={FALSE,FALSE,FALSE};GVariantIter it;guint32 type;g_variant_iter_init(&it,types);
  while(g_variant_iter_next(&it,"u",&type))if(type<3)supported[type]=TRUE;
  GtkStringList *labels=gtk_string_list_new(NULL);gtk_string_list_append(labels,tr("Custom"));
  GArray *indices=g_array_new(FALSE,FALSE,sizeof(guint));
  for(guint i=0;i<G_N_ELEMENTS(presets);i++) if(supported[presets[i].type]) {
    gtk_string_list_append(labels,tr(presets[i].label));g_array_append_val(indices,i);
  }
  GtkWidget *dropdown=gtk_drop_down_new(G_LIST_MODEL(labels),NULL);
  g_object_set_data_full(G_OBJECT(dropdown),"preset-indices",indices,(GDestroyNotify)g_array_unref);
  gtk_widget_set_tooltip_text(dropdown,tr("Choose a common action; edit the value for keys or macros."));
  g_signal_connect(dropdown,"notify::selected",G_CALLBACK(preset_changed),f);return dropdown;
}
static void resolution_action(GtkButton *button,gpointer data) {
  Editor *e=data; if(e->tainted){status(e,tr("Reconnect the device before writing."));return;}
  const char *path=g_object_get_data(G_OBJECT(button),"resolution-path");
  const char *method=g_object_get_data(G_OBJECT(button),"resolution-method");
  g_autoptr(GError) err=NULL;
  if(!mouse_ratbag_method(e->bus,path,IFACE("Resolution"),method,&err)){status(e,err->message);return;}
  if(!mouse_ratbag_method(e->bus,e->device,IFACE("Device"),"Commit",&err)){
    e->tainted=TRUE;status(e,err->message);return;
  }
  status(e,tr("Resolution change requested; verify by reloading."));
}
static void add_field (Editor *e,const char *path,const char *iface,const char *property,const char *key,const char *label) {
  g_autoptr(GError) err=NULL; GVariant *v=prop(e,path,iface,property,&err);
  if(!v) return;
  Field *f=g_new0(Field,1); f->key=g_strdup(key); f->path=g_strdup(path); f->iface=g_strdup(iface); f->property=g_strdup(property); f->original=v;
  GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8), *lbl=gtk_label_new(label);
  gtk_widget_set_size_request(lbl,150,-1); gtk_label_set_xalign(GTK_LABEL(lbl),0);
  f->entry=gtk_entry_new(); gtk_widget_set_hexpand(f->entry,TRUE);
  g_autofree char *text=g_variant_print(v,TRUE); gtk_editable_set_text(GTK_EDITABLE(f->entry),text);
  gtk_box_append(GTK_BOX(row),lbl);
  if(g_str_equal(property,"Mapping")) {
    GtkWidget *presets_widget=preset_dropdown(e,f); if(presets_widget)gtk_box_append(GTK_BOX(row),presets_widget);
  }
  gtk_box_append(GTK_BOX(row),f->entry);
  if(g_str_equal(iface,IFACE("Resolution"))) {
    const char *methods[]={"SetActive","SetDefault"}, *labels[]={"Active","Default"};
    for(guint i=0;i<2;i++) {
      GtkWidget *b=gtk_button_new_with_label(tr(labels[i]));
      g_object_set_data_full(G_OBJECT(b),"resolution-path",g_strdup(path),g_free);
      g_object_set_data(G_OBJECT(b),"resolution-method",(gpointer)methods[i]);
      g_signal_connect(b,"clicked",G_CALLBACK(resolution_action),e);gtk_box_append(GTK_BOX(row),b);
    }
  }
  gtk_box_append(GTK_BOX(e->fields),row);
  g_ptr_array_add(e->items,f);
}
static void add_children (Editor *e,const char *profile,const char *propname,const char *iface,const char *property) {
  g_autoptr(GVariant) paths=prop(e,profile,IFACE("Profile"),propname,NULL);
  if(!paths || !g_variant_is_of_type(paths,G_VARIANT_TYPE("ao"))) return;
  GVariantIter iter; const char *path; guint n=0; g_variant_iter_init(&iter,paths);
  while(g_variant_iter_next(&iter,"&o",&path)) {
    g_autofree char *key=g_strdup_printf("%s-%u-%s",propname,n,property);
    g_autofree char *label=g_strdup_printf("%s %u · %s",propname,n,property);
    add_field(e,path,iface,property,key,label); n++;
  }
}
static void load_profile (Editor *e) {
  while(gtk_widget_get_first_child(e->fields)) gtk_box_remove(GTK_BOX(e->fields),gtk_widget_get_first_child(e->fields));
  g_ptr_array_set_size(e->items,0);
  guint n=gtk_drop_down_get_selected(GTK_DROP_DOWN(e->profiles));
  if(n>=e->profile_paths->len) return;
  const char *path=g_ptr_array_index(e->profile_paths,n);
  add_field(e,path,IFACE("Profile"),"ReportRate","ReportRate",tr("Report rate (Hz)"));
  add_children(e,path,"Resolutions",IFACE("Resolution"),"Resolution");
  add_children(e,path,"Buttons",IFACE("Button"),"Mapping");
  add_children(e,path,"Leds",IFACE("Led"),"Mode");
  add_children(e,path,"Leds",IFACE("Led"),"Color");
  status(e,tr("Edit values or load a local copy. Apply sends changes to the device."));
}
static void profile_changed (GObject *o,GParamSpec *p,gpointer data) { (void)o;(void)p;Editor *e=data;if(!e->loading) load_profile(e); }
static void resync (GDBusConnection *c,const char *sender,const char *path,const char *iface,const char *signal,GVariant *parameters,gpointer data) {
  (void)c;(void)sender;(void)path;(void)iface;(void)signal;(void)parameters;
  Editor *e=data; load_profile(e); status(e,tr("Device write failed or was resynchronized. Values reloaded from ratbagd."));
}
static void apply (GtkButton *button,gpointer data) {
  (void)button; Editor *e=data; g_autoptr(GError) error=NULL;
  if(e->tainted) { status(e,tr("Reconnect the device and reopen the editor before writing.")); return; }
  g_autoptr(GPtrArray) values=g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
  /* Parse every field before changing daemon state. */
  for(guint i=0;i<e->items->len;i++) {
    Field *f=g_ptr_array_index(e->items,i);
    GVariant *v=g_variant_parse(g_variant_get_type(f->original),gtk_editable_get_text(GTK_EDITABLE(f->entry)),NULL,NULL,&error);
    if(!v) { g_autofree char *m=g_strdup_printf("%s: %s",f->key,error->message);status(e,m);return; }
    g_ptr_array_add(values,g_variant_ref_sink(v));
  }
  /* Do not overwrite changes made by another client since the last reload. */
  for(guint i=0;i<e->items->len;i++) {
    Field *f=g_ptr_array_index(e->items,i); GVariant *v=g_ptr_array_index(values,i);
    if(g_variant_equal(v,f->original)) continue;
    g_autoptr(GVariant) current=prop(e,f->path,f->iface,f->property,&error);
    if(!current || !g_variant_equal(current,f->original)) {
      g_autofree char *msg=g_strdup_printf(tr("%s changed or is unavailable. Reload before applying. %s"),f->key,error?error->message:"");
      status(e,msg); return;
    }
  }
  for(guint i=0;i<e->items->len;i++) {
    Field *f=g_ptr_array_index(e->items,i); GVariant *v=g_ptr_array_index(values,i);
    if(g_variant_equal(v,f->original)) continue;
    if(!set_prop(e,f,v,&error)) {
      gboolean restored=TRUE;
      for(guint j=0;j<i;j++) {
        Field *old=g_ptr_array_index(e->items,j);
        GVariant *vold=g_ptr_array_index(values,j);
        if(!g_variant_equal(vold,old->original) && !set_prop(e,old,old->original,NULL)) restored=FALSE;
      }
      e->tainted=!restored;
      g_autofree char *m=g_strdup_printf(tr("%s: %s. %s"),f->key,error->message,
        restored ? tr("Staged changes rolled back; no commit requested.") : tr("Rollback failed; reconnect the device before writing again."));status(e,m); return;
    }
  }
  if(!mouse_ratbag_method(e->bus,e->device,IFACE("Device"),"Commit",&error)) { e->tainted=TRUE; status(e,error->message); return; }
  for(guint i=0;i<e->items->len;i++) { Field *f=g_ptr_array_index(e->items,i); g_variant_unref(f->original); f->original=g_variant_ref(g_ptr_array_index(values,i)); }
  status(e,tr("Write requested. ratbagd may report an asynchronous failure; verify by reloading."));
}
static void activate (GtkButton *button,gpointer data) {
  (void)button; Editor *e=data; if(e->tainted) {status(e,tr("Reconnect the device before writing."));return;} guint n=gtk_drop_down_get_selected(GTK_DROP_DOWN(e->profiles)); if(n>=e->profile_paths->len)return;
  g_autoptr(GError) err=NULL;
  if(!mouse_ratbag_method(e->bus,g_ptr_array_index(e->profile_paths,n),IFACE("Profile"),"SetActive",&err)) {status(e,err->message);return;}
  if(!mouse_ratbag_method(e->bus,e->device,IFACE("Device"),"Commit",&err)) {e->tainted=TRUE;status(e,err->message);return;}
  status(e,tr("Profile activation requested; verify on the device."));
}
static char *local_path (Editor *e) { (void)e; return g_build_filename(g_get_user_config_dir(),"epinephrine", "mouse-profiles.ini",NULL); }
static char *local_group (Editor *e, guint n) {
  const char *name=gtk_editable_get_text(GTK_EDITABLE(e->copy_name));
  g_autofree char *hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,name,-1);
  return g_strdup_printf("%s-profile-%u-%s",e->model,n,hash);
}
static void local_save (GtkButton *button,gpointer data) {
  (void)button; Editor *e=data; guint n=gtk_drop_down_get_selected(GTK_DROP_DOWN(e->profiles));
  g_autofree char *path=local_path(e), *dir=g_path_get_dirname(path), *group=local_group(e,n);
  g_autoptr(GKeyFile) k=g_key_file_new(); g_autoptr(GError) err=NULL;
  if(g_file_test(path,G_FILE_TEST_EXISTS) && !g_key_file_load_from_file(k,path,G_KEY_FILE_NONE,&err)) {status(e,err->message);return;}
  for(guint i=0;i<e->items->len;i++) { Field *f=g_ptr_array_index(e->items,i); g_autoptr(GVariant) v=g_variant_parse(g_variant_get_type(f->original),gtk_editable_get_text(GTK_EDITABLE(f->entry)),NULL,NULL,&err); if(!v){status(e,err->message);return;} g_key_file_set_string(k,group,f->key,gtk_editable_get_text(GTK_EDITABLE(f->entry))); }
  if(g_mkdir_with_parents(dir,0700)<0 || !g_key_file_save_to_file(k,path,&err)) { status(e,err?err->message:tr("Cannot create profile directory"));return; }
  status(e,tr("Local copy saved; no hardware changes were made."));
}
static void local_load (GtkButton *button,gpointer data) {
  (void)button; Editor *e=data; guint n=gtk_drop_down_get_selected(GTK_DROP_DOWN(e->profiles));
  g_autofree char *path=local_path(e), *group=local_group(e,n); g_autoptr(GKeyFile) k=g_key_file_new(); g_autoptr(GError) err=NULL;
  if(!g_key_file_load_from_file(k,path,G_KEY_FILE_NONE,&err) || !g_key_file_has_group(k,group)){ status(e,err?err->message:tr("No local copy for this profile."));return; }
  for(guint i=0;i<e->items->len;i++) { Field *f=g_ptr_array_index(e->items,i); g_autofree char *v=g_key_file_get_string(k,group,f->key,NULL); if(v) gtk_editable_set_text(GTK_EDITABLE(f->entry),v); }
  status(e,tr("Local copy loaded for editing; apply to write to hardware."));
}
static void reload (GtkButton *b,gpointer data) {(void)b;load_profile(data);}
static GtkWidget *button (const char *label,GCallback cb,Editor *e) { GtkWidget *b=gtk_button_new_with_label(tr(label));g_signal_connect(b,"clicked",cb,e);return b; }
void mouse_ratbag_open (GtkWindow *parent,guint16 vendor,guint16 product) {
  Editor *e=g_new0(Editor,1); e->items=g_ptr_array_new_with_free_func(field_free);e->profile_paths=g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GError) err=NULL;
  g_autoptr(GVariant) api=NULL, devices=NULL, profiles=NULL;
  g_autofree char *prefix=NULL;
  e->bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&err);
  if(!e->bus) goto fail;
  api=prop(e,ROOT_PATH,IFACE("Manager"),"APIVersion",&err);
  if(!api || !g_variant_is_of_type(api,G_VARIANT_TYPE_INT32) || g_variant_get_int32(api)!=2) { if(!err)g_set_error_literal(&err,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"ratbagd API version 2 is required");goto fail; }
  devices=prop(e,ROOT_PATH,IFACE("Manager"),"Devices",&err); if(!devices) goto fail;
  GVariantIter iter; const char *path; guint matches=0; prefix=g_strdup_printf("usb:%04x:%04x:",vendor,product);
  g_variant_iter_init(&iter,devices);
  while(g_variant_iter_next(&iter,"&o",&path)) { g_autoptr(GVariant) m=prop(e,path,IFACE("Device"),"Model",NULL); if(m && g_variant_is_of_type(m,G_VARIANT_TYPE_STRING) && g_str_has_prefix(g_variant_get_string(m,NULL),prefix)) {matches++;g_free(e->device);g_free(e->model);e->device=g_strdup(path);e->model=g_variant_dup_string(m,NULL);} }
  if(matches!=1) {g_set_error(&err,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,matches?"Multiple matching devices; disconnect extras before editing":"Device not found in ratbagd; check service, permissions and udev rules");goto fail;}
  profiles=prop(e,e->device,IFACE("Device"),"Profiles",&err); if(!profiles)goto fail;
  g_variant_iter_init(&iter,profiles); while(g_variant_iter_next(&iter,"&o",&path)) g_ptr_array_add(e->profile_paths,g_strdup(path));
  if(!e->profile_paths->len){g_set_error_literal(&err,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"No hardware profiles reported");goto fail;}
  e->window=gtk_window_new(); gtk_window_set_title(GTK_WINDOW(e->window),tr("Mouse profiles · ratbagd"));gtk_window_set_default_size(GTK_WINDOW(e->window),760,700);if(parent)gtk_window_set_transient_for(GTK_WINDOW(e->window),parent);
  g_object_set_data_full(G_OBJECT(e->window),"mouse-editor",e,editor_free);
  GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8); gtk_widget_set_margin_start(box,12);gtk_widget_set_margin_end(box,12);gtk_widget_set_margin_top(box,12);gtk_widget_set_margin_bottom(box,12);gtk_window_set_child(GTK_WINDOW(e->window),box);
  GtkStringList *model=gtk_string_list_new(NULL); for(guint i=0;i<e->profile_paths->len;i++){g_autofree char *s=g_strdup_printf(tr("Profile %u"),i+1);gtk_string_list_append(model,s);}
  e->profiles=gtk_drop_down_new(G_LIST_MODEL(model),NULL);gtk_box_append(GTK_BOX(box),e->profiles);g_signal_connect(e->profiles,"notify::selected",G_CALLBACK(profile_changed),e);
  GtkWidget *help=gtk_label_new(tr("Values use GLib variant syntax. Mapping examples: (uint32 1, <uint32 1>) = left click; (uint32 2, <uint32 1073741837>) = cycle profile. See the mouse documentation."));gtk_label_set_wrap(GTK_LABEL(help),TRUE);gtk_label_set_xalign(GTK_LABEL(help),0);gtk_box_append(GTK_BOX(box),help);
  GtkWidget *scroll=gtk_scrolled_window_new();gtk_widget_set_vexpand(scroll,TRUE);e->fields=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),e->fields);gtk_box_append(GTK_BOX(box),scroll);
  e->copy_name=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(e->copy_name),tr("Local copy name"));
  gtk_editable_set_text(GTK_EDITABLE(e->copy_name),"default"); gtk_box_append(GTK_BOX(box),e->copy_name);
  GtkWidget *bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);gtk_box_append(GTK_BOX(box),bar);
  gtk_box_append(GTK_BOX(bar),button("Reload",G_CALLBACK(reload),e));gtk_box_append(GTK_BOX(bar),button("Save local",G_CALLBACK(local_save),e));gtk_box_append(GTK_BOX(bar),button("Load local",G_CALLBACK(local_load),e));gtk_box_append(GTK_BOX(bar),button("Activate profile",G_CALLBACK(activate),e));gtk_box_append(GTK_BOX(bar),button("Apply to mouse",G_CALLBACK(apply),e));
  e->status=gtk_label_new("");gtk_label_set_wrap(GTK_LABEL(e->status),TRUE);gtk_label_set_xalign(GTK_LABEL(e->status),0);gtk_box_append(GTK_BOX(box),e->status);
  e->signal=g_dbus_connection_signal_subscribe(e->bus,BUS_NAME,IFACE("Device"),"Resync",e->device,NULL,G_DBUS_SIGNAL_FLAGS_NONE,resync,e,NULL);
  load_profile(e);gtk_window_present(GTK_WINDOW(e->window));return;
fail:
  { GtkWidget *d=gtk_message_dialog_new(parent,GTK_DIALOG_MODAL,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",tr("Cannot open mouse profiles"));gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),"%s",err?err->message:"Unknown error");g_signal_connect_swapped(d,"response",G_CALLBACK(gtk_window_destroy),d);gtk_window_present(GTK_WINDOW(d));editor_free(e); }
}
