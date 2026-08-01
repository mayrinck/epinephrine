#include "page-dashboard.h"
#include "page-games.h"
#include "window.h"
#include "epi-privilege.h"

#include <string.h>
#include <time.h>
#include <sys/wait.h>

typedef struct
{
  const char *name;
  const char *resource_icon;
  const char *icon_name;
  const char *pkg;
  const char *flatpak_id;
  const char *binary_name;
  const char *appimage_name;
} GamingResourceDef;

static const GamingResourceDef RESOURCES[DASHBOARD_N_RESOURCES] = {
  { "Steam",          "/io/github/mayrinck/epinephrine/icons/launchers/steam.png",  NULL,                "steam",              "com.valvesoftware.Steam",          NULL,            NULL },
  { "Heroic",         "/io/github/mayrinck/epinephrine/icons/launchers/heroic.png", NULL,                "heroic",             "com.heroicgameslauncher.hlg",      "heroic-games-launcher", "heroic" },
  { "Lutris",         "/io/github/mayrinck/epinephrine/icons/launchers/lutris.png", NULL,                "lutris",             "net.lutris.Lutris",                NULL,            NULL },
  { "Bottles",        "/io/github/mayrinck/epinephrine/icons/launchers/bottles.png",NULL,                "bottles",            "org.usebottles.bottles",           NULL,            NULL },
  { "OpenRGB",        "/io/github/mayrinck/epinephrine/icons-sym/bulb-symbolic.svg", NULL,                "openrgb",            "org.openrgb.OpenRGB",              NULL,            NULL },
  { "MangoHud",       NULL,                                                        "speedometer-symbolic", "mangohud",           NULL,                               NULL,            NULL },
  { "Valve Gamescope", NULL,                                                        "gamescope-symbolic", "gamescope",       NULL,                               NULL,            NULL },
  { "Feral GameMode", NULL,                                                        "addon-symbolic", "gamemode",           NULL,                               "gamemoderun",     NULL },
};

static const char *detected_pm = NULL;
static const char *GAMES_CONFIG_PATH = NULL;

void
dashboard_show_toast (const char *message)
{
  epinephrine_show_toast (message);
}

static void
init_config_path (void)
{
  const char *home = g_get_home_dir ();
  char *dir = g_build_filename (home, ".config", "epinephrine", NULL);
  g_mkdir_with_parents (dir, 0755);
  GAMES_CONFIG_PATH = g_build_filename (dir, "games.ini", NULL);
  g_free (dir);
}

/*
 * Run a shell command safely via g_spawn_sync with argv.
 * Uses /bin/sh -c for commands that require pipes/redirects.
 * Only for compile-time constant string literals.
 */
#define RUN_SH_STATIC(cmd) \
  ({ \
    char *_out = NULL; \
    GError *_err = NULL; \
    int _rc = -1; \
    g_spawn_sync (NULL, \
      (char *[]){ "/bin/sh", "-c", (char *)(cmd), NULL }, \
      NULL, G_SPAWN_DEFAULT, NULL, NULL, \
      &_out, NULL, &_rc, &_err); \
    if (_err) { \
      g_warning ("RUN_SH_STATIC failed: %s (cmd: %s)", _err->message, (cmd)); \
      g_error_free (_err); \
      g_free (_out); \
      _out = NULL; \
    } else if (_rc != 0) { \
      g_warning ("RUN_SH_STATIC exit code %d (cmd: %s)", _rc, (cmd)); \
      g_free (_out); \
      _out = NULL; \
    } else if (_out) { \
      g_strstrip (_out); \
      if (_out[0] == '\0') { g_free (_out); _out = NULL; } \
    } \
    _out; \
  })

static const char *
detect_pm (void)
{
  if (g_file_test ("/usr/bin/apt",        G_FILE_TEST_IS_EXECUTABLE)) return "apt";
  if (g_file_test ("/usr/bin/dnf",        G_FILE_TEST_IS_EXECUTABLE)) return "dnf";
  if (g_file_test ("/usr/bin/pacman",     G_FILE_TEST_IS_EXECUTABLE)) return "pacman";
  if (g_file_test ("/usr/bin/zypper",    G_FILE_TEST_IS_EXECUTABLE)) return "zypper";
  if (g_file_test ("/usr/bin/xbps-install", G_FILE_TEST_IS_EXECUTABLE)) return "xbps";
  return NULL;
}

static gboolean
check_pkg_native (const char *pkg)
{
  if (!detected_pm || !pkg) return FALSE;

  if (strcmp (detected_pm, "apt") == 0)
    {
      char *out = NULL;
      g_spawn_sync (NULL,
        (char *[]){ "dpkg-query", "-W", "-f=${Status}", (char *)pkg, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);
      gboolean ok = out && strstr (out, "ok installed");
      g_free (out);
      return ok;
    }
  else if (strcmp (detected_pm, "dnf") == 0 || strcmp (detected_pm, "zypper") == 0)
    {
      int rc = -1;
      g_spawn_sync (NULL,
        (char *[]){ "rpm", "-q", (char *)pkg, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &rc, NULL);
      return g_spawn_check_wait_status (rc, NULL);
    }
  else if (strcmp (detected_pm, "pacman") == 0)
    {
      int rc = -1;
      g_spawn_sync (NULL,
        (char *[]){ "pacman", "-Q", (char *)pkg, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &rc, NULL);
      return g_spawn_check_wait_status (rc, NULL);
    }
  else if (strcmp (detected_pm, "xbps") == 0)
    {
      int rc = -1;
      g_spawn_sync (NULL,
        (char *[]){ "xbps-query", (char *)pkg, NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &rc, NULL);
      return g_spawn_check_wait_status (rc, NULL);
    }
  else
    return FALSE;
}

static gboolean
check_flatpak_app (const char *app_id)
{
  if (!app_id) return FALSE;
  int rc = -1;
  g_spawn_sync (NULL,
    (char *[]){ "flatpak", "info", (char *)app_id, NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &rc, NULL);
  return g_spawn_check_wait_status (rc, NULL);
}

static gboolean
check_snap_pkg (const char *pkg)
{
  if (!pkg) return FALSE;
  int rc = -1;
  g_spawn_sync (NULL,
    (char *[]){ "snap", "list", (char *)pkg, NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &rc, NULL);
  return g_spawn_check_wait_status (rc, NULL);
}

static gboolean
check_linglong_app (const char *app_id)
{
  if (!app_id) return FALSE;
  char *out = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "ll-cli", "list", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);
  if (!out) return FALSE;
  gboolean found = strstr (out, app_id) != NULL;
  g_free (out);
  return found;
}

static gboolean
check_appimage (const char *name)
{
  if (!name) return FALSE;

  const char *home = g_get_home_dir ();
  const char *dirs[] = { home, "/opt", "/usr/local/bin", NULL };
  for (int d = 0; dirs[d]; d++)
    {
      char *pattern = g_strdup_printf ("%s/%s*.AppImage", dirs[d], name);
      gboolean found = FALSE;
      if (g_file_test (pattern, G_FILE_TEST_EXISTS))
        found = TRUE;
      g_free (pattern);
      if (found) return TRUE;

      pattern = g_strdup_printf ("%s/Applications/%s*.AppImage", dirs[d], name);
      if (g_file_test (pattern, G_FILE_TEST_EXISTS))
        found = TRUE;
      g_free (pattern);
      if (found) return TRUE;
    }
  return FALSE;
}

static gboolean
check_flatpak_binary (const char *flatpak_id)
{
  if (!flatpak_id) return FALSE;
  char buf[1024];
  g_snprintf (buf, sizeof (buf), "/var/lib/flatpak/exports/bin/%s", flatpak_id);
  if (g_file_test (buf, G_FILE_TEST_IS_EXECUTABLE)) return TRUE;
  const char *home = g_get_home_dir ();
  g_snprintf (buf, sizeof (buf),
    "%s/.local/share/flatpak/exports/bin/%s", home, flatpak_id);
  return g_file_test (buf, G_FILE_TEST_IS_EXECUTABLE);
}

static gboolean
check_binary (const char *name)
{
  if (!name) return FALSE;
  if (g_find_program_in_path (name)) return TRUE;
  char buf[1024];
  const char *home = g_get_home_dir ();
  g_snprintf (buf, sizeof (buf),
    "/var/lib/flatpak/exports/bin/%s", name);
  if (g_file_test (buf, G_FILE_TEST_IS_EXECUTABLE)) return TRUE;
  g_snprintf (buf, sizeof (buf),
    "%s/.local/share/flatpak/exports/bin/%s", home, name);
  return g_file_test (buf, G_FILE_TEST_IS_EXECUTABLE);
}

static char *
format_play_time (int secs)
{
  if (secs < 60)
    return g_strdup_printf ("%ds", secs);
  else if (secs < 3600)
    return g_strdup_printf ("%dm %ds", secs / 60, secs % 60);
  else
    return g_strdup_printf ("%dh %dm", secs / 3600, (secs % 3600) / 60);
}

char *
format_time_ago (gint64 last_played)
{
  if (last_played == 0)
    return g_strdup (g_dgettext (GETTEXT_PACKAGE, "Never played"));

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 diff = now - last_played;

  if (diff < 0) return g_strdup (g_dgettext (GETTEXT_PACKAGE, "Just now"));
  if (diff < 60)        return g_strdup (g_dgettext (GETTEXT_PACKAGE, "Just now"));
  if (diff < 3600)      return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d min ago"), (int)(diff / 60));
  if (diff < 86400)     return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d hours ago"), (int)(diff / 3600));
  if (diff < 604800)    return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d days ago"), (int)(diff / 86400));
  if (diff < 2592000)   return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d weeks ago"), (int)(diff / 604800));
  if (diff < 31536000)  return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d months ago"), (int)(diff / 2592000));
  return g_strdup_printf (g_dgettext (GETTEXT_PACKAGE, "%d years ago"), (int)(diff / 31536000));
}

static void
free_games (DashboardPage *page)
{
  for (int i = 0; i < page->n_games; i++)
    {
      g_free (page->games[i].name);
      g_free (page->games[i].command);
      g_free (page->games[i].icon);
      g_free (page->games[i].workdir);
      g_free (page->games[i].source);
      g_free (page->games[i].startup_cmd);
    }
  g_free (page->games);
  page->games = NULL;
  page->n_games = 0;
}

const char *
detect_source_from_command (const char *cmd)
{
  if (!cmd || cmd[0] == '\0')
    return "local";
  if (g_str_has_prefix (cmd, "steam steam://rungameid/"))
    return "steam";
  return "local";
}

static void
load_games_config (DashboardPage *page)
{
  free_games (page);

  if (!GAMES_CONFIG_PATH)
    init_config_path ();

  GKeyFile *keyfile = g_key_file_new ();
  GError *err = NULL;

  if (!g_key_file_load_from_file (keyfile, GAMES_CONFIG_PATH, G_KEY_FILE_NONE, &err))
    {
      g_error_free (err);
      g_key_file_free (keyfile);
      return;
    }

  gsize num_games = 0;
  char **groups = g_key_file_get_groups (keyfile, &num_games);

  page->games = g_new0 (GameShortcut, num_games);
  page->n_games = 0;

  for (gsize i = 0; i < num_games; i++)
    {
      if (g_str_has_prefix (groups[i], "game_"))
        {
          char *name = g_key_file_get_string (keyfile, groups[i], "name", NULL);
          char *command = g_key_file_get_string (keyfile, groups[i], "command", NULL);
          char *icon = g_key_file_get_string (keyfile, groups[i], "icon", NULL);
          char *workdir = g_key_file_get_string (keyfile, groups[i], "workdir", NULL);
          char *source = g_key_file_get_string (keyfile, groups[i], "source", NULL);
          int play_time = g_key_file_get_integer (keyfile, groups[i], "play_time", NULL);
          double avg_fps = g_key_file_get_double (keyfile, groups[i], "avg_fps", NULL);
          gint64 last_played = g_key_file_get_int64 (keyfile, groups[i], "last_played", NULL);

          if (name && command)
            {
              page->games[page->n_games].name = name;
              page->games[page->n_games].command = command;
              page->games[page->n_games].icon = icon ? icon : g_strdup ("input-gaming-symbolic");
              page->games[page->n_games].workdir = workdir;
              if (source && g_strcmp0 (source, "appimage") == 0)
                {
                  g_free (source);
                  source = g_strdup ("local");
                }
              page->games[page->n_games].source = source ? source : g_strdup ("local");
              page->games[page->n_games].startup_cmd = g_key_file_get_string (keyfile, groups[i], "startup_cmd", NULL);
              page->games[page->n_games].feral_gamemode = g_key_file_get_boolean (keyfile, groups[i], "feral_gamemode", NULL);
              page->games[page->n_games].mangohud = g_key_file_get_boolean (keyfile, groups[i], "mangohud", NULL);
              page->games[page->n_games].play_time_secs = play_time;
              page->games[page->n_games].avg_fps = (float) avg_fps;
              page->games[page->n_games].last_played = last_played;
              page->n_games++;
            }
          else
            {
              g_free (name);
              g_free (command);
              g_free (icon);
              g_free (workdir);
              g_free (source);
            }
        }
    }

  g_strfreev (groups);
  g_key_file_free (keyfile);
}

void
save_games_config (DashboardPage *page)
{
  if (!GAMES_CONFIG_PATH)
    init_config_path ();

  GKeyFile *keyfile = g_key_file_new ();

  for (int i = 0; i < page->n_games; i++)
    {
      char group[64];
      snprintf (group, sizeof (group), "game_%d", i);

      g_key_file_set_string (keyfile, group, "name", page->games[i].name);
      g_key_file_set_string (keyfile, group, "command", page->games[i].command);
      g_key_file_set_string (keyfile, group, "icon", page->games[i].icon);
      if (page->games[i].workdir)
        g_key_file_set_string (keyfile, group, "workdir", page->games[i].workdir);
      g_key_file_set_string (keyfile, group, "source",
        page->games[i].source ? page->games[i].source : "local");
      g_key_file_set_integer (keyfile, group, "play_time", page->games[i].play_time_secs);
      g_key_file_set_double (keyfile, group, "avg_fps", page->games[i].avg_fps);
      g_key_file_set_int64 (keyfile, group, "last_played", page->games[i].last_played);
      if (page->games[i].startup_cmd)
        g_key_file_set_string (keyfile, group, "startup_cmd", page->games[i].startup_cmd);
      g_key_file_set_boolean (keyfile, group, "feral_gamemode", page->games[i].feral_gamemode);
      g_key_file_set_boolean (keyfile, group, "mangohud", page->games[i].mangohud);
    }

  GError *err = NULL;
  char *data = g_key_file_to_data (keyfile, NULL, &err);

  if (data)
    {
      g_file_set_contents (GAMES_CONFIG_PATH, data, -1, NULL);
      g_free (data);
    }
  else
    {
      g_warning ("Failed to save games config: %s", err->message);
      g_error_free (err);
    }

  g_key_file_free (keyfile);
}

/* ---- Recent Games (Overview) ---- */

static void
on_recent_play_clicked (GtkButton *btn, DashboardPage *page)
{
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "game-idx")) - 1;
  if (idx < 0 || idx >= page->n_games) return;

  /* Find the actual game in the sorted recent list */
  on_game_launch_clicked (btn, page);
}

static GtkWidget *
create_recent_card (DashboardPage *page, int idx)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (card, GTK_ALIGN_FILL);
  gtk_widget_set_valign (card, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand (card, TRUE);
  gtk_widget_set_size_request (card, -1, 160);
  gtk_widget_add_css_class (card, "game-shortcut");

  if (page->running_game_idx == idx)
    gtk_widget_add_css_class (card, "game-running");

  const char *icon_name = page->games[idx].icon;
  if (!icon_name || icon_name[0] == '\0')
    icon_name = "input-gaming-symbolic";
  GtkWidget *icon = create_game_icon_widget (icon_name, 48);
  gtk_widget_add_css_class (icon, "game-icon");
  gtk_box_append (GTK_BOX (card), icon);

  GtkWidget *name = gtk_label_new (
    page->games[idx].name && page->games[idx].name[0]
    ? page->games[idx].name : "?");
  gtk_widget_add_css_class (name, "game-name");
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (name), 15);
  gtk_box_append (GTK_BOX (card), name);

  char *time_str = format_time_ago (page->games[idx].last_played);
  GtkWidget *stats_label = gtk_label_new (time_str);
  gtk_widget_add_css_class (stats_label, "game-stats");
  gtk_box_append (GTK_BOX (card), stats_label);
  g_free (time_str);

  const char *btn_label = (page->running_game_idx == idx)
    ? g_dgettext (GETTEXT_PACKAGE, "Running...")
    : g_dgettext (GETTEXT_PACKAGE, "Play Now");
  GtkWidget *launch_btn = gtk_button_new_with_label (btn_label);
  gtk_widget_add_css_class (launch_btn, "suggested-action");
  gtk_widget_add_css_class (launch_btn, "play-now-btn");
  if (page->running_game_idx == idx)
    {
      gtk_widget_set_sensitive (launch_btn, FALSE);
      gtk_widget_add_css_class (launch_btn, "play-btn");
    }
  g_object_set_data (G_OBJECT (launch_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (launch_btn, "clicked", G_CALLBACK (on_recent_play_clicked), page);
  gtk_box_append (GTK_BOX (card), launch_btn);

  return card;
}

void
refresh_recent_games (DashboardPage *page)
{
  if (!page->recent_box || !GTK_IS_WIDGET (page->recent_box)) return;

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (page->recent_box)))
    {
      if (GTK_IS_WIDGET (child))
        gtk_widget_unparent (child);
    }

  /* Sort games by last_played descending */
  int *sorted = g_new (int, page->n_games);
  for (int i = 0; i < page->n_games; i++)
    sorted[i] = i;

  for (int i = 0; i < page->n_games - 1; i++)
    for (int j = i + 1; j < page->n_games; j++)
      if (page->games[sorted[j]].last_played > page->games[sorted[i]].last_played)
        { int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp; }

  int count = MIN (page->n_games, DASHBOARD_N_RECENT);
  for (int i = 0; i < count; i++)
    {
      GtkWidget *card = create_recent_card (page, sorted[i]);
      gtk_box_append (GTK_BOX (page->recent_box), card);
    }

  /* Fill remaining slots with empty placeholders */
  for (int i = count; i < DASHBOARD_N_RECENT; i++)
    {
      GtkWidget *placeholder = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_hexpand (placeholder, TRUE);
      gtk_widget_set_size_request (placeholder, 120, 160);
      gtk_box_append (GTK_BOX (page->recent_box), placeholder);
    }

  g_free (sorted);
}

/* ---- Game Process Tracking ---- */

static gboolean
has_child_process (GPid parent_pid)
{
  char pid_str[32];
  g_snprintf (pid_str, sizeof (pid_str), "%d", (int) parent_pid);
  char *out = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "ps", "--ppid", pid_str, "-o", "pid=", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);
  if (out)
    {
      g_free (out);
      return TRUE;
    }
  return FALSE;
}

static gboolean
has_descendant_running (GPid pid)
{
  if (has_child_process (pid))
    return TRUE;

  char pid_str[32];
  g_snprintf (pid_str, sizeof (pid_str), "%d", (int) pid);
  char *out = NULL;
  g_spawn_sync (NULL,
    (char *[]){ "ps", "--ppid", pid_str, "-o", "pid=", NULL },
    NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);

  if (!out || out[0] == '\0') { g_free (out); return FALSE; }

  char **pids = g_strsplit (out, "\n", -1);
  gboolean found = FALSE;
  for (int i = 0; pids[i] && !found; i++)
    {
      g_strstrip (pids[i]);
      if (pids[i][0] == '\0') continue;
      GPid child_pid = (GPid) atoi (pids[i]);
      if (child_pid > 0)
        found = has_descendant_running (child_pid);
    }
  g_strfreev (pids);
  g_free (out);
  return found;
}

static float
parse_mangohud_fps (void)
{
  const char *home = g_get_home_dir ();
  char *mango_dir = g_build_filename (home, ".local", "share", "MangoHud", NULL);

  GDir *dir = g_dir_open (mango_dir, 0, NULL);
  if (!dir) { g_free (mango_dir); return 0.0f; }

  char *latest = NULL;
  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      if (!g_str_has_suffix (entry, ".csv")) continue;
      if (!latest || strcmp (entry, latest) > 0)
        {
          g_free (latest);
          latest = g_strdup (entry);
        }
    }
  g_dir_close (dir);

  if (!latest)
    {
      g_free (mango_dir);
      return 0.0f;
    }

  char *csv_path = g_build_filename (mango_dir, latest, NULL);
  g_free (latest);
  g_free (mango_dir);

  char *contents = NULL;
  GError *err = NULL;
  if (!g_file_get_contents (csv_path, &contents, NULL, &err))
    {
      g_free (csv_path);
      if (err) g_error_free (err);
      return 0.0f;
    }
  g_free (csv_path);

  float total_fps = 0;
  int count = 0;
  char **lines = g_strsplit (contents, "\n", -1);

  for (int i = 1; lines[i]; i++)
    {
      g_strstrip (lines[i]);
      if (lines[i][0] == '\0') continue;

      char **cols = g_strsplit (lines[i], ",", -1);
      if (cols[0])
        {
          float fps = (float) atof (cols[0]);
          if (fps > 0)
            {
              total_fps += fps;
              count++;
            }
        }
      g_strfreev (cols);
    }

  g_strfreev (lines);
  g_free (contents);

  return count > 0 ? total_fps / count : 0.0f;
}

static gboolean
steam_game_watch_cb (gpointer user_data)
{
  DashboardPage *page = (DashboardPage *) user_data;
  int idx = page->running_game_idx;
  if (idx < 0 || idx >= page->n_games)
    {
      page->steam_watch_id = 0;
      page->steam_watch_idle_secs = 0;
      return G_SOURCE_REMOVE;
    }

  /* Check for Steam game processes (binaries in steamapps/common/) */
  gboolean found = FALSE;
  char *result = RUN_SH_STATIC (
    "ps -u $(whoami) -o args= 2>/dev/null | grep -q 'steamapps/common/' 2>/dev/null && echo yes");
  if (result && g_strcmp0 (result, "yes") == 0)
    found = TRUE;
  g_free (result);

  if (found)
    {
      page->steam_watch_idle_secs = 0;
      return G_SOURCE_CONTINUE;
    }

  /* No game process found — give the game time to start first */
  gint64 elapsed = g_get_real_time () - page->game_start_us;
  if (elapsed < 15 * G_USEC_PER_SEC)
    return G_SOURCE_CONTINUE;

  /* Game process has disappeared — accumulate idle time before declaring game over */
  page->steam_watch_idle_secs += 2;
  if (page->steam_watch_idle_secs < 8)
    return G_SOURCE_CONTINUE;

  /* Game has ended */
  {
    gint64 elapsed_us = g_get_real_time () - page->game_start_us;
    int elapsed_secs = (int) (elapsed_us / G_USEC_PER_SEC);
    if (elapsed_secs > 1 && idx >= 0 && idx < page->n_games)
      {
        page->games[idx].play_time_secs += elapsed_secs;
        page->games[idx].last_played = g_get_real_time () / G_USEC_PER_SEC;
        float fps = parse_mangohud_fps ();
        if (fps > 0)
          page->games[idx].avg_fps = fps;
      }

    page->running_game_idx = -1;
    page->game_pid = 0;
    page->steam_watch_id = 0;
    page->steam_watch_idle_secs = 0;

    save_games_config (page);
    refresh_recent_games (page);
  }

  return G_SOURCE_REMOVE;
}

static gboolean
game_poll_cb (gpointer user_data)
{
  DashboardPage *page = (DashboardPage *) user_data;
  int idx = page->running_game_idx;

  if (idx < 0 || idx >= page->n_games)
    {
      page->poll_timer_id = 0;
      return G_SOURCE_REMOVE;
    }

  gboolean still_running = FALSE;

  if (page->game_pid > 0)
    {
      int status;
      GPid reaped = waitpid (page->game_pid, &status, WNOHANG);
      if (reaped == 0)
        still_running = TRUE;
      else if (reaped > 0)
        still_running = has_descendant_running (page->game_pid);
    }

  if (!still_running)
    {
      gint64 elapsed_us = g_get_real_time () - page->game_start_us;
      int elapsed_secs = (int) (elapsed_us / G_USEC_PER_SEC);
      page->games[idx].play_time_secs += elapsed_secs;
      page->games[idx].last_played = g_get_real_time () / G_USEC_PER_SEC;

      float fps = parse_mangohud_fps ();
      if (fps > 0)
        page->games[idx].avg_fps = fps;

      page->running_game_idx = -1;
      page->game_pid = 0;
      page->poll_timer_id = 0;

      save_games_config (page);
      refresh_recent_games (page);

      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static void
on_steam_exit (GPid pid, int status, gpointer user_data)
{
  (void) status;
  DashboardPage *page = (DashboardPage *) user_data;

  if (page->game_pid != pid) return;

  if (has_descendant_running (pid))
    {
      /* Launcher exited but game process is still running.
       * Reset start time so we only count actual game time. */
      page->game_start_us = g_get_real_time ();
      if (page->poll_timer_id == 0)
        page->poll_timer_id = g_timeout_add_seconds (2, game_poll_cb, page);
    }
  else if (page->is_steam_game
           && !page->games[page->running_game_idx].mangohud)
    {
      /* Steam CLI helper exited (normal — it sends the URL to the daemon and
         quits), but the game is still running under the persistent Steam
         process.  Start the Steam game watcher to track the real game. */
      if (page->steam_watch_id == 0)
        page->steam_watch_id = g_timeout_add_seconds (2, steam_game_watch_cb, page);
    }
  else
    {
      gint64 elapsed_us = g_get_real_time () - page->game_start_us;
      int elapsed_secs = (int) (elapsed_us / G_USEC_PER_SEC);
      int idx = page->running_game_idx;
      if (idx >= 0 && idx < page->n_games)
        {
          page->games[idx].play_time_secs += elapsed_secs;
          page->games[idx].last_played = g_get_real_time () / G_USEC_PER_SEC;
          float fps = parse_mangohud_fps ();
          if (fps > 0)
            page->games[idx].avg_fps = fps;
        }

      page->running_game_idx = -1;
      page->game_pid = 0;

      save_games_config (page);
      refresh_recent_games (page);
    }
}

gboolean
gamemode_available (void)
{
  static gint cached = -1;
  if (cached < 0)
    cached = g_file_test ("/usr/bin/gamemoderun", G_FILE_TEST_EXISTS) ? 1 : 0;
  return (gboolean) cached;
}

gboolean
mangohud_available (void)
{
  return g_find_program_in_path ("mangohud") != NULL;
}

void
on_game_launch_clicked (GtkButton *btn, DashboardPage *page)
{
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "game-idx")) - 1;
  if (idx < 0 || idx >= page->n_games) return;
  if (page->running_game_idx >= 0) return;

  const char *cmd = page->games[idx].command;
  if (!cmd || cmd[0] == '\0') return;

  /* Minimize the window — expected behaviour when launching a game */
  {
    GList *windows = gtk_application_get_windows (GTK_APPLICATION (g_application_get_default ()));
    if (windows)
      gtk_window_minimize (GTK_WINDOW (windows->data));
  }

  page->running_game_idx = idx;
  page->game_start_us = g_get_real_time ();
  page->is_steam_game = (g_strcmp0 (page->games[idx].source, "steam") == 0);
  page->steam_watch_id = 0;
  page->steam_watch_idle_secs = 0;

  /* For Steam games with mangohud: shut down the running Steam daemon first so
     the new Steam process inherits mangohud's LD_PRELOAD and propagates it to
     the game child.  The shutdown is done synchronously here, NOT chained into
     the launch shell, to avoid process-group confusion. */
  if (page->is_steam_game
      && page->games[idx].mangohud && mangohud_available ())
    {
      /* Ignore errors — Steam may not be running, which is fine */
      g_spawn_sync (NULL,
        (char *[]) { "/bin/sh", "-c",
          "steam -shutdown 2>/dev/null; sleep 1", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
    }

  GError *err = NULL;
  GPid child_pid = 0;

  /* Determine working directory */
  gchar *workdir = NULL;
  if (page->games[idx].workdir && page->games[idx].workdir[0])
    workdir = g_strdup (page->games[idx].workdir);
  else
    {
      gchar **argv0 = NULL;
      if (g_shell_parse_argv (cmd, NULL, &argv0, NULL))
        workdir = g_path_get_dirname (argv0[0]);
      g_strfreev (argv0);
    }

  /* Build launch command with optional startup_cmd, gamemode, and VRS */
  gboolean use_shell = FALSE;
  GString *launch = g_string_new ("");

  /* Check VRS setting from epinephrine.conf for RADV */
  {
    char *cfg_path = g_build_filename (g_get_user_config_dir (),
      "epinephrine", "epinephrine.conf", NULL);
    GKeyFile *kf = g_key_file_new ();
    if (g_key_file_load_from_file (kf, cfg_path, G_KEY_FILE_NONE, NULL))
      {
        gboolean vrs_on = g_key_file_get_boolean (kf, "gpu", "vrs_enabled", NULL);
        if (vrs_on)
          {
            char *vrs_rate = g_key_file_get_string (kf, "gpu", "vrs_rate", NULL);
            const char *rate = vrs_rate ? vrs_rate : "2x2";
            g_string_append_printf (launch, "RADV_FORCE_VRS=%s ", rate);
            g_free (vrs_rate);
            use_shell = TRUE;
          }
      }
    g_key_file_free (kf);
    g_free (cfg_path);
  }

  if (page->games[idx].startup_cmd && page->games[idx].startup_cmd[0])
    {
      g_string_append_printf (launch, "%s && ", page->games[idx].startup_cmd);
      use_shell = TRUE;
    }
  if (page->games[idx].feral_gamemode && gamemode_available ())
    {
      g_string_append (launch, "gamemoderun ");
      use_shell = TRUE;
    }
  if (page->games[idx].mangohud && mangohud_available ())
    {
      /* Steam shutdown was already done synchronously above — just add mangohud */
      g_string_append (launch, "mangohud ");
      use_shell = TRUE;
    }
  g_string_append (launch, cmd);

  if (use_shell)
    {
      const char *sh_argv[] = { "/bin/sh", "-c", launch->str, NULL };
      g_spawn_async (workdir, (char **) sh_argv, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &child_pid, &err);
    }
  else
    {
      gchar **argv = NULL;
      g_shell_parse_argv (cmd, NULL, &argv, &err);
      if (argv)
        {
          g_spawn_async (workdir, argv, NULL,
            G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
            NULL, NULL, &child_pid, &err);
          g_strfreev (argv);
        }
    }

  g_string_free (launch, TRUE);
  g_free (workdir);

  if (err)
    {
      g_warning ("Failed to launch game: %s", err->message);
      g_error_free (err);
      page->running_game_idx = -1;
      return;
    }

  page->game_pid = child_pid;
  g_child_watch_add (child_pid, on_steam_exit, page);
}

GtkWidget *
create_game_icon_widget (const char *icon, int pixel_size)
{
  GtkWidget *img;
  if (icon && (icon[0] == '/' || icon[0] == '~'))
    {
      char *path = (icon[0] == '~')
        ? g_build_filename (g_get_home_dir (), icon + 1, NULL)
        : g_strdup (icon);
      img = gtk_image_new_from_file (path);
      g_free (path);
    }
  else
    img = gtk_image_new_from_icon_name (
      icon && icon[0] ? icon : "input-gaming-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (img), pixel_size);
  return img;
}

static void
on_delete_game_clicked (GtkButton *btn, gpointer user_data)
{
  (void) user_data;
  DashboardPage *page = g_object_get_data (G_OBJECT (btn), "page");
  if (!page) return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "game-idx")) - 1;
  AdwDialog *dialog = g_object_get_data (G_OBJECT (btn), "dialog");
  if (!dialog) return;

  if (idx < 0 || idx >= page->n_games) return;

  g_free (page->games[idx].name);
  g_free (page->games[idx].command);
  g_free (page->games[idx].icon);
  g_free (page->games[idx].workdir);
  g_free (page->games[idx].source);
  g_free (page->games[idx].startup_cmd);

  for (int i = idx; i < page->n_games - 1; i++)
    page->games[i] = page->games[i + 1];

  page->n_games--;
  if (page->n_games > 0)
    {
      /* Zero out the old tail to prevent shared pointers with the last shifted element */
      memset (&page->games[page->n_games], 0, sizeof (GameShortcut));
    }
  else
    {
      g_free (page->games);
      page->games = NULL;
    }

  save_games_config (page);
  refresh_games_grid (page);
  refresh_recent_games (page);
  adw_dialog_close (dialog);
  dashboard_show_toast ("Game removed");
}

static void
on_browse_file_callback (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkWidget *entry = GTK_WIDGET (user_data);
  GtkFileDialog *dlg = GTK_FILE_DIALOG (source);
  GError *err = NULL;
  GFile *file = gtk_file_dialog_open_finish (dlg, result, &err);
  if (file)
    {
      char *path = g_file_get_path (file);
      if (path)
        {
          gtk_editable_set_text (GTK_EDITABLE (entry), path);
          g_free (path);
        }
      g_object_unref (file);
    }
  else if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_warning ("Error selecting file: %s", err->message);
      g_error_free (err);
    }
  g_object_unref (dlg);
}

static void
on_browse_folder_callback (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkWidget *entry = GTK_WIDGET (user_data);
  GtkFileDialog *dlg = GTK_FILE_DIALOG (source);
  GError *err = NULL;
  GFile *file = gtk_file_dialog_select_folder_finish (dlg, result, &err);
  if (file)
    {
      char *path = g_file_get_path (file);
      if (path)
        {
          gtk_editable_set_text (GTK_EDITABLE (entry), path);
          g_free (path);
        }
      g_object_unref (file);
    }
  else if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_warning ("Error selecting folder: %s", err->message);
      g_error_free (err);
    }
  g_object_unref (dlg);
}

void
on_browse_folder_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  GtkWidget *entry = GTK_WIDGET (user_data);
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_select_folder (dlg, NULL, NULL,
    on_browse_folder_callback, entry);
}

void
on_browse_file_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  GtkWidget *entry = GTK_WIDGET (user_data);
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_open (dlg, NULL, NULL,
    on_browse_file_callback, entry);
}

static void
on_edit_confirm (GtkButton *btn, gpointer user_data)
{
  (void) user_data;

  GtkWidget *icon_entry = g_object_get_data (G_OBJECT (btn), "icon-entry");
  GtkWidget *name_entry = g_object_get_data (G_OBJECT (btn), "name-entry");
  GtkWidget *cmd_entry = g_object_get_data (G_OBJECT (btn), "cmd-entry");
  GtkWidget *workdir_entry = g_object_get_data (G_OBJECT (btn), "workdir-entry");
  GtkDropDown *source_drop = g_object_get_data (G_OBJECT (btn), "source-drop");
  GtkWidget *startup_entry = g_object_get_data (G_OBJECT (btn), "startup-entry");
  GtkWidget *gamemode_switch = g_object_get_data (G_OBJECT (btn), "gamemode-switch");
  GtkWidget *mangohud_switch = g_object_get_data (G_OBJECT (btn), "mangohud-switch");
  AdwDialog *dialog = g_object_get_data (G_OBJECT (btn), "dialog");
  DashboardPage *page = g_object_get_data (G_OBJECT (btn), "page");
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "game-idx")) - 1;

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

  g_free (page->games[idx].name);
  g_free (page->games[idx].command);
  g_free (page->games[idx].icon);
  g_free (page->games[idx].workdir);
  g_free (page->games[idx].source);
  g_free (page->games[idx].startup_cmd);

  page->games[idx].name = g_strdup (name);
  page->games[idx].command = g_strdup (cmd);
  page->games[idx].icon = (icon_text && icon_text[0] != '\0')
    ? g_strdup (icon_text)
    : g_strdup ("input-gaming-symbolic");
  page->games[idx].workdir = (workdir && workdir[0] != '\0')
    ? g_strdup (workdir)
    : NULL;
  page->games[idx].source = g_strdup (source);
  page->games[idx].startup_cmd = (startup_text && startup_text[0] != '\0')
    ? g_strdup (startup_text) : NULL;
  page->games[idx].feral_gamemode = gamemode_switch ?
    gtk_switch_get_active (GTK_SWITCH (gamemode_switch)) : FALSE;
  page->games[idx].mangohud = mangohud_switch ?
    gtk_switch_get_active (GTK_SWITCH (mangohud_switch)) : FALSE;

  save_games_config (page);
  refresh_games_grid (page);
  refresh_recent_games (page);
  adw_dialog_close (dialog);
}

static void
edit_source_factory_setup (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
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
edit_source_factory_bind (GtkListItemFactory *factory, GtkListItem *item, gpointer user_data)
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
  gtk_label_set_text (GTK_LABEL (lbl), pos < 6 ? names[pos] : "?");
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

void
open_edit_dialog (DashboardPage *page, int idx)
{
  if (idx < 0 || idx >= page->n_games) return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, g_dgettext (GETTEXT_PACKAGE, "Edit Game"));
  adw_dialog_set_content_width (dialog, 750);
  adw_dialog_set_content_height (dialog, 480);

  /* Main horizontal box: left 30% / right 70% */
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_start (hbox, 24);
  gtk_widget_set_margin_end (hbox, 24);
  gtk_widget_set_margin_top (hbox, 16);
  gtk_widget_set_margin_bottom (hbox, 16);

  /* ---- LEFT COLUMN (30%) ---- */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request (left, 200, -1);
  gtk_widget_set_hexpand (left, FALSE);
  gtk_box_append (GTK_BOX (hbox), left);

  /* Game icon */
  GtkWidget *icon_img = create_game_icon_widget (page->games[idx].icon, 80);
  gtk_widget_set_halign (icon_img, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (left), icon_img);

  /* Play time */
  char *time_str = format_play_time (page->games[idx].play_time_secs);
  char *time_label_text = g_strdup_printf ("%s: %s",
    g_dgettext (GETTEXT_PACKAGE, "Play Time"), time_str);
  GtkWidget *time_label = gtk_label_new (time_label_text);
  gtk_widget_set_halign (time_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (time_label, "dim-label");
  gtk_box_append (GTK_BOX (left), time_label);
  g_free (time_str);
  g_free (time_label_text);

  /* FPS */
  char *fps_str = g_strdup_printf ("%.1f FPS", page->games[idx].avg_fps);
  GtkWidget *fps_label = gtk_label_new (fps_str);
  gtk_widget_set_halign (fps_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (fps_label, "dim-label");
  gtk_box_append (GTK_BOX (left), fps_label);
  g_free (fps_str);

  /* Play button */
  GtkWidget *play_btn = gtk_button_new_with_label (g_dgettext (GETTEXT_PACKAGE, "Play"));
  gtk_widget_add_css_class (play_btn, "suggested-action");
  gtk_widget_set_halign (play_btn, GTK_ALIGN_CENTER);
  g_object_set_data (G_OBJECT (play_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_object_set_data (G_OBJECT (play_btn), "dialog", dialog);
  g_signal_connect (play_btn, "clicked", G_CALLBACK (on_game_launch_clicked), page);
  gtk_box_append (GTK_BOX (left), play_btn);

  /* ---- RIGHT COLUMN (70%) ---- */
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand (right, TRUE);
  gtk_box_append (GTK_BOX (hbox), right);

  /* Name */
  GtkWidget *name_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Game Name"));
  gtk_widget_set_halign (name_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (name_label, "dim-label");
  gtk_box_append (GTK_BOX (right), name_label);
  GtkWidget *name_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (name_entry),
    page->games[idx].name ? page->games[idx].name : "Unnamed");
  gtk_widget_set_hexpand (name_entry, TRUE);
  gtk_box_append (GTK_BOX (right), name_entry);

  /* Source */
  GtkWidget *source_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Source / Platform"));
  gtk_widget_set_halign (source_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (source_label, "dim-label");
  gtk_box_append (GTK_BOX (right), source_label);
  const char *sources[] = { "Local", "Steam", "Heroic", "Lutris", "Bottles", "Other", NULL };
  GtkStringList *source_list = gtk_string_list_new (sources);
  GtkDropDown *source_drop = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (source_list), NULL));
  g_object_unref (source_list);
  {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (edit_source_factory_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (edit_source_factory_bind), NULL);
    gtk_drop_down_set_factory (source_drop, factory);
    g_object_unref (factory);
  }
  /* Map current source to index */
  static const char *source_keys[] = { "local", "steam", "heroic", "lutris", "bottles", "other" };
  guint src_sel = 0;
  for (int s = 0; s < 6; s++)
    {
      if (g_strcmp0 (page->games[idx].source, source_keys[s]) == 0)
        { src_sel = s; break; }
    }
  gtk_drop_down_set_selected (source_drop, src_sel);
  gtk_widget_set_hexpand (GTK_WIDGET (source_drop), TRUE);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (source_drop));

  /* Command */
  GtkWidget *cmd_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Launch Command"));
  gtk_widget_set_halign (cmd_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (cmd_label, "dim-label");
  gtk_box_append (GTK_BOX (right), cmd_label);
  GtkWidget *cmd_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (cmd_hbox, TRUE);
  GtkWidget *cmd_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (cmd_entry), page->games[idx].command);
  gtk_widget_set_hexpand (cmd_entry, TRUE);
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_entry);
  GtkWidget *cmd_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (cmd_browse, 36, -1);
  gtk_widget_add_css_class (cmd_browse, "flat");
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_browse);
  gtk_box_append (GTK_BOX (right), cmd_hbox);

  /* Icon */
  GtkWidget *icon_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Icon (name or path)"));
  gtk_widget_set_halign (icon_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (icon_label, "dim-label");
  gtk_box_append (GTK_BOX (right), icon_label);
  GtkWidget *icon_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (icon_hbox, TRUE);
  GtkWidget *icon_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (icon_entry),
    page->games[idx].icon ? page->games[idx].icon : "");
  gtk_widget_set_hexpand (icon_entry, TRUE);
  gtk_box_append (GTK_BOX (icon_hbox), icon_entry);
  GtkWidget *icon_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (icon_browse, 36, -1);
  gtk_widget_add_css_class (icon_browse, "flat");
  gtk_box_append (GTK_BOX (icon_hbox), icon_browse);
  gtk_box_append (GTK_BOX (right), icon_hbox);

  /* Workdir */
  GtkWidget *workdir_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Working Directory"));
  gtk_widget_set_halign (workdir_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (workdir_label, "dim-label");
  gtk_box_append (GTK_BOX (right), workdir_label);
  GtkWidget *workdir_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (workdir_hbox, TRUE);
  GtkWidget *workdir_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (workdir_entry),
    page->games[idx].workdir ? page->games[idx].workdir : "");
  gtk_widget_set_hexpand (workdir_entry, TRUE);
  gtk_box_append (GTK_BOX (workdir_hbox), workdir_entry);
  GtkWidget *workdir_browse = gtk_button_new_with_label ("...");
  gtk_widget_set_size_request (workdir_browse, 36, -1);
  gtk_widget_add_css_class (workdir_browse, "flat");
  gtk_box_append (GTK_BOX (workdir_hbox), workdir_browse);
  gtk_box_append (GTK_BOX (right), workdir_hbox);

  /* Browse button callbacks */
  g_signal_connect (workdir_browse, "clicked", G_CALLBACK (on_browse_folder_clicked), workdir_entry);
  g_signal_connect (cmd_browse, "clicked", G_CALLBACK (on_browse_file_clicked), cmd_entry);
  g_signal_connect (icon_browse, "clicked", G_CALLBACK (on_browse_file_clicked), icon_entry);

  /* ---- Tweaks ---- */
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (sep, 8);
  gtk_widget_set_margin_bottom (sep, 8);
  gtk_box_append (GTK_BOX (right), sep);

  GtkWidget *tweaks_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Tweaks"));
  gtk_widget_set_halign (tweaks_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (tweaks_label, "heading");
  gtk_box_append (GTK_BOX (right), tweaks_label);

  GtkWidget *startup_label = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Startup commands"));
  gtk_widget_set_halign (startup_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (startup_label, "dim-label");
  gtk_box_append (GTK_BOX (right), startup_label);
  GtkWidget *startup_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (startup_entry),
    g_dgettext (GETTEXT_PACKAGE, "e.g. gamemoderun %command%"));
  if (page->games[idx].startup_cmd)
    gtk_editable_set_text (GTK_EDITABLE (startup_entry), page->games[idx].startup_cmd);
  gtk_widget_set_hexpand (startup_entry, TRUE);
  gtk_box_append (GTK_BOX (right), startup_entry);

  GtkWidget *gamemode_switch = NULL;
  if (gamemode_available ())
    {
      GtkWidget *gamemode_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_top (gamemode_row, 4);
      GtkWidget *gamemode_lbl = gtk_label_new (
        g_dgettext (GETTEXT_PACKAGE, "Enable Feral GameMode"));
      gtk_widget_set_halign (gamemode_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand (gamemode_lbl, TRUE);
      gtk_box_append (GTK_BOX (gamemode_row), gamemode_lbl);
      gamemode_switch = gtk_switch_new ();
      gtk_switch_set_active (GTK_SWITCH (gamemode_switch), page->games[idx].feral_gamemode);
      gtk_widget_set_halign (gamemode_switch, GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (gamemode_row), gamemode_switch);
      gtk_box_append (GTK_BOX (right), gamemode_row);
    }

  GtkWidget *mangohud_switch = NULL;
  if (mangohud_available ())
    {
      GtkWidget *mangohud_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_set_margin_top (mangohud_row, 4);
      GtkWidget *mangohud_lbl = gtk_label_new (
        g_dgettext (GETTEXT_PACKAGE, "Enable MangoHud"));
      gtk_widget_set_halign (mangohud_lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand (mangohud_lbl, TRUE);
      gtk_box_append (GTK_BOX (mangohud_row), mangohud_lbl);
      mangohud_switch = gtk_switch_new ();
      gtk_switch_set_active (GTK_SWITCH (mangohud_switch), page->games[idx].mangohud);
      gtk_widget_set_halign (mangohud_switch, GTK_ALIGN_END);
      gtk_box_append (GTK_BOX (mangohud_row), mangohud_switch);
      gtk_box_append (GTK_BOX (right), mangohud_row);
    }

  /* ---- BOTTOM BUTTONS ---- */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top (btn_box, 8);

  GtkWidget *cancel_btn = gtk_button_new_with_label (g_dgettext (GETTEXT_PACKAGE, "Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), cancel_btn);

  GtkWidget *delete_btn = gtk_button_new_with_label (g_dgettext (GETTEXT_PACKAGE, "Delete"));
  gtk_widget_add_css_class (delete_btn, "destructive-action");
  g_object_set_data (G_OBJECT (delete_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_object_set_data (G_OBJECT (delete_btn), "page", page);
  g_object_set_data (G_OBJECT (delete_btn), "dialog", dialog);
  g_signal_connect (delete_btn, "clicked", G_CALLBACK (on_delete_game_clicked), page);
  gtk_box_append (GTK_BOX (btn_box), delete_btn);

  GtkWidget *save_btn = gtk_button_new_with_label (g_dgettext (GETTEXT_PACKAGE, "Save"));
  gtk_widget_add_css_class (save_btn, "suggested-action");
  gtk_box_append (GTK_BOX (btn_box), save_btn);

  gtk_box_append (GTK_BOX (right), btn_box);

  /* Set child and wire save button */
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (vbox), hbox);
  adw_dialog_set_child (dialog, vbox);

  g_object_set_data (G_OBJECT (save_btn), "icon-entry", icon_entry);
  g_object_set_data (G_OBJECT (save_btn), "name-entry", name_entry);
  g_object_set_data (G_OBJECT (save_btn), "cmd-entry", cmd_entry);
  g_object_set_data (G_OBJECT (save_btn), "workdir-entry", workdir_entry);
  g_object_set_data (G_OBJECT (save_btn), "source-drop", source_drop);
  g_object_set_data (G_OBJECT (save_btn), "startup-entry", startup_entry);
  if (gamemode_switch)
    g_object_set_data (G_OBJECT (save_btn), "gamemode-switch", gamemode_switch);
  if (mangohud_switch)
    g_object_set_data (G_OBJECT (save_btn), "mangohud-switch", mangohud_switch);
  g_object_set_data (G_OBJECT (save_btn), "dialog", dialog);
  g_object_set_data (G_OBJECT (save_btn), "page", page);
  g_object_set_data (G_OBJECT (save_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (save_btn, "clicked", G_CALLBACK (on_edit_confirm), page);

  adw_dialog_present (dialog, GTK_WIDGET (page->box));
}

/* ---- Import Games ---- */

static void
add_game_entry (DashboardPage *page, const char *name, const char *cmd,
                const char *icon, const char *source)
{
  for (int i = 0; i < page->n_games; i++)
    {
      if (g_strcmp0 (page->games[i].command, cmd) == 0)
        return;
    }

  page->n_games++;
  page->games = g_realloc (page->games,
    page->n_games * sizeof (GameShortcut));

  GameShortcut *g = &page->games[page->n_games - 1];
  g->name = g_strdup (name);
  g->command = g_strdup (cmd);
  g->icon = g_strdup (icon ? icon : "input-gaming-symbolic");
  g->workdir = NULL;
  g->source = g_strdup (source ? source : "local");
  g->startup_cmd = NULL;
  g->feral_gamemode = FALSE;
  g->mangohud = FALSE;
  g->play_time_secs = 0;
  g->avg_fps = 0;
  g->last_played = 0;
}

static gboolean
is_steam_non_game (const char *name)
{
  if (!name) return FALSE;
  if (g_str_has_prefix (name, "Steam Linux Runtime")) return TRUE;
  if (g_str_has_prefix (name, "Proton")) return TRUE;
  if (g_str_has_prefix (name, "Steamworks Common")) return TRUE;
  return FALSE;
}

int
import_steam_games (DashboardPage *page)
{
  int count = 0;
  const char *home = g_get_home_dir ();

  /* Collect all steamapps directories to scan */
  GPtrArray *steamapps_dirs = g_ptr_array_new_with_free_func (g_free);

  /* 1. Parse libraryfolders.vdf for Steam library paths */
  char *libraryfolders = g_build_filename (home, ".steam", "steam", "steamapps", "libraryfolders.vdf", NULL);
  char *vdf_content = NULL;
  if (g_file_get_contents (libraryfolders, &vdf_content, NULL, NULL))
    {
      /* Extract "path" "..." lines */
      char **lines = g_strsplit (vdf_content, "\n", -1);
      for (int i = 0; lines[i]; i++)
        {
          const char *p = strstr (lines[i], "\"path\"");
          if (!p) continue;
          p = strchr (p + 6, '"');
          if (!p) continue;
          p++;
          const char *end = strchr (p, '"');
          if (!end) continue;
          int len = end - p;
          char *path = g_strndup (p, len);
          char *steamapps = g_build_filename (path, "steamapps", NULL);
          g_ptr_array_add (steamapps_dirs, steamapps);
          g_free (path);
        }
      g_strfreev (lines);
    }
  g_free (vdf_content);
  g_free (libraryfolders);

  /* 2. Also check ~/.steam/steam/steamapps as fallback (default library) */
  char *default_sa = g_build_filename (home, ".steam", "steam", "steamapps", NULL);
  gboolean found_default = FALSE;
  for (guint i = 0; i < steamapps_dirs->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (steamapps_dirs, i), default_sa) == 0)
        { found_default = TRUE; break; }
    }
  if (!found_default)
    g_ptr_array_add (steamapps_dirs, default_sa);
  else
    g_free (default_sa);

  /* 3. Also check ~/.local/share/Steam/steamapps */
  char *local_sa = g_build_filename (home, ".local", "share", "Steam", "steamapps", NULL);
  gboolean found_local = FALSE;
  for (guint i = 0; i < steamapps_dirs->len; i++)
    {
      if (g_strcmp0 (g_ptr_array_index (steamapps_dirs, i), local_sa) == 0)
        { found_local = TRUE; break; }
    }
  if (!found_local)
    g_ptr_array_add (steamapps_dirs, local_sa);
  else
    g_free (local_sa);

  /* 4. Scan each steamapps directory for appmanifest_*.acf files */
  for (guint p = 0; p < steamapps_dirs->len; p++)
    {
      const char *sa_dir = g_ptr_array_index (steamapps_dirs, p);

      GDir *dir = g_dir_open (sa_dir, 0, NULL);
      if (!dir) continue;

      const char *entry;
      while ((entry = g_dir_read_name (dir)) != NULL)
        {
          if (!g_str_has_prefix (entry, "appmanifest_") || !g_str_has_suffix (entry, ".acf"))
            continue;

          char *full_path = g_build_filename (sa_dir, entry, NULL);

          /* Extract game name from ACF file */
          char *acf_content = NULL;
          char *appname = NULL;
          if (g_file_get_contents (full_path, &acf_content, NULL, NULL))
            {
              /* Parse: "name"    "GameName" */
              const char *np = strstr (acf_content, "\"name\"");
              if (np)
                {
                  np = strchr (np + 6, '"');
                  if (np)
                    {
                      np++;
                      const char *end = strchr (np, '"');
                      if (end)
                        appname = g_strndup (np, end - np);
                    }
                }
            }

          if (appname)
            {
              if (is_steam_non_game (appname))
                {
                  g_free (appname);
                  g_free (acf_content);
                  g_free (full_path);
                  continue;
                }
              /* Extract app ID from filename: appmanifest_<ID>.acf */
              const char *appid_start = entry + 12; /* skip "appmanifest_" */
              int appid_len = (int)(strlen (entry) - 12 - 4); /* - ".acf" */
              char *launch_cmd = g_strdup_printf ("steam steam://rungameid/%.*s", appid_len, appid_start);
              char *icon_name = g_strdup_printf ("steam_icon_%.*s", appid_len, appid_start);
              add_game_entry (page, appname, launch_cmd, icon_name, "steam");
              count++;
              g_free (icon_name);
              g_free (launch_cmd);
              g_free (appname);
            }
          g_free (acf_content);
          g_free (full_path);
        }
      g_dir_close (dir);
    }

  g_ptr_array_unref (steamapps_dirs);
  return count;
}

int
import_bottles_games (DashboardPage *page)
{
  int count = 0;
  const char *home = g_get_home_dir ();

  /* Possible Bottles data directories */
  const char *bottles_candidates[] = {
    ".var/app/org.usebottles.bottles/data/bottles/bottles",
    ".local/share/bottles/bottles",
    NULL
  };

  for (int c = 0; bottles_candidates[c]; c++)
    {
      char *bottles_dir = g_build_filename (home, bottles_candidates[c], NULL);
      if (!g_file_test (bottles_dir, G_FILE_TEST_IS_DIR))
        {
          g_free (bottles_dir);
          continue;
        }

      GDir *dir = g_dir_open (bottles_dir, 0, NULL);
      if (!dir)
        {
          g_free (bottles_dir);
          continue;
        }

      const char *bottle_name;
      while ((bottle_name = g_dir_read_name (dir)))
        {
          char *bottle_path = g_build_filename (bottles_dir, bottle_name, NULL);
          if (!g_file_test (bottle_path, G_FILE_TEST_IS_DIR))
            {
              g_free (bottle_path);
              continue;
            }

          /* Try programs.yml (Bottles < 51) */
          char *prog_yml = g_build_filename (bottle_path, "programs.yml", NULL);
          if (g_file_test (prog_yml, G_FILE_TEST_EXISTS))
            {
              char *prog_content = NULL;
              if (g_file_get_contents (prog_yml, &prog_content, NULL, NULL))
                {
                  char **lines = g_strsplit (prog_content, "\n", -1);
                  g_free (prog_content);
                  for (int i = 0; lines[i]; i++)
                    {
                      g_strstrip (lines[i]);
                      if (g_str_has_prefix (lines[i], "name:"))
                        {
                          char *prog_name = g_strstrip (lines[i] + 5);
                          if (prog_name[0])
                            {
                              char *launch_cmd = g_strdup_printf (
                                "bottles-cli run -b \"%s\" -p \"%s\"", bottle_name, prog_name);
                              add_game_entry (page, prog_name, launch_cmd,
                                NULL, "bottles");
                              count++;
                              g_free (launch_cmd);
                            }
                        }
                    }
                  g_strfreev (lines);
                }
            }
          g_free (prog_yml);

          /* Try programs/ directory (Bottles >= 51) */
          char *progs_dir = g_build_filename (bottle_path, "programs", NULL);
          if (g_file_test (progs_dir, G_FILE_TEST_IS_DIR))
            {
              GDir *prog_dir = g_dir_open (progs_dir, 0, NULL);
              if (prog_dir)
                {
                  const char *prog_entry;
                  while ((prog_entry = g_dir_read_name (prog_dir)))
                    {
                      if (!g_str_has_suffix (prog_entry, ".yml")) continue;
                      char *prog_file = g_build_filename (progs_dir, prog_entry, NULL);
                      char *content = NULL;
                      if (g_file_get_contents (prog_file, &content, NULL, NULL))
                        {
                          char **plines = g_strsplit (content, "\n", -1);
                          g_free (content);
                          char *prog_name = NULL;
                          for (int j = 0; plines[j]; j++)
                            {
                              g_strstrip (plines[j]);
                              if (g_str_has_prefix (plines[j], "name:"))
                                {
                                  prog_name = g_strstrip (plines[j] + 5);
                                  break;
                                }
                            }
                          if (prog_name && prog_name[0])
                            {
                              char *launch_cmd = g_strdup_printf (
                                "bottles-cli run -b \"%s\" -p \"%s\"", bottle_name, prog_name);
                              add_game_entry (page, prog_name, launch_cmd,
                                NULL, "bottles");
                              count++;
                              g_free (launch_cmd);
                            }
                          g_strfreev (plines);
                        }
                      g_free (prog_file);
                    }
                  g_dir_close (prog_dir);
                }
            }
          g_free (progs_dir);
          g_free (bottle_path);
        }
      g_dir_close (dir);
      g_free (bottles_dir);
    }
  return count;
}

int
import_heroic_games (DashboardPage *page)
{
  int count = 0;
  const char *home = g_get_home_dir ();
  char *heroic_dir = g_build_filename (home, ".config", "heroic", "games", NULL);

  GDir *dir = g_dir_open (heroic_dir, 0, NULL);
  if (!dir)
    {
      g_free (heroic_dir);
      return 0;
    }

  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      char *game_json = g_build_filename (heroic_dir, entry, NULL);
      char *content = NULL;
      char *game_name = NULL;
      if (g_file_get_contents (game_json, &content, NULL, NULL))
        {
          /* Parse JSON "app_name":"..." */
          const char *np = strstr (content, "\"app_name\"");
          if (np)
            {
              np = strchr (np + 10, '"');
              if (np)
                {
                  np++;
                  const char *end = strchr (np, '"');
                  if (end)
                    game_name = g_strndup (np, end - np);
                }
            }
        }

      if (game_name)
        {
          char *launch_cmd = g_strdup_printf ("heroic launch \"%s\"", game_name);
          add_game_entry (page, game_name, launch_cmd, "heroic-icon", "heroic");
          count++;
          g_free (launch_cmd);
          g_free (game_name);
        }
      g_free (content);
      g_free (game_json);
    }

  g_dir_close (dir);
  g_free (heroic_dir);
  return count;
}

int
import_lutris_games (DashboardPage *page)
{
  int count = 0;
  const char *home = g_get_home_dir ();
  char *lutris_dir = g_build_filename (home, ".local", "share", "lutris", "games", NULL);

  GDir *dir = g_dir_open (lutris_dir, 0, NULL);
  if (!dir)
    {
      g_free (lutris_dir);
      return 0;
    }

  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    {
      char *game_dir = g_build_filename (lutris_dir, entry, NULL);
      if (!g_file_test (game_dir, G_FILE_TEST_IS_DIR))
        { g_free (game_dir); continue; }

      char *game_yml = g_build_filename (game_dir, "game.yml", NULL);
      char *yml_content = NULL;
      char *game_name = NULL;
      if (g_file_get_contents (game_yml, &yml_content, NULL, NULL))
        {
          /* Parse "name: GameName" */
          char **lines = g_strsplit (yml_content, "\n", -1);
          for (int j = 0; lines[j]; j++)
            {
              if (g_str_has_prefix (lines[j], "name:"))
                {
                  const char *n = lines[j] + 5;
                  while (*n == ' ') n++;
                  if (*n)
                    { game_name = g_strdup (n); break; }
                }
            }
          g_strfreev (lines);
        }

      if (!game_name)
        game_name = g_strdup (entry);

      char *launch_cmd = g_strdup_printf ("lutris -L \"%s\"", game_name);
      add_game_entry (page, game_name, launch_cmd, "lutris-icon", "lutris");
      count++;
      g_free (launch_cmd);
      g_free (game_name);
      g_free (game_yml);
      g_free (game_dir);
    }

  g_dir_close (dir);
  g_free (lutris_dir);
  return count;
}

/* ---- Install ---- */

static gboolean
restore_install_btn (gpointer data)
{
  DashboardPage *page = data;
  for (int i = 0; i < DASHBOARD_N_RESOURCES; i++)
    if (gtk_widget_get_visible (GTK_WIDGET (page->resources[i].progress)))
      {
        gtk_widget_set_visible (page->resources[i].install_btn, TRUE);
        gtk_widget_set_sensitive (page->resources[i].install_btn, TRUE);
        gtk_widget_set_visible (GTK_WIDGET (page->resources[i].progress), FALSE);
      }
  return G_SOURCE_REMOVE;
}

static const char *
install_cmd_for_idx (int idx)
{
  if (!detected_pm) return NULL;
  const char *pkg = RESOURCES[idx].pkg;
  if (!pkg) return NULL;

  static char buf[1024];
  if (strcmp (detected_pm, "apt") == 0)
    g_snprintf (buf, sizeof (buf), "apt install -y %s", pkg);
  else if (strcmp (detected_pm, "dnf") == 0)
    g_snprintf (buf, sizeof (buf), "dnf install -y %s", pkg);
  else if (strcmp (detected_pm, "pacman") == 0)
    g_snprintf (buf, sizeof (buf), "pacman -S --noconfirm %s", pkg);
  else if (strcmp (detected_pm, "zypper") == 0)
    g_snprintf (buf, sizeof (buf), "zypper install -y %s", pkg);
  else if (strcmp (detected_pm, "xbps") == 0)
    g_snprintf (buf, sizeof (buf), "xbps-install -y %s", pkg);
  else
    return NULL;

  return buf;
}

static void
on_install_clicked (GtkButton *btn, gpointer data)
{
  DashboardPage *page = (DashboardPage *) data;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "res-idx"));

  const char *cmd = install_cmd_for_idx (idx);
  if (!cmd) return;

  gtk_widget_set_visible (GTK_WIDGET (btn), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (page->resources[idx].progress), TRUE);
  gtk_progress_bar_set_fraction (page->resources[idx].progress, 0.0);
  adw_action_row_set_subtitle (page->resources[idx].row,
    g_dgettext (GETTEXT_PACKAGE, "Installing..."));

  epi_privilege_exec_async (cmd);

  /* Restore button after a short delay (we can't track completion) */
  g_timeout_add (5000, restore_install_btn, page);
}

/* ---- Card builders ---- */

static GtkWidget *
create_card (const char *title, AdwPreferencesGroup **out_group)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start (card, 12);
  gtk_widget_set_margin_end (card, 12);
  gtk_widget_set_margin_top (card, 12);
  gtk_widget_set_margin_bottom (card, 12);
  gtk_widget_set_size_request (card, 400, 200);
  gtk_widget_add_css_class (card, "dashboard-card");
  gtk_widget_set_hexpand (card, TRUE);

  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), title);
  gtk_box_append (GTK_BOX (card), group);

  if (out_group)
    *out_group = ADW_PREFERENCES_GROUP (group);

  return card;
}

/* ---- Thread-safe data gathering ---- */

typedef struct
{
  char cpu[256];
  char ram[256];
  char gpu[256];
  char vram[256];
  char vulkan[256];
  char driver[512];
  char storage[8][512];
  gboolean storage_ssd[8];
  int n_storage;
  gboolean resource_installed[DASHBOARD_N_RESOURCES];

  /* Gaming score fields */
  int cpu_cores;
  int cpu_threads;
  float cpu_freq_ghz;
  int cpu_year;
  float gpu_vram_gb;
  int gpu_year;
  int ram_gb;
  int ram_speed_mhz;
  int ram_ddr;        /* 4, 5, etc. 0 = unknown */
  int monitor_w;
  int monitor_h;
  gboolean score_valid;
} GatherResult;

/* ---- Gaming Score Calculation ---- */

/*
 * Gaming Score (0.0 – 11.0)
 *
 * Reference system (baseline = 5.5/11):
 *   CPU: 8 cores, 8 threads, 2.5 GHz, 2020+
 *   GPU: 6 GB VRAM, 2020+
 *   RAM: 16 GB DDR4, 2400 MHz+
 *   Monitor: 1920×1080, 16:9
 *
 * Year factor: hardware from 2020+ = 1.0, each year before subtracts 0.1
 *   down to a minimum of 0.4 (7-year defasagem).
 *
 * A system with 1.5× the reference specs scores 11.0 (max).
 */
static float
calculate_gaming_score (const GatherResult *r)
{
  if (!r->score_valid)
    return -1.0f;

  /* Year factor: 2020+ → 1.0, each year before → -0.1, min 0.4 */
  float year_factor_cpu = 1.0f;
  float year_factor_gpu = 1.0f;

  if (r->cpu_year > 0 && r->cpu_year < 2020)
    {
      year_factor_cpu = 1.0f - (2020 - r->cpu_year) * 0.1f;
      if (year_factor_cpu < 0.4f) year_factor_cpu = 0.4f;
    }
  else if (r->cpu_year >= 2020)
    year_factor_cpu = 1.0f;

  if (r->gpu_year > 0 && r->gpu_year < 2020)
    {
      year_factor_gpu = 1.0f - (2020 - r->gpu_year) * 0.1f;
      if (year_factor_gpu < 0.4f) year_factor_gpu = 0.4f;
    }
  else if (r->gpu_year >= 2020)
    year_factor_gpu = 1.0f;

  /* CPU score (0 – 3.0) */
  float cpu_cores_norm  = (float) r->cpu_cores / 8.0f;
  float cpu_threads_norm = (float) r->cpu_threads / 8.0f;
  float cpu_freq_norm   = r->cpu_freq_ghz / 2.5f;
  float cpu_raw = (cpu_cores_norm * 1.0f + cpu_threads_norm * 0.5f + cpu_freq_norm * 1.0f)
                  * year_factor_cpu;
  if (cpu_raw > 3.0f) cpu_raw = 3.0f;
  if (cpu_raw < 0.3f && r->cpu_cores >= 1) cpu_raw = 0.3f;

  /* GPU score (0 – 3.0) */
  float gpu_vram_norm = r->gpu_vram_gb / 6.0f;
  float gpu_raw = (gpu_vram_norm * 2.0f + year_factor_gpu * 1.0f);
  if (gpu_raw > 3.0f) gpu_raw = 3.0f;
  if (gpu_raw < 0.2f && r->gpu_vram_gb > 0) gpu_raw = 0.2f;

  /* RAM score (0 – 2.5) */
  float ram_gb_norm = (float) r->ram_gb / 16.0f;
  float ram_spd_norm = (r->ram_speed_mhz > 0) ? (float) r->ram_speed_mhz / 2400.0f : 0.5f;
  float ddr_bonus = 0.0f;
  if (r->ram_ddr >= 5) ddr_bonus = 0.5f;
  else if (r->ram_ddr >= 4) ddr_bonus = 0.4f;
  else if (r->ram_ddr >= 3) ddr_bonus = 0.2f;
  else if (r->ram_ddr >= 2) ddr_bonus = 0.1f;
  float ram_raw = ram_gb_norm * 1.5f + ram_spd_norm * 0.5f + ddr_bonus;
  if (ram_raw > 2.5f) ram_raw = 2.5f;
  if (ram_raw < 0.2f && r->ram_gb >= 1) ram_raw = 0.2f;

  /* Monitor score (0 – 2.5) */
  float res_factor = 0.5f;
  int pixels = r->monitor_w * r->monitor_h;
  if (pixels >= 3840 * 2160) res_factor = 1.5f;
  else if (pixels >= 2560 * 1440) res_factor = 1.25f;
  else if (pixels >= 1920 * 1080) res_factor = 1.0f;
  else if (pixels >= 1280 * 720) res_factor = 0.7f;
  else res_factor = 0.4f;

  float aspect_factor = 0.5f;
  if (r->monitor_w > 0 && r->monitor_h > 0)
    {
      float ratio = (float) r->monitor_w / (float) r->monitor_h;
      if (ratio >= 1.7f && ratio <= 1.8f) aspect_factor = 1.0f;
      else if (ratio >= 1.55f && ratio <= 1.65f) aspect_factor = 0.9f;
      else if (ratio >= 2.2f && ratio <= 2.4f) aspect_factor = 0.85f;
      else if (ratio >= 1.2f && ratio <= 1.4f) aspect_factor = 0.6f;
      else aspect_factor = 0.5f;
    }
  float mon_raw = res_factor * 1.5f + aspect_factor * 1.0f;
  if (mon_raw > 2.5f) mon_raw = 2.5f;

  float total = cpu_raw + gpu_raw + ram_raw + mon_raw;
  if (total > 11.0f) total = 11.0f;
  if (total < 0.0f) total = 0.0f;

  return total;
}

static GatherResult *
gather_all (void)
{
  GatherResult *r = g_new0 (GatherResult, 1);
  char *tmp;

  tmp = RUN_SH_STATIC ("grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->cpu, tmp, sizeof (r->cpu)); g_free (tmp); }
  else     g_strlcpy (r->cpu, "N/A", sizeof (r->cpu));

  tmp = RUN_SH_STATIC ("awk '/MemTotal/{printf \"%.0f MB (%.1f GB)\", $2, $2/1024/1024}' /proc/meminfo");
  if (tmp) { g_strlcpy (r->ram, tmp, sizeof (r->ram)); g_free (tmp); }
  else     g_strlcpy (r->ram, "N/A", sizeof (r->ram));

  tmp = RUN_SH_STATIC ("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | sed 's/.*: //'");
  if (!tmp) tmp = RUN_SH_STATIC ("glxinfo 2>/dev/null | grep -i 'OpenGL renderer' | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->gpu, tmp, sizeof (r->gpu)); g_free (tmp); }
  else     g_strlcpy (r->gpu, "N/A", sizeof (r->gpu));

  tmp = RUN_SH_STATIC ("cat /sys/class/drm/*/device/mem_info_vram_total 2>/dev/null | head -1 | awk '{printf \"%.0f MB\", $1/1024/1024}'");
  if (!tmp)
    tmp = RUN_SH_STATIC ("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | awk '{printf \"%.0f MB\", $1}')");
  if (tmp) { g_strlcpy (r->vram, tmp, sizeof (r->vram)); g_free (tmp); }
  else     g_strlcpy (r->vram, "N/A", sizeof (r->vram));

  tmp = RUN_SH_STATIC ("vulkaninfo 2>/dev/null | grep -i 'Vulkan Instance Version' | head -1 | sed 's/.*: //'");
  if (!tmp) tmp = RUN_SH_STATIC ("glxinfo 2>/dev/null | grep -i 'OpenGL version' | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->vulkan, tmp, sizeof (r->vulkan)); g_free (tmp); }
  else     g_strlcpy (r->vulkan, "N/A", sizeof (r->vulkan));

  tmp = RUN_SH_STATIC ("lspci -v 2>/dev/null | grep -iE 'vga|3d|display' -A 20 | grep -i 'Kernel driver' | head -1 | sed 's/.*: //'");
  if (tmp)
    {
      char *drv_name = tmp;
      char *drv_ver = RUN_SH_STATIC ("modinfo $(lspci -v 2>/dev/null | grep -iE 'vga|3d|display' -A 20 | grep -i 'Kernel driver' | head -1 | sed 's/.*: //') 2>/dev/null | grep -m1 '^version:' | sed 's/.*: //'");
      if (!drv_ver || drv_ver[0] == '\0')
        {
          g_free (drv_ver);
          drv_ver = RUN_SH_STATIC ("modinfo $(lspci -v 2>/dev/null | grep -iE 'vga|3d|display' -A 20 | grep -i 'Kernel driver' | head -1 | sed 's/.*: //') 2>/dev/null | grep 'vermagic:' | head -1 | sed 's/.*: //' | awk '{print $1}'");
        }
      if (drv_ver && drv_ver[0] != '\0')
        g_snprintf (r->driver, sizeof (r->driver), "%s (%s)", drv_name, drv_ver);
      else
        g_strlcpy (r->driver, drv_name, sizeof (r->driver));
      g_free (drv_ver);
      g_free (drv_name);
    }
  else
    g_strlcpy (r->driver, "N/A", sizeof (r->driver));

  /* ---- Gaming score: detailed hardware detection ---- */
  r->score_valid = TRUE;

  /* CPU: cores, threads, frequency */
  {
    char *nproc = RUN_SH_STATIC ("nproc");
    if (nproc) { r->cpu_cores = atoi (nproc); g_free (nproc); }
    if (r->cpu_cores < 1) r->cpu_cores = 1;
  }
  {
    char *threads = RUN_SH_STATIC ("lscpu 2>/dev/null | grep -i 'Thread(s) per core' | awk '{print $NF}'");
    int tpc = threads ? atoi (threads) : 1;
    g_free (threads);
    if (tpc < 1) tpc = 1;
    r->cpu_threads = r->cpu_cores * tpc;
  }
  {
    char *freq = RUN_SH_STATIC ("lscpu 2>/dev/null | grep -i 'CPU MHz' | awk '{print $NF}'");
    if (!freq) freq = RUN_SH_STATIC ("grep -m1 'cpu MHz' /proc/cpuinfo | awk -F: '{print $2}'");
    if (freq) { r->cpu_freq_ghz = atof (freq) / 1000.0f; g_free (freq); }
    if (r->cpu_freq_ghz < 0.5f) r->cpu_freq_ghz = 2.5f;
  }
  /* CPU year from model name */
  {
    char *model = RUN_SH_STATIC ("grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'");
    r->cpu_year = 0;
    if (model)
      {
        /* Try to extract year from common patterns:
         * AMD Ryzen 5 4600G → 4xxx → 2020 (4600 - 4000 = 600, +2020 = 2020)
         * AMD Ryzen 7 5800X → 5xxx → 2020
         * Intel Core i7-10700K → 10xxx → 2020 (10700 → 2020)
         * Intel Core i7-14700K → 14xxx → 2024
         * Intel Core i5-12400 → 12xxx → 2021
         * Intel Core 2 Duo E8400 → older, 0
         */
        int model_num = 0;
        if (sscanf (model, "%*s %*s %*s %*s %d", &model_num) < 1)
          sscanf (model, "%*s %*s %*s %*s %*s %d", &model_num);
        if (model_num > 0)
          {
            if (model_num >= 10000)
              {
                /* Intel 10th gen+ (10xxx, 11xxx, 12xxx, 13xxx, 14xxx) */
                int gen = model_num / 1000;
                r->cpu_year = 2019 + gen; /* 10→2020, 11→2021, 12→2021, 13→2022, 14→2023 */
                if (r->cpu_year < 2020) r->cpu_year = 2020;
              }
            else if (model_num >= 2000 && model_num < 10000)
              {
                /* AMD Ryzen 1xxx-5xxx or Intel 1xxx-9xxx */
                int first_digit = model_num / 1000;
                if (model_num >= 3000 && model_num < 6000)
                  {
                    /* AMD Ryzen: 1xxx=2017, 2xxx=2018, 3xxx=2019, 4xxx=2020, 5xxx=2022 */
                    int ryzen_gen = first_digit;
                    if (ryzen_gen <= 1) r->cpu_year = 2017;
                    else if (ryzen_gen == 2) r->cpu_year = 2018;
                    else if (ryzen_gen == 3) r->cpu_year = 2019;
                    else if (ryzen_gen == 4) r->cpu_year = 2020;
                    else r->cpu_year = 2022;
                  }
                else if (model_num >= 2000 && model_num < 3000)
                  {
                    /* AMD FX or Intel 2xxx-9xxx */
                    if (first_digit >= 8) r->cpu_year = 2017 + (first_digit - 8);
                    else r->cpu_year = 2010 + first_digit;
                  }
                else
                  r->cpu_year = 2010 + first_digit;
              }
            else
              r->cpu_year = 0;
          }
        g_free (model);
      }
  }

  /* GPU: VRAM in GB and year */
  {
    char *vram_kb = RUN_SH_STATIC ("cat /sys/class/drm/*/device/mem_info_vram_total 2>/dev/null | head -1");
    if (vram_kb)
      {
        float kb = (float) atof (vram_kb);
        r->gpu_vram_gb = kb / 1024.0f / 1024.0f;
        g_free (vram_kb);
      }
    /* NVIDIA drivers don't expose mem_info_vram_total; fall back to
       nvidia-smi (memory.total is reported in MiB). */
    if (r->gpu_vram_gb < 0.5f)
      {
        char *nv = RUN_SH_STATIC ("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1");
        if (nv)
          {
            float mib = (float) atof (nv);
            if (mib > 0) r->gpu_vram_gb = mib / 1024.0f;
            g_free (nv);
          }
      }
    if (r->gpu_vram_gb < 0.5f) r->gpu_vram_gb = 0;
  }
  {
    char *gpu_name = RUN_SH_STATIC ("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | sed 's/.*: //'");
    r->gpu_year = 0;
    if (gpu_name)
      {
        /* Try to extract GPU year from name */
        if (strstr (gpu_name, "RDNA 3") || strstr (gpu_name, "RX 7"))
          r->gpu_year = 2022;
        else if (strstr (gpu_name, "RDNA 2") || strstr (gpu_name, "RX 6"))
          r->gpu_year = 2020;
        else if (strstr (gpu_name, "RDNA") || strstr (gpu_name, "RX 5"))
          r->gpu_year = 2019;
        else if (strstr (gpu_name, "Polaris") || strstr (gpu_name, "RX 4") || strstr (gpu_name, "RX 580") || strstr (gpu_name, "RX 570"))
          r->gpu_year = 2016;
        else if (strstr (gpu_name, "Navi") || strstr (gpu_name, "5500") || strstr (gpu_name, "5600") || strstr (gpu_name, "5700"))
          r->gpu_year = 2019;
        else if (strstr (gpu_name, "Arc") || strstr (gpu_name, "A3") || strstr (gpu_name, "A5") || strstr (gpu_name, "A7"))
          r->gpu_year = 2022;
        else if (strstr (gpu_name, "RTX 4") || strstr (gpu_name, "RTX 30"))
          r->gpu_year = 2020;
        else if (strstr (gpu_name, "RTX 2") || strstr (gpu_name, "GTX 16"))
          r->gpu_year = 2018;
        else if (strstr (gpu_name, "GTX 10"))
          r->gpu_year = 2016;
        else if (strstr (gpu_name, "GTX 9"))
          r->gpu_year = 2014;
        else if (strstr (gpu_name, "GTX 7"))
          r->gpu_year = 2013;
        else if (strstr (gpu_name, "GTX 6"))
          r->gpu_year = 2012;
        else if (strstr (gpu_name, "GT 7") || strstr (gpu_name, "GT 6") || strstr (gpu_name, "GT 5"))
          r->gpu_year = 2011;
        else
          r->gpu_year = 2015;
        g_free (gpu_name);
      }
  }

  /* RAM: amount in GB */
  {
    char *mem = RUN_SH_STATIC ("awk '/MemTotal/{printf \"%.0f\", $2/1024/1024}' /proc/meminfo");
    if (mem) { r->ram_gb = atoi (mem); g_free (mem); }
    if (r->ram_gb < 1) r->ram_gb = 0;
  }

  /* RAM: speed and DDR type via dmidecode */
  {
    char *spd = RUN_SH_STATIC ("dmidecode -t memory 2>/dev/null | grep -i 'Configured Memory Speed' | head -1 | awk '{print $NF}' | tr -d 'MHz'");
    if (spd)
      {
        r->ram_speed_mhz = atoi (spd);
        g_free (spd);
      }
    if (r->ram_speed_mhz < 100) r->ram_speed_mhz = 0;
  }
  {
    char *ddr = RUN_SH_STATIC ("dmidecode -t memory 2>/dev/null | grep -i 'Type:' | head -1 | awk '{print $2}'");
    r->ram_ddr = 0;
    if (ddr)
      {
        if (g_str_has_prefix (ddr, "DDR5")) r->ram_ddr = 5;
        else if (g_str_has_prefix (ddr, "DDR4")) r->ram_ddr = 4;
        else if (g_str_has_prefix (ddr, "DDR3")) r->ram_ddr = 3;
        else if (g_str_has_prefix (ddr, "DDR2")) r->ram_ddr = 2;
        else if (g_str_has_prefix (ddr, "DDR")) r->ram_ddr = 1;
        g_free (ddr);
      }
  }
  /* Fallback: try sysfs for RAM speed */
  if (r->ram_speed_mhz < 100)
    {
      char *spd2 = RUN_SH_STATIC ("cat /sys/devices/system/edac/mc/mc*/dimm*/freq 2>/dev/null | head -1");
      if (spd2)
        {
          r->ram_speed_mhz = atoi (spd2);
          g_free (spd2);
        }
    }
  /* Fallback: try lspci for DDR info */
  if (r->ram_ddr == 0)
    {
      char *ddr2 = RUN_SH_STATIC ("dmidecode -t memory 2>/dev/null | grep -i 'Memory Device' -A 5 | grep -i 'Type:' | head -1 | awk '{print $2}'");
      if (ddr2)
        {
          if (g_str_has_prefix (ddr2, "DDR5")) r->ram_ddr = 5;
          else if (g_str_has_prefix (ddr2, "DDR4")) r->ram_ddr = 4;
          else if (g_str_has_prefix (ddr2, "DDR3")) r->ram_ddr = 3;
          g_free (ddr2);
        }
    }

  /* Monitor: resolution */
  {
    char *res = RUN_SH_STATIC ("xrandr 2>/dev/null | grep ' connected' | grep -oE '[0-9]+x[0-9]+' | head -1");
    if (!res)
      res = RUN_SH_STATIC ("kscreen-doctor 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | head -1");
    if (res)
      {
        sscanf (res, "%dx%d", &r->monitor_w, &r->monitor_h);
        g_free (res);
      }
    /* Fallback: try xdpyinfo */
    if (r->monitor_w < 100 || r->monitor_h < 100)
      {
        char *dim = RUN_SH_STATIC ("xdpyinfo 2>/dev/null | grep dimensions | awk '{print $2}'");
        if (dim)
          {
            sscanf (dim, "%dx%d", &r->monitor_w, &r->monitor_h);
            g_free (dim);
          }
      }
    /* Final fallback: assume 1080p but don't invalidate the whole score,
       so systems where resolution can't be detected (e.g. Wayland without
       xrandr/kscreen-doctor) still get a partial score. */
    if (r->monitor_w < 100 || r->monitor_h < 100)
      {
        r->monitor_w = 1920;
        r->monitor_h = 1080;
      }
  }

  /* Validate that we have enough core data to calculate a score.
     VRAM and monitor resolution are optional: the GPU and monitor
     sub-scores degrade gracefully when they are unknown (e.g. NVIDIA
     drivers that don't expose mem_info_vram_total). */
  if (r->cpu_cores < 1 || r->ram_gb < 1)
    r->score_valid = FALSE;

  /* Detect storage devices */
  char *lsblk = RUN_SH_STATIC ("lsblk -d -n -o NAME,SIZE 2>/dev/null | grep -E '^[a-z]'");
  if (lsblk)
    {
      char **lines = g_strsplit (lsblk, "\n", -1);
      r->n_storage = 0;
      for (int i = 0; lines[i] && r->n_storage < 8; i++)
        {
          g_strstrip (lines[i]);
          if (lines[i][0] == '\0') continue;

          char dev[32] = {0};
          char size[64] = {0};
          if (sscanf (lines[i], "%31s %63[^\n]", dev, size) < 2) continue;

          /* Skip zram / loop */
          if (g_str_has_prefix (dev, "zram") || g_str_has_prefix (dev, "loop")) continue;

          /* Check if device is rotational (HDD) or non-rotational (SSD/NVMe) */
          char *rot_path = g_strdup_printf ("/sys/block/%s/queue/rotational", dev);
          char *rot_content = NULL;
          if (g_file_get_contents (rot_path, &rot_content, NULL, NULL))
            r->storage_ssd[r->n_storage] = (rot_content && rot_content[0] == '0');
          else
            r->storage_ssd[r->n_storage] = FALSE;
          g_free (rot_content);
          g_free (rot_path);

          /* Get disk usage via lsblk + df */
          char *lsblk_dev_out = NULL;
          char *lsblk_cmd = g_strdup_printf ("/dev/%s", dev);
          g_spawn_sync (NULL,
            (char *[]){ "lsblk", lsblk_cmd, "-nlo", "NAME,MOUNTPOINT", NULL },
            NULL, G_SPAWN_DEFAULT, NULL, NULL, &lsblk_dev_out, NULL, NULL, NULL);
          g_free (lsblk_cmd);
          char *used = NULL;
          if (lsblk_dev_out)
            {
              /* Find first partition with a mountpoint */
              char **part_lines = g_strsplit (lsblk_dev_out, "\n", -1);
              for (int pi = 0; part_lines[pi]; pi++)
                {
                  g_strstrip (part_lines[pi]);
                  char part_name[64] = {0}, mnt[256] = {0};
                  if (sscanf (part_lines[pi], "%63s %255[^\n]", part_name, mnt) >= 2
                      && part_name[0] && mnt[0])
                    {
                      char *df_out = NULL;
                      g_spawn_sync (NULL,
                        (char *[]){ "df", "--block-size=1G", part_name, NULL },
                        NULL, G_SPAWN_DEFAULT, NULL, NULL, &df_out, NULL, NULL, NULL);
                      if (df_out)
                        {
                          /* Parse "Filesystem 1G-blocks Used Available Use% Mounted on" */
                          char **df_lines = g_strsplit (df_out, "\n", -1);
                          if (df_lines[0] && df_lines[1])
                            {
                              char used_gb[32] = {0}, total_gb[32] = {0}, pct_str[16] = {0};
                              // Use df --output=size,used,pcent for reliable parsing
                              g_free (df_out);
                              g_strfreev (df_lines);
                              g_spawn_sync (NULL,
                                (char *[]){ "df", "--block-size=1G", "--output=size,used,pcent", part_name, NULL },
                                NULL, G_SPAWN_DEFAULT, NULL, NULL, &df_out, NULL, NULL, NULL);
                              if (df_out)
                                {
                                  df_lines = g_strsplit (df_out, "\n", -1);
                                  if (df_lines[0] && df_lines[1])
                                    {
                                      if (sscanf (df_lines[1], "%31s %31s %15s", total_gb, used_gb, pct_str) >= 2)
                                        used = g_strdup_printf ("Total: %s GB — Used: %s GB (%s)", total_gb, used_gb, pct_str);
                                    }
                                  g_strfreev (df_lines);
                                }
                            }
                          else
                            g_strfreev (df_lines);
                        }
                      g_free (df_out);
                      break;
                    }
                }
              g_strfreev (part_lines);
            }
          g_free (lsblk_dev_out);

          if (used && used[0] != '\0')
            {
              char used_gb[32] = {0}, total_gb[32] = {0}, pct[16] = {0};
              if (sscanf (used, "%31[^,],%31[^,],%15s", used_gb, total_gb, pct) >= 2)
                g_snprintf (r->storage[r->n_storage], sizeof (r->storage[0]),
                  "Total: %s GB — Used: %s GB (%s)", total_gb, used_gb, pct);
              else
                g_snprintf (r->storage[r->n_storage], sizeof (r->storage[0]),
                  "/dev/%s %s", dev, size);
            }
          else
            g_snprintf (r->storage[r->n_storage], sizeof (r->storage[0]),
              "/dev/%s %s", dev, size);
          r->n_storage++;

          g_free (used);
        }
      g_strfreev (lines);
      g_free (lsblk);
    }

  for (int i = 0; i < DASHBOARD_N_RESOURCES; i++)
    {
      gboolean installed = check_pkg_native (RESOURCES[i].pkg)
                        || check_flatpak_app (RESOURCES[i].flatpak_id)
                        || check_snap_pkg (RESOURCES[i].pkg)
                        || check_linglong_app (RESOURCES[i].flatpak_id)
                        || check_flatpak_binary (RESOURCES[i].flatpak_id)
                        || check_binary (RESOURCES[i].pkg)
                        || (RESOURCES[i].binary_name && check_binary (RESOURCES[i].binary_name))
                        || check_appimage (RESOURCES[i].appimage_name);

      /* Feral GameMode: also check via gamemode_available() */
      if (i == 7 && !installed)
        installed = gamemode_available ();

      /* Heroic: also try alternate package names */
      if (i == 1 && !installed)
        {
          installed = check_pkg_native ("heroic-games-launcher")
                   || check_pkg_native ("heroic-games-launcher-bin");
        }

      r->resource_installed[i] = installed;
    }

  return r;
}

typedef struct
{
  DashboardPage *page;
  GatherResult *result;
} GatherCbData;

typedef struct
{
  DashboardPage *page;
  float score;
} ScoreDisplayData;

static gboolean
show_gaming_score (gpointer data)
{
  ScoreDisplayData *sd = (ScoreDisplayData *) data;
  DashboardPage *page = sd->page;

  gtk_spinner_stop (GTK_SPINNER (page->gaming_score_spinner));
  gtk_widget_set_visible (page->gaming_score_spinner, FALSE);

  if (sd->score < 0.0f)
    {
      /* Score couldn't be calculated — hide module and show toast */
      gtk_widget_set_visible (page->gaming_score_box, FALSE);
      dashboard_show_toast ("Hardware couldn't be properly identified for calculations. Try again later.");
    }
  else
    {
      char buf[32];
      g_snprintf (buf, sizeof (buf), "%.1f/11", sd->score);
      gtk_label_set_text (GTK_LABEL (page->gaming_score_label), buf);
      gtk_widget_set_visible (page->gaming_score_label, TRUE);
    }

  g_free (sd);
  return G_SOURCE_REMOVE;
}

static gboolean
apply_gathered_data (gpointer data)
{
  GatherCbData *gd = (GatherCbData *) data;
  DashboardPage *page = gd->page;
  GatherResult *r = gd->result;

  adw_action_row_set_subtitle (page->cpu_row,    r->cpu);
  adw_action_row_set_subtitle (page->ram_row,    r->ram);
  adw_action_row_set_subtitle (page->gpu_row,    r->gpu);
  adw_action_row_set_subtitle (page->vram_row,   r->vram);
  adw_action_row_set_subtitle (page->vulkan_row, r->vulkan);
  adw_action_row_set_subtitle (page->driver_row, r->driver);

  /* Add storage rows */
  for (int i = 0; i < r->n_storage; i++)
    {
      page->storage_rows[i] = ADW_ACTION_ROW (adw_action_row_new ());
      g_object_set (page->storage_rows[i],
                    "title", r->storage_ssd[i] ? "SSD" : "HDD",
                    "icon-name", r->storage_ssd[i] ? "ssd-symbolic" : "harddisk-symbolic",
                    "subtitle", r->storage[i], NULL);
      adw_preferences_group_add (page->hw_group, GTK_WIDGET (page->storage_rows[i]));
    }
  page->n_storage_rows = r->n_storage;

  for (int i = 0; i < DASHBOARD_N_RESOURCES; i++)
    {
      page->resources[i].installed = r->resource_installed[i];
      if (r->resource_installed[i])
        {
          gtk_widget_set_visible (page->resources[i].install_btn, FALSE);
          gtk_widget_set_visible (GTK_WIDGET (page->resources[i].progress), FALSE);
          gtk_widget_set_visible (page->resources[i].status_icon, TRUE);
          gtk_widget_set_visible (page->resources[i].installed_label, TRUE);
          adw_action_row_set_subtitle (page->resources[i].row, g_dgettext (GETTEXT_PACKAGE, "Installed"));
        }
      else
        {
          gtk_widget_set_visible (page->resources[i].install_btn, TRUE);
          gtk_widget_set_visible (GTK_WIDGET (page->resources[i].progress), FALSE);
          gtk_widget_set_visible (page->resources[i].status_icon, FALSE);
          gtk_widget_set_visible (page->resources[i].installed_label, FALSE);
          adw_action_row_set_subtitle (page->resources[i].row, g_dgettext (GETTEXT_PACKAGE, "Not installed"));
        }
    }

  gtk_widget_set_visible (page->spinner, FALSE);
  gtk_widget_set_visible (page->scroll, TRUE);

  /* Gaming Score: calculate and show with fake loading */
  float score = calculate_gaming_score (r);
  gtk_widget_set_visible (page->gaming_score_box, TRUE);
  gtk_widget_set_visible (page->gaming_score_spinner, TRUE);
  gtk_widget_set_visible (page->gaming_score_label, FALSE);
  gtk_spinner_start (GTK_SPINNER (page->gaming_score_spinner));

  ScoreDisplayData *sd = g_new0 (ScoreDisplayData, 1);
  sd->page = page;
  sd->score = score;
  g_timeout_add_seconds (1, show_gaming_score, sd);

  g_free (r);
  g_free (gd);
  return G_SOURCE_REMOVE;
}

static gpointer
gather_thread (gpointer data)
{
  DashboardPage *page = (DashboardPage *) data;
  GatherResult *r = gather_all ();

  GatherCbData *gd = g_new0 (GatherCbData, 1);
  gd->page = page;
  gd->result = r;

  g_idle_add (apply_gathered_data, gd);
  return NULL;
}

static void
on_see_more_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn; (void) user_data;
  dashboard_navigate_to_games ();
}

/* ---- Public API ---- */

DashboardPage *
dashboard_page_new (void)
{
  DashboardPage *page = g_new0 (DashboardPage, 1);

  page->running_game_idx = -1;

  init_config_path ();

  GtkBuilder *builder = gtk_builder_new_from_resource (
    "/io/github/mayrinck/epinephrine/ui/page-dashboard.ui");

  page->box      = GTK_WIDGET (gtk_builder_get_object (builder, "dashboard-page"));
  page->spinner  = GTK_WIDGET (gtk_builder_get_object (builder, "dash-spinner"));
  page->scroll   = GTK_WIDGET (gtk_builder_get_object (builder, "dash-scroll"));
  page->flowbox  = GTK_FLOW_BOX (gtk_builder_get_object (builder, "dash-flowbox"));
  page->recent_box = GTK_WIDGET (gtk_builder_get_object (builder, "recent-games-box"));
  page->gaming_score_box = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-box"));
  page->gaming_score_label = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-label"));
  page->gaming_score_spinner = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-spinner"));

  /* See more button -> navigate to Games tab */
  GtkWidget *see_more_btn = GTK_WIDGET (gtk_builder_get_object (builder, "see-more-button"));
  g_signal_connect (see_more_btn, "clicked", G_CALLBACK (on_see_more_clicked), NULL);

  gtk_spinner_start (GTK_SPINNER (page->spinner));

  detected_pm = detect_pm ();

  /* Load games config */
  load_games_config (page);
  refresh_recent_games (page);

  /* Hardware card */
  AdwPreferencesGroup *hw_group;
  GtkWidget *hw_card = create_card ("Hardware Summary", &hw_group);
  page->hw_group = hw_group;

  page->cpu_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->cpu_row, "title", "CPU", "icon-name", "cpu-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->cpu_row));

  page->ram_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->ram_row, "title", "RAM", "icon-name", "memory-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->ram_row));

  page->gpu_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->gpu_row, "title", "GPU", "icon-name", "pci-card-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->gpu_row));

  page->vram_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->vram_row, "title", "VRAM", "icon-name", "memory-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->vram_row));

  page->vulkan_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->vulkan_row, "title", "Vulkan / OpenGL", "icon-name", "resource-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->vulkan_row));

  page->driver_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->driver_row, "title", "Driver", "icon-name", "executable-symbolic", NULL);
  adw_preferences_group_add (hw_group, GTK_WIDGET (page->driver_row));

  gtk_flow_box_insert (page->flowbox, hw_card, -1);

  /* Gaming resources card */
  AdwPreferencesGroup *gr_group;
  GtkWidget *gr_card = create_card ("Gaming Components", &gr_group);

  for (int i = 0; i < DASHBOARD_N_RESOURCES; i++)
    {
      page->resources[i].row = ADW_ACTION_ROW (adw_action_row_new ());
      g_object_set (page->resources[i].row, "title", RESOURCES[i].name, NULL);
      adw_preferences_group_add (gr_group, GTK_WIDGET (page->resources[i].row));

      /* Prefix: launcher icon from resource or icon name */
      GtkWidget *pix = NULL;
      if (RESOURCES[i].resource_icon)
        {
          pix = gtk_image_new_from_resource (RESOURCES[i].resource_icon);
          gtk_image_set_pixel_size (GTK_IMAGE (pix), 24);
        }
      else if (RESOURCES[i].icon_name)
        {
          pix = gtk_image_new_from_icon_name (RESOURCES[i].icon_name);
          gtk_image_set_pixel_size (GTK_IMAGE (pix), 24);
        }
      if (pix)
        {
          adw_action_row_add_prefix (page->resources[i].row, pix);
          page->resources[i].prefix_icon = pix;
        }

      /* Suffix: right side hbox — spacer pushes everything to the far right */
      GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_widget_set_valign (hbox, GTK_ALIGN_CENTER);

      GtkWidget *spacer = gtk_label_new ("");
      gtk_widget_set_hexpand (spacer, TRUE);
      gtk_box_append (GTK_BOX (hbox), spacer);

      /* Everything below sits at the far right edge, same spot as the install button */

      /* Green checkmark (visible only when installed) */
      GtkWidget *chk = gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (chk), 16);
      gtk_widget_add_css_class (chk, "status-ok");
      gtk_box_append (GTK_BOX (hbox), chk);
      page->resources[i].status_icon = chk;

      /* Green "Installed" label (visible only when installed) */
      GtkWidget *inst_lbl = gtk_label_new (g_dgettext (GETTEXT_PACKAGE, "Installed"));
      gtk_widget_add_css_class (inst_lbl, "status-ok");
      gtk_box_append (GTK_BOX (hbox), inst_lbl);
      page->resources[i].installed_label = inst_lbl;

      /* Progress bar (hidden by default, shown during install) */
      GtkWidget *pbar = gtk_progress_bar_new ();
      gtk_widget_set_size_request (pbar, 100, -1);
      gtk_widget_set_halign (pbar, GTK_ALIGN_CENTER);
      gtk_widget_set_visible (pbar, FALSE);
      gtk_box_append (GTK_BOX (hbox), pbar);
      page->resources[i].progress = GTK_PROGRESS_BAR (pbar);

      /* Install button (visible only when not installed) */
      GtkWidget *ibtn = gtk_button_new_with_label (g_dgettext (GETTEXT_PACKAGE, "Install"));
      gtk_widget_add_css_class (ibtn, "suggested-action");
      gtk_widget_set_valign (ibtn, GTK_ALIGN_CENTER);
      g_object_set_data (G_OBJECT (ibtn), "res-idx", GINT_TO_POINTER (i));
      g_signal_connect (ibtn, "clicked", G_CALLBACK (on_install_clicked), page);
      gtk_box_append (GTK_BOX (hbox), ibtn);
      page->resources[i].install_btn = ibtn;

      adw_action_row_add_suffix (page->resources[i].row, hbox);

      /* Initially hide installed-state widgets (apply_gathered_data will fix) */
      gtk_widget_set_visible (chk, FALSE);
      gtk_widget_set_visible (inst_lbl, FALSE);
    }

  gtk_flow_box_insert (page->flowbox, gr_card, -1);

  /* Show spinner, hide scroll until data is ready */
  gtk_widget_set_visible (page->scroll, FALSE);

  GThread *scan_thread = g_thread_new ("hw-scan", gather_thread, page);
  g_thread_unref (scan_thread);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
