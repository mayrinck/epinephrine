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

static GtkWidget *
create_game_card_grid (DashboardPage *page, int idx)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (card, GTK_ALIGN_FILL);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_hexpand (card, TRUE);
  gtk_widget_set_size_request (card, -1, -1);
  gtk_widget_add_css_class (card, "game-shortcut");
  g_object_set_data (G_OBJECT (card), "game-idx", GINT_TO_POINTER (idx + 1));

  if (page->running_game_idx == idx)
    gtk_widget_add_css_class (card, "game-running");

  /* Icon with source badge overlay */
  const char *icon_name = page->games[idx].icon;
  if (!icon_name || icon_name[0] == '\0')
    icon_name = "input-gaming-symbolic";
  GtkWidget *icon = create_game_icon_widget (icon_name, 48);
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

  GtkWidget *name = gtk_label_new (
    page->games[idx].name && page->games[idx].name[0]
    ? page->games[idx].name : "?");
  gtk_widget_add_css_class (name, "game-name");
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 15);

  GtkGesture *name_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (name_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (name_click, "pressed", G_CALLBACK (on_card_clicked), page);
  gtk_widget_add_controller (name, GTK_EVENT_CONTROLLER (name_click));
  gtk_box_append (GTK_BOX (card), name);

  char *time_str = format_time_ago (page->games[idx].last_played);
  GtkWidget *stats_label = gtk_label_new (time_str);
  gtk_widget_add_css_class (stats_label, "game-stats");

  GtkGesture *stats_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (stats_click), GDK_BUTTON_PRIMARY);
  g_signal_connect (stats_click, "pressed", G_CALLBACK (on_card_clicked), page);
  gtk_widget_add_controller (stats_label, GTK_EVENT_CONTROLLER (stats_click));
  gtk_box_append (GTK_BOX (card), stats_label);
  g_free (time_str);

  const char *btn_label = (page->running_game_idx == idx)
    ? tr ("Running...")
    : tr ("Play");
  GtkWidget *launch_btn = gtk_button_new_with_label (btn_label);
  gtk_widget_add_css_class (launch_btn, "suggested-action");
  if (page->running_game_idx == idx)
    {
      gtk_widget_set_sensitive (launch_btn, FALSE);
      gtk_widget_add_css_class (launch_btn, "play-btn");
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

void
refresh_games_grid (DashboardPage *page)
{
  if (!page->games_grid || !GTK_IS_FLOW_BOX (page->games_grid)) return;

  gtk_flow_box_remove_all (page->games_grid);

  for (int i = 0; i < page->n_games; i++)
    {
      GtkWidget *card = create_game_card_grid (page, i);
      gtk_flow_box_insert (page->games_grid, card, -1);
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
source_factory_setup (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void) factory; (void) user_data;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *img = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (img), 20);
  GtkWidget *lbl = gtk_label_new (NULL);
  gtk_widget_set_halign (lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), img);
  gtk_box_append (GTK_BOX (box), lbl);
  gtk_list_item_set_child (item, box);
}

static void
source_factory_bind (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
{
  (void) factory; (void) user_data;
  guint pos = gtk_list_item_get_position (item);
  GtkWidget *box = gtk_list_item_get_child (item);
  GtkWidget *img = gtk_widget_get_first_child (box);
  GtkWidget *lbl = gtk_widget_get_next_sibling (img);
  static const char *names[] = { "Local", "Steam", "Heroic", "Lutris", "Bottles", "Other" };
  static const char *launcher_pngs[] =
    { NULL, "icons/launchers/steam.png", "icons/launchers/heroic.png",
      "icons/launchers/lutris.png", "icons/launchers/bottles.png", NULL };
  static const char *sys_icons[] =
    { "media-optical-symbolic", NULL, NULL, NULL, NULL, "application-x-executable-symbolic" };
  gtk_label_set_text (GTK_LABEL (lbl), pos < 6 ? tr (names[pos]) : "?");
  if (pos < 6 && launcher_pngs[pos])
    {
      char *path = g_strdup_printf ("/io/github/mayrinck/epinephrine/%s", launcher_pngs[pos]);
      gtk_image_set_from_resource (GTK_IMAGE (img), path);
      gtk_image_set_pixel_size (GTK_IMAGE (img), 24);
      g_free (path);
    }
  else if (pos < 6 && sys_icons[pos])
    gtk_image_set_from_icon_name (GTK_IMAGE (img), sys_icons[pos]);
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
  {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (source_factory_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (source_factory_bind), NULL);
    gtk_drop_down_set_factory (source_drop, factory);
    g_object_unref (factory);
  }
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

static void
on_import_steam (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter;
  DashboardPage *page = (DashboardPage *) user_data;
  int count = import_steam_games (page);
  char *msg = NULL;
  if (count > 0)
    msg = g_strdup (count > 1
      ? tr ("Imported %d games from Steam")
      : tr ("Imported %d game from Steam"));
  else
    msg = g_strdup (tr ("No Steam games found"));
  dashboard_show_toast (msg);
  g_free (msg);
  on_import_done (page);
}

static void
on_import_heroic (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter;
  DashboardPage *page = (DashboardPage *) user_data;
  int count = import_heroic_games (page);
  char *msg = NULL;
  if (count > 0)
    msg = g_strdup (count > 1
      ? tr ("Imported %d games from Heroic")
      : tr ("Imported %d game from Heroic"));
  else
    msg = g_strdup (tr ("No Heroic games found"));
  dashboard_show_toast (msg);
  g_free (msg);
  on_import_done (page);
}

static void
on_import_lutris (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter;
  DashboardPage *page = (DashboardPage *) user_data;
  int count = import_lutris_games (page);
  char *msg = NULL;
  if (count > 0)
    msg = g_strdup (count > 1
      ? tr ("Imported %d games from Lutris")
      : tr ("Imported %d game from Lutris"));
  else
    msg = g_strdup (tr ("No Lutris games found"));
  dashboard_show_toast (msg);
  g_free (msg);
  on_import_done (page);
}

static void
on_import_bottles (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter;
  DashboardPage *page = (DashboardPage *) user_data;
  int count = import_bottles_games (page);
  char *msg = NULL;
  if (count > 0)
    msg = g_strdup (count > 1
      ? tr ("Imported %d games from Bottles")
      : tr ("Imported %d game from Bottles"));
  else
    msg = g_strdup (tr ("No Bottles games found"));
  dashboard_show_toast (msg);
  g_free (msg);
  on_import_done (page);
}

static GActionEntry games_entries[] = {
  { "import-steam",   on_import_steam,   NULL, NULL, NULL, { 0 } },
  { "import-heroic",  on_import_heroic,  NULL, NULL, NULL, { 0 } },
  { "import-lutris",  on_import_lutris,  NULL, NULL, NULL, { 0 } },
  { "import-bottles", on_import_bottles, NULL, NULL, NULL, { 0 } },
};

void
games_page_register_actions (GActionMap *map, DashboardPage *dash_page)
{
  g_action_map_add_action_entries (map, games_entries,
    G_N_ELEMENTS (games_entries), dash_page);
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

  /* Header bar with Import + Add */
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top (header, 16);
  gtk_widget_set_margin_bottom (header, 8);
  gtk_widget_set_halign (header, GTK_ALIGN_END);
  gtk_widget_set_hexpand (header, TRUE);

  /* Import menu */
  GMenu *import_menu = g_menu_new ();
  GMenuItem *steam_item = g_menu_item_new (tr ("Steam"), "import-steam");
  g_menu_append_item (import_menu, steam_item);
  g_object_unref (steam_item);
  GMenuItem *heroic_item = g_menu_item_new (tr ("Heroic"), "import-heroic");
  g_menu_append_item (import_menu, heroic_item);
  g_object_unref (heroic_item);
  GMenuItem *lutris_item = g_menu_item_new (tr ("Lutris"), "import-lutris");
  g_menu_append_item (import_menu, lutris_item);
  g_object_unref (lutris_item);
  GMenuItem *bottles_item = g_menu_item_new (tr ("Bottles"), "import-bottles");
  g_menu_append_item (import_menu, bottles_item);
  g_object_unref (bottles_item);

  GtkWidget *import_btn = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (import_btn), tr ("Import Games"));
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (import_btn), G_MENU_MODEL (import_menu));
  g_object_unref (import_menu);
  gtk_widget_add_css_class (import_btn, "flat");
  gtk_widget_add_css_class (import_btn, "accent-btn");
  gtk_box_append (GTK_BOX (header), import_btn);

  GtkWidget *add_btn = gtk_button_new_with_label (tr ("Add Game"));
  gtk_widget_add_css_class (add_btn, "suggested-action");
  g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_game_clicked), dash_page);
  gtk_box_append (GTK_BOX (header), add_btn);

  gtk_box_append (GTK_BOX (page_box), header);

  /* Scrollable game grid */
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 24);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 24);
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 5);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 5);
  gtk_widget_set_hexpand (flow, TRUE);
  gtk_widget_add_css_class (flow, "games-flowbox");
  dash_page->games_grid = GTK_FLOW_BOX (flow);

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
