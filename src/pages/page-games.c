#include "page-games.h"
#include "page-dashboard.h"

#include "../epi-i18n.h"

#include <string.h>

#define tr(x) epi_i18n_get (x)

static void on_card_clicked (GtkGestureClick *gesture, int n_press, double x, double y, DashboardPage *page);
static void on_add_game_clicked (GtkButton *btn, DashboardPage *page);
static void on_select_toggled (GtkCheckButton *check, DashboardPage *page);
static void refresh_edit_actions (DashboardPage *page);
static void refresh_category_bar (DashboardPage *page);
static void refresh_service_dropdown (DashboardPage *page);
static void on_service_dropdown_changed (GObject *obj, GParamSpec *pspec, gpointer user_data);
static gboolean game_in_active_service (DashboardPage *page, int idx);
static void set_edit_mode (DashboardPage *page, gboolean on);
static void on_select_all_clicked (GtkButton *btn, DashboardPage *page);
static void on_select_none_clicked (GtkButton *btn, DashboardPage *page);
static void on_select_group_clicked (GtkButton *btn, DashboardPage *page);
static void on_select_service_clicked (GtkButton *btn, DashboardPage *page);
static void on_edit_mode_toggled (GtkToggleButton *toggle, DashboardPage *page);

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
  if (g_strcmp0 (source, "hydra") == 0)
    return "/io/github/mayrinck/epinephrine/icons/launchers/hydra.svg";
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
  if (g_strcmp0 (source, "hydra") == 0) return "Hydra";
  if (g_strcmp0 (source, "local") == 0) return "Local";
  return "Other";
}

static GtkWidget *
create_game_card_grid (DashboardPage *page, int idx)
{
  /* Outer overlay so the selection checkbox floats at the card's top-right. */
  GtkWidget *card_ov = gtk_overlay_new ();
  gtk_widget_set_halign (card_ov, GTK_ALIGN_START);
  gtk_widget_set_valign (card_ov, GTK_ALIGN_START);
  g_object_set_data (G_OBJECT (card_ov), "game-idx", GINT_TO_POINTER (idx + 1));

  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign (card, GTK_ALIGN_START);
  gtk_widget_set_valign (card, GTK_ALIGN_START);
  gtk_widget_set_size_request (card, 180, -1);
  gtk_widget_add_css_class (card, "game-card");
  g_object_set_data (G_OBJECT (card), "game-idx", GINT_TO_POINTER (idx + 1));
  gtk_overlay_set_child (GTK_OVERLAY (card_ov), card);

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

  /* Selection checkbox (always visible, top-right of the card). Checking it
     selects the game and enters edit mode when not already in it. */
  GtkWidget *check = gtk_check_button_new ();
  gtk_check_button_set_active (GTK_CHECK_BUTTON (check),
    page->selected && page->selected[idx]);
  gtk_widget_add_css_class (check, "game-select-check");
  gtk_widget_set_halign (check, GTK_ALIGN_START);
  gtk_widget_set_valign (check, GTK_ALIGN_START);
  gtk_widget_set_margin_start (check, 4);
  gtk_widget_set_margin_top (check, 4);
  g_object_set_data (G_OBJECT (check), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (check, "toggled", G_CALLBACK (on_select_toggled), page);
  gtk_overlay_add_overlay (GTK_OVERLAY (card_ov), check);
  if (page->selected && page->selected[idx])
    gtk_widget_add_css_class (card, "game-selected");

  return card_ov;
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

  if (page->edit_mode)
    {
      if (page->selected)
        page->selected[idx] = !page->selected[idx];
      refresh_games_grid (page);
      refresh_edit_actions (page);
      return;
    }

  open_edit_dialog (page, idx);
}

static void
on_select_toggled (GtkCheckButton *check, DashboardPage *page)
{
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (check), "game-idx")) - 1;
  if (idx < 0 || idx >= page->n_games) return;
  gboolean active = gtk_check_button_get_active (check);

  /* Checking a box while not in edit mode selects the game AND enters edit mode. */
  if (!page->edit_mode && active)
    {
      if (page->selected && idx < page->n_games)
        page->selected[idx] = TRUE;
      set_edit_mode (page, TRUE);
      return;
    }

  if (!page->selected) return;
  if (page->selected[idx] != active)
    {
      page->selected[idx] = active;
      refresh_edit_actions (page);
      refresh_games_grid (page);
    }
}

static int
count_selected (DashboardPage *page)
{
  int n = 0;
  if (!page->selected) return 0;
  for (int i = 0; i < page->n_games; i++)
    if (page->selected[i]) n++;
  return n;
}

static gboolean
any_selected_grouped (DashboardPage *page)
{
  if (!page->selected) return FALSE;
  for (int i = 0; i < page->n_games; i++)
    if (page->selected[i] && page->games[i].group) return TRUE;
  return FALSE;
}

static void
refresh_edit_actions (DashboardPage *page)
{
  if (!page->edit_actions) return;
  int n = count_selected (page);
  gboolean any = n > 0;
  GtkWidget *del_btn = g_object_get_data (G_OBJECT (page->edit_actions), "del-btn");
  GtkWidget *grp_btn = g_object_get_data (G_OBJECT (page->edit_actions), "grp-btn");
  GtkWidget *ungrp_btn = g_object_get_data (G_OBJECT (page->edit_actions), "ungrp-btn");
  GtkWidget *count_lbl = g_object_get_data (G_OBJECT (page->edit_actions), "count-lbl");
  if (del_btn) gtk_widget_set_sensitive (del_btn, any);
  if (grp_btn) gtk_widget_set_sensitive (grp_btn, any);
  if (ungrp_btn) gtk_widget_set_sensitive (ungrp_btn, any && any_selected_grouped (page));
  if (count_lbl)
    {
      char *s = g_strdup_printf (tr ("%d selected"), n);
      gtk_label_set_text (GTK_LABEL (count_lbl), s);
      g_free (s);
    }
  gtk_widget_set_visible (page->edit_actions, page->edit_mode && any);
}

static void
set_edit_mode (DashboardPage *page, gboolean on)
{
  page->edit_mode = on;

  if (page->selected && !on)
    memset (page->selected, 0, page->n_games * sizeof (gboolean));

  GtkWidget *mode_toggle = g_object_get_data (G_OBJECT (page->edit_btn), "mode-toggle");
  if (mode_toggle)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (mode_toggle), on);
  refresh_edit_actions (page);
  refresh_games_grid (page);
}

static void
on_edit_mode_toggled (GtkToggleButton *toggle, DashboardPage *page)
{
  set_edit_mode (page, gtk_toggle_button_get_active (toggle));
}

static void
on_done_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  set_edit_mode (page, FALSE);
}

static void
on_select_all_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  set_edit_mode (page, TRUE);
  if (page->selected)
    {
      for (int i = 0; i < page->n_games; i++)
        page->selected[i] = TRUE;
      refresh_edit_actions (page);
      refresh_games_grid (page);
    }
}

static void
on_select_none_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  set_edit_mode (page, TRUE);
  if (page->selected)
    {
      memset (page->selected, 0, page->n_games * sizeof (gboolean));
      refresh_edit_actions (page);
      refresh_games_grid (page);
    }
}

/* ---- Select group / service picker dialogs ---- */

static void
select_games_of_group (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  DashboardPage *page = user_data;
  const char *grp = g_object_get_data (G_OBJECT (row), "group");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (box), "dialog");
  if (!grp) return;

  set_edit_mode (page, TRUE);
  if (page->selected)
    {
      for (int i = 0; i < page->n_games; i++)
        page->selected[i] = page->games[i].group
                         && g_strcmp0 (page->games[i].group, grp) == 0;
      refresh_edit_actions (page);
      refresh_games_grid (page);
    }
  if (dialog) adw_dialog_close (dialog);
}

static void
select_games_of_service (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  DashboardPage *page = user_data;
  const char *src = g_object_get_data (G_OBJECT (row), "source");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (box), "dialog");
  if (!src) return;

  set_edit_mode (page, TRUE);
  if (page->selected)
    {
      for (int i = 0; i < page->n_games; i++)
        page->selected[i] = g_strcmp0 (page->games[i].source, src) == 0;
      refresh_edit_actions (page);
      refresh_games_grid (page);
    }
  if (dialog) adw_dialog_close (dialog);
}

static void
on_select_group_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  /* Collect unique groups */
  GPtrArray *groups = g_ptr_array_new_with_free_func (g_free);
  for (int i = 0; i < page->n_games; i++)
    {
      const char *g = page->games[i].group;
      if (!g || !g[0]) continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < groups->len; j++)
        {
          if (g_strcmp0 (g_ptr_array_index (groups, j), g) == 0)
            {
              dup = TRUE;
              break;
            }
        }
      if (!dup) g_ptr_array_add (groups, g_strdup (g));
    }
  if (groups->len == 0)
    {
      g_ptr_array_unref (groups);
      dashboard_show_toast (tr ("No groups yet"));
      return;
    }

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Select group"));
  adw_dialog_set_content_width (dialog, 380);

  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  g_object_set_data (G_OBJECT (list), "dialog", dialog);
  g_signal_connect (list, "row-activated", G_CALLBACK (select_games_of_group), page);

  for (guint j = 0; j < groups->len; j++)
    {
      GtkWidget *row = gtk_list_box_row_new ();
      GtkWidget *lbl = gtk_label_new (g_ptr_array_index (groups, j));
      gtk_widget_set_margin_start (lbl, 12);
      gtk_widget_set_margin_end (lbl, 12);
      gtk_widget_set_margin_top (lbl, 8);
      gtk_widget_set_margin_bottom (lbl, 8);
      gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), lbl);
      g_object_set_data_full (G_OBJECT (row), "group", g_strdup (g_ptr_array_index (groups, j)), g_free);
      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }
  g_ptr_array_unref (groups);

  adw_dialog_set_child (dialog, list);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

static void
on_select_service_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  /* Collect unique sources */
  GPtrArray *srcs = g_ptr_array_new_with_free_func (g_free);
  for (int i = 0; i < page->n_games; i++)
    {
      const char *s = page->games[i].source;
      if (!s || !s[0]) continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < srcs->len; j++)
        {
          if (g_strcmp0 (g_ptr_array_index (srcs, j), s) == 0)
            {
              dup = TRUE;
              break;
            }
        }
      if (!dup) g_ptr_array_add (srcs, g_strdup (s));
    }
  if (srcs->len == 0)
    {
      g_ptr_array_unref (srcs);
      dashboard_show_toast (tr ("No services yet"));
      return;
    }

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Select service"));
  adw_dialog_set_content_width (dialog, 380);

  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  g_object_set_data (G_OBJECT (list), "dialog", dialog);
  g_signal_connect (list, "row-activated", G_CALLBACK (select_games_of_service), page);

  for (guint j = 0; j < srcs->len; j++)
    {
      const char *src = g_ptr_array_index (srcs, j);
      const char *label = get_source_label (src);
      GtkWidget *row = gtk_list_box_row_new ();
      GtkWidget *lbl = gtk_label_new (label ? label : src);
      gtk_widget_set_margin_start (lbl, 12);
      gtk_widget_set_margin_end (lbl, 12);
      gtk_widget_set_margin_top (lbl, 8);
      gtk_widget_set_margin_bottom (lbl, 8);
      gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), lbl);
      g_object_set_data_full (G_OBJECT (row), "source", g_strdup (src), g_free);
      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }
  g_ptr_array_unref (srcs);

  adw_dialog_set_child (dialog, list);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

/* ---- Edit actions: delete / group / ungroup ---- */

static void
confirm_delete_selected (GtkButton *confirm_btn, DashboardPage *page)
{
  AdwDialog *dialog = g_object_get_data (G_OBJECT (confirm_btn), "dialog");

  for (int i = page->n_games - 1; i >= 0; i--)
    {
      if (!page->selected || !page->selected[i]) continue;

      g_free (page->games[i].name);
      g_free (page->games[i].command);
      g_free (page->games[i].icon);
      g_free (page->games[i].workdir);
      g_free (page->games[i].source);
      g_free (page->games[i].startup_cmd);
      g_free (page->games[i].group);

      for (int j = i; j < page->n_games - 1; j++)
        {
          page->games[j] = page->games[j + 1];
          page->selected[j] = page->selected[j + 1];
        }
      page->n_games--;
    }

  save_games_config (page);
  refresh_games_grid (page);
  refresh_recent_games (page);
  refresh_category_bar (page);
  refresh_edit_actions (page);
  if (dialog) adw_dialog_close (dialog);
  dashboard_show_toast (tr ("Games removed"));
}

static void
on_edit_delete_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  int n = count_selected (page);
  if (n == 0) return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Delete games"));
  adw_dialog_set_content_width (dialog, 380);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 16);

  char *msg = g_strdup_printf (
    tr ("Are you sure you want to delete %d game(s)? This cannot be undone."), n);
  GtkWidget *label = gtk_label_new (msg);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), label);
  g_free (msg);

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  GtkWidget *cancel_btn = gtk_button_new_with_label (tr ("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), cancel_btn);
  GtkWidget *confirm_btn = gtk_button_new_with_label (tr ("Delete"));
  gtk_widget_add_css_class (confirm_btn, "destructive-action");
  g_object_set_data (G_OBJECT (confirm_btn), "dialog", dialog);
  g_signal_connect (confirm_btn, "clicked", G_CALLBACK (confirm_delete_selected), page);
  gtk_box_append (GTK_BOX (btn_box), confirm_btn);
  gtk_box_append (GTK_BOX (box), btn_box);

  adw_dialog_set_child (dialog, box);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

static void
apply_group (GtkButton *apply_btn, DashboardPage *page)
{
  AdwDialog *dialog = g_object_get_data (G_OBJECT (apply_btn), "dialog");
  GtkWidget *entry = g_object_get_data (G_OBJECT (apply_btn), "entry");
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  g_autofree char *name = g_strstrip (g_strdup (text ? text : ""));

  if (!page->selected) { if (dialog) adw_dialog_close (dialog); return; }
  if (name[0] == '\0')
    {
      dashboard_show_toast (tr ("Group name cannot be empty"));
      return;
    }

  for (int i = 0; i < page->n_games; i++)
    {
      if (!page->selected[i]) continue;
      g_free (page->games[i].group);
      page->games[i].group = g_strdup (name);
    }

  save_games_config (page);
  if (dialog) adw_dialog_close (dialog);
  refresh_category_bar (page);
  refresh_games_grid (page);
  refresh_edit_actions (page);
  dashboard_show_toast (tr ("Games grouped"));
}

static void
on_group_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  if (count_selected (page) == 0) return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Group games"));
  adw_dialog_set_content_width (dialog, 380);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 16);

  GtkWidget *label = gtk_label_new (tr ("Category name:"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), label);

  GtkWidget *entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), tr ("e.g. Emulators, RPG..."));
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_box_append (GTK_BOX (box), entry);

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  GtkWidget *cancel_btn = gtk_button_new_with_label (tr ("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), cancel_btn);
  GtkWidget *apply_btn = gtk_button_new_with_label (tr ("Group"));
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  g_object_set_data (G_OBJECT (apply_btn), "dialog", dialog);
  g_object_set_data (G_OBJECT (apply_btn), "entry", entry);
  g_signal_connect (apply_btn, "clicked", G_CALLBACK (apply_group), page);
  gtk_box_append (GTK_BOX (btn_box), apply_btn);
  gtk_box_append (GTK_BOX (box), btn_box);

  adw_dialog_set_child (dialog, box);
  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

static void
on_ungroup_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  if (!page->selected) return;
  for (int i = 0; i < page->n_games; i++)
    {
      if (!page->selected[i]) continue;
      g_clear_pointer (&page->games[i].group, g_free);
    }

  save_games_config (page);
  refresh_category_bar (page);
  refresh_games_grid (page);
  refresh_edit_actions (page);
  dashboard_show_toast (tr ("Removed from group"));
}

/* ---- Category bar ---- */

static void
on_category_clicked (GtkButton *btn, DashboardPage *page)
{
  const char *group = g_object_get_data (G_OBJECT (btn), "group");
  g_free (page->active_group);
  page->active_group = (group && group[0]) ? g_strdup (group) : NULL;
  refresh_category_bar (page);
  refresh_games_grid (page);
}

static void
refresh_category_bar (DashboardPage *page)
{
  if (!page->category_bar) return;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (page->category_bar)))
    gtk_flow_box_remove (GTK_FLOW_BOX (page->category_bar), child);

  /* "All" chip */
  GtkWidget *all_btn = gtk_toggle_button_new_with_label (tr ("All"));
  gtk_widget_add_css_class (all_btn, "category-chip");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (all_btn),
    !page->active_group || !page->active_group[0]);
  g_object_set_data (G_OBJECT (all_btn), "group", NULL);
  g_signal_connect (all_btn, "clicked", G_CALLBACK (on_category_clicked), page);
  gtk_flow_box_append (GTK_FLOW_BOX (page->category_bar), all_btn);

  /* Unique group chips */
  for (int i = 0; i < page->n_games; i++)
    {
      const char *grp = page->games[i].group;
      if (!grp || !grp[0]) continue;
      gboolean dup = FALSE;
      for (int j = 0; j < i; j++)
        if (g_strcmp0 (page->games[j].group, grp) == 0) { dup = TRUE; break; }
      if (dup) continue;

      GtkWidget *chip = gtk_toggle_button_new_with_label (grp);
      gtk_widget_add_css_class (chip, "category-chip");
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chip),
        g_strcmp0 (grp, page->active_group) == 0);
      g_object_set_data_full (G_OBJECT (chip), "group", g_strdup (grp), g_free);
      g_signal_connect (chip, "clicked", G_CALLBACK (on_category_clicked), page);
      gtk_flow_box_append (GTK_FLOW_BOX (page->category_bar), chip);
    }

  gtk_widget_set_visible (page->category_bar,
    page->n_games > 0);

  refresh_service_dropdown (page);
}

/* ---- Service dropdown filter ---- */

static void
on_service_dropdown_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  DashboardPage *page = user_data;
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (obj));
  char **keys = g_object_get_data (obj, "service-keys");
  g_free (page->active_service);
  page->active_service = NULL;
  if (keys && sel > 0 && keys[sel - 1])
    page->active_service = g_strdup (keys[sel - 1]);
  refresh_games_grid (page);
}

static void
refresh_service_dropdown (DashboardPage *page)
{
  if (!page->service_dropdown) return;

  /* Collect unique sources in deterministic order */
  static const char *pref[] = { "steam", "heroic", "lutris", "bottles", "hydra", "local", NULL };
  GPtrArray *srcs = g_ptr_array_new_with_free_func (g_free);
  for (int p = 0; pref[p]; p++)
    {
      for (int i = 0; i < page->n_games; i++)
        {
          if (g_strcmp0 (page->games[i].source, pref[p]) == 0)
            {
              gboolean dup = FALSE;
              for (guint j = 0; j < srcs->len; j++)
                if (g_strcmp0 (g_ptr_array_index (srcs, j), pref[p]) == 0)
                  { dup = TRUE; break; }
              if (!dup) g_ptr_array_add (srcs, g_strdup (pref[p]));
              break;
            }
        }
    }
  /* Any other source not in the preferred list */
  for (int i = 0; i < page->n_games; i++)
    {
      const char *s = page->games[i].source;
      if (!s || !s[0]) continue;
      gboolean in_pref = FALSE;
      for (int p = 0; pref[p]; p++)
        if (g_strcmp0 (s, pref[p]) == 0) { in_pref = TRUE; break; }
      if (in_pref) continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < srcs->len; j++)
        if (g_strcmp0 (g_ptr_array_index (srcs, j), s) == 0)
          { dup = TRUE; break; }
      if (!dup) g_ptr_array_add (srcs, g_strdup (s));
    }

  /* Build model: "All Services" + one label per source */
  guint n = srcs->len;
  const char **items = g_new0 (const char *, n + 2);
  items[0] = tr ("All Services");
  for (guint j = 0; j < n; j++)
    {
      const char *label = get_source_label (g_ptr_array_index (srcs, j));
      items[j + 1] = label ? tr (label) : g_ptr_array_index (srcs, j);
    }

  char **keys = g_new0 (char *, n + 1);
  for (guint j = 0; j < n; j++)
    keys[j] = g_strdup (g_ptr_array_index (srcs, j));

  g_object_set_data_full (G_OBJECT (page->service_dropdown), "service-keys",
    keys, (GDestroyNotify) g_strfreev);

  GtkStringList *model = gtk_string_list_new (items);
  gtk_drop_down_set_model (GTK_DROP_DOWN (page->service_dropdown), G_LIST_MODEL (model));
  g_object_unref (model);
  g_free (items);

  /* Restore selection */
  guint sel = 0;
  if (page->active_service && page->active_service[0])
    {
      for (guint j = 0; j < n; j++)
        if (g_strcmp0 (keys[j], page->active_service) == 0)
          { sel = j + 1; break; }
    }
  gtk_drop_down_set_selected (GTK_DROP_DOWN (page->service_dropdown), sel);

  g_ptr_array_unref (srcs);
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

static gboolean
game_in_active_group (DashboardPage *page, int idx)
{
  if (!page->active_group || !page->active_group[0])
    return TRUE;
  const char *g = page->games[idx].group;
  return g && g_strcmp0 (g, page->active_group) == 0;
}

static gboolean
game_in_active_service (DashboardPage *page, int idx)
{
  if (!page->active_service || !page->active_service[0])
    return TRUE;
  return g_strcmp0 (page->games[idx].source, page->active_service) == 0;
}

void
refresh_games_grid (DashboardPage *page)
{
  if (!page->games_grid || !GTK_IS_FLOW_BOX (page->games_grid)) return;

  /* Remove all children from the flow box */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (page->games_grid)))
    gtk_flow_box_remove (GTK_FLOW_BOX (page->games_grid), child);

  int shown = 0;
  for (int i = 0; i < page->n_games; i++)
    {
      if (!game_matches_search (page, i)) continue;
      if (!game_in_active_group (page, i)) continue;
      if (!game_in_active_service (page, i)) continue;
      GtkWidget *card = create_game_card_grid (page, i);
      gtk_flow_box_append (GTK_FLOW_BOX (page->games_grid), card);
      shown++;
    }

  if (page->games_stack)
    {
      gtk_stack_set_visible_child_name (page->games_stack,
        shown > 0 ? "grid" : "empty");
    }
}

static void
on_import_done (DashboardPage *page)
{
  save_games_config (page);
  refresh_category_bar (page);
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
  else if (g_ascii_strcasecmp (launcher, "hydra") == 0)
    count = import_hydra_games (page);
  else if (g_strcmp0 (launcher, "Hydra Classics") == 0)
    count = import_hydra_classics_games (page);

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
    { "Hydra",   NULL,      ".config/hydralauncher/hydra-db", "icons/launchers/hydra.svg" },
    { "Hydra Classics", NULL, ".config/hydralauncher/hydra-db", "icons/launchers/hydra.svg" },
  };

  for (int i = 0; i < 6; i++)
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

  static const char *source_keys[] = { "local", "steam", "heroic", "lutris", "bottles", "hydra", "other" };
  guint src_idx = gtk_drop_down_get_selected (source_drop);
  const char *source = (src_idx < 7) ? source_keys[src_idx] : "local";
  if (src_idx == 0)
    {
      const char *detected = detect_source_from_command (cmd);
      if (detected && g_strcmp0 (detected, "local") != 0)
        {
          source = detected;
          for (int s = 0; s < 7; s++)
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
  page->selected = g_realloc (page->selected, page->n_games * sizeof (gboolean));
  page->selected[page->n_games - 1] = FALSE;

  GameShortcut *g = &page->games[page->n_games - 1];
  g->name = g_strdup (name);
  g->command = g_strdup (cmd);
  g->icon = (icon_text && icon_text[0] != '\0')
    ? g_strdup (icon_text) : g_strdup ("input-gaming-symbolic");
  g->workdir = (workdir && workdir[0] != '\0') ? g_strdup (workdir) : NULL;
  g->source = g_strdup (source);
  g->startup_cmd = (startup_text && startup_text[0] != '\0')
    ? g_strdup (startup_text) : NULL;
  g->group = NULL;
  g->feral_gamemode = gamemode_switch ?
    gtk_switch_get_active (GTK_SWITCH (gamemode_switch)) : FALSE;
  g->mangohud = mangohud_switch ?
    gtk_switch_get_active (GTK_SWITCH (mangohud_switch)) : FALSE;
  g->play_time_secs = 0;
  g->avg_fps = 0;
  g->last_played = 0;

  save_games_config (page);
  refresh_category_bar (page);
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
  const char *sources[] = { tr ("Local"), tr ("Steam"), tr ("Heroic"), tr ("Lutris"), tr ("Bottles"), "Hydra", tr ("Other"), NULL };
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

  /* Toolbar: search + edit + import + add */
  GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top (toolbar, 16);
  gtk_widget_set_margin_bottom (toolbar, 8);
  gtk_widget_set_hexpand (toolbar, TRUE);

  /* Search entry */
  GtkWidget *search_entry = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search_entry), tr ("Search games..."));
  gtk_widget_add_css_class (search_entry, "game-search-bar");
  gtk_widget_set_hexpand (search_entry, TRUE);
  g_signal_connect (search_entry, "search-changed", G_CALLBACK (on_search_changed), dash_page);
  gtk_box_append (GTK_BOX (toolbar), search_entry);

  /* Service filter dropdown (sources in use) */
  GtkWidget *service_dd = gtk_drop_down_new (NULL, NULL);
  gtk_widget_set_valign (service_dd, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (service_dd, FALSE);
  g_signal_connect (service_dd, "notify::selected",
    G_CALLBACK (on_service_dropdown_changed), dash_page);
  gtk_box_append (GTK_BOX (toolbar), service_dd);
  dash_page->service_dropdown = service_dd;

  /* Edit mode toggle button */
  GtkWidget *edit_btn = gtk_toggle_button_new ();
  GtkWidget *edit_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *edit_icon = gtk_image_new_from_icon_name ("document-edit-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (edit_icon), 16);
  gtk_box_append (GTK_BOX (edit_box), edit_icon);
  GtkWidget *edit_lbl = gtk_label_new (tr ("Edit Mode"));
  gtk_box_append (GTK_BOX (edit_box), edit_lbl);
  gtk_button_set_child (GTK_BUTTON (edit_btn), edit_box);
  gtk_widget_add_css_class (edit_btn, "flat");
  gtk_widget_add_css_class (edit_btn, "accent-btn");
  gtk_widget_set_tooltip_text (edit_btn, tr ("Edit games"));
  gtk_widget_set_valign (edit_btn, GTK_ALIGN_CENTER);
  g_signal_connect (edit_btn, "toggled", G_CALLBACK (on_edit_mode_toggled), dash_page);
  gtk_box_append (GTK_BOX (toolbar), edit_btn);
  dash_page->edit_btn = edit_btn;
  g_object_set_data (G_OBJECT (edit_btn), "mode-toggle", edit_btn);

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

  /* Category bar (All + groups) */
  GtkWidget *category_bar = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (category_bar), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (category_bar), FALSE);
  gtk_widget_set_margin_bottom (category_bar, 8);
  gtk_widget_set_hexpand (category_bar, TRUE);
  gtk_widget_set_halign (category_bar, GTK_ALIGN_FILL);
  dash_page->category_bar = category_bar;
  gtk_box_append (GTK_BOX (page_box), category_bar);
  refresh_category_bar (dash_page);

  /* Edit actions bar (visible only in edit mode). Selection helpers come
     first (moved here from the old popover), then the group/delete actions,
     then the "Done" button that leaves edit mode. */
  GtkWidget *edit_actions = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (edit_actions), GTK_SELECTION_NONE);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (edit_actions), 6);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (edit_actions), 6);
  gtk_widget_set_margin_bottom (edit_actions, 8);
  gtk_widget_set_hexpand (edit_actions, TRUE);
  gtk_widget_set_halign (edit_actions, GTK_ALIGN_FILL);
  gtk_widget_set_visible (edit_actions, FALSE);
  gtk_box_append (GTK_BOX (page_box), edit_actions);
  dash_page->edit_actions = edit_actions;

  GtkWidget *count_lbl = gtk_label_new ("");
  gtk_widget_set_valign (count_lbl, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end (count_lbl, 8);
  gtk_widget_add_css_class (count_lbl, "dim-label");
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), count_lbl);
  g_object_set_data (G_OBJECT (edit_actions), "count-lbl", count_lbl);

  GtkWidget *sel_all = gtk_button_new_with_label (tr ("Select All"));
  gtk_widget_add_css_class (sel_all, "flat");
  g_signal_connect (sel_all, "clicked", G_CALLBACK (on_select_all_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), sel_all);

  GtkWidget *sel_none = gtk_button_new_with_label (tr ("Select None"));
  gtk_widget_add_css_class (sel_none, "flat");
  g_signal_connect (sel_none, "clicked", G_CALLBACK (on_select_none_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), sel_none);

  GtkWidget *sel_group = gtk_button_new_with_label (tr ("Select Group..."));
  gtk_widget_add_css_class (sel_group, "flat");
  g_signal_connect (sel_group, "clicked", G_CALLBACK (on_select_group_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), sel_group);

  GtkWidget *sel_service = gtk_button_new_with_label (tr ("Select Service..."));
  gtk_widget_add_css_class (sel_service, "flat");
  g_signal_connect (sel_service, "clicked", G_CALLBACK (on_select_service_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), sel_service);

  GtkWidget *ungrp_btn = gtk_button_new_with_label (tr ("Remove from Group"));
  gtk_widget_add_css_class (ungrp_btn, "flat");
  g_signal_connect (ungrp_btn, "clicked", G_CALLBACK (on_ungroup_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), ungrp_btn);
  g_object_set_data (G_OBJECT (edit_actions), "ungrp-btn", ungrp_btn);

  GtkWidget *grp_btn = gtk_button_new_with_label (tr ("Group..."));
  gtk_widget_add_css_class (grp_btn, "flat");
  gtk_widget_add_css_class (grp_btn, "accent-btn");
  g_signal_connect (grp_btn, "clicked", G_CALLBACK (on_group_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), grp_btn);
  g_object_set_data (G_OBJECT (edit_actions), "grp-btn", grp_btn);

  GtkWidget *del_btn = gtk_button_new_with_label (tr ("Delete"));
  gtk_widget_add_css_class (del_btn, "destructive-action");
  g_signal_connect (del_btn, "clicked", G_CALLBACK (on_edit_delete_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), del_btn);
  g_object_set_data (G_OBJECT (edit_actions), "del-btn", del_btn);

  GtkWidget *done_btn = gtk_button_new_with_label (tr ("Done"));
  gtk_widget_add_css_class (done_btn, "suggested-action");
  g_signal_connect (done_btn, "clicked", G_CALLBACK (on_done_clicked), dash_page);
  gtk_flow_box_append (GTK_FLOW_BOX (edit_actions), done_btn);

  refresh_edit_actions (dash_page);

  /* Scrollable game grid */
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_set_hexpand (scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  /* Responsive game grid: children flow left-to-right and wrap to as many
     columns as the window width allows (no fixed per-line limit). */
  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 12);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 12);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 1);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), -1);
  gtk_widget_set_halign (flow, GTK_ALIGN_FILL);
  gtk_widget_set_valign (flow, GTK_ALIGN_START);
  gtk_widget_set_hexpand (flow, TRUE);
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
