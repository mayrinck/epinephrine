#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "pages/mouse-ratbag-client.h"

#define PATH "/org/freedesktop/ratbag1/button/mouse/p0/b8"
#define IFACE "org.freedesktop.ratbag1.Button"
static const char xml[] = "<node><interface name='" IFACE "'>"
  "<property name='Mapping' type='(uv)' access='readwrite'/>"
  "<property name='Denied' type='u' access='readwrite'/>"
  "<method name='Commit'><arg type='u' direction='out'/></method></interface></node>";
typedef struct { GMutex lock; GCond cond; gboolean ready; GMainLoop *loop; guint type, val, commits; gboolean fail_commit; } Mock;
static GVariant *get_property (GDBusConnection *c,const gchar *sender,const gchar *path,const gchar *iface,const gchar *name,GError **err,gpointer data) {
  (void)c;(void)sender;(void)path;(void)iface;(void)name;(void)err; Mock *m=data;
  return g_variant_new("(uv)",m->type,g_variant_new_uint32(m->val));
}
static gboolean set_property (GDBusConnection *c,const gchar *sender,const gchar *path,const gchar *iface,const gchar *name,GVariant *value,GError **err,gpointer data) {
  (void)c;(void)sender;(void)path;(void)iface; Mock *m=data;
  if(g_str_equal(name,"Denied")) { g_set_error_literal(err,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED,"permission denied"); return FALSE; }
  g_autoptr(GVariant) inner=NULL; g_variant_get(value,"(uv)",&m->type,&inner);m->val=g_variant_get_uint32(inner);return TRUE;
}
static void method (GDBusConnection *c,const gchar *sender,const gchar *path,const gchar *iface,const gchar *name,GVariant *p,GDBusMethodInvocation *inv,gpointer data) {
  (void)c;(void)sender;(void)path;(void)iface;(void)name;(void)p;Mock *m=data;m->commits++;g_dbus_method_invocation_return_value(inv,g_variant_new("(u)",m->fail_commit ? (guint32)-1001 : 0));
}
static const GDBusInterfaceVTable vtable={.method_call=method,.get_property=get_property,.set_property=set_property};
static gpointer server_thread (gpointer data) {
  Mock *m=data; GMainContext *ctx=g_main_context_new();g_main_context_push_thread_default(ctx);g_autoptr(GError) err=NULL;
  GDBusConnection *conn=g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&err);g_assert_no_error(err);
  g_autoptr(GDBusNodeInfo) node=g_dbus_node_info_new_for_xml(xml,&err);g_assert_no_error(err);
  guint id=g_dbus_connection_register_object(conn,PATH,node->interfaces[0],&vtable,m,NULL,&err);g_assert_no_error(err);
  g_autoptr(GVariant) owned=g_dbus_connection_call_sync(conn,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(su)",MOUSE_RATBAG_BUS,0),G_VARIANT_TYPE("(u)"),0,5000,NULL,&err);g_assert_no_error(err);
  GMainLoop *loop=g_main_loop_new(ctx,FALSE);g_mutex_lock(&m->lock);m->loop=loop;m->ready=TRUE;g_cond_signal(&m->cond);g_mutex_unlock(&m->lock);
  g_main_loop_run(loop);g_dbus_connection_unregister_object(conn,id);g_object_unref(conn);g_main_loop_unref(loop);g_main_context_pop_thread_default(ctx);g_main_context_unref(ctx);return NULL;
}
static void test_roundtrip (void) {
  int probe=socket(AF_UNIX,SOCK_STREAM,0);
  struct sockaddr_un addr={.sun_family=AF_UNIX};
  g_snprintf(addr.sun_path,sizeof addr.sun_path,"%s/epinephrine-dbus-%ld",g_get_tmp_dir(),(long)getpid());
  if(probe<0 || bind(probe,(struct sockaddr *)&addr,sizeof addr)<0) {
    if(probe>=0)close(probe);
    g_test_skip("Unix socket binding is unavailable in this sandbox"); return;
  }
  close(probe);unlink(addr.sun_path);
  g_autoptr(GTestDBus) bus=g_test_dbus_new(G_TEST_DBUS_NONE);g_test_dbus_up(bus);
  Mock m={0};g_mutex_init(&m.lock);g_cond_init(&m.cond);m.type=1;m.val=1;GThread *thread=g_thread_new("mock-ratbag",server_thread,&m);
  g_mutex_lock(&m.lock);while(!m.ready)g_cond_wait(&m.cond,&m.lock);g_mutex_unlock(&m.lock);
  g_autoptr(GError) err=NULL;g_autoptr(GDBusConnection) client=g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,NULL,NULL,&err);g_assert_no_error(err);
  g_autoptr(GVariant) before=mouse_ratbag_get(client,PATH,IFACE,"Mapping",&err);g_assert_no_error(err);g_assert_true(g_variant_is_of_type(before,G_VARIANT_TYPE("(uv)")));
  g_assert_true(mouse_ratbag_set(client,PATH,IFACE,"Mapping",g_variant_new("(uv)",2,g_variant_new_uint32(1073741837)),&err));g_assert_no_error(err);
  g_autoptr(GVariant) after=mouse_ratbag_get(client,PATH,IFACE,"Mapping",&err);g_assert_no_error(err);guint type=0;g_autoptr(GVariant) inner=NULL;g_variant_get(after,"(uv)",&type,&inner);g_assert_cmpuint(type,==,2);g_assert_cmpuint(g_variant_get_uint32(inner),==,1073741837);
  g_assert_false(mouse_ratbag_set(client,PATH,IFACE,"Denied",g_variant_new_uint32(0),&err));g_assert_error(err,G_DBUS_ERROR,G_DBUS_ERROR_ACCESS_DENIED);g_clear_error(&err);
  g_assert_true(mouse_ratbag_method(client,PATH,IFACE,"Commit",&err));g_assert_no_error(err);
  m.fail_commit=TRUE;g_assert_false(mouse_ratbag_method(client,PATH,IFACE,"Commit",&err));g_assert_error(err,G_IO_ERROR,G_IO_ERROR_FAILED);g_clear_error(&err);
  g_main_loop_quit(m.loop);g_thread_join(thread);g_dbus_connection_close_sync(client,NULL,NULL);g_test_dbus_down(bus);g_mutex_clear(&m.lock);g_cond_clear(&m.cond);
}
int main(int argc,char **argv) {g_test_init(&argc,&argv,NULL);g_test_add_func("/mouse/ratbag/roundtrip",test_roundtrip);return g_test_run();}
