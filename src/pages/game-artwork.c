#include "game-artwork.h"
#include "page-games.h"
#include "../epi-i18n.h"
#include <glib/gstdio.h>
#include <string.h>

#define tr(x) epi_i18n_get (x)

typedef struct {
  GWeakRef target;
  char **args;
  GameArtworkReady callback;
  gpointer data;
  GDestroyNotify destroy;
} ArtRequest;

static GQueue requests = G_QUEUE_INIT;
static int active_requests;
static void start_next (void);

static void
request_free (ArtRequest *request)
{
  g_weak_ref_clear (&request->target);
  g_strfreev (request->args);
  if (request->destroy) request->destroy (request->data);
  g_free (request);
}

static void
request_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  ArtRequest *request = data;
  g_autofree char *output = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result, &output, NULL, NULL);
  g_autoptr(GObject) target = g_weak_ref_get (&request->target);
  g_autoptr(GKeyFile) values = g_key_file_new ();
  if (output) g_key_file_load_from_data (values, output, -1, G_KEY_FILE_NONE, NULL);
  if (target) request->callback (GTK_WIDGET (target), values, request->data);
  g_object_unref (source);
  request_free (request);
  active_requests--;
  start_next ();
}

static void
start_next (void)
{
  while (active_requests < 3 && !g_queue_is_empty (&requests))
    {
      ArtRequest *request = g_queue_pop_head (&requests);
      g_autoptr(GObject) target = g_weak_ref_get (&request->target);
      if (!target) { request_free (request); continue; }
      g_autoptr(GBytes) script = g_resources_lookup_data (
        "/io/github/mayrinck/epinephrine/scripts/game_metadata.py", 0, NULL);
      g_autofree char *code = script ? g_strndup (g_bytes_get_data (script, NULL), g_bytes_get_size (script)) : NULL;
      g_autoptr(GPtrArray) argv = g_ptr_array_new ();
      g_ptr_array_add (argv, "python3");
      g_ptr_array_add (argv, "-c");
      g_ptr_array_add (argv, code);
      for (int i = 0; request->args[i]; i++) g_ptr_array_add (argv, request->args[i]);
      g_ptr_array_add (argv, NULL);
      GSubprocess *proc = code ? g_subprocess_newv ((const char * const *) argv->pdata,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL) : NULL;
      if (proc)
        {
          active_requests++;
          g_subprocess_communicate_utf8_async (proc, NULL, NULL, request_ready, request);
        }
      else
        {
          g_autoptr(GKeyFile) empty = g_key_file_new ();
          request->callback (GTK_WIDGET (target), empty, request->data);
          request_free (request);
        }
    }
}

void
game_artwork_request (GtkWidget *target, const char * const *args,
                      GameArtworkReady callback, gpointer data, GDestroyNotify destroy)
{
  ArtRequest *request = g_new0 (ArtRequest, 1);
  g_weak_ref_init (&request->target, target);
  request->args = g_strdupv ((char **) args);
  request->callback = callback;
  request->data = data;
  request->destroy = destroy;
  if (g_strcmp0 (args[0], "--cover") == 0)
    g_queue_push_tail (&requests, request);
  else
    g_queue_push_head (&requests, request);
  start_next ();
}

GKeyFile *
game_artwork_config_load (void)
{
  GKeyFile *config = g_key_file_new ();
  g_autofree char *path = g_build_filename (g_get_user_config_dir (), "epinephrine", "library.ini", NULL);
  g_key_file_load_from_file (config, path, G_KEY_FILE_NONE, NULL);
  return config;
}

gboolean
game_artwork_config_save (GKeyFile *config)
{
  g_autofree char *dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  g_autofree char *path = g_build_filename (dir, "library.ini", NULL);
  g_autofree char *contents = g_key_file_to_data (config, NULL, NULL);
  g_mkdir_with_parents (dir, 0700);
  gboolean ok = g_file_set_contents_full (path, contents, -1,
    G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL);
  if (ok) g_chmod (path, 0600);
  else dashboard_show_toast (tr ("Could not save online services."));
  return ok;
}

static void
picture_ready (GtkWidget *target, GKeyFile *values, gpointer data)
{
  (void) data;
  g_autofree char *path = g_key_file_get_string (values, "game", "path", NULL);
  g_autoptr(GdkTexture) texture = path && *path ? gdk_texture_new_from_filename (path, NULL) : NULL;
  if (!texture) return;
  GtkPicture *picture = g_object_get_data (G_OBJECT (target), "picture");
  GtkWidget *fallback = g_object_get_data (G_OBJECT (target), "fallback");
  gtk_picture_set_paintable (picture, GDK_PAINTABLE (texture));
  gtk_widget_set_visible (fallback, FALSE);
}

char *
game_artwork_identity (const char *command)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, command, -1);
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  char *id = g_key_file_get_string (config, "identities", hash, NULL);
  return id ? id : g_strdup (hash);
}

gboolean
game_artwork_move_identity (const char *old_command, const char *new_command)
{
  if (!old_command || !new_command || g_strcmp0 (old_command, new_command) == 0) return TRUE;
  g_autofree char *id = game_artwork_identity (old_command);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, new_command, -1);
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  g_key_file_set_string (config, "identities", hash, id);
  return game_artwork_config_save (config);
}

static void
cover_mapped (GtkWidget *cover, gpointer data)
{
  (void) data;
  if (g_object_get_data (G_OBJECT (cover), "requested")) return;
  g_object_set_data (G_OBJECT (cover), "requested", GINT_TO_POINTER (1));
  const char *args[] = { "--cover", g_object_get_data (G_OBJECT (cover), "name"),
    g_object_get_data (G_OBJECT (cover), "command"), NULL };
  game_artwork_request (cover, args, picture_ready, NULL, NULL);
}

static GtkWidget *
image_placeholder (const char *icon, int width, int height)
{
  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_size_request (overlay, width, height);
  gtk_widget_set_halign (overlay, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (overlay, "library-cover");
  gtk_widget_set_overflow (overlay, GTK_OVERFLOW_HIDDEN);
  GtkWidget *picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  /* Keep remote image dimensions out of FlowBox measurement. Otherwise a
     600px cover forces one enormous tile per row after it finishes loading. */
  GtkWidget *canvas = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request (canvas, width, height);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), canvas);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), picture);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (overlay), picture, FALSE);
  GtkWidget *fallback = create_game_icon_widget (icon, 48);
  gtk_widget_set_halign (fallback, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (fallback, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), fallback);
  g_object_set_data (G_OBJECT (overlay), "picture", picture);
  g_object_set_data (G_OBJECT (overlay), "fallback", fallback);
  return overlay;
}

GtkWidget *
game_artwork_cover (GameShortcut *game, int width, int height)
{
  GtkWidget *cover = image_placeholder (game->icon, width, height);
  g_object_set_data_full (G_OBJECT (cover), "name", g_strdup (game->name ? game->name : ""), g_free);
  g_object_set_data_full (G_OBJECT (cover), "command", g_strdup (game->command ? game->command : ""), g_free);
  g_signal_connect (cover, "map", G_CALLBACK (cover_mapped), NULL);
  return cover;
}

static const char *
picker_kind (GtkWidget *picker)
{
  GtkDropDown *dropdown = g_object_get_data (G_OBJECT (picker), "kind");
  return gtk_drop_down_get_selected (dropdown) == 0 ? "cover" : "hero";
}

static void
artwork_changed (GtkWidget *picker)
{
  DashboardPage *page = g_object_get_data (G_OBJECT (picker), "page");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (picker), "dialog");
  refresh_games_grid (page);
  refresh_recent_games (page);
  adw_dialog_close (dialog);
}

static gboolean
save_artwork (GtkWidget *picker, const char *path, const char *credit, const char *source, const char *url)
{
  int width = 0, height = 0;
  if (!path || !gdk_pixbuf_get_file_info (path, &width, &height) ||
      width <= 0 || height <= 0 || (guint64) width * height > 50000000)
    {
      dashboard_show_toast (tr ("Choose a valid image file."));
      return FALSE;
    }
  g_autofree char *contents = NULL;
  gsize length = 0;
  GStatBuf file_stat;
  if (g_stat (path, &file_stat) != 0 || file_stat.st_size > 12 * 1024 * 1024 ||
      !g_file_get_contents (path, &contents, &length, NULL))
    {
      dashboard_show_toast (tr ("Image unavailable or larger than 12 MB."));
      return FALSE;
    }
  const char *command = g_object_get_data (G_OBJECT (picker), "command");
  g_autofree char *identity = game_artwork_identity (command);
  const char *kind = picker_kind (picker);
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "epinephrine", "artwork", NULL);
  g_autofree char *digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) contents, length);
  g_autofree char *filename = g_strdup_printf ("%s-%s-%.16s.image", identity, kind, digest);
  g_autofree char *dest = g_build_filename (dir, filename, NULL);
  g_mkdir_with_parents (dir, 0700);
  if (!g_file_set_contents_full (dest, contents, length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    {
      dashboard_show_toast (tr ("Could not save artwork."));
      return FALSE;
    }
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  const char *fields[] = { "path", "credit", "source", "url" };
  const char *values[] = { dest, credit ? credit : "", source ? source : "", url ? url : "" };
  for (int i = 0; i < 4; i++)
    {
      g_autofree char *key = g_strdup_printf ("%s_%s", kind, fields[i]);
      g_key_file_set_string (config, identity, key, values[i]);
    }
  return game_artwork_config_save (config);
}

static void
local_file_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  GWeakRef *reference = data;
  g_autoptr(GObject) picker = g_weak_ref_get (reference);
  g_weak_ref_clear (reference);
  g_free (reference);
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);
  if (!file || !picker) return;
  g_autofree char *path = g_file_get_path (file);
  if (save_artwork (GTK_WIDGET (picker), path, tr ("Custom artwork"), "", "")) artwork_changed (GTK_WIDGET (picker));
}

static void
choose_local_file (GtkButton *button, gpointer data)
{
  (void) button;
  GtkWidget *picker = data;
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, tr ("Choose artwork"));
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, tr ("Images"));
  gtk_file_filter_add_mime_type (filter, "image/*");
  gtk_file_dialog_set_default_filter (dialog, filter);
  GWeakRef *reference = g_new0 (GWeakRef, 1);
  g_weak_ref_init (reference, picker);
  gtk_file_dialog_open (dialog, GTK_WINDOW (gtk_widget_get_root (picker)), NULL, local_file_ready, reference);
}

static void
restore_artwork (GtkButton *button, gpointer data)
{
  (void) button;
  GtkWidget *picker = data;
  const char *command = g_object_get_data (G_OBJECT (picker), "command");
  g_autofree char *identity = game_artwork_identity (command);
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  const char *fields[] = { "path", "credit", "source", "url" };
  for (int i = 0; i < 4; i++)
    {
      g_autofree char *key = g_strdup_printf ("%s_%s", picker_kind (picker), fields[i]);
      g_key_file_remove_key (config, identity, key, NULL);
    }
  if (game_artwork_config_save (config)) artwork_changed (picker);
}

static void
selection_ready (GtkWidget *button, GKeyFile *values, gpointer data)
{
  (void) data;
  GtkWidget *picker = g_object_get_data (G_OBJECT (button), "picker");
  gtk_widget_set_sensitive (picker, TRUE);
  g_autofree char *path = g_key_file_get_string (values, "game", "path", NULL);
  if (save_artwork (picker, path, g_object_get_data (G_OBJECT (button), "credit"),
      g_object_get_data (G_OBJECT (button), "source"), g_object_get_data (G_OBJECT (button), "url")))
    artwork_changed (picker);
}

static void
select_gallery_item (GtkButton *button, gpointer data)
{
  GtkWidget *picker = data;
  gtk_widget_set_sensitive (picker, FALSE);
  const char *args[] = { "--image", g_object_get_data (G_OBJECT (button), "url"), NULL };
  game_artwork_request (GTK_WIDGET (button), args, selection_ready, NULL, NULL);
}

static void
load_gallery (GtkWidget *picker);

static void
gallery_ready (GtkWidget *flow, GKeyFile *values, gpointer data)
{
  GtkWidget *picker = data; /* flow is a child of this picker. */
  const char *command = g_object_get_data (G_OBJECT (picker), "command");
  g_autofree char *identity = game_artwork_identity (command);
  g_autofree char *url_key = g_strdup_printf ("%s_url", picker_kind (picker));
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  g_autofree char *selected_url = g_key_file_get_string (config, identity, url_key, NULL);
  GtkWidget *status = g_object_get_data (G_OBJECT (picker), "status");
  g_auto(GStrv) groups = g_key_file_get_groups (values, NULL);
  int count = 0;
  for (int i = 0; groups[i]; i++)
    {
      if (!g_str_has_prefix (groups[i], "item")) continue;
      g_autofree char *url = g_key_file_get_string (values, groups[i], "url", NULL);
      g_autofree char *thumb = g_key_file_get_string (values, groups[i], "thumb", NULL);
      g_autofree char *credit = g_key_file_get_string (values, groups[i], "credit", NULL);
      g_autofree char *source = g_key_file_get_string (values, groups[i], "source", NULL);
      if (!url || !thumb) continue;
      GtkWidget *button = gtk_button_new ();
      gtk_widget_add_css_class (button, "artwork-choice");
      gtk_widget_set_halign (button, GTK_ALIGN_START);
      if (g_strcmp0 (selected_url, url) == 0)
        gtk_widget_add_css_class (button, "artwork-selected");
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
      gboolean cover = g_strcmp0 (picker_kind (picker), "cover") == 0;
      GtkWidget *image = image_placeholder (NULL, cover ? 140 : 250, cover ? 210 : 110);
      gtk_box_append (GTK_BOX (box), image);
      GtkWidget *label = gtk_label_new (credit);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (label), cover ? 18 : 30);
      gtk_box_append (GTK_BOX (box), label);
      gtk_button_set_child (GTK_BUTTON (button), box);
      g_object_set_data_full (G_OBJECT (button), "url", g_strdup (url), g_free);
      g_object_set_data_full (G_OBJECT (button), "credit", g_strdup (credit), g_free);
      g_object_set_data_full (G_OBJECT (button), "source", g_strdup (source), g_free);
      g_object_set_data (G_OBJECT (button), "picker", picker);
      g_signal_connect (button, "clicked", G_CALLBACK (select_gallery_item), picker);
      gtk_flow_box_append (GTK_FLOW_BOX (flow), button);
      const char *args[] = { "--image", thumb, NULL };
      game_artwork_request (image, args, picture_ready, NULL, NULL);
      count++;
    }
  gtk_label_set_text (GTK_LABEL (status), count ? tr ("Choose an image to apply it.") :
    tr ("No artwork found. Search for another title, configure SteamGridDB, or choose a local image."));
}

static void
load_gallery (GtkWidget *picker)
{
  GtkWidget *scroll = g_object_get_data (G_OBJECT (picker), "gallery-scroll");
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 5);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_widget_set_valign (flow, GTK_ALIGN_START);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), flow);
  gtk_label_set_text (g_object_get_data (G_OBJECT (picker), "status"), tr ("Loading artwork…"));
  const char *args[] = { "--gallery", picker_kind (picker), g_object_get_data (G_OBJECT (picker), "name"),
    g_object_get_data (G_OBJECT (picker), "command"), NULL };
  game_artwork_request (flow, args, gallery_ready, picker, NULL);
}

static void
kind_changed (GObject *object, GParamSpec *spec, gpointer data)
{
  (void) object; (void) spec;
  load_gallery (data);
}

static void
use_search_match (GtkButton *button, gpointer data)
{
  GtkWidget *picker = data;
  const char *command = g_object_get_data (G_OBJECT (picker), "command");
  const char *id = g_object_get_data (G_OBJECT (button), "id");
  gboolean steam = g_strcmp0 (g_object_get_data (G_OBJECT (button), "provider"), "Steam") == 0;
  g_autofree char *identity = game_artwork_identity (command);
  g_autoptr(GKeyFile) config = game_artwork_config_load ();
  g_key_file_set_string (config, identity, steam ? "appid" : "sgdb_id", id);
  if (steam) g_key_file_remove_key (config, identity, "sgdb_id", NULL);
  if (!game_artwork_config_save (config)) return;
  GtkWidget *matches = g_object_get_data (G_OBJECT (picker), "matches");
  gtk_widget_set_visible (matches, FALSE);
  load_gallery (picker);
}

static void
search_ready (GtkWidget *results, GKeyFile *values, gpointer data)
{
  GtkWidget *picker = data;
  g_auto(GStrv) groups = g_key_file_get_groups (values, NULL);
  int count = 0;
  for (int i = 0; groups[i]; i++)
    {
      if (!g_str_has_prefix (groups[i], "match")) continue;
      g_autofree char *name = g_key_file_get_string (values, groups[i], "name", NULL);
      g_autofree char *id = g_key_file_get_string (values, groups[i], "id", NULL);
      g_autofree char *provider = g_key_file_get_string (values, groups[i], "provider", NULL);
      if (!name || !id) continue;
      g_autofree char *label = g_strdup_printf ("%s · %s", name, provider ? provider : "");
      GtkWidget *button = gtk_button_new_with_label (label);
      gtk_widget_add_css_class (button, "flat");
      gtk_widget_set_tooltip_text (button, label);
      GtkWidget *text = gtk_button_get_child (GTK_BUTTON (button));
      gtk_label_set_ellipsize (GTK_LABEL (text), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (text), 70);
      g_object_set_data_full (G_OBJECT (button), "id", g_strdup (id), g_free);
      g_object_set_data_full (G_OBJECT (button), "provider", g_strdup (provider), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (use_search_match), picker);
      gtk_box_append (GTK_BOX (results), button);
      count++;
    }
  if (!count) gtk_box_append (GTK_BOX (results), gtk_label_new (tr ("No games found.")));
}

static void
search_artwork (GtkWidget *button, gpointer data)
{
  (void) button;
  GtkWidget *picker = data;
  GtkEditable *entry = g_object_get_data (G_OBJECT (picker), "search");
  const char *query = gtk_editable_get_text (entry);
  if (!query || !*query) return;
  GtkWidget *scroll = g_object_get_data (G_OBJECT (picker), "matches");
  GtkWidget *results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), results);
  gtk_widget_set_visible (scroll, TRUE);
  const char *args[] = { "--search", query, NULL };
  game_artwork_request (results, args, search_ready, picker, NULL);
}

void
game_artwork_edit (GtkButton *button, DashboardPage *page)
{
  const char *command = g_object_get_data (G_OBJECT (button), "game-command");
  int idx = -1;
  for (int i = 0; i < page->n_games; i++)
    if (g_strcmp0 (command, page->games[i].command) == 0) { idx = i; break; }
  if (idx < 0) return;
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Customize artwork"));
  adw_dialog_set_content_width (dialog, 800);
  adw_dialog_set_content_height (dialog, 680);
  GtkWidget *picker = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class (picker, "artwork-picker");
  GtkWidget *header = adw_header_bar_new ();
  gtk_box_append (GTK_BOX (picker), header);
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (content, 20);
  gtk_widget_set_margin_end (content, 20);
  gtk_widget_set_margin_bottom (content, 20);
  gtk_widget_set_vexpand (content, TRUE);
  gtk_box_append (GTK_BOX (picker), content);
  const char *kinds[] = { tr ("Cover"), tr ("Banner"), NULL };
  GtkWidget *kind = gtk_drop_down_new_from_strings (kinds);
  gtk_box_append (GTK_BOX (content), kind);
  GtkWidget *search_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *search = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (search), page->games[idx].name);
  gtk_widget_set_hexpand (search, TRUE);
  gtk_box_append (GTK_BOX (search_row), search);
  GtkWidget *search_btn = gtk_button_new_with_label (tr ("Search artwork"));
  gtk_box_append (GTK_BOX (search_row), search_btn);
  gtk_box_append (GTK_BOX (content), search_row);
  GtkWidget *matches = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (matches), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (matches), 110);
  gtk_widget_set_visible (matches, FALSE);
  gtk_box_append (GTK_BOX (content), matches);
  GtkWidget *status = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (status), TRUE);
  gtk_label_set_xalign (GTK_LABEL (status), 0);
  gtk_box_append (GTK_BOX (content), status);
  GtkWidget *gallery_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (gallery_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (gallery_scroll), FALSE);
  gtk_widget_set_vexpand (gallery_scroll, TRUE);
  gtk_box_append (GTK_BOX (content), gallery_scroll);
  GtkWidget *hint = gtk_label_new (tr ("More choices are available with a SteamGridDB key in Artwork and online services."));
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (content), hint);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *local = gtk_button_new_with_label (tr ("Choose local image…"));
  g_signal_connect (local, "clicked", G_CALLBACK (choose_local_file), picker);
  gtk_box_append (GTK_BOX (actions), local);
  GtkWidget *reset = gtk_button_new_with_label (tr ("Restore automatic artwork"));
  g_signal_connect (reset, "clicked", G_CALLBACK (restore_artwork), picker);
  gtk_box_append (GTK_BOX (actions), reset);
  gtk_box_append (GTK_BOX (content), actions);
  g_object_set_data (G_OBJECT (picker), "page", page);
  g_object_set_data (G_OBJECT (picker), "dialog", dialog);
  g_object_set_data (G_OBJECT (picker), "kind", kind);
  g_object_set_data (G_OBJECT (picker), "search", search);
  g_object_set_data (G_OBJECT (picker), "matches", matches);
  g_object_set_data (G_OBJECT (picker), "gallery-scroll", gallery_scroll);
  g_object_set_data (G_OBJECT (picker), "status", status);
  g_object_set_data_full (G_OBJECT (picker), "command", g_strdup (command), g_free);
  g_object_set_data_full (G_OBJECT (picker), "name", g_strdup (page->games[idx].name), g_free);
  g_signal_connect (kind, "notify::selected", G_CALLBACK (kind_changed), picker);
  g_signal_connect (search_btn, "clicked", G_CALLBACK (search_artwork), picker);
  g_signal_connect (search, "activate", G_CALLBACK (search_artwork), picker);
  adw_dialog_set_child (dialog, picker);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
  load_gallery (picker);
}
