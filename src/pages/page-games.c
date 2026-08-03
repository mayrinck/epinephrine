#include "page-games.h"
#include "page-dashboard.h"

#include "../epi-i18n.h"

#include <string.h>

#define tr(x) epi_i18n_get (x)

static void on_card_clicked (GtkGestureClick *gesture, int n_press, double x, double y, DashboardPage *page);
static void on_add_game_clicked (GtkButton *btn, DashboardPage *page);

static const char *
get_source_icon (const char *source)
{
  if (!source) return NULL;
  if (g_strcmp0 (source, "steam") == 0)
    return "/io/github/mayrinck/epinephrine/icons/launchers/steam.png";
  if (g_strcmp0 (source, "heroic") == 0)
    return "/io/github/mayrinck/epinephrine/icons/launchers/heroic.png";
  if (g_strcmp0 (source, "lutris") == 0)
    return "/io/github/mayrinck/epinephrine/icons/launchers/lutris.png";
  if (g_strcmp0 (source, "bottles") == 0)
    return "/io/github/mayrinck/epinephrine/icons/launchers/bottles.png";
  return NULL;
}

static const char *
get_source_label (const char *source)
{
  if (!source) return NULL;
  if (g_strcmp0 (source, "steam") == 0) return "Steam";
  if (g_strcmp0 (source, "heroic") == 0) return "Heroic";
  if (g_strcmp0 (source, "lutris") == 0) return "Lutris";
  if (g_strcmp0 (source, "bottles") == 0) return "Bottles";
  if (g_strcmp0 (source, "local") == 0) return "Local";
  return "Other";
}

static GtkWidget *
create_game_card_grid (DashboardPage *page, int idx)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign (card, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_size_request (card, 180, -1);
  gtk_widget_add_css_class (card, "game-card");
  g_object_set_data (G_OBJECT (card), "game-idx", GINT_TO_POINTER (idx + 1));

  if (page->running_game_idx == idx)
    gtk_widget_add_css_class (card, "game-running");

  /* Icon with source badge overlay */
  const char *icon_name = page->games[idx].icon;
  if (!icon_name || icon_name[0] == '\0')
    icon_name = "input-gaming-symbolic";
  GtkWidget *icon = create_game_icon_widget (icon_name, 32);
  gtk_widget_add_css_class (icon, "game-icon");

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_widget_set_halign (overlay, GTK_ALIGN_CENTER);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), icon);

  const char *src_icon_path = get_source_icon (page->games[idx].source);
  if (src_icon_path)
    {
      GtkWidget *src_img = gtk_image_new_from_resource (src_icon_path);
      gtk_image_set_pixel_size (GTK_IMAGE (src_img), 14);
      gtk_widget_set_halign (src_img, GTK_ALIGN_END);
      gtk_widget_set_valign (src_img, GTK_ALIGN_END);
      gtk_widget_set_margin_end (src_img, 2);
      gtk_widget_set_margin_bottom (src_img, 2);
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), src_img);
    }

  GtkGesture *icon_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (icon_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (icon_click, "pressed", G_CALLBACK (on_card_clicked), page);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (icon_click));
  gtk_box_append (GTK_BOX (card), overlay);

  /* Game name */
  GtkWidget *name = gtk_label_new (
    page->games[idx].name && page->games[idx].name[0]
    ? page->games[idx].name : "?");
  gtk_widget_add_css_class (name, "game-name");
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 18);
  gtk_label_set_xalign (GTK_LABEL (name), 0.0f);

  GtkGesture *name_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (name_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (name_click, "pressed", G_CALLBACK (on_card_clicked), page);
  gtk_widget_add_controller (name, GTK_EVENT_CONTROLLER (name_click));
  gtk_box_append (GTK_BOX (card), name);

  /* Time ago + source label */
  char *time_str = format_time_ago (page->games[idx].last_played);
  const char *src_label = get_source_label (page->games[idx].source);
  char *stats_text = NULL;
  if (src_label && time_str && time_str[0])
    stats_text = g_strdup_printf ("%s \u2022 %s", src_label, time_str);
  else if (time_str && time_str[0])
    stats_text = g_strdup (time_str);
  else if (src_label)
    stats_text = g_strdup (src_label);
  else
    stats_text = g_strdup ("");
  GtkWidget *stats_label = gtk_label_new (stats_text);
  gtk_widget_add_css_class (stats_label, "game-stats");
  gtk_label_set_xalign (GTK_LABEL (stats_label), 0.0f);
  g_free (stats_text);

  GtkGesture *stats_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (stats_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (stats_click, "pressed", G_CALLBACK (on_card_clicked), page);
  gtk_widget_add_controller (stats_label, GTK_EVENT_CONTROLLER (stats_click));
  gtk_box_append (GTK_BOX (card), stats_label);
  g_free (time_str);

  /* Play Now button */
  const char *btn_label = (page->running_game_idx == idx)
    ? tr ("Running...")
    : tr ("Play Now");
  GtkWidget *launch_btn = gtk_button_new_with_label (btn_label);
  gtk_widget_add_css_class (launch_btn, "play-now-btn");
  gtk_widget_add_css_class (launch_btn, "suggested-action");
  if (page->running_game_idx == idx)
    {
      gtk_widget_set_sensitive (launch_btn, FALSE);
    }
  g_object_set_data (G_OBJECT (launch_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (launch_btn, "clicked", G_CALLBACK (on_game_launch_clicked), page);
  gtk_box_append (GTK_BOX (card), launch_btn);

  return card;
}

static int
find_game_idx_from_widget (GtkWidget *widget)
{
  GtkWidget *w = widget;
  while (w)
    {
      gpointer val = g_object_get_data (G_OBJECT (w), "game-idx");
      if (val)
        {
          int idx = GPOINTER_TO_INT (val) - 1;
          if (idx >= 0) return idx;
        }
      w = gtk_widget_get_parent (w);
    }
  return -1;
}

static void
on_card_clicked (GtkGestureClick *gesture, int n_press, double x, double y, DashboardPage *page)
{
  (void) n_press; (void) x; (void) y;
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = find_game_idx_from_widget (widget);
  if (idx < 0 || idx >= page->n_games) return;
  open_edit_dialog (page, idx);
}

/* ---- Search / Filter ---- */

static char *g_search_text = NULL;
static guint g_search_timer_id = 0;

static gboolean
on_search_timeout (gpointer user_data)
{
  DashboardPage *page = (DashboardPage *) user_data;
  g_search_timer_id = 0;
  refresh_games_grid (page);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry *entry, DashboardPage *page)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  g_free (g_search_text);
  g_search_text = (text && text[0]) ? g_strdup (text) : NULL;

  if (g_search_timer_id > 0)
    g_source_remove (g_search_timer_id);
  g_search_timer_id = g_timeout_add (400, on_search_timeout, page);
}

static gboolean
game_matches_search (DashboardPage *page, int idx)
{
  if (!g_search_text) return TRUE;

  g_autofree char *search_lower = g_utf8_strdown (g_search_text, -1);

  const char *name = page->games[idx].name;
  if (name && name[0])
    {
      g_autofree char *name_lower = g_utf8_strdown (name, -1);
      if (strstr (name_lower, search_lower)) return TRUE;
    }

  const char *source = page->games[idx].source;
  if (source && source[0])
    {
      g_autofree char *source_lower = g_utf8_strdown (source, -1);
      if (strstr (source_lower, search_lower)) return TRUE;
    }

  return FALSE;
}

void
refresh_games_grid (DashboardPage *page)
{
  if (!page->games_grid || !GTK_IS_GRID (page->games_grid)) return;

  /* Remove all children from grid */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (page->games_grid)))
    gtk_grid_remove (GTK_GRID (page->games_grid), child);

  int col = 0;
  int row = 0;
  for (int i = 0; i < page->n_games; i++)
    {
      if (!game_matches_search (page, i)) continue;
      GtkWidget *card = create_game_card_grid (page, i);
      gtk_grid_attach (GTK_GRID (page->games_grid), card, col, row, 1, 1);
      col++;
      if (col >= 5)
        {
          col = 0;
          row++;
        }
    }

  if (page->games_stack)
    {
      gtk_stack_set_visible_child_name (page->games_stack,
        page->n_games > 0 ? "grid" : "empty");
    }
}

static void
on_import_done (DashboardPage *page)
{
  save_games_config (page);
  refresh_games_grid (page);
  refresh_recent_games (page);
}

/* ---- Import Dialog ---- */

static void
on_import_launcher_clicked (GtkButton *btn, gpointer user_data)
{
  DashboardPage *page = (DashboardPage *) user_data;
  const char *launcher = g_object_get_data (G_OBJECT (btn), "launcher");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (btn), "dialog");

  int count = 0;
  if (g_ascii_strcasecmp (launcher, "steam") == 0)
    count = import_steam_games (page);
  else if (g_ascii_strcasecmp (launcher, "heroic") == 0)
    count = import_heroic_games (page);
  else if (g_ascii_strcasecmp (launcher, "lutris") == 0)
    count = import_lutris_games (page);
  else if (g_ascii_strcasecmp (launcher, "bottles") == 0)
    count = import_bottles_games (page);

  char *msg = NULL;
  if (count > 0)
    msg = g_strdup_printf (tr ("Imported %d game(s) from %s"), count, launcher);
  else
    msg = g_strdup_printf (tr ("No games found in %s"), launcher);
  dashboard_show_toast (msg);
  g_free (msg);

  adw_dialog_close (dialog);
  on_import_done (page);
}

static gboolean
check_launcher_installed (const char *binary)
{
  if (!binary) return FALSE;
  /* Check PATH */
  g_autofree char *path = g_find_program_in_path (binary);
  if (path) return TRUE;
  /* Check flatpak user exports */
  const char *home = g_get_home_dir ();
  g_autofree char *flatpak_user = g_build_filename (home, ".local", "share", "flatpak", "exports", "bin", binary, NULL);
  if (g_file_test (flatpak_user, G_FILE_TEST_IS_EXECUTABLE)) return TRUE;
  /* Check flatpak system exports */
  g_autofree char *flatpak_sys = g_build_filename ("/var/lib/flatpak/exports/bin", binary, NULL);
  return g_file_test (flatpak_sys, G_FILE_TEST_IS_EXECUTABLE);
}

static gboolean
check_launcher_config (const char *subpath)
{
  g_autofree char *full = g_build_filename (g_get_home_dir (), subpath, NULL);
  return g_file_test (full, G_FILE_TEST_EXISTS);
}

static void
on_import_games_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Import Games"));
  adw_dialog_set_content_width (dialog, 450);
  adw_dialog_set_content_height (dialog, 400);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 16);

  GtkWidget *desc = gtk_label_new (tr ("Select a launcher to import games from:"));
  gtk_widget_set_halign (desc, GTK_ALIGN_START);
  gtk_widget_add_css_class (desc, "dim-label");
  gtk_box_append (GTK_BOX (box), desc);

  /* Launcher entries */
  struct {
    const char *name;
    const char *binary;
    const char *config_subpath;
    const char *icon;
  } launchers[] = {
    { "Steam",   "steam",   ".steam/steam",   "icons/launchers/steam.png" },
    { "Heroic",  "heroic",  ".config/heroic", "icons/launchers/heroic.png" },
    { "Lutris",  "lutris",  ".local/share/lutris", "icons/launchers/lutris.png" },
    { "Bottles", "bottles", ".local/share/bottles", "icons/launchers/bottles.png" },
  };

  for (int i = 0; i < 4; i++)
    {
      gboolean installed = check_launcher_installed (launchers[i].binary)
                        || check_launcher_config (launchers[i].config_subpath);

      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_widget_add_css_class (row, "import-launcher-row");
      gtk_widget_add_css_class (row, installed ? "detected" : "not-detected");

      /* Icon */
      GtkWidget *icon_img = gtk_image_new ();
      gtk_image_set_pixel_size (GTK_IMAGE (icon_img), 32);
      char *icon_path = g_strdup_printf ("/io/github/mayrinck/epinephrine/%s", launchers[i].icon);
      gtk_image_set_from_resource (GTK_IMAGE (icon_img), icon_path);
      g_free (icon_path);
      gtk_box_append (GTK_BOX (row), icon_img);

      /* Name + status */
      GtkWidget *info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *name_label = gtk_label_new (launchers[i].name);
      gtk_widget_set_halign (name_label, GTK_ALIGN_START);
      gtk_widget_add_css_class (name_label, "heading");
      gtk_box_append (GTK_BOX (info_box), name_label);

      const char *status = installed ? tr ("Detected") : tr ("Not found");
      GtkWidget *status_label = gtk_label_new (status);
      gtk_widget_set_halign (status_label, GTK_ALIGN_START);
      gtk_widget_add_css_class (status_label, installed ? "dim-label" : "dim-label");
      if (installed)
        gtk_widget_add_css_class (status_label, "accent-label");
      gtk_box_append (GTK_BOX (info_box), status_label);
      gtk_box_append (GTK_BOX (row), info_box);

      /* Spacer */
      GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_hexpand (spacer, TRUE);
      gtk_box_append (GTK_BOX (row), spacer);

      /* Import button */
      GtkWidget *import_btn = gtk_button_new_with_label (tr ("Import"));
      gtk_widget_add_css_class (import_btn, "suggested-action");
      gtk_widget_set_sensitive (import_btn, installed);
      g_object_set_data_full (G_OBJECT (import_btn), "launcher", g_strdup (launchers[i].name), g_free);
      g_object_set_data (G_OBJECT (import_btn), "dialog", dialog);
      g_signal_connect (import_btn, "clicked", G_CALLBACK (on_import_launcher_clicked), page);
      gtk_box_append (GTK_BOX (row), import_btn);

      gtk_box_append (GTK_BOX (box), row);
    }

  /* Close button */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top (btn_box, 12);
  GtkWidget *close_btn = gtk_button_new_with_label (tr ("Close"));
  g_signal_connect_swapped (close_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), close_btn);
  gtk_box_append (GTK_BOX (box), btn_box);

  adw_dialog_set_child (dialog, box);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

/* ---- Add Game Dialog ---- */

static void
on_add_confirm (GtkButton *btn, gpointer user_data)
{
  (void) user_data;
  DashboardPage *page = g_object_get_data (G_OBJECT (btn), "page");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (btn), "dialog");
  GtkWidget *icon_entry = g_object_get_data (G_OBJECT (btn), "icon-entry");
  GtkWidget *name_entry = g_object_get_data (G_OBJECT (btn), "name-entry");
  GtkWidget *cmd_entry = g_object_get_data (G_OBJECT (btn), "cmd-entry");
  GtkWidget *workdir_entry = g_object_get_data (G_OBJECT (btn), "workdir-entry");
  GtkDropDown *source_drop = g_object_get_data (G_OBJECT (btn), "source-drop");
  GtkWidget *startup_entry = g_object_get_data (G_OBJECT (btn), "startup-entry");
  GtkWidget *gamemode_switch = g_object_get_data (G_OBJECT (btn), "gamemode-switch");
  GtkWidget *mangohud_switch = g_object_get_data (G_OBJECT (btn), "mangohud-switch");

  const char *name = gtk_editable_get_text (GTK_EDITABLE (name_entry));
  const char *cmd = gtk_editable_get_text (GTK_EDITABLE (cmd_entry));
  const char *icon_text = gtk_editable_get_text (GTK_EDITABLE (icon_entry));
  const char *workdir = gtk_editable_get_text (GTK_EDITABLE (workdir_entry));
  const char *startup_text = startup_entry ? gtk_editable_get_text (GTK_EDITABLE (startup_entry)) : NULL;

  if (!name || name[0] == '\0' || !cmd || cmd[0] == '\0')
    return;

  static const char *source_keys[] = { "local", "steam", "heroic", "lutris", "bottles", "other" };
  guint src_idx = gtk_drop_down_get_selected (source_drop);
  const char *source = (src_idx < 6) ? source_keys[src_idx] : "local";
  if (src_idx == 0)
    {
      const char *detected = detect_source_from_command (cmd);
      if (detected && g_strcmp0 (detected, "local") != 0)
        {
          source = detected;
          for (int s = 0; s < 6; s++)
            {
              if (g_strcmp0 (source_keys[s], detected) == 0)
                {
                  gtk_drop_down_set_selected (source_drop, s);
                  break;
                }
            }
        }
    }

  page->n_games++;
  page->games = g_realloc (page->games, page->n_games * sizeof (GameShortcut));

  GameShortcut *g = &page->games[page->n_games - 1];
  g->name = g_strdup (name);
  g->command = g_strdup (cmd);
  g->icon = (icon_text && icon_text[0] != '\0')
    ? g_strdup (icon_text) : g_strdup ("input-gaming-symbolic");
  g->workdir = (workdir && workdir[0] != '\0') ? g_strdup (workdir) : NULL;
  g->source = g_strdup (source);
  g->startup_cmd = (startup_text && startup_text[0] != '\0')
    ? g_strdup (startup_text) : NULL;
  g->feral_gamemode = gamemode_switch ?
    gtk_switch_get_active (GTK_SWITCH (gamemode_switch)) : FALSE;
  g->mangohud = mangohud_switch ?
    gtk_switch_get_active (GTK_SWITCH (mangohud_switch)) : FALSE;
  g->play_time_secs = 0;
  g->avg_fps = 0;
  g->last_played = 0;

  save_games_config (page);
  refresh_games_grid (page);
  refresh_recent_games (page);
  adw_dialog_close (dialog);
}

static void
on_add_game_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Add Game"));
  adw_dialog_set_content_width (dialog, 500);
  adw_dialog_set_content_height (dialog, 500);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 16);

  /* Name */
  GtkWidget *name_label = gtk_label_new (tr ("Game Name"));
  gtk_widget_set_halign (name_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (name_label, "dim-label");
  gtk_box_append (GTK_BOX (box), name_label);
  GtkWidget *name_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (name_entry, TRUE);
  gtk_box_append (GTK_BOX (box), name_entry);

  /* Source */
  GtkWidget *source_label = gtk_label_new (tr ("Source / Platform"));
  gtk_widget_set_halign (source_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (source_label, "dim-label");
  gtk_box_append (GTK_BOX (box), source_label);
  const char *sources[] = { tr ("Local"), tr ("Steam"), tr ("Heroic"), tr ("Lutris"), tr ("Bottles"), tr ("Other"), NULL };
  GtkStringList *source_list = gtk_string_list_new (sources);
  GtkDropDown *source_drop = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (source_list), NULL));
  g_object_unref (source_list);
  gtk_widget_set_hexpand (GTK_WIDGET (source_drop), TRUE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (source_drop));

  /* Command */
  GtkWidget *cmd_label = gtk_label_new (tr ("Launch Command"));
  gtk_widget_set_halign (cmd_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (cmd_label, "dim-label");
  gtk_box_append (GTK_BOX (box), cmd_label);
  GtkWidget *cmd_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (cmd_hbox, TRUE);
  GtkWidget *cmd_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (cmd_entry, TRUE);
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_entry);
  GtkWidget *cmd_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (cmd_browse, 36, -1);
  gtk_widget_add_css_class (cmd_browse, "flat");
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_browse);
  gtk_box_append (GTK_BOX (box), cmd_hbox);

  /* Icon */
  GtkWidget *icon_label = gtk_label_new (tr ("Icon (name or path)"));
  gtk_widget_set_halign (icon_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (icon_label, "dim-label");
  gtk_box_append (GTK_BOX (box), icon_label);
  GtkWidget *icon_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (icon_hbox, TRUE);
  GtkWidget *icon_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (icon_entry), "input-gaming-symbolic");
  gtk_widget_set_hexpand (icon_entry, TRUE);
  gtk_box_append (GTK_BOX (icon_hbox), icon_entry);
  GtkWidget *icon_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (icon_browse, 36, -1);
  gtk_widget_add_css_class (icon_browse, "flat");
  gtk_box_append (GTK_BOX (icon_hbox), icon_browse);
  gtk_box_append (GTK_BOX (box), icon_hbox);

  /* Workdir */
  GtkWidget *workdir_label = gtk_label_new (tr ("Working Directory"));
  gtk_widget_set_halign (workdir_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (workdir_label, "dim-label");
  gtk_box_append (GTK_BOX (box), workdir_label);
  GtkWidget *workdir_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (workdir_hbox, TRUE);
  GtkWidget *workdir_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (workdir_entry, TRUE);
  gtk_box_append (GTK_BOX (workdir_hbox), workdir_entry);
  GtkWidget *workdir_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (workdir_browse, 36, -1);
  gtk_widget_add_css_class (workdir_browse, "flat");
  gtk_box_append (GTK_BOX (workdir_hbox), workdir_browse);
  gtk_box_append (GTK_BOX (box), workdir_hbox);

  /* Browse buttons */
  extern void on_browse_file_clicked (GtkButton *btn, gpointer user_data);
  extern void on_browse_folder_clicked (GtkButton *btn, gpointer user_data);
  g_signal_connect (cmd_browse, "clicked", G_CALLBACK (on_browse_file_clicked), cmd_entry);
  g_signal_connect (icon_browse, "clicked", G_CALLBACK (on_browse_file_clicked), icon_entry);
  g_signal_connect (workdir_browse, "clicked", G_CALLBACK (on_browse_folder_clicked), workdir_entry);

  /* ---- Tweaks ---- */
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (sep, 8);
  gtk_widget_set_margin_bottom (sep, 8);
  gtk_box_append (GTK_BOX (box), sep);

  GtkWidget *tweaks_label = gtk_label_new (tr ("Tweaks"));
  gtk_widget_set_halign (tweaks_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (tweaks_label, "heading");
  gtk_box_append (GTK_BOX (box), tweaks_label);

  GtkWidget *startup_label = gtk_label_new (tr ("Startup commands"));
  gtk_widget_set_halign (startup_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (startup_label, "dim-label");
  gtk_box_append (GTK_BOX (box), startup_label);
  GtkWidget *startup_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (startup_entry),
    tr ("e.g. gamemoderun %command%"));
  gtk_widget_set_hexpand (startup_entry, TRUE);
  gtk_box_append (GTK_BOX (box), startup_entry);

  GtkWidget *gamemode_switch = NULL;
  if (gamemode_available ())
    {
      GtkWidget *gamemode_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_top (gamemode_row, 4);
      GtkWidget *gamemode_lbl = gtk_label_new (
        tr ("Enable Feral GameMode"));
      gtk_widget_set_halign (gamemode_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand (gamemode_lbl, TRUE);
      gtk_box_append (GTK_BOX (gamemode_row), gamemode_lbl);
      gamemode_switch = gtk_switch_new ();
      gtk_widget_set_halign (gamemode_switch, GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (gamemode_row), gamemode_switch);
      gtk_box_append (GTK_BOX (box), gamemode_row);
    }

  GtkWidget *mangohud_switch = NULL;
  if (mangohud_available ())
    {
      GtkWidget *mangohud_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_top (mangohud_row, 4);
      GtkWidget *mangohud_lbl = gtk_label_new (
        tr ("Enable MangoHud"));
      gtk_widget_set_halign (mangohud_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand (mangohud_lbl, TRUE);
      gtk_box_append (GTK_BOX (mangohud_row), mangohud_lbl);
      mangohud_switch = gtk_switch_new ();
      gtk_widget_set_halign (mangohud_switch, GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (mangohud_row), mangohud_switch);
      gtk_box_append (GTK_BOX (box), mangohud_row);
    }

  /* Buttons */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top (btn_box, 8);
  GtkWidget *cancel_btn = gtk_button_new_with_label (tr ("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), cancel_btn);
  GtkWidget *add_btn = gtk_button_new_with_label (tr ("Add"));
  gtk_widget_add_css_class (add_btn, "suggested-action");
  gtk_box_append (GTK_BOX (btn_box), add_btn);
  gtk_box_append (GTK_BOX (box), btn_box);

  adw_dialog_set_child (dialog, box);

  g_object_set_data (G_OBJECT (add_btn), "icon-entry", icon_entry);
  g_object_set_data (G_OBJECT (add_btn), "name-entry", name_entry);
  g_object_set_data (G_OBJECT (add_btn), "cmd-entry", cmd_entry);
  g_object_set_data (G_OBJECT (add_btn), "workdir-entry", workdir_entry);
  g_object_set_data (G_OBJECT (add_btn), "source-drop", source_drop);
  g_object_set_data (G_OBJECT (add_btn), "startup-entry", startup_entry);
  if (gamemode_switch)
    g_object_set_data (G_OBJECT (add_btn), "gamemode-switch", gamemode_switch);
  if (mangohud_switch)
    g_object_set_data (G_OBJECT (add_btn), "mangohud-switch", mangohud_switch);
  g_object_set_data (G_OBJECT (add_btn), "dialog", dialog);
  g_object_set_data (G_OBJECT (add_btn), "page", page);
  g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_confirm), page);

  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

void
games_page_register_actions (GActionMap *map, DashboardPage *dash_page)
{
  (void) map; (void) dash_page;
}

GtkWidget *
games_page_new (DashboardPage *dash_page)
{
  GtkWidget *page_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (page_box, 24);
  gtk_widget_set_margin_end (page_box, 24);
  gtk_widget_set_margin_top (page_box, 24);
  gtk_widget_set_margin_bottom (page_box, 16);

  /* Title */
  GtkWidget *title = gtk_label_new (tr ("Games"));
  gtk_widget_set_halign (title, GTK_ALIGN_START);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (page_box), title);

  GtkWidget *subtitle = gtk_label_new (tr ("Your game library"));
  gtk_widget_set_halign (subtitle, GTK_ALIGN_START);
  gtk_widget_set_opacity (subtitle, 0.7);
  gtk_box_append (GTK_BOX (page_box), subtitle);

  /* Toolbar: search + import + add */
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top (toolbar, 16);
  gtk_widget_set_margin_bottom (toolbar, 8);
  gtk_widget_set_hexpand (toolbar, TRUE);

  /* Search entry */
  GtkWidget *search_entry = gtk_search_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (search_entry), tr ("Search games..."));
  gtk_widget_add_css_class (search_entry, "game-search-bar");
  gtk_widget_set_hexpand (search_entry, TRUE);
  g_signal_connect (search_entry, "search-changed", G_CALLBACK (on_search_changed), dash_page);
  gtk_box_append (GTK_BOX (toolbar), search_entry);

  /* Import button */
  GtkWidget *import_btn = gtk_button_new_with_label (tr ("Import Games"));
  gtk_widget_add_css_class (import_btn, "flat");
  gtk_widget_add_css_class (import_btn, "accent-btn");
  g_signal_connect (import_btn, "clicked", G_CALLBACK (on_import_games_clicked), dash_page);
  gtk_box_append (GTK_BOX (toolbar), import_btn);

  /* Add button */
  GtkWidget *add_btn = gtk_button_new_with_label (tr ("Add Game"));
  gtk_widget_add_css_class (add_btn, "suggested-action");
  g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_game_clicked), dash_page);
  gtk_box_append (GTK_BOX (toolbar), add_btn);

  gtk_box_append (GTK_BOX (page_box), toolbar);

  /* Scrollable game grid */
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *flow = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (flow), 12);
  gtk_grid_set_row_spacing (GTK_GRID (flow), 12);
  gtk_widget_set_hexpand (flow, TRUE);
  gtk_widget_set_valign (flow, GTK_ALIGN_START);
  gtk_widget_add_css_class (flow, "games-grid");
  dash_page->games_grid = flow;

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), flow);

  /* Empty state */
  GtkWidget *empty_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (empty_box, TRUE);
  gtk_widget_set_hexpand (empty_box, TRUE);
  gtk_widget_set_valign (empty_box, GTK_ALIGN_CENTER);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name ("input-gaming-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (empty_icon), 64);
  gtk_widget_set_halign (empty_icon, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity (empty_icon, 0.3);
  gtk_box_append (GTK_BOX (empty_box), empty_icon);

  GtkWidget *empty_label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (empty_label),
    tr ("There are no registered games...\nYet."));
  gtk_widget_set_halign (empty_label, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity (empty_label, 0.5);
  gtk_widget_set_margin_top (empty_label, 8);
  gtk_box_append (GTK_BOX (empty_box), empty_label);

  /* Stack: grid vs empty */
  GtkWidget *stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (stack), 200);
  gtk_stack_add_titled (GTK_STACK (stack), scroll, "grid", tr ("Grid"));
  gtk_stack_add_titled (GTK_STACK (stack), empty_box, "empty", tr ("Empty"));
  gtk_widget_set_vexpand (stack, TRUE);
  gtk_widget_set_hexpand (stack, TRUE);
  dash_page->games_stack = GTK_STACK (stack);

  gtk_box_append (GTK_BOX (page_box), stack);

  g_object_ref_sink (page_box);
  return page_box;
}
