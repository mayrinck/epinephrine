#include "game-details.h"
#include "page-games.h"
#include "game-artwork.h"
#include "../epi-i18n.h"
#include <glib/gstdio.h>
#include <string.h>

#define tr(x) epi_i18n_get (x)

static GtkWidget *
text_label (const char *text, const char *style)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  if (style) gtk_widget_add_css_class (label, style);
  return label;
}

static char *
library_config_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "epinephrine", "library.ini", NULL);
}

static int
button_game_index (GtkWidget *button, DashboardPage *page)
{
  const char *command = g_object_get_data (G_OBJECT (button), "game-command");
  for (int i = 0; i < page->n_games; i++)
    if (g_strcmp0 (command, page->games[i].command) == 0) return i;
  return -1;
}

static void
edit_game (GtkButton *button, DashboardPage *page)
{
  int idx = button_game_index (GTK_WIDGET (button), page);
  if (idx >= 0) open_edit_dialog (page, idx);
}

static void
save_services (GtkButton *button, DashboardPage *page)
{
  GtkEditable *key = g_object_get_data (G_OBJECT (button), "key-entry");
  GtkEditable *id = g_object_get_data (G_OBJECT (button), "id-entry");
  const char *appid = gtk_editable_get_text (id);
  if (strlen (appid) > 10 || (*appid && strspn (appid, "0123456789") != strlen (appid)))
    {
      dashboard_show_toast (tr ("Enter a numeric Steam App ID."));
      return;
    }
  g_autofree char *path = library_config_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr(GKeyFile) config = g_key_file_new ();
  g_key_file_load_from_file (config, path, G_KEY_FILE_NONE, NULL);
  g_key_file_set_string (config, "services", "steamgriddb_key", gtk_editable_get_text (key));
  const char *command = g_object_get_data (G_OBJECT (button), "game-command");
  g_autofree char *identity = game_artwork_identity (command);
  g_key_file_set_string (config, identity, "appid", appid);
  g_autofree char *data = g_key_file_to_data (config, NULL, NULL);
  g_autoptr(GError) error = NULL;
  g_mkdir_with_parents (dir, 0700);
  if (!g_file_set_contents_full (path, data, -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error))
    {
      dashboard_show_toast (tr ("Could not save online services."));
      return;
    }
  g_chmod (path, 0600);
  adw_dialog_close (g_object_get_data (G_OBJECT (button), "dialog"));
  refresh_games_grid (page);
}

static void
configure_services (GtkButton *button, DashboardPage *page)
{
  int idx = button_game_index (GTK_WIDGET (button), page);
  if (idx < 0) return;
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Artwork and online services"));
  adw_dialog_set_content_width (dialog, 460);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 24);
  gtk_widget_set_margin_bottom (box, 24);
  gtk_box_append (GTK_BOX (box), text_label (tr ("SteamGridDB API key"), "heading"));
  GtkWidget *key = gtk_password_entry_new ();
  gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (key), TRUE);
  gtk_box_append (GTK_BOX (box), key);
  gtk_box_append (GTK_BOX (box), gtk_link_button_new_with_label (
    "https://www.steamgriddb.com/profile/preferences/api", tr ("Get a SteamGridDB key")));
  gtk_box_append (GTK_BOX (box), text_label (tr ("Steam App ID (optional)"), "heading"));
  GtkWidget *id = gtk_entry_new ();
  gtk_entry_set_input_purpose (GTK_ENTRY (id), GTK_INPUT_PURPOSE_DIGITS);
  gtk_entry_set_placeholder_text (GTK_ENTRY (id), "570");
  gtk_box_append (GTK_BOX (box), id);
  gtk_box_append (GTK_BOX (box), text_label (tr ("Link games from any launcher to Steam for artwork and ProtonDB. Steam games are detected automatically."), "dim-label"));
  gtk_box_append (GTK_BOX (box), text_label (tr ("The selected game's name or Steam ID is sent to the artwork and compatibility services. Results are cached locally."), "dim-label"));
  g_autofree char *path = library_config_path ();
  g_autoptr(GKeyFile) config = g_key_file_new ();
  g_key_file_load_from_file (config, path, G_KEY_FILE_NONE, NULL);
  g_autofree char *saved_key = g_key_file_get_string (config, "services", "steamgriddb_key", NULL);
  g_autofree char *identity = game_artwork_identity (page->games[idx].command);
  g_autofree char *saved_id = g_key_file_get_string (config, identity, "appid", NULL);
  gtk_editable_set_text (GTK_EDITABLE (key), saved_key ? saved_key : "");
  gtk_editable_set_text (GTK_EDITABLE (id), saved_id ? saved_id : "");
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  GtkWidget *cancel = gtk_button_new_with_label (tr ("Cancel"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (buttons), cancel);
  GtkWidget *save = gtk_button_new_with_label (tr ("Save"));
  gtk_widget_add_css_class (save, "suggested-action");
  g_object_set_data (G_OBJECT (save), "key-entry", key);
  g_object_set_data (G_OBJECT (save), "id-entry", id);
  g_object_set_data (G_OBJECT (save), "dialog", dialog);
  g_object_set_data_full (G_OBJECT (save), "game-command", g_strdup (page->games[idx].command), g_free);
  g_signal_connect (save, "clicked", G_CALLBACK (save_services), page);
  gtk_box_append (GTK_BOX (buttons), save);
  gtk_box_append (GTK_BOX (box), buttons);
  adw_dialog_set_child (dialog, box);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

typedef struct { GWeakRef panel; } MetadataRequest;

static void
stop_request (gpointer data)
{
  g_subprocess_force_exit (G_SUBPROCESS (data));
  g_object_unref (data);
}

static void
metadata_ready (GObject *source, GAsyncResult *result, gpointer data)
{
  MetadataRequest *request = data;
  g_autoptr(GObject) panel = g_weak_ref_get (&request->panel);
  g_weak_ref_clear (&request->panel);
  g_free (request);
  g_autofree char *output = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result, &output, NULL, NULL);
  if (!panel) return;
  /* The child has exited; remove the cancellation hook without force_exit. */
  GSubprocess *proc = g_object_steal_data (panel, "metadata-process");
  if (proc) g_object_unref (proc);
  GtkLabel *tier_label = g_object_get_data (panel, "tier-label");
  GtkLabel *report_label = g_object_get_data (panel, "report-label");
  gtk_label_set_text (tier_label, tr ("No compatibility data"));
  gtk_label_set_text (report_label, tr ("Service unavailable or game not linked. You can still play and consult the reports."));
  g_autoptr(GKeyFile) metadata = g_key_file_new ();
  if (!ok || !output || !g_key_file_load_from_data (metadata, output, -1, G_KEY_FILE_NONE, NULL)) return;
  g_autofree char *hero = g_key_file_get_string (metadata, "game", "hero", NULL);
  if (hero && *hero)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GdkTexture) texture = gdk_texture_new_from_filename (hero, &error);
      if (texture)
        {
          gtk_picture_set_paintable (g_object_get_data (panel, "hero-picture"), GDK_PAINTABLE (texture));
          gtk_widget_set_visible (g_object_get_data (panel, "hero-fallback"), FALSE);
          GtkLinkButton *credit = g_object_get_data (panel, "art-credit");
          g_autofree char *name = g_key_file_get_string (metadata, "game", "credit", NULL);
          g_autofree char *url = g_key_file_get_string (metadata, "game", "art_url", NULL);
          gtk_button_set_label (GTK_BUTTON (credit), name ? name : "Steam");
          gtk_link_button_set_uri (credit, url && *url ? url : "https://www.steamgriddb.com");
          gtk_widget_set_sensitive (GTK_WIDGET (credit), url && *url);
          gtk_widget_set_visible (GTK_WIDGET (credit), TRUE);
        }
    }
  g_autofree char *tier = g_key_file_get_string (metadata, "game", "tier", NULL);
  g_autofree char *reports = g_key_file_get_string (metadata, "game", "reports", NULL);
  g_autofree char *status = g_key_file_get_string (metadata, "game", "status", NULL);
  if (tier && *tier)
    {
      g_autofree char *label = g_strdup_printf ("ProtonDB · %s", tier);
      gtk_label_set_text (tier_label, label);
      g_autofree char *summary = g_strdup_printf (tr ("%s community reports · %s"), reports ? reports : "0",
        g_strcmp0 (status, "cached") == 0 ? tr ("Cached data") : tr ("Community rating"));
      gtk_label_set_text (report_label, summary);
    }
  g_autofree char *appid = g_key_file_get_string (metadata, "game", "appid", NULL);
  if (appid && *appid && strspn (appid, "0123456789") == strlen (appid))
    {
      g_autofree char *proton = g_strdup_printf ("https://www.protondb.com/app/%s", appid);
      g_autofree char *steamdb = g_strdup_printf ("https://steamdb.info/app/%s/", appid);
      gtk_link_button_set_uri (g_object_get_data (panel, "proton-link"), proton);
      gtk_link_button_set_uri (g_object_get_data (panel, "steamdb-link"), steamdb);
    }
  g_autofree char *art_error = g_key_file_get_string (metadata, "game", "art_error", NULL);
  if (art_error)
    gtk_widget_set_visible (g_object_get_data (panel, "art-error"), TRUE);
}

static void
append_stat (GtkWidget *box, const char *title, const char *value)
{
  GtkWidget *stat = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_tooltip_text (stat, tr ("Recorded by Epinephrine"));
  gtk_widget_set_hexpand (stat, TRUE);
  gtk_widget_add_css_class (stat, "library-stat");
  gtk_box_append (GTK_BOX (stat), text_label (title, "dim-label"));
  gtk_box_append (GTK_BOX (stat), text_label (value, "heading"));
  gtk_box_append (GTK_BOX (box), stat);
}

void
game_details_show (GtkWidget *host, DashboardPage *page, int idx)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (host))) gtk_box_remove (GTK_BOX (host), child);
  if (idx < 0 || idx >= page->n_games) return;
  GameShortcut *game = &page->games[idx];
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_hexpand (scroll, TRUE);
  GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (panel, "library-detail");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), panel);
  gtk_box_append (GTK_BOX (host), scroll);

  GtkWidget *hero = gtk_overlay_new ();
  gtk_widget_add_css_class (hero, "library-hero");
  gtk_widget_set_overflow (hero, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_size_request (hero, -1, 270);
  GtkWidget *picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (hero), picture);
  GtkWidget *fallback = create_game_icon_widget (game->icon, 112);
  gtk_widget_set_halign (fallback, GTK_ALIGN_END);
  gtk_widget_set_valign (fallback, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (fallback, 40);
  gtk_widget_set_opacity (fallback, 0.25);
  gtk_overlay_add_overlay (GTK_OVERLAY (hero), fallback);
  GtkWidget *shade = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (shade, "library-hero-shade");
  gtk_widget_set_valign (shade, GTK_ALIGN_FILL);
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (shade), spacer);
  GtkWidget *eyebrow = text_label (tr ("IN YOUR LIBRARY"), "library-eyebrow");
  gtk_box_append (GTK_BOX (shade), eyebrow);
  gtk_overlay_add_overlay (GTK_OVERLAY (hero), shade);
  gtk_box_append (GTK_BOX (panel), hero);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_add_css_class (body, "library-body");
  gtk_box_append (GTK_BOX (panel), body);
  GtkWidget *title = text_label (game->name, "library-game-title");
  gtk_label_set_wrap_mode (GTK_LABEL (title), PANGO_WRAP_WORD_CHAR);
  gtk_box_append (GTK_BOX (body), title);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *play = gtk_button_new_with_label (page->running_game_idx == idx ? tr ("Running...") : tr ("Play Now"));
  gtk_widget_add_css_class (play, "suggested-action");
  gtk_widget_add_css_class (play, "library-play");
  gtk_widget_set_sensitive (play, page->running_game_idx < 0);
  g_object_set_data (G_OBJECT (play), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (play, "clicked", G_CALLBACK (on_game_launch_clicked), page);
  gtk_box_append (GTK_BOX (actions), play);
  GtkWidget *source_label = text_label (game->source ? game->source : tr ("Local"), "dim-label");
  gtk_widget_set_hexpand (source_label, TRUE);
  gtk_box_append (GTK_BOX (actions), source_label);
  GtkWidget *edit = gtk_button_new_from_icon_name ("emblem-system-symbolic");
  gtk_widget_set_tooltip_text (edit, tr ("Edit Game"));
  g_object_set_data_full (G_OBJECT (edit), "game-command", g_strdup (game->command), g_free);
  g_signal_connect (edit, "clicked", G_CALLBACK (edit_game), page);
  gtk_box_append (GTK_BOX (actions), edit);
  GtkWidget *artwork = gtk_button_new_from_icon_name ("insert-image-symbolic");
  gtk_widget_set_tooltip_text (artwork, tr ("Customize artwork"));
  g_object_set_data_full (G_OBJECT (artwork), "game-command", g_strdup (game->command), g_free);
  g_signal_connect (artwork, "clicked", G_CALLBACK (game_artwork_edit), page);
  gtk_box_append (GTK_BOX (actions), artwork);
  gtk_box_append (GTK_BOX (body), actions);

  GtkWidget *stats = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous (GTK_BOX (stats), TRUE);
  g_autofree char *time = g_strdup_printf ("%d h %02d min", game->play_time_secs / 3600, (game->play_time_secs % 3600) / 60);
  g_autofree char *ago = format_time_ago (game->last_played);
  g_autofree char *fps = game->avg_fps > 0 ? g_strdup_printf ("%.1f FPS", game->avg_fps) : g_strdup (tr ("Not measured"));
  append_stat (stats, tr ("Time played"), time);
  append_stat (stats, tr ("Last played"), ago && *ago ? ago : tr ("Never played"));
  append_stat (stats, tr ("Average FPS"), fps);
  gtk_box_append (GTK_BOX (body), stats);

  GtkWidget *compat = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (compat, "library-info-card");
  gtk_box_append (GTK_BOX (compat), text_label (tr ("LINUX COMPATIBILITY"), "library-eyebrow"));
  GtkWidget *tier_label = text_label (tr ("Checking ProtonDB…"), "title-2");
  GtkWidget *report_label = text_label (tr ("Compatibility reports from the community"), "dim-label");
  gtk_box_append (GTK_BOX (compat), tier_label);
  gtk_box_append (GTK_BOX (compat), report_label);
  g_autofree char *query = g_uri_escape_string (game->name ? game->name : "", NULL, FALSE);
  g_autofree char *proton_url = g_strdup_printf ("https://www.protondb.com/search?q=%s", query);
  g_autofree char *db_url = g_strdup_printf ("https://steamdb.info/search/?a=app&q=%s", query);
  g_autofree char *hltb_url = g_strdup_printf ("https://howlongtobeat.com/?q=%s", query);
  GtkWidget *links = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (links), GTK_SELECTION_NONE);
  GtkWidget *proton_link = gtk_link_button_new_with_label (proton_url, tr ("ProtonDB reports ↗"));
  GtkWidget *db_link = gtk_link_button_new_with_label (db_url, "SteamDB ↗");
  gtk_flow_box_append (GTK_FLOW_BOX (links), proton_link);
  gtk_flow_box_append (GTK_FLOW_BOX (links), db_link);
  gtk_flow_box_append (GTK_FLOW_BOX (links), gtk_link_button_new_with_label (hltb_url, "HowLongToBeat ↗"));
  gtk_box_append (GTK_BOX (compat), links);
  gtk_box_append (GTK_BOX (body), compat);

  GtkWidget *launch = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (launch, "library-info-card");
  gtk_box_append (GTK_BOX (launch), text_label (tr ("Launch configuration"), "heading"));
  g_autofree char *tweaks = g_strdup_printf ("GameMode: %s   ·   MangoHud: %s", game->feral_gamemode ? tr ("Enabled") : tr ("Disabled"), game->mangohud ? tr ("Enabled") : tr ("Disabled"));
  gtk_box_append (GTK_BOX (launch), text_label (tweaks, "dim-label"));
  GtkWidget *command = text_label (game->startup_cmd && *game->startup_cmd ? game->startup_cmd : game->command, "monospace");
  gtk_label_set_selectable (GTK_LABEL (command), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (command), PANGO_WRAP_WORD_CHAR);
  gtk_box_append (GTK_BOX (launch), command);
  GtkWidget *edit_command = gtk_button_new_with_label (tr ("Edit launch options"));
  gtk_widget_set_halign (edit_command, GTK_ALIGN_START);
  g_object_set_data_full (G_OBJECT (edit_command), "game-command", g_strdup (game->command), g_free);
  g_signal_connect (edit_command, "clicked", G_CALLBACK (edit_game), page);
  gtk_box_append (GTK_BOX (launch), edit_command);
  gtk_box_append (GTK_BOX (body), launch);

  GtkWidget *services = gtk_button_new_with_label (tr ("Artwork and online services"));
  gtk_widget_add_css_class (services, "flat");
  gtk_widget_set_halign (services, GTK_ALIGN_START);
  g_object_set_data_full (G_OBJECT (services), "game-command", g_strdup (game->command), g_free);
  g_signal_connect (services, "clicked", G_CALLBACK (configure_services), page);
  gtk_box_append (GTK_BOX (body), services);
  GtkWidget *art_error = text_label (tr ("SteamGridDB artwork unavailable. Check your API key or try again later."), "dim-label");
  gtk_widget_set_visible (art_error, FALSE);
  gtk_box_append (GTK_BOX (body), art_error);
  GtkWidget *credit = gtk_link_button_new_with_label ("https://www.steamgriddb.com", "SteamGridDB");
  gtk_widget_set_halign (credit, GTK_ALIGN_START);
  gtk_widget_add_css_class (credit, "dim-label");
  gtk_widget_set_visible (credit, FALSE);
  gtk_box_append (GTK_BOX (body), credit);
  g_object_set_data (G_OBJECT (panel), "tier-label", tier_label);
  g_object_set_data (G_OBJECT (panel), "report-label", report_label);
  g_object_set_data (G_OBJECT (panel), "hero-picture", picture);
  g_object_set_data (G_OBJECT (panel), "hero-fallback", fallback);
  g_object_set_data (G_OBJECT (panel), "art-credit", credit);
  g_object_set_data (G_OBJECT (panel), "art-error", art_error);
  g_object_set_data (G_OBJECT (panel), "proton-link", proton_link);
  g_object_set_data (G_OBJECT (panel), "steamdb-link", db_link);

  g_autoptr(GBytes) script = g_resources_lookup_data ("/io/github/mayrinck/epinephrine/scripts/game_metadata.py", 0, NULL);
  g_autofree char *code = script ? g_strndup (g_bytes_get_data (script, NULL), g_bytes_get_size (script)) : NULL;
  GSubprocess *proc = code ? g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
    NULL, "python3", "-c", code, game->name ? game->name : "", game->command ? game->command : "", NULL) : NULL;
  if (proc)
    {
      MetadataRequest *request = g_new0 (MetadataRequest, 1);
      g_weak_ref_init (&request->panel, panel);
      g_object_set_data_full (G_OBJECT (panel), "metadata-process", proc, stop_request);
      g_subprocess_communicate_utf8_async (proc, NULL, NULL, metadata_ready, request);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (tier_label), tr ("No compatibility data"));
      gtk_label_set_text (GTK_LABEL (report_label), tr ("Install Python 3 to load artwork and compatibility data."));
    }
}
