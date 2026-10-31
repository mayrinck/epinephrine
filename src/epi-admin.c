#include <gio/gio.h>
#include <unistd.h>
int main(void) {
  if (geteuid () != 0) return 1;
  GBytes *bytes = g_resources_lookup_data ("/io/github/mayrinck/epinephrine/scripts/admin_helper.py",0,NULL);
  if (!bytes) return 1;
  char *code = g_strndup (g_bytes_get_data(bytes,NULL),g_bytes_get_size(bytes));
  execl ("/usr/bin/python3", "python3", "-I", "-c", code, NULL);
  return 1;
}
