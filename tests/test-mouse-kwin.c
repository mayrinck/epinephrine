#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "pages/mouse-kwin.h"
#define ROOT "/org/kde/KWin/InputDevice"
#define PATH ROOT "/event9"
#define DEV "org.kde.KWin.InputDevice"
#define MAN "org.kde.KWin.InputDeviceManager"
static const char xml[]="<node><interface name='" MAN "'><method name='ListPointers'><arg type='as' direction='out'/></method></interface><interface name='" DEV "'>"
"<property name='supportedButtons' type='i' access='read'/><property name='vendor' type='u' access='read'/><property name='product' type='u' access='read'/>"
"<property name='pointer' type='b' access='read'/><property name='touchpad' type='b' access='read'/><property name='isVirtual' type='b' access='read'/>"
"<property name='supportsPointerAcceleration' type='b' access='read'/><property name='supportsNaturalScroll' type='b' access='read'/>"
"<property name='supportsPointerAccelerationProfileFlat' type='b' access='read'/><property name='supportsPointerAccelerationProfileAdaptive' type='b' access='read'/>"
"<property name='defaultPointerAccelerationProfileFlat' type='b' access='read'/><property name='defaultPointerAccelerationProfileAdaptive' type='b' access='read'/>"
"<property name='pointerAcceleration' type='d' access='readwrite'/><property name='naturalScroll' type='b' access='readwrite'/>"
"<property name='pointerAccelerationProfileFlat' type='b' access='readwrite'/><property name='pointerAccelerationProfileAdaptive' type='b' access='readwrite'/>"
"<property name='scrollFactor' type='d' access='readwrite'/><property name='supportsScrollOnButtonDown' type='b' access='read'/><property name='scrollOnButtonDown' type='b' access='readwrite'/></interface></node>";
typedef struct {GMutex lock;GCond cond;gboolean ready;GMainLoop *loop;double speed,factor;gboolean flat,natural,deny,scroll_button;} Mock;
static GVariant *get(GDBusConnection *c,const gchar *s,const gchar *p,const gchar *i,const gchar *k,GError **err,gpointer data){
 (void)c;(void)s;(void)p;(void)i;(void)err;Mock *m=data;
 if(g_str_equal(k,"supportedButtons"))return g_variant_new_int32(31);
 if(g_str_equal(k,"vendor"))return g_variant_new_uint32(0x46d);
 if(g_str_equal(k,"product"))return g_variant_new_uint32(0xc246);
 if(g_str_equal(k,"pointerAcceleration"))return g_variant_new_double(m->speed);
 if(g_str_equal(k,"scrollFactor"))return g_variant_new_double(m->factor);
 if(g_str_equal(k,"scrollOnButtonDown"))return g_variant_new_boolean(m->scroll_button);
 if(g_str_equal(k,"naturalScroll"))return g_variant_new_boolean(m->natural);
 if(g_str_equal(k,"pointerAccelerationProfileFlat"))return g_variant_new_boolean(m->flat);
 if(g_str_equal(k,"pointerAccelerationProfileAdaptive"))return g_variant_new_boolean(!m->flat);
 if(g_str_equal(k,"touchpad")||g_str_equal(k,"isVirtual")||g_str_equal(k,"defaultPointerAccelerationProfileFlat"))return g_variant_new_boolean(FALSE);
 return g_variant_new_boolean(TRUE);
}
static gboolean set(GDBusConnection *c,const gchar *s,const gchar *p,const gchar *i,const gchar *k,GVariant *v,GError **err,gpointer data){
 (void)c;(void)s;(void)p;(void)i;Mock *m=data;
 if(g_str_equal(k,"naturalScroll")&&m->deny){m->deny=FALSE;g_set_error_literal(err,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED,"denied");return FALSE;}
 if(g_str_equal(k,"pointerAcceleration"))m->speed=g_variant_get_double(v);
 if(g_str_equal(k,"scrollFactor"))m->factor=g_variant_get_double(v);
 if(g_str_equal(k,"scrollOnButtonDown"))m->scroll_button=g_variant_get_boolean(v);
 if(g_str_equal(k,"naturalScroll"))m->natural=g_variant_get_boolean(v);
 if(g_str_equal(k,"pointerAccelerationProfileFlat"))m->flat=g_variant_get_boolean(v);
 if(g_str_equal(k,"pointerAccelerationProfileAdaptive"))m->flat=!g_variant_get_boolean(v);
 return TRUE;
}
static void method(GDBusConnection *c,const gchar *s,const gchar *p,const gchar *i,const gchar *name,GVariant *args,GDBusMethodInvocation *inv,gpointer data){
 (void)c;(void)s;(void)p;(void)i;(void)name;(void)args;(void)data;const char *a[]={"event9",NULL};g_dbus_method_invocation_return_value(inv,g_variant_new("(^as)",a));
}
static const GDBusInterfaceVTable vt={.method_call=method,.get_property=get,.set_property=set};
static gpointer server(gpointer data){
 Mock *m=data;GMainContext *ctx=g_main_context_new();g_main_context_push_thread_default(ctx);g_autoptr(GError) err=NULL;
 GDBusConnection *c=g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&err);g_assert_no_error(err);
 g_autoptr(GDBusNodeInfo) node=g_dbus_node_info_new_for_xml(xml,&err);g_assert_no_error(err);
 guint a=g_dbus_connection_register_object(c,ROOT,node->interfaces[0],&vt,m,NULL,&err);g_assert_no_error(err);guint b=g_dbus_connection_register_object(c,PATH,node->interfaces[1],&vt,m,NULL,&err);g_assert_no_error(err);
 g_autoptr(GVariant) own=g_dbus_connection_call_sync(c,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(su)","org.kde.KWin",0),G_VARIANT_TYPE("(u)"),0,5000,NULL,&err);g_assert_no_error(err);
 GMainLoop *loop=g_main_loop_new(ctx,FALSE);g_mutex_lock(&m->lock);m->loop=loop;m->ready=TRUE;g_cond_signal(&m->cond);g_mutex_unlock(&m->lock);g_main_loop_run(loop);
 g_dbus_connection_unregister_object(c,a);g_dbus_connection_unregister_object(c,b);g_object_unref(c);g_main_loop_unref(loop);g_main_context_pop_thread_default(ctx);g_main_context_unref(ctx);return NULL;
}
static void test_kwin(void){
 int probe=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un addr={.sun_family=AF_UNIX};g_snprintf(addr.sun_path,sizeof addr.sun_path,"%s/epi-kwin-%ld",g_get_tmp_dir(),(long)getpid());
 if(probe<0||bind(probe,(struct sockaddr *)&addr,sizeof addr)<0){if(probe>=0)close(probe);g_test_skip("Unix sockets unavailable");return;}close(probe);unlink(addr.sun_path);
 g_autoptr(GTestDBus) bus=g_test_dbus_new(G_TEST_DBUS_NONE);g_test_dbus_up(bus);Mock m={.factor=1};g_mutex_init(&m.lock);g_cond_init(&m.cond);GThread *t=g_thread_new("mock-kwin",server,&m);g_mutex_lock(&m.lock);while(!m.ready)g_cond_wait(&m.cond,&m.lock);g_mutex_unlock(&m.lock);
 g_autoptr(GError) err=NULL;g_autoptr(GDBusConnection) c=g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&err);g_assert_no_error(err);
 g_autofree char *path=NULL;g_assert_true(mouse_kwin_find(c,0x46d,0xc246,&path,&err));g_assert_no_error(err);g_assert_cmpstr(path,==,PATH);
 MouseKwinState v;g_assert_true(mouse_kwin_read(c,path,&v,&err));g_assert_cmpuint(v.profile,==,2);g_assert_cmpfloat(v.scroll_factor,==,1);
 v.speed=0.5;v.profile=1;v.natural=TRUE;v.scroll_factor=2;v.scroll_on_button=TRUE;g_assert_true(mouse_kwin_write(c,path,&v,&err));g_assert_no_error(err);MouseKwinState actual;g_assert_true(mouse_kwin_read(c,path,&actual,&err));g_assert_cmpfloat(actual.speed,==,0.5);g_assert_cmpuint(actual.profile,==,1);g_assert_true(actual.natural);g_assert_cmpfloat(actual.scroll_factor,==,2);g_assert_true(actual.scroll_on_button);
 m.deny=TRUE;v.speed=-0.5;v.profile=2;v.natural=FALSE;g_assert_false(mouse_kwin_write(c,path,&v,&err));g_assert_error(err,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED);g_clear_error(&err);g_assert_true(mouse_kwin_read(c,path,&actual,&err));g_assert_cmpfloat(actual.speed,==,0.5);g_assert_cmpuint(actual.profile,==,1);g_assert_true(actual.natural);
 g_main_loop_quit(m.loop);g_thread_join(t);g_dbus_connection_close_sync(c,NULL,NULL);g_test_dbus_down(bus);g_mutex_clear(&m.lock);g_cond_clear(&m.cond);
}
int main(int argc,char **argv){g_test_init(&argc,&argv,NULL);g_test_add_func("/mouse/kwin/roundtrip",test_kwin);return g_test_run();}
