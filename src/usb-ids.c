#include "usb-ids.h"

#include <string.h>
#include <stdlib.h>

#define USB_IDS_URL "http://www.linux-usb.org/usb.ids"

static const char *SYSTEM_PATHS[] = {
  "/usr/share/hwdata/usb.ids",
  "/usr/share/misc/usb.ids",
  "/var/lib/usbutils/usb.ids",
  "/usr/share/usb.ids",
  NULL
};

struct _UsbIds
{
  GHashTable *vendor_names;
  GHashTable *device_names;
  char *cache_path;
  char *cache_dir;
};

static gboolean
parse_usb_ids_data (UsbIds *ids, const char *data, gssize len)
{
  GHashTable *new_vendors = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, g_free);
  GHashTable *new_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, g_free);

  const char *end = data + (len < 0 ? (gssize) strlen (data) : len);
  const char *cur = data;
  char current_vendor[8] = "";

  while (cur < end)
    {
      const char *nl = memchr (cur, '\n', end - cur);
      if (!nl) nl = end;
      size_t line_len = nl - cur;
      const char *line = cur;
      cur = nl + 1;

      if (line_len == 0 || line[0] == '#')
        continue;

      /* Device line: one leading tab + 4 hex digits */
      if (line_len > 5 && line[0] == '\t' && line[1] != '\t')
        {
          const char *p = line + 1;
          while (p < nl && *p == ' ') p++;
          if (nl - p < 4) continue;

          char buf[8];
          int i = 0;
          while (p < nl && i < 6 && g_ascii_isxdigit (*p))
            buf[i++] = *(p++);
          buf[i] = '\0';
          if (i != 4) continue;

          while (p < nl && (*p == ' ' || *p == '\t')) p++;

          if (p < nl && current_vendor[0])
            {
              char *key = g_strdup_printf ("%s:%s", current_vendor, buf);
              char *name = g_strndup (p, nl - p);
              g_strstrip (name);
              if (name[0])
                g_hash_table_insert (new_devices, key, name);
              else
                {
                  g_free (key);
                  g_free (name);
                }
            }
          continue;
        }

      /* Interface line: two tabs — skip */
      if (line_len > 6 && line[0] == '\t' && line[1] == '\t')
        continue;

      /* Vendor line: 4 hex digits at line start, no leading tab */
      if (line_len >= 4 && g_ascii_isxdigit (line[0])
          && g_ascii_isxdigit (line[1]) && g_ascii_isxdigit (line[2])
          && g_ascii_isxdigit (line[3]))
        {
          const char *p = line;
          char buf[8];
          int i = 0;
          while (p < nl && i < 6 && g_ascii_isxdigit (*p))
            buf[i++] = *(p++);
          buf[i] = '\0';
          if (i != 4) continue;

          g_strlcpy (current_vendor, buf, sizeof (current_vendor));

          while (p < nl && (*p == ' ' || *p == '\t')) p++;

          if (p < nl)
            {
              char *name = g_strndup (p, nl - p);
              g_strstrip (name);
              if (name[0])
                g_hash_table_insert (new_vendors, g_strdup (buf), name);
              else
                g_free (name);
            }
          continue;
        }
    }

  /* Atomically swap in new tables */
  if (ids->vendor_names) g_hash_table_destroy (ids->vendor_names);
  if (ids->device_names) g_hash_table_destroy (ids->device_names);
  ids->vendor_names = new_vendors;
  ids->device_names = new_devices;

  return (g_hash_table_size (new_devices) > 0);
}

static gboolean
load_from_file (UsbIds *ids, const char *path)
{
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &contents, &len, NULL))
    return FALSE;

  gboolean ok = parse_usb_ids_data (ids, contents, (gssize) len);
  g_free (contents);
  return ok;
}

gboolean
usb_ids_load (UsbIds *ids)
{
  /* Try system paths first */
  for (int i = 0; SYSTEM_PATHS[i]; i++)
    {
      if (load_from_file (ids, SYSTEM_PATHS[i]))
        return TRUE;
    }

  /* Try cache file */
  if (ids->cache_path && load_from_file (ids, ids->cache_path))
    return TRUE;

  return FALSE;
}

const char *
usb_ids_lookup (UsbIds *ids, uint16_t vendor, uint16_t product)
{
  if (!ids || !ids->device_names) return NULL;
  char key[16];
  g_snprintf (key, sizeof (key), "%04x:%04x", vendor, product);
  return g_hash_table_lookup (ids->device_names, key);
}

/* ---- Async download via thread ---- */

typedef struct
{
  UsbIds *ids;
  char *data;
  gsize len;
} RefreshData;

static gboolean
refresh_process_on_main (gpointer user_data)
{
  RefreshData *rd = user_data;

  if (rd->data && rd->len > 0)
    {
      if (parse_usb_ids_data (rd->ids, rd->data, (gssize) rd->len))
        {
          if (rd->ids->cache_path)
            {
              g_mkdir_with_parents (rd->ids->cache_dir, 0755);
              g_file_set_contents (rd->ids->cache_path, rd->data, (gssize) rd->len, NULL);
            }
        }
    }

  g_free (rd->data);
  g_free (rd);
  return G_SOURCE_REMOVE;
}

static gpointer
refresh_in_thread (gpointer user_data)
{
  RefreshData *rd = user_data;
  GError *error = NULL;

  const char *argv[] = {
    "curl", "-sL", "--max-time", "30",
    "-A", "epinephrine/0.1.0",
    USB_IDS_URL,
    NULL
  };

  GSubprocessLauncher *launcher = g_subprocess_launcher_new (
    G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  GSubprocess *proc = g_subprocess_launcher_spawnv (launcher,
    (const char * const *) argv, &error);

  if (!proc || error)
    {
      if (error) g_error_free (error);
      if (proc) g_object_unref (proc);
      g_object_unref (launcher);
      g_idle_add (refresh_process_on_main, rd);
      return NULL;
    }

  GBytes *stdout_bytes = NULL;
  if (!g_subprocess_communicate (proc, NULL, NULL, &stdout_bytes, NULL, &error))
    {
      if (error) g_error_free (error);
      g_object_unref (proc);
      g_object_unref (launcher);
      g_idle_add (refresh_process_on_main, rd);
      return NULL;
    }

  if (stdout_bytes)
    {
      gsize size = 0;
      const char *data = g_bytes_get_data (stdout_bytes, &size);
      if (data && size > 0)
        {
          rd->data = g_memdup2 (data, size);
          rd->len = size;
        }
      g_bytes_unref (stdout_bytes);
    }

  g_object_unref (proc);
  g_object_unref (launcher);

  g_idle_add (refresh_process_on_main, rd);
  return NULL;
}

void
usb_ids_refresh_async (UsbIds *ids)
{
  if (!ids) return;
  if (!g_find_program_in_path ("curl")) return;

  RefreshData *rd = g_new0 (RefreshData, 1);
  rd->ids = ids;

  g_thread_new ("usb-ids-refresh", refresh_in_thread, rd);
}

UsbIds *
usb_ids_new (void)
{
  UsbIds *ids = g_new0 (UsbIds, 1);
  ids->vendor_names = NULL;
  ids->device_names = NULL;

  char *cache_dir = g_build_filename (g_get_user_cache_dir (), "epinephrine", NULL);
  ids->cache_dir = cache_dir;
  ids->cache_path = g_build_filename (cache_dir, "usb.ids", NULL);

  return ids;
}

void
usb_ids_free (UsbIds *ids)
{
  if (!ids) return;
  if (ids->vendor_names) g_hash_table_destroy (ids->vendor_names);
  if (ids->device_names) g_hash_table_destroy (ids->device_names);
  g_free (ids->cache_path);
  g_free (ids->cache_dir);
  g_free (ids);
}
