#include "epi-i18n.h"

#include <string.h>

#define EPI_TR_KEY "epi-tr-key"

static GHashTable *g_tr = NULL;
static char       *g_lang_name = NULL;

static char *
lang_file_name (int lang_index)
{
  switch (lang_index)
    {
    case 2: return g_strdup ("en_US");
    case 0:
    case 1:
    default: return g_strdup ("pt_BR");
    }
}

static char *
load_lang_contents (const char *name)
{
  char *res_path = g_strdup_printf (
    "/io/github/mayrinck/epinephrine/lang/%s.md", name);
  char *content = NULL;

  GBytes *gbytes = g_resources_lookup_data (res_path, 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    {
      char *path = g_strdup_printf (DATADIR "/epinephrine/lang/%s.md", name);
      g_file_get_contents (path, &content, NULL, NULL);
      g_free (path);
    }
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "lang", name, NULL);
      char *rel_md = g_strdup_printf ("%s.md", rel);
      g_file_get_contents (rel_md, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
      g_free (rel_md);
    }

  g_free (res_path);
  return content;
}

static void
replace_all (char *str, const char *from, const char *to)
{
  size_t from_len = strlen (from);
  size_t to_len = strlen (to);
  if (from_len == 0) return;
  char *p = str;
  while ((p = strstr (p, from)))
    {
      size_t rest = strlen (p + from_len) + 1;
      memmove (p + to_len, p + from_len, rest);
      memcpy (p, to, to_len);
      p += to_len;
    }
}

static void
fill_table_from_content (GHashTable *table, const char *content)
{
  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---"))
        {
          in_table = TRUE;
          continue;
        }

      if (!in_table || line[0] != '|') continue;

      char *dup = g_strdup (line);
      char **cells = g_strsplit (dup, "|", -1);

      int n = 0;
      for (; cells[n]; n++);
      if (n >= 3)
        {
          char *key = g_strdup (cells[1]);
          char *value = g_strdup (cells[2]);
          g_strstrip (key);
          g_strstrip (value);
          replace_all (key, "\\n", "\n");
          replace_all (value, "\\n", "\n");
          if (key[0] && value[0])
            {
              g_hash_table_remove (table, key);
              g_hash_table_insert (table, key, value);
            }
          else
            {
              g_free (key);
              g_free (value);
            }
        }

      g_strfreev (cells);
      g_free (dup);
    }

  g_strfreev (lines);
}

static void
load_language (int lang_index)
{
  if (g_tr)
    g_hash_table_unref (g_tr);
  g_free (g_lang_name);

  g_lang_name = lang_file_name (lang_index);
  g_tr = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, g_free);

  char *content = load_lang_contents (g_lang_name);
  if (content)
    {
      fill_table_from_content (g_tr, content);
      g_free (content);
    }
}

void
epi_i18n_set_language (int lang_index)
{
  load_language (lang_index);
}

const char *
epi_i18n_get (const char *key)
{
  if (!key) return key;
  if (g_tr)
    {
      const char *value = g_hash_table_lookup (g_tr, key);
      if (value) return value;
    }
  return key;
}

static void
translate_widget (GtkWidget *widget)
{
  if (!widget) return;

  if (GTK_IS_LABEL (widget))
    {
      const char *key = g_object_get_data (G_OBJECT (widget), EPI_TR_KEY);
      const char *text = key ? key : gtk_label_get_label (GTK_LABEL (widget));
      if (text && text[0])
        {
          const char *value = epi_i18n_get (text);
          if (value != text)
            {
              g_object_set_data_full (G_OBJECT (widget), EPI_TR_KEY,
                g_strdup (text), g_free);
              gtk_label_set_label (GTK_LABEL (widget), value);
            }
        }
    }
  else if (GTK_IS_BUTTON (widget))
    {
      const char *key = g_object_get_data (G_OBJECT (widget), EPI_TR_KEY);
      const char *text = key ? key : gtk_button_get_label (GTK_BUTTON (widget));
      if (text && text[0])
        {
          const char *value = epi_i18n_get (text);
          if (value != text)
            {
              g_object_set_data_full (G_OBJECT (widget), EPI_TR_KEY,
                g_strdup (text), g_free);
              gtk_button_set_label (GTK_BUTTON (widget), value);
            }
        }
    }
  else if (GTK_IS_EXPANDER (widget))
    {
      const char *key = g_object_get_data (G_OBJECT (widget), EPI_TR_KEY);
      const char *text = key ? key : gtk_expander_get_label (GTK_EXPANDER (widget));
      if (text && text[0])
        {
          const char *value = epi_i18n_get (text);
          if (value != text)
            {
              g_object_set_data_full (G_OBJECT (widget), EPI_TR_KEY,
                g_strdup (text), g_free);
              gtk_expander_set_label (GTK_EXPANDER (widget), value);
            }
        }
    }
  else if (ADW_IS_ACTION_ROW (widget))
    {
      AdwActionRow *row = ADW_ACTION_ROW (widget);
      const char *key = g_object_get_data (G_OBJECT (widget), EPI_TR_KEY);
      char *title = NULL;
      g_object_get (row, "title", &title, NULL);
      const char *title_text = key ? key : (title ? title : "");
      if (title_text[0])
        {
          const char *value = epi_i18n_get (title_text);
          if (value != title_text)
            {
              g_object_set_data_full (G_OBJECT (widget), EPI_TR_KEY,
                g_strdup (title_text), g_free);
              g_object_set (row, "title", value, NULL);
            }
        }
      g_free (title);
      const char *subtitle = adw_action_row_get_subtitle (row);
      if (subtitle && subtitle[0])
        {
          const char *value = epi_i18n_get (subtitle);
          if (value != subtitle)
            adw_action_row_set_subtitle (row, value);
        }
    }
  else if (ADW_IS_PREFERENCES_GROUP (widget))
    {
      AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (widget);
      const char *title = adw_preferences_group_get_title (group);
      if (title && title[0])
        {
          const char *value = epi_i18n_get (title);
          if (value != title)
            adw_preferences_group_set_title (group, value);
        }
      const char *desc = adw_preferences_group_get_description (group);
      if (desc && desc[0])
        {
          const char *value = epi_i18n_get (desc);
          if (value != desc)
            adw_preferences_group_set_description (group, value);
        }
    }
  else if (GTK_IS_WINDOW (widget))
    {
      const char *title = gtk_window_get_title (GTK_WINDOW (widget));
      if (title && title[0])
        {
          const char *value = epi_i18n_get (title);
          if (value != title)
            gtk_window_set_title (GTK_WINDOW (widget), value);
        }
    }
  else if (GTK_IS_ENTRY (widget))
    {
      const char *placeholder = gtk_entry_get_placeholder_text (GTK_ENTRY (widget));
      if (placeholder && placeholder[0])
        {
          const char *value = epi_i18n_get (placeholder);
          if (value != placeholder)
            gtk_entry_set_placeholder_text (GTK_ENTRY (widget), value);
        }
    }
}

static void
translate_children (GtkWidget *widget)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      translate_widget (child);
      translate_children (child);
      child = next;
    }
}

void
epi_i18n_apply_widgets (GtkWidget *root)
{
  if (!root) return;
  translate_widget (root);
  translate_children (root);
}
