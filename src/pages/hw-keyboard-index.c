#include "hw-keyboard-index.h"
#include <gio/gio.h>
#include <stdio.h>
char *
hw_keyboard_index_lookup (guint16 vendor, guint16 product, const char *input_name)
{
  if (!input_name || !*input_name) return NULL;
  g_autoptr(GBytes) bytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/KEYBOARD_INDEX.md", 0, NULL);
  if (!bytes) return NULL;
  gsize length=0; const char *data=g_bytes_get_data(bytes,&length);
  g_autofree char *text=g_strndup(data,length);
  g_auto(GStrv) lines=g_strsplit(text,"\n",-1);
  for(guint i=0;lines[i];i++) {
    if(lines[i][0]!='|')continue;
    g_auto(GStrv) cols=g_strsplit(lines[i],"|",-1);
    if(g_strv_length(cols)<9)continue;
    guint v=0,p=0;
    if(sscanf(g_strstrip(cols[2]),"%x:%x",&v,&p)!=2 || v!=vendor || p!=product)continue;
    const char *expected=g_strstrip(cols[7]);
    if(*expected && g_ascii_strcasecmp(expected,input_name)==0)
      return g_strdup(g_strstrip(cols[1]));
  }
  return NULL;
}
