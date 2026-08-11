#include "epi-i18n.h"

#include <locale.h>
#include <string.h>

#define EPI_TR_KEY "epi-tr-key"
#define EPI_TR_COMBO_KEYS "epi-tr-combo-keys"

static GHashTable *g_tr = NULL;
static char       *g_lang_name = NULL;
static char       *g_cur_request = NULL;
static gboolean    g_in_apply = FALSE;
static GRecMutex   g_tr_lock;

/* ---- discovery helpers ---- */

static void
collect_dir_languages (const char *dir, GPtrArray *out)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d) return;

  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      if (g_str_has_suffix (name, ".md"))
        {
          char *code = g_strndup (name, strlen (name) - 3);
          g_ptr_array_add (out, code);
        }
    }
  g_dir_close (d);
}

static gboolean
lang_code_seen (GPtrArray *codes, const char *code)
{
  for (guint i = 0; i < codes->len; i++)
    if (g_strcmp0 (codes->pdata[i], code) == 0)
      return TRUE;
  return FALSE;
}

/* ---- table parsing ---- */

static char *
load_lang_contents (const char *code)
{
  char *res_path = g_strdup_printf (
    "/io/github/mayrinck/epinephrine/lang/%s.md", code);
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
      char *path = g_strdup_printf (DATADIR "/epinephrine/lang/%s.md", code);
      g_file_get_contents (path, &content, NULL, NULL);
      g_free (path);
    }
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel_md = g_strdup_printf ("%s/lang/%s.md", cwd, code);
      g_file_get_contents (rel_md, &content, NULL, NULL);
      g_free (cwd);
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

/* ---- language list ---- */

EpiLanguage *
epi_i18n_list_languages (int *count)
{
  GPtrArray *codes = g_ptr_array_new_with_free_func (g_free);

  /* Resources */
  GError *err = NULL;
  char **children = g_resources_enumerate_children (
    "/io/github/mayrinck/epinephrine/lang", 0, &err);
  if (children)
    {
      for (int i = 0; children[i]; i++)
        {
          const char *name = children[i];
          if (g_str_has_suffix (name, ".md"))
            {
              char *code = g_strndup (name, strlen (name) - 3);
              if (!lang_code_seen (codes, code))
                g_ptr_array_add (codes, code);
              else
                g_free (code);
            }
        }
      g_strfreev (children);
    }
  else
    g_clear_error (&err);

  /* DATADIR */
  collect_dir_languages (DATADIR "/epinephrine/lang", codes);

  /* current working directory */
  {
    char *cwd = g_get_current_dir ();
    char *dir = g_build_filename (cwd, "lang", NULL);
    collect_dir_languages (dir, codes);
    g_free (cwd);
    g_free (dir);
  }

  /* Deduplicate in order */
  GPtrArray *uniq = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < codes->len; i++)
    if (!lang_code_seen (uniq, codes->pdata[i]))
      g_ptr_array_add (uniq, g_strdup (codes->pdata[i]));
  g_ptr_array_free (codes, TRUE);

  /* Build result with native names */
  EpiLanguage *langs = g_new0 (EpiLanguage, uniq->len);
  for (guint i = 0; i < uniq->len; i++)
    {
      const char *code = uniq->pdata[i];
      langs[i].code = g_strdup (code);

      char *native = NULL;
      char *content = load_lang_contents (code);
      if (content)
        {
          GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal,
            g_free, g_free);
          fill_table_from_content (t, content);
          const char *n = g_hash_table_lookup (t, "Language name");
          native = g_strdup (n ? n : code);
          g_hash_table_unref (t);
          g_free (content);
        }
      langs[i].native_name = native ? native : g_strdup (code);
    }

  *count = uniq->len;
  g_ptr_array_free (uniq, TRUE);
  return langs;
}

void
epi_i18n_free_languages (EpiLanguage *langs, int count)
{
  for (int i = 0; i < count; i++)
    {
      g_free (langs[i].code);
      g_free (langs[i].native_name);
    }
  g_free (langs);
}

static gboolean
language_available (const char *code)
{
  int count = 0;
  EpiLanguage *langs = epi_i18n_list_languages (&count);
  gboolean found = FALSE;
  for (int i = 0; i < count; i++)
    if (g_strcmp0 (langs[i].code, code) == 0)
      found = TRUE;
  epi_i18n_free_languages (langs, count);
  return found;
}

static char *
detect_system_language (void)
{
  const char *locale = setlocale (LC_MESSAGES, NULL);
  if (!locale || !locale[0]) locale = g_getenv ("LANG");
  if (!locale || !locale[0]) locale = g_getenv ("LC_ALL");
  if (!locale || !locale[0]) return g_strdup ("en_US");

  char *code = g_strdup (locale);
  char *dot = strchr (code, '.');
  if (dot) *dot = '\0';
  char *at = strchr (code, '@');
  if (at) *at = '\0';

  char *result = NULL;
  if (language_available (code))
    result = g_strdup (code);
  else
    {
      char *us = strchr (code, '_');
      if (us) *us = '\0';
      int count = 0;
      EpiLanguage *langs = epi_i18n_list_languages (&count);
      size_t prefix = strlen (code);
      for (int i = 0; i < count; i++)
        {
          if (strncmp (langs[i].code, code, prefix) == 0 &&
              langs[i].code[prefix] == '_')
            {
              result = g_strdup (langs[i].code);
              break;
            }
        }
      epi_i18n_free_languages (langs, count);
    }

  if (!result) result = g_strdup ("en_US");
  g_free (code);
  return result;
}

/* ---- loading ---- */

static void
load_language (const char *code)
{
  if (g_tr)
    g_hash_table_unref (g_tr);
  g_free (g_lang_name);

  g_lang_name = g_strdup (code);
  g_tr = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* English base, always present for fallback */
  char *en = load_lang_contents ("en_US");
  if (en)
    {
      fill_table_from_content (g_tr, en);
      g_free (en);
    }

  if (g_strcmp0 (code, "en_US") != 0)
    {
      char *content = load_lang_contents (code);
      if (content)
        {
          fill_table_from_content (g_tr, content);
          g_free (content);
        }
    }
}

void
epi_i18n_set_language (const char *lang_code)
{
  g_rec_mutex_lock (&g_tr_lock);

  g_free (g_cur_request);
  g_cur_request = g_strdup (lang_code && lang_code[0] ? lang_code : "system");

  char *active = NULL;
  if (!lang_code || !lang_code[0] || g_strcmp0 (lang_code, "system") == 0)
    active = detect_system_language ();
  else
    active = g_strdup (lang_code);

  if (!language_available (active))
    {
      g_free (active);
      active = g_strdup ("en_US");
    }

  load_language (active);
  g_free (active);

  g_rec_mutex_unlock (&g_tr_lock);
}

const char *
epi_i18n_get_language (void)
{
  return g_cur_request ? g_cur_request : "system";
}

const char *
epi_i18n_get (const char *key)
{
  if (!key) return key;
  g_rec_mutex_lock (&g_tr_lock);
  const char *value = NULL;
  if (g_tr)
    value = g_hash_table_lookup (g_tr, key);
  g_rec_mutex_unlock (&g_tr_lock);
  return value ? value : key;
}

/* ---- widget translation ---- */

static void
translate_combo_model (AdwComboRow *row)
{
  if (g_object_get_data (G_OBJECT (row), EPI_I18N_SKIP_MODEL))
    return;

  GListModel *model = adw_combo_row_get_model (row);
  if (!model || !GTK_IS_STRING_LIST (model)) return;

  GPtrArray *keys = g_object_get_data (G_OBJECT (row), EPI_TR_COMBO_KEYS);
  if (!keys)
    {
      keys = g_ptr_array_new_with_free_func (g_free);
      guint n = g_list_model_get_n_items (model);
      for (guint i = 0; i < n; i++)
        {
          gpointer item = g_list_model_get_item (model, i);
          const char *s = "";
          if (GTK_IS_STRING_OBJECT (item))
            s = gtk_string_object_get_string (item);
          g_ptr_array_add (keys, g_strdup (s));
          g_object_unref (item);
        }
      g_object_set_data_full (G_OBJECT (row), EPI_TR_COMBO_KEYS, keys,
        (GDestroyNotify) g_ptr_array_unref);
    }

  guint n = keys->len;
  if (n == 0) return;

  const char **additions = g_new (const char *, n + 1);
  for (guint i = 0; i < n; i++)
    additions[i] = epi_i18n_get (keys->pdata[i]);
  additions[n] = NULL;

  /* Splice in place so the selected index and the model identity are kept,
     avoiding notify::selected churn from replacing the model. */
  gtk_string_list_splice (GTK_STRING_LIST (model), 0, n, additions);
  g_free (additions);
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

      if (ADW_IS_COMBO_ROW (widget))
        translate_combo_model (ADW_COMBO_ROW (widget));
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
  g_in_apply = TRUE;
  translate_widget (root);
  translate_children (root);
  g_in_apply = FALSE;
}

gboolean
epi_i18n_in_apply (void)
{
  return g_in_apply;
}
