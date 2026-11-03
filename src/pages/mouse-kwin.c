#include "mouse-kwin.h"
#include <math.h>
#include <string.h>
#define NAME "org.kde.KWin"
#define ROOT "/org/kde/KWin/InputDevice"
#define MANAGER "org.kde.KWin.InputDeviceManager"
#define DEVICE "org.kde.KWin.InputDevice"
#define PROPS "org.freedesktop.DBus.Properties"
static GVariant *call(GDBusConnection *bus,const char *path,const char *interface,const char *method,GVariant *args,const GVariantType *reply,GError **error) {
  return g_dbus_connection_call_sync(bus,NAME,path,interface,method,args,reply,G_DBUS_CALL_FLAGS_NONE,3000,NULL,error);
}
static GVariant *all(GDBusConnection *bus,const char *path,GError **error) {
  g_autoptr(GVariant) r=call(bus,path,PROPS,"GetAll",g_variant_new("(s)",DEVICE),G_VARIANT_TYPE("(a{sv})"),error);
  if (!r) return NULL;
  GVariant *dict=NULL;
  g_variant_get(r,"(@a{sv})",&dict);
  return dict;
}
static gboolean get_bool(GVariant *d,const char *key,gboolean *v) {return g_variant_lookup(d,key,"b",v);}
gboolean mouse_kwin_find(GDBusConnection *bus,guint16 vendor,guint16 product,char **path,GError **error) {
  *path=NULL;
  g_autoptr(GVariant) r=call(bus,ROOT,MANAGER,"ListPointers",NULL,G_VARIANT_TYPE("(as)"),error);if(!r)return FALSE;
  g_auto(GStrv) names=NULL;g_variant_get(r,"(^as)",&names);guint count=0;
  for(guint i=0;names[i];i++) {
    if(!g_str_has_prefix(names[i],"event"))continue;
    const char *p=names[i]+5;if(!*p)continue;while(g_ascii_isdigit(*p))p++;if(*p)continue;
    g_autofree char *candidate=g_strdup_printf(ROOT "/%s",names[i]);g_autoptr(GVariant) d=all(bus,candidate,NULL);
    guint32 v=0,id=0;gint32 buttons=0;gboolean pointer=FALSE,touchpad=TRUE,virt=TRUE;
    if(!d || !g_variant_lookup(d,"vendor","u",&v) || !g_variant_lookup(d,"product","u",&id) ||
       !get_bool(d,"pointer",&pointer) || !get_bool(d,"touchpad",&touchpad) ||
       !get_bool(d,"isVirtual",&virt) || !g_variant_lookup(d,"supportedButtons","i",&buttons))continue;
    if(pointer && !touchpad && !virt && buttons != 0 && v==vendor && id==product) {count++;g_free(*path);*path=g_strdup(candidate);}
  }
  if(count!=1){g_clear_pointer(path,g_free);g_set_error(error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND,count ? "Multiple matching mice; disconnect duplicates" : "Mouse not found in KWin");return FALSE;}
  return TRUE;
}
gboolean mouse_kwin_read(GDBusConnection *bus,const char *path,MouseKwinState *state,GError **error) {
  g_autoptr(GVariant) d=all(bus,path,error);if(!d)return FALSE;
  gboolean speed=FALSE,natural=FALSE,flat=FALSE,adaptive=FALSE,profile_flat=FALSE,profile_adaptive=FALSE;
  memset(state,0,sizeof *state);
  if(!get_bool(d,"supportsPointerAcceleration",&speed) || !speed ||
     !get_bool(d,"supportsNaturalScroll",&natural) || !natural ||
     !get_bool(d,"supportsPointerAccelerationProfileFlat",&flat) || !flat ||
     !get_bool(d,"supportsPointerAccelerationProfileAdaptive",&adaptive) || !adaptive ||
     !g_variant_lookup(d,"pointerAcceleration","d",&state->speed) ||
     !get_bool(d,"naturalScroll",&state->natural) ||
     !get_bool(d,"pointerAccelerationProfileFlat",&profile_flat) ||
     !get_bool(d,"pointerAccelerationProfileAdaptive",&profile_adaptive)) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Mouse does not expose all required KWin settings");return FALSE;
  }
  state->profile=profile_flat ? 1 : profile_adaptive ? 2 : 0;
  state->has_scroll_factor=g_variant_lookup(d,"scrollFactor","d",&state->scroll_factor);
  gboolean supports=FALSE;
  state->has_scroll_on_button=get_bool(d,"supportsScrollOnButtonDown",&supports) && supports &&
    get_bool(d,"scrollOnButtonDown",&state->scroll_on_button);
  return TRUE;
}
static gboolean set(GDBusConnection *bus,const char *path,const char *key,GVariant *value,GError **error) {
  g_autoptr(GVariant) r=call(bus,path,PROPS,"Set",g_variant_new("(ssv)",DEVICE,key,value),G_VARIANT_TYPE("()"),error);return r!=NULL;
}
gboolean mouse_kwin_write(GDBusConnection *bus,const char *path,const MouseKwinState *desired,GError **error) {
  if(!isfinite(desired->speed) || desired->speed < -1 || desired->speed > 1 || desired->profile>2 ||
     (desired->has_scroll_factor && (!isfinite(desired->scroll_factor) || desired->scroll_factor < 0.1 || desired->scroll_factor > 10))) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Invalid KWin mouse setting");return FALSE;
  }
  MouseKwinState old;if(!mouse_kwin_read(bus,path,&old,error))return FALSE;
  g_autoptr(GVariant) d=all(bus,path,error);if(!d)return FALSE;
  gboolean default_flat=FALSE,default_adaptive=FALSE;
  if(desired->profile==0 && (!get_bool(d,"defaultPointerAccelerationProfileFlat",&default_flat) || !get_bool(d,"defaultPointerAccelerationProfileAdaptive",&default_adaptive))) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Default acceleration profile unavailable");return FALSE;
  }
  if (desired->profile==0 && default_flat==default_adaptive) {
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Unsupported default acceleration profile");return FALSE;
  }
  gboolean flat=desired->profile==1 || (desired->profile==0 && default_flat);
  gboolean adaptive=desired->profile==2 || (desired->profile==0 && default_adaptive);
  gboolean ok=set(bus,path,"pointerAcceleration",g_variant_new_double(desired->speed),error) &&
    set(bus,path,"pointerAccelerationProfileFlat",g_variant_new_boolean(flat),error) &&
    set(bus,path,"pointerAccelerationProfileAdaptive",g_variant_new_boolean(adaptive),error) &&
    set(bus,path,"naturalScroll",g_variant_new_boolean(desired->natural),error) &&
    (!desired->has_scroll_factor || (old.has_scroll_factor && set(bus,path,"scrollFactor",g_variant_new_double(desired->scroll_factor),error))) &&
    (!desired->has_scroll_on_button || (old.has_scroll_on_button && set(bus,path,"scrollOnButtonDown",g_variant_new_boolean(desired->scroll_on_button),error)));
  MouseKwinState actual;
  if(ok && (!mouse_kwin_read(bus,path,&actual,error) || fabs(actual.speed-desired->speed)>0.0001 || actual.natural!=desired->natural ||
            (flat && actual.profile!=1) || (adaptive && actual.profile!=2) ||
            (desired->has_scroll_factor && (!actual.has_scroll_factor || fabs(actual.scroll_factor-desired->scroll_factor)>0.0001)) ||
            (desired->has_scroll_on_button && (!actual.has_scroll_on_button || actual.scroll_on_button!=desired->scroll_on_button)))) {
    ok=FALSE;if(error && !*error)g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"KWin did not apply the requested values");
  }
  if(!ok) {
    if(error && !*error) g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,"Requested mouse option is no longer available");
    set(bus,path,"pointerAcceleration",g_variant_new_double(old.speed),NULL);
    set(bus,path,"pointerAccelerationProfileFlat",g_variant_new_boolean(old.profile==1),NULL);
    set(bus,path,"pointerAccelerationProfileAdaptive",g_variant_new_boolean(old.profile==2),NULL);
    set(bus,path,"naturalScroll",g_variant_new_boolean(old.natural),NULL);
    if(old.has_scroll_factor)set(bus,path,"scrollFactor",g_variant_new_double(old.scroll_factor),NULL);
    if(old.has_scroll_on_button)set(bus,path,"scrollOnButtonDown",g_variant_new_boolean(old.scroll_on_button),NULL);
  }
  return ok;
}
