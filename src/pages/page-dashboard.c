#include "page-dashboard.h"
#include "page-games.h"
#include "window.h"
#include "epi-privilege.h"
#include "../epi-i18n.h"
#include "../hydra-ldb.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define tr(x) epi_i18n_get (x)

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
    return g_strdup (tr ("Never played"));

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 diff = now - last_played;

  if (diff < 0) return g_strdup (tr ("Just now"));
  if (diff < 60)        return g_strdup (tr ("Just now"));
  if (diff < 3600)      return g_strdup_printf (tr ("%d min ago"), (int)(diff / 60));
  if (diff < 86400)     return g_strdup_printf (tr ("%d hours ago"), (int)(diff / 3600));
  if (diff < 604800)    return g_strdup_printf (tr ("%d days ago"), (int)(diff / 86400));
  if (diff < 2592000)   return g_strdup_printf (tr ("%d weeks ago"), (int)(diff / 604800));
  if (diff < 31536000)  return g_strdup_printf (tr ("%d months ago"), (int)(diff / 2592000));
  return g_strdup_printf (tr ("%d years ago"), (int)(diff / 31536000));
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
      g_free (page->games[i].group);
    }
  g_free (page->games);
  page->games = NULL;
  page->n_games = 0;
  g_free (page->selected);
  page->selected = NULL;
  page->edit_mode = FALSE;
  g_clear_pointer (&page->active_group, g_free);
  g_clear_pointer (&page->active_service, g_free);
}

const char *
detect_source_from_command (const char *cmd)
{
  if (!cmd || cmd[0] == '\0')
    return "local";
  if (g_str_has_prefix (cmd, "steam steam://rungameid/"))
    return "steam";
  if (g_str_has_prefix (cmd, "xdg-open hydralauncher://"))
    return "hydra";
  if (g_str_has_prefix (cmd, "heroic "))
    return "heroic";
  if (g_str_has_prefix (cmd, "lutris "))
    return "lutris";
  if (g_str_has_prefix (cmd, "bottles"))
    return "bottles";
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
  page->selected = g_new0 (gboolean, num_games);
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
              page->games[page->n_games].group = g_key_file_get_string (keyfile, groups[i], "group", NULL);
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
      if (page->games[i].group)
        g_key_file_set_string (keyfile, group, "group", page->games[i].group);
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
    ? page->games[idx].name : tr ("?"));
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
    ? tr ("Running...")
    : tr ("Play Now");
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
    {
      cached = 0;
      if (g_find_program_in_path ("gamemoderun"))
        cached = 1;
      else if (g_file_test ("/usr/bin/gamemoderun", G_FILE_TEST_EXISTS))
        cached = 1;
      else if (g_file_test ("/usr/local/bin/gamemoderun", G_FILE_TEST_EXISTS))
        cached = 1;
      else if (g_find_program_in_path ("gamemoded"))
        cached = 1;
    }
  return (gboolean) cached;
}

gboolean
mangohud_available (void)
{
  return g_find_program_in_path ("mangohud") != NULL;
}

/* ---- Winetricks / Protontricks (Wine prefix tools) ----
 *
 * Protontricks only understands Steam prefixes (it deliberately ignores
 * WINEPREFIX and reads Steam's config to locate them).  The other launchers
 * (Heroic, Hydra, Lutris, Bottles) manage their own Wine prefixes, so for
 * those we run winetricks directly against the per-game prefix — each
 * launcher "in its own way".
 */

gboolean
winetricks_available (void)
{
  static gint cached = -1;
  if (cached < 0)
    cached = (g_find_program_in_path ("winetricks") != NULL) ? 1 : 0;
  return (gboolean) cached;
}

gboolean
protontricks_available (void)
{
  static gint cached = -1;
  if (cached >= 0) return (gboolean) cached;

  cached = 0;
  if (g_find_program_in_path ("protontricks"))
    {
      /* Native protontricks still needs a winetricks on PATH to run commands. */
      cached = winetricks_available () ? 1 : 0;
      return (gboolean) cached;
    }

  /* Flatpak installation bundles its own winetricks, so only the app matters. */
  if (g_find_program_in_path ("flatpak"))
    {
      GError *err = NULL;
      gint status = 0;
      g_spawn_sync (NULL,
        (char *[]) { "flatpak", "info", "com.github.Matoking.protontricks", NULL },
        NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, &err);
      if (err)
        g_error_free (err);
      else if (status == 0)
        cached = 1;
    }
  return (gboolean) cached;
}

static char *
extract_steam_appid (const char *cmd)
{
  if (!cmd || !cmd[0]) return NULL;

  const char *p = strstr (cmd, "rungameid/");
  if (!p) p = strstr (cmd, "launch/");
  if (!p) return NULL;

  p = strchr (p, '/');
  if (!p) return NULL;
  p++;

  const char *start = p;
  while (g_ascii_isdigit (*p)) p++;
  if (p == start) return NULL;

  return g_strndup (start, (gsize) (p - start));
}

/* ---- Launcher key extraction (from the stored launch command) ---- */

static char *
extract_heroic_app_name (const char *cmd)
{
  gchar **argv = NULL;
  if (!g_shell_parse_argv (cmd, NULL, &argv, NULL))
    return NULL;
  char *res = NULL;
  if (argv && argv[0] && argv[1] && argv[2]
      && g_strcmp0 (argv[0], "heroic") == 0
      && g_strcmp0 (argv[1], "launch") == 0)
    res = g_strdup (argv[2]);
  g_strfreev (argv);
  return res;
}

static char *
extract_hydra_object_id (const char *cmd)
{
  gchar **argv = NULL;
  if (!g_shell_parse_argv (cmd, NULL, &argv, NULL))
    return NULL;
  char *res = NULL;
  if (argv && argv[1])
    {
      const char *p = strstr (argv[1], "objectId=");
      if (p)
        {
          p += strlen ("objectId=");
          const char *end = strchr (p, '&');
          if (!end) end = p + strlen (p);
          if (end > p)
            res = g_strndup (p, (gsize) (end - p));
        }
    }
  g_strfreev (argv);
  return res;
}

static char *
extract_bottle_name (const char *cmd)
{
  gchar **argv = NULL;
  if (!g_shell_parse_argv (cmd, NULL, &argv, NULL))
    return NULL;
  char *res = NULL;
  if (argv)
    for (int i = 0; argv[i]; i++)
      if (g_strcmp0 (argv[i], "-b") == 0 && argv[i + 1])
        { res = g_strdup (argv[i + 1]); break; }
  g_strfreev (argv);
  return res;
}

static char *
extract_lutris_name (const char *cmd)
{
  gchar **argv = NULL;
  if (!g_shell_parse_argv (cmd, NULL, &argv, NULL))
    return NULL;
  char *res = NULL;
  if (argv && argv[0] && argv[1] && argv[2]
      && g_strcmp0 (argv[0], "lutris") == 0
      && g_strcmp0 (argv[1], "-L") == 0)
    res = g_strdup (argv[2]);
  g_strfreev (argv);
  return res;
}

/* ---- Steam prefix discovery ---- */

/* Locate <steam_root>/steamapps/compatdata/<appid>/pfx across every Steam
 * installation (native + Flatpak, plus libraryfolders.vdf libraries).
 * On success *prefix_out is the prefix dir and *flatpak_out reports whether
 * the Steam installation lives under the Flatpak data directory. */
static gboolean
find_steam_prefix (const char *appid, char **prefix_out, gboolean *flatpak_out)
{
  const char *home = g_get_home_dir ();
  GPtrArray *roots = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *flatpak_steam = g_build_filename (home, ".var", "app",
    "com.valvesoftware.Steam", "data", "Steam", NULL);

  g_ptr_array_add (roots, g_build_filename (home, ".steam", "steam", NULL));
  g_ptr_array_add (roots, g_build_filename (home, ".local", "share", "Steam", NULL));
  g_ptr_array_add (roots, g_strdup (flatpak_steam));

  /* Parse libraryfolders.vdf for additional Steam library paths */
  char *vdf = g_build_filename (home, ".steam", "steam", "steamapps",
    "libraryfolders.vdf", NULL);
  char *vdf_content = NULL;
  if (g_file_get_contents (vdf, &vdf_content, NULL, NULL))
    {
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
          g_ptr_array_add (roots, g_strndup (p, (gsize) (end - p)));
        }
      g_strfreev (lines);
    }
  g_free (vdf_content);
  g_free (vdf);

  g_autofree char *want = g_strdup_printf ("steamapps/compatdata/%s/pfx", appid);
  gboolean found = FALSE;
  gboolean is_flatpak = FALSE;
  char *pfx = NULL;

  for (guint i = 0; i < roots->len; i++)
    {
      const char *root = g_ptr_array_index (roots, i);
      pfx = g_build_filename (root, want, NULL);
      if (g_file_test (pfx, G_FILE_TEST_IS_DIR))
        {
          is_flatpak = (g_str_has_prefix (root, flatpak_steam) != FALSE);
          found = TRUE;
          break;
        }
      g_free (pfx);
      pfx = NULL;
    }

  g_ptr_array_unref (roots);

  if (!found)
    {
      g_free (pfx);
      return FALSE;
    }

  if (prefix_out)
    *prefix_out = pfx;
  else
    g_free (pfx);
  if (flatpak_out)
    *flatpak_out = is_flatpak;
  return TRUE;
}

/* ---- Non-Steam prefix discovery ---- */

/* Naive JSON string extractor: finds "key" : "value". */
static char *
json_get_string (const char *json, const char *key)
{
  if (!json) return NULL;
  g_autofree char *needle = g_strdup_printf ("\"%s\"", key);
  const char *p = strstr (json, needle);
  if (!p) return NULL;
  const char *q = strchr (p + strlen (needle), '"');
  if (!q) return NULL;
  q++;
  const char *end = strchr (q, '"');
  if (!end) return NULL;
  return g_strndup (q, (gsize) (end - q));
}

/* Extract a string key, but only looking shortly after `scope` appears
 * (e.g. "name"/"type" inside the "wineVersion" object). */
static char *
json_get_scoped_string (const char *json, const char *scope, const char *key)
{
  if (!json) return NULL;
  const char *s = strstr (json, scope);
  if (!s) return NULL;
  g_autofree char *needle = g_strdup_printf ("\"%s\"", key);
  const char *json_end = json + strlen (json);
  const char *limit = s + strlen (scope) + 400;
  if (limit > json_end) limit = json_end;
  const char *p = s + strlen (scope);
  while (p && p < limit)
    {
      const char *hit = strstr (p, needle);
      if (!hit || hit >= limit) break;
      const char *q = strchr (hit + strlen (needle), '"');
      if (q)
        {
          q++;
          const char *end = strchr (q, '"');
          if (end) return g_strndup (q, (gsize) (end - q));
        }
      p = hit + 1;
    }
  return NULL;
}

/* Best-effort: locate the Wine/Proton binary a launcher downloaded under
 * ~/.config/heroic/tools.  Returns NULL to let winetricks use PATH wine. */
static char *
heroic_wine_binary (const char *version_name, const char *version_type)
{
  if (!version_name || !version_name[0]) return NULL;
  const char *home = g_get_home_dir ();
  const char *stem = strstr (version_name, "- ");
  stem = stem ? stem + 2 : version_name;

  if (g_strcmp0 (version_type, "proton") == 0)
    {
      g_autofree char *w = g_build_filename (home, ".config", "heroic",
        "tools", "proton", stem, "files", "bin", "wine", NULL);
      if (g_file_test (w, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup (w);
      /* Proton installations managed by Steam itself */
      g_autofree char *w2 = g_build_filename (home, ".local", "share", "Steam",
        "steamapps", "common", stem, "files", "bin", "wine", NULL);
      if (g_file_test (w2, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup (w2);
      g_autofree char *w3 = g_build_filename (home, ".steam", "steam",
        "steamapps", "common", stem, "files", "bin", "wine", NULL);
      if (g_file_test (w3, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup (w3);
    }
  else
    {
      g_autofree char *w = g_build_filename (home, ".config", "heroic",
        "tools", "wine", stem, "bin", "wine", NULL);
      if (g_file_test (w, G_FILE_TEST_IS_EXECUTABLE)) return g_strdup (w);
    }
  return NULL;
}

/* Resolve the Wine prefix a launcher manages for a given game key.
 * Returns TRUE on success; *wine_out may be NULL (system wine is fine). */
static gboolean
resolve_nonsteam_prefix (const char *source, const char *key,
                         char **prefix_out, char **wine_out)
{
  const char *home = g_get_home_dir ();
  *wine_out = NULL;

  if (g_strcmp0 (source, "heroic") == 0)
    {
      char *json_path = NULL;
      char *content = NULL;
      const char *dirs[] = { "GamesConfig", "games" };
      for (int i = 0; i < 2 && !content; i++)
        {
          json_path = g_build_filename (home, ".config", "heroic",
            dirs[i], key, NULL);
          if (!g_str_has_suffix (json_path, ".json"))
            {
              char *jp = g_strdup_printf ("%s.json", json_path);
              g_free (json_path);
              json_path = jp;
            }
          if (g_file_test (json_path, G_FILE_TEST_EXISTS))
            g_file_get_contents (json_path, &content, NULL, NULL);
          else
            { g_free (json_path); json_path = NULL; }
        }
      if (!content) return FALSE;

      char *wp = json_get_string (content, "winePrefix");
      char *ver_type = json_get_scoped_string (content, "wineVersion", "type");
      if (!wp || !wp[0])
        {
          g_free (wp);
          wp = g_build_filename (home, "Games", "Heroic", "Prefixes", key, NULL);
        }

      if (g_strcmp0 (ver_type, "proton") == 0)
        {
          /* For Proton, winePrefix is the compat data dir; pfx lives inside. */
          char *pfx = g_build_filename (wp, "pfx", NULL);
          g_free (wp);
          if (!g_file_test (pfx, G_FILE_TEST_IS_DIR))
            { g_free (pfx); g_free (content); g_free (json_path); return FALSE; }
          *prefix_out = pfx;
          g_autofree char *vname = json_get_scoped_string (content,
            "wineVersion", "name");
          *wine_out = heroic_wine_binary (vname, ver_type);
        }
      else
        {
          if (!g_file_test (wp, G_FILE_TEST_IS_DIR))
            { g_free (wp); g_free (content); g_free (json_path); return FALSE; }
          *prefix_out = g_strdup (wp);
          g_autofree char *vname = json_get_scoped_string (content,
            "wineVersion", "name");
          *wine_out = heroic_wine_binary (vname, ver_type);
        }

      g_free (ver_type);
      g_free (content);
      g_free (json_path);
      return TRUE;
    }

  if (g_strcmp0 (source, "hydra") == 0)
    {
      g_autofree char *base = g_build_filename (home, ".config",
        "hydralauncher", "wine-prefixes", NULL);
      g_autofree char *p1 = g_build_filename (base, key, NULL);
      if (g_file_test (p1, G_FILE_TEST_IS_DIR))
        { *prefix_out = g_strdup (p1); return TRUE; }
      g_autofree char *p2 = g_build_filename (home, ".config",
        "hydralauncher", "wine-prefix", key, NULL);
      if (g_file_test (p2, G_FILE_TEST_IS_DIR))
        { *prefix_out = g_strdup (p2); return TRUE; }
      return FALSE;
    }

  if (g_strcmp0 (source, "bottles") == 0)
    {
      const char *cands[] = {
        ".var/app/org.usebottles.bottles/data/bottles/bottles",
        ".local/share/bottles/bottles",
        NULL
      };
      for (int i = 0; cands[i]; i++)
        {
          g_autofree char *b = g_build_filename (home, cands[i], key, NULL);
          if (g_file_test (b, G_FILE_TEST_IS_DIR))
            {
              /* Each bottle has a 'wine' symlink to its runner. */
              g_autofree char *w = g_build_filename (b, "wine", NULL);
              if (g_file_test (w, G_FILE_TEST_IS_EXECUTABLE))
                *wine_out = g_strdup (w);
              else
                {
                  g_autofree char *w2 = g_build_filename (b, "wine", "bin", "wine", NULL);
                  if (g_file_test (w2, G_FILE_TEST_IS_EXECUTABLE))
                    *wine_out = g_strdup (w2);
                }
              *prefix_out = g_strdup (b);
              return TRUE;
            }
        }
      return FALSE;
    }

  if (g_strcmp0 (source, "lutris") == 0)
    {
      g_autofree char *conf = g_build_filename (home, ".config",
        "lutris", "games", NULL);
      GDir *d = g_dir_open (conf, 0, NULL);
      if (d)
        {
          const char *entry;
          while ((entry = g_dir_read_name (d)))
            {
              if (!g_str_has_suffix (entry, ".yml")) continue;
              g_autofree char *fp = g_build_filename (conf, entry, NULL);
              char *content = NULL;
              if (g_file_get_contents (fp, &content, NULL, NULL))
                {
                  char **lines = g_strsplit (content, "\n", -1);
                  char *name = NULL, *wine_prefix = NULL;
                  for (int i = 0; lines[i]; i++)
                    {
                      char *l = g_strstrip (lines[i]);
                      if (g_str_has_prefix (l, "name:"))
                        { g_free (name); name = g_strdup (g_strstrip (l + 5)); }
                      else if (g_str_has_prefix (l, "wine_prefix:"))
                        { g_free (wine_prefix); wine_prefix = g_strdup (g_strstrip (l + 13)); }
                      if (name && wine_prefix) break;
                    }
                  g_strfreev (lines);
                  g_free (content);
                  if (name && wine_prefix && wine_prefix[0]
                      && g_strcmp0 (name, key) == 0)
                    {
                      g_free (name);
                      g_dir_close (d);
                      if (g_file_test (wine_prefix, G_FILE_TEST_IS_DIR))
                        {
                          *prefix_out = g_strdup (wine_prefix);
                          g_free (wine_prefix);
                          return TRUE;
                        }
                      g_free (wine_prefix);
                      return FALSE;
                    }
                  g_free (name);
                  g_free (wine_prefix);
                }
            }
          g_dir_close (d);
        }
      return FALSE;
    }

  return FALSE;
}

/* ---- Spawning the prefix tool with visible diagnostics ---- */

typedef struct
{
  gboolean is_steam;
  GString *out;
  GString *err;
  gint out_fd;
  gint err_fd;
} TricksCtx;

static void
drain_fd (gint fd, GString *buf)
{
  char tmp[1024];
  for (;;)
    {
      ssize_t n = read (fd, tmp, sizeof tmp);
      if (n <= 0) break;
      g_string_append_len (buf, tmp, (gsize) n);
    }
}

static void
on_tricks_child_exited (GPid pid, gint status, gpointer user_data)
{
  TricksCtx *ctx = user_data;
  g_spawn_close_pid (pid);

  if (ctx->out_fd >= 0)
    { drain_fd (ctx->out_fd, ctx->out); close (ctx->out_fd); }
  if (ctx->err_fd >= 0)
    { drain_fd (ctx->err_fd, ctx->err); close (ctx->err_fd); }

  if (status == 0)
    {
      dashboard_show_toast (ctx->is_steam
        ? tr ("Protontricks started.")
        : tr ("Winetricks started."));
    }
  else
    {
      const char *raw = (ctx->err && ctx->err->str && ctx->err->str[0])
        ? ctx->err->str
        : (ctx->out && ctx->out->str ? ctx->out->str : "");
      char *one_line = g_strdup (raw);
      g_strstrip (one_line);
      char *nl = strchr (one_line, '\n');
      if (nl) *nl = '\0';
      if (strlen (one_line) > 200) one_line[200] = '\0';
      char *toast = g_strdup_printf (
        ctx->is_steam
          ? tr ("Failed to launch Protontricks: %s")
          : tr ("Failed to launch Winetricks: %s"),
        one_line[0] ? one_line : "?");
      dashboard_show_toast (toast);
      g_free (toast);
      g_free (one_line);
    }

  g_string_free (ctx->out, TRUE);
  g_string_free (ctx->err, TRUE);
  g_free (ctx);
}

static void
launch_tricks_process (gboolean is_steam, char *argv[],
                       const char *steam_root, const char *prefix,
                       const char *wine)
{
  gchar **envp = g_get_environ ();
  if (steam_root && steam_root[0])
    envp = g_environ_setenv (envp, "STEAM_DIR", steam_root, TRUE);
  if (prefix && prefix[0])
    envp = g_environ_setenv (envp, "WINEPREFIX", prefix, TRUE);
  if (wine && wine[0])
    envp = g_environ_setenv (envp, "WINE", wine, TRUE);

  TricksCtx *ctx = g_new0 (TricksCtx, 1);
  ctx->is_steam = is_steam;
  ctx->out = g_string_new ("");
  ctx->err = g_string_new ("");
  ctx->out_fd = -1;
  ctx->err_fd = -1;

  GError *err = NULL;
  gint in_fd = -1, out_fd = -1, err_fd = -1;
  GPid pid = 0;

  if (!g_spawn_async_with_pipes (NULL, argv, envp,
        G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, &in_fd, &out_fd, &err_fd, &pid, &err))
    {
      char *toast = g_strdup_printf (is_steam
          ? tr ("Failed to launch Protontricks: %s")
          : tr ("Failed to launch Winetricks: %s"),
        err && err->message ? err->message : "?");
      dashboard_show_toast (toast);
      g_free (toast);
      g_clear_error (&err);
      g_strfreev (envp);
      if (in_fd >= 0) close (in_fd);
      if (out_fd >= 0) close (out_fd);
      if (err_fd >= 0) close (err_fd);
      g_string_free (ctx->out, TRUE);
      g_string_free (ctx->err, TRUE);
      g_free (ctx);
      return;
    }

  g_strfreev (envp);
  if (in_fd >= 0) close (in_fd);
  ctx->out_fd = out_fd;
  ctx->err_fd = err_fd;
  g_child_watch_add (pid, on_tricks_child_exited, ctx);
}

static void
on_protontricks_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "game-idx")) - 1;
  if (idx < 0 || idx >= page->n_games) return;

  const char *source = page->games[idx].source;
  const char *cmd = page->games[idx].command;
  gboolean is_steam = (g_strcmp0 (source, "steam") == 0);

  if (is_steam)
    {
      if (!protontricks_available ())
        {
          dashboard_show_toast (tr ("Protontricks is not installed."));
          return;
        }

      g_autofree char *appid = extract_steam_appid (cmd);
      if (!appid)
        {
          dashboard_show_toast (tr ("Could not find the game prefix."));
          return;
        }

      g_autofree char *prefix = NULL;
      gboolean is_flatpak_steam = FALSE;
      if (!find_steam_prefix (appid, &prefix, &is_flatpak_steam))
        {
          dashboard_show_toast (tr ("Launch the game once through Steam to create its prefix."));
          return;
        }

      g_autofree char *pt_bin = g_find_program_in_path ("protontricks");
      if (pt_bin)
        {
          /* Native protontricks. Flatpak Steam cannot be auto-detected, so
             point STEAM_DIR at its data dir on the host. */
          g_autofree char *steam_dir = NULL;
          if (is_flatpak_steam)
            {
              const char *home = g_get_home_dir ();
              steam_dir = g_build_filename (home, ".var", "app",
                "com.valvesoftware.Steam", "data", "Steam", NULL);
            }
          char *argv[] = { pt_bin, appid, "--gui", NULL };
          launch_tricks_process (TRUE, argv, steam_dir, NULL, NULL);
        }
      else
        {
          /* Flatpak protontricks bundles winetricks. Grant access to the
             game's Steam library so prefixes on other drives also work. */
          GPtrArray *args = g_ptr_array_new ();
          g_ptr_array_add (args, (gpointer) "flatpak");
          g_ptr_array_add (args, (gpointer) "run");
          if (prefix)
            {
              const char *apps = strstr (prefix, "/steamapps/");
              g_autofree char *libroot = apps
                ? g_strndup (prefix, (gsize) (apps - prefix))
                : g_strdup (prefix);
              g_autofree char *fs = g_strdup_printf ("--filesystem=%s", libroot);
              g_ptr_array_add (args, (gpointer) g_steal_pointer (&fs));
            }
          g_ptr_array_add (args, (gpointer) "com.github.Matoking.protontricks");
          g_ptr_array_add (args, (gpointer) appid);
          g_ptr_array_add (args, (gpointer) "--gui");
          g_ptr_array_add (args, NULL);

          char **argv = (char **) g_ptr_array_free (args, FALSE);
          launch_tricks_process (TRUE, argv, NULL, NULL, NULL);
          g_free (argv);
        }
      return;
    }

  /* Non-Steam launchers: winetricks against the launcher's own prefix. */
  if (!winetricks_available ())
    {
      dashboard_show_toast (tr ("Winetricks is not installed."));
      return;
    }

  char *key = NULL;
  if (g_strcmp0 (source, "heroic") == 0)
    key = extract_heroic_app_name (cmd);
  else if (g_strcmp0 (source, "hydra") == 0)
    key = extract_hydra_object_id (cmd);
  else if (g_strcmp0 (source, "bottles") == 0)
    key = extract_bottle_name (cmd);
  else if (g_strcmp0 (source, "lutris") == 0)
    key = extract_lutris_name (cmd);

  if (!key)
    {
      dashboard_show_toast (tr ("Could not find the game prefix."));
      return;
    }

  g_autofree char *prefix = NULL;
  g_autofree char *wine = NULL;
  if (!resolve_nonsteam_prefix (source, key, &prefix, &wine))
    {
      g_free (key);
      dashboard_show_toast (tr ("Could not find the game prefix."));
      return;
    }
  g_free (key);

  char *winetricks = g_find_program_in_path ("winetricks");
  if (!winetricks)
    {
      dashboard_show_toast (tr ("Winetricks is not installed."));
      return;
    }
  char *argv[] = { winetricks, NULL };
  launch_tricks_process (FALSE, argv, NULL, prefix, wine);
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
  g_free (page->games[idx].group);

  for (int i = idx; i < page->n_games - 1; i++)
    {
      page->games[i] = page->games[i + 1];
      page->selected[i] = page->selected[i + 1];
    }

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
  dashboard_show_toast (tr ("Game removed"));
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

  static const char *source_keys[] = { "local", "steam", "heroic", "lutris", "bottles", "hydra", "other" };
  guint src_idx = gtk_drop_down_get_selected (source_drop);
  const char *source = (src_idx < 7) ? source_keys[src_idx] : "local";

  g_free (page->games[idx].name);
  g_free (page->games[idx].command);
  g_free (page->games[idx].icon);
  g_free (page->games[idx].workdir);
  g_free (page->games[idx].source);
  g_free (page->games[idx].startup_cmd);

  page->games[idx].name = g_strdup (name);  page->games[idx].command = g_strdup (cmd);
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

void
open_edit_dialog (DashboardPage *page, int idx)
{
  if (idx < 0 || idx >= page->n_games) return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, tr ("Edit Game"));
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
    tr ("Play Time"), time_str);
  GtkWidget *time_label = gtk_label_new (time_label_text);
  gtk_widget_set_halign (time_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (time_label, "dim-label");
  gtk_box_append (GTK_BOX (left), time_label);
  g_free (time_str);
  g_free (time_label_text);

  /* FPS */
  char *fps_str = g_strdup_printf (tr ("%.1f FPS"), page->games[idx].avg_fps);
  GtkWidget *fps_label = gtk_label_new (fps_str);
  gtk_widget_set_halign (fps_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (fps_label, "dim-label");
  gtk_box_append (GTK_BOX (left), fps_label);
  g_free (fps_str);

  /* Play button */
  GtkWidget *play_btn = gtk_button_new_with_label (tr ("Play"));
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
  GtkWidget *name_label = gtk_label_new (tr ("Game Name"));
  gtk_widget_set_halign (name_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (name_label, "dim-label");
  gtk_box_append (GTK_BOX (right), name_label);
  GtkWidget *name_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (name_entry),
    page->games[idx].name ? page->games[idx].name : tr ("Unnamed"));
  gtk_widget_set_hexpand (name_entry, TRUE);
  gtk_box_append (GTK_BOX (right), name_entry);

  /* Source */
  GtkWidget *source_label = gtk_label_new (tr ("Source / Platform"));
  gtk_widget_set_halign (source_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (source_label, "dim-label");
  gtk_box_append (GTK_BOX (right), source_label);
  const char *sources[] = { tr ("Local"), "Steam", "Heroic", "Lutris", "Bottles", "Hydra", tr ("Other"), NULL };
  GtkStringList *source_list = gtk_string_list_new (sources);
  GtkDropDown *source_drop = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (source_list), NULL));
  g_object_unref (source_list);
  /* Map current source to index */
  static const char *source_keys[] = { "local", "steam", "heroic", "lutris", "bottles", "hydra", "other" };
  guint src_sel = 0;
  for (int s = 0; s < 7; s++)
    {
      if (g_strcmp0 (page->games[idx].source, source_keys[s]) == 0)
        { src_sel = s; break; }
    }
  gtk_drop_down_set_selected (source_drop, src_sel);
  gtk_widget_set_hexpand (GTK_WIDGET (source_drop), TRUE);
  gtk_box_append (GTK_BOX (right), GTK_WIDGET (source_drop));

  /* Command */
  GtkWidget *cmd_label = gtk_label_new (tr ("Launch Command"));
  gtk_widget_set_halign (cmd_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (cmd_label, "dim-label");
  gtk_box_append (GTK_BOX (right), cmd_label);
  GtkWidget *cmd_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand (cmd_hbox, TRUE);
  GtkWidget *cmd_entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (cmd_entry), page->games[idx].command);
  gtk_widget_set_hexpand (cmd_entry, TRUE);
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_entry);
  GtkWidget *cmd_browse = gtk_button_new_with_label (tr ("..."));
  gtk_widget_set_size_request (cmd_browse, 36, -1);
  gtk_widget_add_css_class (cmd_browse, "flat");
  gtk_box_append (GTK_BOX (cmd_hbox), cmd_browse);
  gtk_box_append (GTK_BOX (right), cmd_hbox);

  /* Icon */
  GtkWidget *icon_label = gtk_label_new (tr ("Icon (name or path)"));
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
  GtkWidget *icon_browse = gtk_button_new_with_label (tr ("..."));
  gtk_widget_set_size_request (icon_browse, 36, -1);
  gtk_widget_add_css_class (icon_browse, "flat");
  gtk_box_append (GTK_BOX (icon_hbox), icon_browse);
  gtk_box_append (GTK_BOX (right), icon_hbox);

  /* Workdir */
  GtkWidget *workdir_label = gtk_label_new (tr ("Working Directory"));
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
  GtkWidget *workdir_browse = gtk_button_new_with_label (tr ("..."));
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

  GtkWidget *tweaks_label = gtk_label_new (tr ("Tweaks"));
  gtk_widget_set_halign (tweaks_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (tweaks_label, "heading");
  gtk_box_append (GTK_BOX (right), tweaks_label);

  /* Protontricks (Steam) / Winetricks (other launchers) on the game prefix. */
  gboolean pt_is_steam = (g_strcmp0 (page->games[idx].source, "steam") == 0);
  GtkWidget *pt_btn = gtk_button_new_with_label (pt_is_steam
    ? tr ("Protontricks...") : tr ("Winetricks..."));
  gtk_widget_set_margin_top (pt_btn, 4);
  gtk_widget_add_css_class (pt_btn, "flat");
  gtk_widget_add_css_class (pt_btn, "accent-btn");
  gtk_widget_set_halign (pt_btn, GTK_ALIGN_START);
  g_object_set_data (G_OBJECT (pt_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_signal_connect (pt_btn, "clicked", G_CALLBACK (on_protontricks_clicked), page);

  {
    gboolean tool_ok = pt_is_steam ? protontricks_available () : winetricks_available ();
    if (!tool_ok)
      {
        gtk_widget_set_sensitive (pt_btn, FALSE);
        gtk_widget_set_tooltip_text (pt_btn, pt_is_steam
          ? tr ("Protontricks is not installed.")
          : tr ("Winetricks is not installed."));
      }
  }
  gtk_box_append (GTK_BOX (right), pt_btn);

  GtkWidget *startup_label = gtk_label_new (tr ("Startup commands"));
  gtk_widget_set_halign (startup_label, GTK_ALIGN_START);
  gtk_widget_add_css_class (startup_label, "dim-label");
  gtk_box_append (GTK_BOX (right), startup_label);
  GtkWidget *startup_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (startup_entry),
    tr ("e.g. gamemoderun %command%"));
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
        tr ("Enable Feral GameMode"));
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
        tr ("Enable MangoHud"));
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

  GtkWidget *cancel_btn = gtk_button_new_with_label (tr ("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (btn_box), cancel_btn);

  GtkWidget *delete_btn = gtk_button_new_with_label (tr ("Delete"));
  gtk_widget_add_css_class (delete_btn, "destructive-action");
  g_object_set_data (G_OBJECT (delete_btn), "game-idx", GINT_TO_POINTER (idx + 1));
  g_object_set_data (G_OBJECT (delete_btn), "page", page);
  g_object_set_data (G_OBJECT (delete_btn), "dialog", dialog);
  g_signal_connect (delete_btn, "clicked", G_CALLBACK (on_delete_game_clicked), page);
  gtk_box_append (GTK_BOX (btn_box), delete_btn);

  GtkWidget *save_btn = gtk_button_new_with_label (tr ("Save"));
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
  page->selected = g_realloc (page->selected,
    page->n_games * sizeof (gboolean));
  page->selected[page->n_games - 1] = FALSE;

  GameShortcut *g = &page->games[page->n_games - 1];
  g->name = g_strdup (name);
  g->command = g_strdup (cmd);
  g->icon = g_strdup (icon ? icon : "input-gaming-symbolic");
  g->workdir = NULL;
  g->source = g_strdup (source ? source : "local");
  g->startup_cmd = NULL;
  g->group = NULL;
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

  /* 4. Also check flatpak Steam path */
  char *flatpak_sa = g_build_filename (home, ".var", "app", "com.valvesoftware.Steam", "data", "Steam", "steamapps", NULL);
  if (g_file_test (flatpak_sa, G_FILE_TEST_IS_DIR))
    {
      gboolean found_flatpak = FALSE;
      for (guint i = 0; i < steamapps_dirs->len; i++)
        {
          if (g_strcmp0 (g_ptr_array_index (steamapps_dirs, i), flatpak_sa) == 0)
            { found_flatpak = TRUE; break; }
        }
      if (!found_flatpak)
        g_ptr_array_add (steamapps_dirs, flatpak_sa);
      else
        g_free (flatpak_sa);
    }
  else
    g_free (flatpak_sa);

  /* 5. Scan each steamapps directory for appmanifest_*.acf files */
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
              char *appid = g_strndup (appid_start, appid_len);
              char *launch_cmd = g_strdup_printf ("steam steam://rungameid/%s", appid);
              g_free (appid);
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
                              char *q_bottle = g_shell_quote (bottle_name);
                              char *q_prog = g_shell_quote (prog_name);
                              char *launch_cmd = g_strdup_printf (
                                "bottles-cli run -b %s -p %s", q_bottle, q_prog);
                              g_free (q_bottle);
                              g_free (q_prog);
                              add_game_entry (page, prog_name, launch_cmd,
                                NULL, "bottles");
                              count++;
                              g_free (launch_cmd);
                            }
                          break;
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
                              char *q_bottle = g_shell_quote (bottle_name);
                              char *q_prog = g_shell_quote (prog_name);
                              char *launch_cmd = g_strdup_printf (
                                "bottles-cli run -b %s -p %s", q_bottle, q_prog);
                              g_free (q_bottle);
                              g_free (q_prog);
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
          char *q_name = g_shell_quote (game_name);
          char *launch_cmd = g_strdup_printf ("heroic launch %s", q_name);
          g_free (q_name);
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
          g_free (yml_content);
        }

      if (!game_name)
        game_name = g_strdup (entry);

      char *q_name = g_shell_quote (game_name);
      char *launch_cmd = g_strdup_printf ("lutris -L %s", q_name);
      g_free (q_name);
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

/* ---- Hydra Launcher import ---- */

typedef struct
{
  DashboardPage *page;
  int count;
  gboolean classics; /* TRUE: only emulator (launchbox) games; FALSE: skip them */
} HydraImportCtx;

static void
on_hydra_game (const HydraGame *g, void *user_data)
{
  HydraImportCtx *ctx = user_data;

  /* Classics = emulator ROMs imported via Hydra's "launchbox" shop.  The
   * main import skips them (they get their own "Classics" option); the
   * classics-only pass imports nothing else. */
  gboolean is_emu = (g_strcmp0 (g->shop, "launchbox") == 0);
  if (ctx->classics != is_emu) return;

  /* Deep-link into Hydra: hydralauncher://run?shop=<shop>&objectId=<oid>.
   * xdg-open resolves the x-scheme-handler/hydralauncher handler (registered
   * by hydra.desktop), falling back to launching the AppImage.  This is the
   * primary path; game launches themselves are handled by Hydra, so per-game
   * mangohud/gamemode wrapping from this app does not apply. */
  g_autofree char *uri = g_strdup_printf (
    "hydralauncher://run?shop=%s&objectId=%s", g->shop, g->object_id);
  g_autofree char *q_uri = g_shell_quote (uri);
  g_autofree char *cmd = g_strdup_printf ("xdg-open %s", q_uri);

  add_game_entry (ctx->page, g->title, cmd, NULL, "hydra");
  ctx->count++;
}

static int
hydra_import_from_python (DashboardPage *page, const char *db_dir,
                          gboolean classics)
{
  g_autofree char *script = g_build_filename (
    DATADIR, "epinephrine", "scripts", "hydra_games.py", NULL);
  if (!g_file_test (script, G_FILE_TEST_IS_REGULAR)) return 0;

  g_autofree char *python = g_find_program_in_path ("python3");
  if (!python) return 0;

  g_autofree char *stdout_str = NULL;
  g_autoptr(GError) err = NULL;
  char *argv[] = { python, script, (char *) db_dir,
                   classics ? "classics" : "all", NULL };
  if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, &stdout_str, NULL, NULL, &err))
    return 0;

  HydraImportCtx ctx = { .page = page, .count = 0, .classics = classics };
  char **lines = g_strsplit (stdout_str, "\n", -1);
  for (int i = 0; lines[i]; i++)
    {
      g_strstrip (lines[i]);
      if (!lines[i][0]) continue;
      char **f = g_strsplit (lines[i], "\t", 5);
      if (g_strv_length (f) >= 3)
        {
          gboolean is_emu = (g_strcmp0 (f[2], "launchbox") == 0);
          if (ctx.classics == is_emu)
            {
              g_autofree char *uri = g_strdup_printf (
                "hydralauncher://run?shop=%s&objectId=%s", f[2], f[1]);
              g_autofree char *q_uri = g_shell_quote (uri);
              g_autofree char *cmd = g_strdup_printf ("xdg-open %s", q_uri);
              add_game_entry (page, f[0], cmd, NULL, "hydra");
              ctx.count++;
            }
        }
      g_strfreev (f);
    }
  g_strfreev (lines);
  return ctx.count;
}

static int
import_hydra_filtered (DashboardPage *page, gboolean classics)
{
  const char *home = g_get_home_dir ();
  g_autofree char *db_dir = g_build_filename (
    home, ".config", "hydralauncher", "hydra-db", NULL);

  if (!g_file_test (db_dir, G_FILE_TEST_IS_DIR))
    return 0;

  GDir *dir = g_dir_open (db_dir, 0, NULL);
  if (!dir)
    return 0;

  GPtrArray *ldb_files = g_ptr_array_new_with_free_func (g_free);
  const char *entry;
  while ((entry = g_dir_read_name (dir)) != NULL)
    if (g_str_has_suffix (entry, ".ldb"))
      g_ptr_array_add (ldb_files, g_strdup (entry));
  g_dir_close (dir);

  /* Newest .ldb first so overlapping entries prefer fresh data */
  g_ptr_array_sort (ldb_files, (GCompareFunc) g_strcmp0);
  for (guint i = 0, j = ldb_files->len - 1; i < j; i++, j--)
    {
      gpointer tmp = g_ptr_array_index (ldb_files, i);
      g_ptr_array_index (ldb_files, i) = g_ptr_array_index (ldb_files, j);
      g_ptr_array_index (ldb_files, j) = tmp;
    }

  HydraImportCtx ctx = { .page = page, .count = 0, .classics = classics };
  for (guint i = 0; i < ldb_files->len; i++)
    {
      g_autofree char *full = g_build_filename (
        db_dir, g_ptr_array_index (ldb_files, i), NULL);
      g_autofree char *content = NULL;
      gsize len = 0;
      if (g_file_get_contents (full, &content, &len, NULL))
        {
          int n = hydra_ldb_parse ((const uint8_t *) content, len,
                                   on_hydra_game, &ctx);
          if (n > 0)
            break; /* newest table yielded games — stop */
        }
    }
  g_ptr_array_unref (ldb_files);

  /* Fallback: if the native parser found nothing (format drift), use the
   * bundled Python reader. */
  if (ctx.count == 0)
    ctx.count = hydra_import_from_python (page, db_dir, classics);

  return ctx.count;
}

/* ---- Linux Games (from .desktop files) ---- */

static gboolean
categories_contain_game (const char *categories)
{
  if (!categories)
    return FALSE;
  if (strstr (categories, "Game;"))
    return TRUE;
  if (strstr (categories, "ActionGame;"))
    return TRUE;
  if (strstr (categories, "AdventureGame;"))
    return TRUE;
  if (strstr (categories, "ArcadeGame;"))
    return TRUE;
  if (strstr (categories, "BoardGame;"))
    return TRUE;
  if (strstr (categories, "BlocksGame;"))
    return TRUE;
  if (strstr (categories, "CardGame;"))
    return TRUE;
  if (strstr (categories, "KidsGame;"))
    return TRUE;
  if (strstr (categories, "LogicGame;"))
    return TRUE;
  if (strstr (categories, "RolePlaying;"))
    return TRUE;
  if (strstr (categories, "Simulation;"))
    return TRUE;
  if (strstr (categories, "SportsGame;"))
    return TRUE;
  if (strstr (categories, "StrategyGame;"))
    return TRUE;
  if (strstr (categories, "Emulator;"))
    return TRUE;
  return FALSE;
}

static gboolean
is_launcher_binary (const char *exec)
{
  if (!exec || exec[0] == '\0')
    return FALSE;
  const char *launchers[] = {
    "steam", "steam-native", "steam-runtime",
    "heroic", "lutris", "bottles",
    "mangohud", "gamemoderun", "gamescope", "protontricks",
    NULL
  };
  for (int i = 0; launchers[i]; i++)
    {
      size_t len = strlen (launchers[i]);
      if (g_ascii_strncasecmp (exec, launchers[i], len) == 0 &&
          (exec[len] == '\0' || exec[len] == ' '))
        return TRUE;
    }
  return FALSE;
}

static void
strip_exec_field_codes (char *exec)
{
  if (!exec)
    return;
  char *src = exec, *dst = exec;
  while (*src)
    {
      if (*src == '%' && (src[1] == 'f' || src[1] == 'F' ||
                          src[1] == 'u' || src[1] == 'U' ||
                          src[1] == 'd' || src[1] == 'D' ||
                          src[1] == 'n' || src[1] == 'N' ||
                          src[1] == 'i' || src[1] == 'c' ||
                          src[1] == 'k'))
        {
          src += 2;
        }
      else if (*src == '%' && src[1] == '%')
        {
          *dst++ = '%';
          src += 2;
        }
      else
        {
          *dst++ = *src++;
        }
    }
  *dst = '\0';
}

static void
scan_desktop_dir (const char *dir_path, DashboardPage *page, int *count)
{
  GDir *dir = g_dir_open (dir_path, 0, NULL);
  if (!dir)
    return;

  const char *name;
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      if (!g_str_has_suffix (name, ".desktop"))
        continue;

      g_autofree char *full_path = g_build_filename (dir_path, name, NULL);

      GKeyFile *kf = g_key_file_new ();
      GError *err = NULL;
      if (!g_key_file_load_from_file (kf, full_path, G_KEY_FILE_NONE, &err))
        {
          g_error_free (err);
          g_key_file_free (kf);
          continue;
        }

      /* Skip hidden/deleted entries */
      if (g_key_file_get_boolean (kf, "Desktop Entry", "Hidden", NULL))
        { g_key_file_free (kf); continue; }
      if (g_key_file_get_boolean (kf, "Desktop Entry", "NoDisplay", NULL))
        { g_key_file_free (kf); continue; }

      /* Only Application type */
      g_autofree char *type = g_key_file_get_string (kf, "Desktop Entry", "Type", NULL);
      if (!type || g_strcmp0 (type, "Application") != 0)
        { g_key_file_free (kf); continue; }

      /* Must have Game category */
      g_autofree char *categories = g_key_file_get_string (kf, "Desktop Entry", "Categories", NULL);
      if (!categories_contain_game (categories))
        { g_key_file_free (kf); continue; }

      /* Must have Exec */
      g_autofree char *exec = g_key_file_get_string (kf, "Desktop Entry", "Exec", NULL);
      if (!exec || exec[0] == '\0')
        { g_key_file_free (kf); continue; }

      /* Skip if Exec is a launcher binary */
      if (is_launcher_binary (exec))
        { g_key_file_free (kf); continue; }

      /* Clean field codes */
      strip_exec_field_codes (exec);

      /* Get name and icon */
      g_autofree char *display_name = g_key_file_get_locale_string (kf, "Desktop Entry", "Name", NULL, NULL);
      g_autofree char *icon = g_key_file_get_string (kf, "Desktop Entry", "Icon", NULL);

      if (!display_name || display_name[0] == '\0')
        { g_key_file_free (kf); continue; }

      add_game_entry (page, display_name, exec, icon, "linux");
      page->games[page->n_games - 1].group = g_strdup ("Nativo");
      (*count)++;

      g_key_file_free (kf);
    }
  g_dir_close (dir);
}

int
import_desktop_games (DashboardPage *page)
{
  int count = 0;

  /* User data dir */
  g_autofree char *user_data = g_build_filename (g_get_user_data_dir (), "applications", NULL);
  scan_desktop_dir (user_data, page, &count);

  /* System data dirs */
  const gchar * const *sys_dirs = g_get_system_data_dirs ();
  for (int i = 0; sys_dirs[i]; i++)
    {
      g_autofree char *app_dir = g_build_filename (sys_dirs[i], "applications", NULL);
      scan_desktop_dir (app_dir, page, &count);
    }

  /* Flatpak system exports */
  scan_desktop_dir ("/var/lib/flatpak/exports/share/applications", page, &count);

  /* Flatpak user exports */
  g_autofree char *flatpak_user = g_build_filename (g_get_home_dir (),
    ".local", "share", "flatpak", "exports", "share", "applications", NULL);
  scan_desktop_dir (flatpak_user, page, &count);

  /* Snap */
  scan_desktop_dir ("/var/lib/snapd/desktop/applications", page, &count);

  return count;
}

/* ---- Hydra ---- */

int
import_hydra_games (DashboardPage *page)
{
  return import_hydra_filtered (page, FALSE);
}

int
import_hydra_classics_games (DashboardPage *page)
{
  return import_hydra_filtered (page, TRUE);
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
    tr ("Installing..."));

  epi_privilege_exec_async (cmd);

  /* Restore button after a short delay (we can't track completion) */
  g_timeout_add (5000, restore_install_btn, page);
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
 *   CPU: 8 cores, 8 threads, 2.5 GHz, 2020+        → 0 – 3.0
 *   GPU: 6 GB VRAM, 2020+                          → 0 – 3.0
 *   RAM: 16 GB DDR4, 2400 MHz+                     → 0 – 2.5
 *   Monitor: 1920×1080, 16:9                       → 0 – 2.5
 *
 * Year factor: hardware from 2020+ = 1.0, each year before subtracts 0.1
 *   down to a minimum of 0.4 (7-year defasagem).
 *
 * A system with 1.5× the reference specs scores 11.0 (max).
 */
typedef struct
{
  float cpu;
  float gpu;
  float ram;
  float monitor;
} GamingScoreParts;

static float
calculate_gaming_score (const GatherResult *r, GamingScoreParts *parts)
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

  if (parts)
    {
      parts->cpu = cpu_raw;
      parts->gpu = gpu_raw;
      parts->ram = ram_raw;
      parts->monitor = mon_raw;
    }

  float total = cpu_raw + gpu_raw + ram_raw + mon_raw;
  if (total > 11.0f) total = 11.0f;
  if (total < 0.0f) total = 0.0f;

  return total;
}

/* ---- GPU_INDEX.md VRAM fallback ---- */

typedef struct
{
  char *name;      /* normalized index GPU name */
  float vram_gb;   /* VRAM declared in the index, 0 if unknown */
} GpuIndexEntry;

static GPtrArray *g_gpu_index_entries = NULL;

static void
free_gpu_index_entry (gpointer data)
{
  GpuIndexEntry *e = data;
  if (e)
    {
      g_free (e->name);
      g_free (e);
    }
}

static char *
normalize_gpu_name (const char *name)
{
  char *s = g_ascii_strdown (name, -1);

  char *r;
  while ((r = strstr (s, "(r)")) != NULL)
    memmove (r, r + 3, strlen (r + 3) + 1);
  while ((r = strstr (s, "(tm)")) != NULL)
    memmove (r, r + 4, strlen (r + 4) + 1);

  char *w = s;
  for (char *p = s; *p; p++)
    {
      if ((unsigned char)*p == 0xC2 && (unsigned char)p[1] == 0xAE)
        { p++; continue; }   /* ® */
      if ((unsigned char)*p == 0xE2 && (unsigned char)p[1] == 0x84
          && (unsigned char)p[2] == 0xA2)
        { p += 2; continue; } /* ™ */
      *w++ = *p;
    }
  *w = '\0';
  g_strstrip (s);

  /* Remove parenthesized suffix, e.g. "(RADV POLARIS11)" */
  char *paren = strrchr (s, '(');
  if (paren && paren != s)
    {
      *paren = '\0';
      g_strstrip (s);
    }
  return s;
}

static void
load_gpu_index_cache (void)
{
  if (g_gpu_index_entries) return;
  g_gpu_index_entries = g_ptr_array_new_with_free_func (free_gpu_index_entry);

  char *content = NULL;
  GBytes *gbytes = g_resources_lookup_data (
    "/io/github/mayrinck/epinephrine/database/GPU_INDEX.md", 0, NULL);
  if (gbytes)
    {
      content = g_strndup (g_bytes_get_data (gbytes, NULL),
        g_bytes_get_size (gbytes));
      g_bytes_unref (gbytes);
    }
  if (!content)
    g_file_get_contents (DATADIR "/epinephrine/database/GPU_INDEX.md", &content, NULL, NULL);
  if (!content)
    {
      char *cwd = g_get_current_dir ();
      char *rel = g_build_filename (cwd, "database", "GPU_INDEX.md", NULL);
      g_file_get_contents (rel, &content, NULL, NULL);
      g_free (cwd);
      g_free (rel);
    }

  if (!content) return;

  gboolean in_table = FALSE;
  char **lines = g_strsplit (content, "\n", -1);
  g_free (content);

  for (int i = 0; lines[i]; i++)
    {
      const char *line = lines[i];

      if (g_str_has_prefix (line, "|---|---"))
        {
          in_table = TRUE;
          continue;
        }

      if (!in_table || line[0] != '|') continue;

      char **cols = g_strsplit (line, "|", 0);
      if (!cols[1])
        {
          g_strfreev (cols);
          continue;
        }

      char *gpu_name = g_strstrip (cols[1]);
      if (!gpu_name[0])
        {
          g_strfreev (cols);
          continue;
        }

      GpuIndexEntry *e = g_new0 (GpuIndexEntry, 1);
      e->name = normalize_gpu_name (gpu_name);

      if (cols[2])
        {
          char *vram_str = g_strstrip (cols[2]);
          if (vram_str[0] && g_strcmp0 (vram_str, "NULL") != 0)
            {
              char *end = NULL;
              double gb = g_ascii_strtod (vram_str, &end);
              if (gb <= 0)
                {
                  const char *p = vram_str;
                  while (*p && !g_ascii_isdigit (*p)) p++;
                  if (*p) gb = g_ascii_strtod (p, &end);
                }
              if (gb > 0) e->vram_gb = (float) gb;
            }
        }

      g_ptr_array_add (g_gpu_index_entries, e);
      g_strfreev (cols);
    }

  g_strfreev (lines);
}

static float
gpu_vram_from_index (const char *detected_name)
{
  if (!detected_name || !detected_name[0]) return 0.0f;
  load_gpu_index_cache ();
  if (!g_gpu_index_entries || g_gpu_index_entries->len == 0) return 0.0f;

  char *norm = normalize_gpu_name (detected_name);
  float vram = 0.0f;

  static const char *keys[] = {
    "arc", "geforce", "radeon", "iris", "uhd", "hd graphics", "adreno", "fenghua"
  };

  for (guint i = 0; i < g_gpu_index_entries->len; i++)
    {
      GpuIndexEntry *e = g_ptr_array_index (g_gpu_index_entries, i);
      if (!e->name || !e->name[0]) continue;

      gboolean match = (g_strcmp0 (norm, e->name) == 0);
      if (!match)
        match = g_str_has_prefix (norm, e->name)
             || g_str_has_prefix (e->name, norm);
      if (!match)
        {
          const char *key_d = NULL;
          for (guint k = 0; k < G_N_ELEMENTS (keys); k++)
            if ((key_d = strstr (norm, keys[k]))) break;

          const char *key_i = NULL;
          for (guint k = 0; k < G_N_ELEMENTS (keys); k++)
            if ((key_i = strstr (e->name, keys[k]))) break;

          if (key_d && key_i)
            {
              size_t min_len = MIN (strlen (key_d), strlen (key_i));
              match = (strncmp (key_d, key_i, min_len) == 0);
            }
        }

      if (match && e->vram_gb > 0)
        {
          vram = e->vram_gb;
          break;
        }
    }

  g_free (norm);
  return vram;
}

static GatherResult *
gather_all (void)
{
  GatherResult *r = g_new0 (GatherResult, 1);
  char *tmp;

  tmp = RUN_SH_STATIC ("grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->cpu, tmp, sizeof (r->cpu)); g_free (tmp); }
  else     g_strlcpy (r->cpu, tr ("N/A"), sizeof (r->cpu));

  tmp = RUN_SH_STATIC ("awk '/MemTotal/{printf \"%.0f MB (%.1f GB)\", $2, $2/1024/1024}' /proc/meminfo");
  if (tmp) { g_strlcpy (r->ram, tmp, sizeof (r->ram)); g_free (tmp); }
  else     g_strlcpy (r->ram, tr ("N/A"), sizeof (r->ram));

  tmp = RUN_SH_STATIC ("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 | sed 's/.*: //'");
  if (!tmp) tmp = RUN_SH_STATIC ("glxinfo 2>/dev/null | grep -i 'OpenGL renderer' | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->gpu, tmp, sizeof (r->gpu)); g_free (tmp); }
  else     g_strlcpy (r->gpu, tr ("N/A"), sizeof (r->gpu));

  /* VRAM total. Prefer the live sysfs / nvidia-smi readings; when none of
     them yields a relevant value, fall back to the VRAM declared for this
     GPU in GPU_INDEX.md (matching by detected GPU name). */
  tmp = RUN_SH_STATIC ("cat /sys/class/drm/*/device/mem_info_vram_total 2>/dev/null | head -1 | awk '{printf \"%.0f MB\", $1/1024/1024}'");
  if (!tmp)
    tmp = RUN_SH_STATIC ("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | awk '{printf \"%.0f MB\", $1}'");
  if (tmp)
    {
      if (atof (tmp) > 1.0f)
        {
          g_strlcpy (r->vram, tmp, sizeof (r->vram));
          g_free (tmp);
        }
      else
        {
          g_free (tmp);
          tmp = NULL;
        }
    }
  if (!tmp)
    {
      float idx_vram = gpu_vram_from_index (r->gpu);
      if (idx_vram > 0.5f)
        g_snprintf (r->vram, sizeof (r->vram), "%.0f MB", idx_vram * 1024.0f);
      else
        g_strlcpy (r->vram, tr ("N/A"), sizeof (r->vram));
    }

  tmp = RUN_SH_STATIC ("vulkaninfo 2>/dev/null | grep -i 'Vulkan Instance Version' | head -1 | sed 's/.*: //'");
  if (!tmp) tmp = RUN_SH_STATIC ("glxinfo 2>/dev/null | grep -i 'OpenGL version' | sed 's/.*: //'");
  if (tmp) { g_strlcpy (r->vulkan, tmp, sizeof (r->vulkan)); g_free (tmp); }
  else     g_strlcpy (r->vulkan, tr ("N/A"), sizeof (r->vulkan));

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
    g_strlcpy (r->driver, tr ("N/A"), sizeof (r->driver));

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
    if (r->gpu_vram_gb < 0.5f)
      {
        /* Fallback: look up declared VRAM in GPU_INDEX.md using the
           detected GPU name (lspci or glxinfo). */
        float idx_vram = gpu_vram_from_index (r->gpu);
        if (idx_vram > 0.5f)
          r->gpu_vram_gb = idx_vram;
        else
          r->gpu_vram_gb = 0;
      }
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
                                        used = g_strdup_printf (tr ("Total: %s GB — Used: %s GB (%s)"), total_gb, used_gb, pct_str);
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
                  tr ("Total: %s GB — Used: %s GB (%s)"), total_gb, used_gb, pct);
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
                        || check_flatpak_binary (RESOURCES[i].pkg)
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
  GamingScoreParts parts;
} ScoreDisplayData;

/* Color the progress bar by how close the score is to its max:
   < 40% red, 40–70% orange, >= 70% green. */
static void
set_score_bar_color (GtkWidget *bar, float fraction)
{
  gtk_widget_remove_css_class (bar, "score-bar-low");
  gtk_widget_remove_css_class (bar, "score-bar-mid");
  gtk_widget_remove_css_class (bar, "score-bar-good");

  const char *cls;
  if (fraction >= 0.7f)
    cls = "score-bar-good";
  else if (fraction >= 0.4f)
    cls = "score-bar-mid";
  else
    cls = "score-bar-low";

  gtk_widget_add_css_class (bar, cls);
}

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
      dashboard_show_toast (tr ("Hardware couldn't be properly identified for calculations. Try again later."));
    }
  else
    {
      char buf[32];
      g_snprintf (buf, sizeof (buf), tr ("%.1f/11"), sd->score);
      gtk_label_set_text (GTK_LABEL (page->gaming_score_label), buf);
      gtk_widget_set_visible (page->gaming_score_label, TRUE);

      /* Tier label: colour + wording that conveys what the score means */
      static const char *tier_classes[] = {
        "tier-ruim", "tier-regular", "tier-boa",
        "tier-muito-boa", "tier-ideal",
      };
      for (guint i = 0; i < G_N_ELEMENTS (tier_classes); i++)
        gtk_widget_remove_css_class (page->gaming_score_tier, tier_classes[i]);

      const char *tier_class;
      const char *tier_text;
      const char *tier_tooltip;
      if (sd->score >= 9.5f)
        {
          tier_class = "tier-ideal";
          tier_text = tr ("Ideal");
          tier_tooltip =
            tr ("Hardware supports most modern titles at up to 4K with high "
            "graphics settings. Don't worry, just play :)");
        }
      else if (sd->score >= 7.5f)
        {
          tier_class = "tier-muito-boa";
          tier_text = tr ("Very Good");
          tier_tooltip =
            tr ("Hardware will let you play at 1080p with high graphics quality or "
            "1440p with medium graphics quality. Most titles will run at 60FPS or "
            "above. Settings can improve the frame rate.");
        }
      else if (sd->score >= 5.5f)
        {
          tier_class = "tier-boa";
          tier_text = tr ("Good");
          tier_tooltip =
            tr ("Hardware can run most games at 1080p in 30/60FPS with medium or "
            "high quality, depending on the type of game. Some modern titles can "
            "also be played with low graphics quality or with LowSpec mods.");
        }
      else if (sd->score >= 3.0f)
        {
          tier_class = "tier-regular";
          tier_text = tr ("Fair");
          tier_tooltip =
            tr ("Hardware is a bit outdated, but still usable. It allows playing "
            "indie titles, light games, high-quality emulation and old PC games "
            "at 1080p averaging 30FPS. In some cases, also modern games, with "
            "very low graphics settings or LowSpec mods.");
        }
      else
        {
          tier_class = "tier-ruim";
          tier_text = tr ("Poor");
          tier_tooltip =
            tr ("Hardware is very outdated; recommended for light games, indies "
            "(preferably 2D), emulation and legacy games. It is recommended to "
            "play at 720p or below for the best experience.");
        }

      gtk_widget_add_css_class (page->gaming_score_tier, tier_class);
      gtk_label_set_text (GTK_LABEL (page->gaming_score_tier), tier_text);
      gtk_widget_set_tooltip_text (page->gaming_score_tier, tier_tooltip);
      gtk_widget_set_visible (page->gaming_score_tier, TRUE);

      /* Per-component progress bars (fraction = score / component max) */
      float fracs[4] = {
        sd->parts.cpu / 3.0f,
        sd->parts.gpu / 3.0f,
        sd->parts.ram / 2.5f,
        sd->parts.monitor / 2.5f,
      };
      GtkWidget *bars[4] = {
        page->gaming_score_cpu_bar,
        page->gaming_score_gpu_bar,
        page->gaming_score_ram_bar,
        page->gaming_score_monitor_bar,
      };
      for (int i = 0; i < 4; i++)
        {
          float f = CLAMP (fracs[i], 0.0f, 1.0f);
          gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bars[i]), f);
          set_score_bar_color (bars[i], f);
        }

      gtk_widget_set_visible (page->gaming_score_footer, FALSE);
      gtk_widget_set_visible (page->gaming_score_details_btn, TRUE);
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
                    "title", r->storage_ssd[i] ? tr ("SSD") : tr ("HDD"),
                    "icon-name", r->storage_ssd[i] ? "ssd-symbolic" : "harddisk-symbolic",
                    "subtitle", r->storage[i], NULL);
      gtk_box_append (GTK_BOX (page->hw_box), GTK_WIDGET (page->storage_rows[i]));
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
          adw_action_row_set_subtitle (page->resources[i].row, tr ("Installed"));
        }
      else
        {
          gtk_widget_set_visible (page->resources[i].install_btn, TRUE);
          gtk_widget_set_visible (GTK_WIDGET (page->resources[i].progress), FALSE);
          gtk_widget_set_visible (page->resources[i].status_icon, FALSE);
          gtk_widget_set_visible (page->resources[i].installed_label, FALSE);
          adw_action_row_set_subtitle (page->resources[i].row, tr ("Not installed"));
        }
    }

  gtk_widget_set_visible (page->spinner, FALSE);
  gtk_widget_set_visible (page->scroll, TRUE);

  /* Gaming Score: calculate and show with fake loading */
  GamingScoreParts parts;
  float score = calculate_gaming_score (r, &parts);
  gtk_widget_set_visible (page->gaming_score_box, TRUE);
  gtk_widget_set_visible (page->gaming_score_spinner, TRUE);
  gtk_widget_set_visible (page->gaming_score_label, FALSE);
  gtk_widget_set_visible (page->gaming_score_tier, FALSE);
  gtk_widget_set_visible (page->gaming_score_details_btn, FALSE);
  gtk_widget_set_visible (page->gaming_score_details, FALSE);
  gtk_widget_set_visible (page->gaming_score_footer, FALSE);
  gtk_spinner_start (GTK_SPINNER (page->gaming_score_spinner));

  ScoreDisplayData *sd = g_new0 (ScoreDisplayData, 1);
  sd->page = page;
  sd->score = score;
  sd->parts = parts;
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

static void
on_hw_goto_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn; (void) user_data;
  dashboard_navigate_to_hardware ();
}

static void
on_gaming_score_details_clicked (GtkButton *btn, DashboardPage *page)
{
  (void) btn;
  gboolean visible = gtk_widget_get_visible (page->gaming_score_details);
  gtk_widget_set_visible (page->gaming_score_details, !visible);
  gtk_widget_set_visible (page->gaming_score_footer, !visible);
  gtk_button_set_label (GTK_BUTTON (page->gaming_score_details_btn),
    tr (visible ? "Details" : "Hide details"));
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
  page->recent_box = GTK_WIDGET (gtk_builder_get_object (builder, "recent-games-box"));
  page->gaming_score_box = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-box"));
  page->gaming_score_label = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-label"));
  page->gaming_score_tier = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-tier"));
  page->gaming_score_spinner = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-spinner"));
  page->gaming_score_details_btn = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-details-btn"));
  page->gaming_score_details = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-details"));
  page->gaming_score_footer = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-footer"));
  page->gaming_score_cpu_bar = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-cpu-bar"));
  page->gaming_score_gpu_bar = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-gpu-bar"));
  page->gaming_score_ram_bar = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-ram-bar"));
  page->gaming_score_monitor_bar = GTK_WIDGET (gtk_builder_get_object (builder, "gaming-score-monitor-bar"));
  g_signal_connect (page->gaming_score_details_btn, "clicked",
    G_CALLBACK (on_gaming_score_details_clicked), page);

  /* See more button -> navigate to Games tab */
  GtkWidget *see_more_btn = GTK_WIDGET (gtk_builder_get_object (builder, "see-more-button"));
  g_signal_connect (see_more_btn, "clicked", G_CALLBACK (on_see_more_clicked), NULL);

  /* Hardware goto button -> navigate to Hardware tab */
  GtkWidget *hw_goto_btn = GTK_WIDGET (gtk_builder_get_object (builder, "hw-goto-button"));
  g_signal_connect (hw_goto_btn, "clicked", G_CALLBACK (on_hw_goto_clicked), NULL);

  gtk_spinner_start (GTK_SPINNER (page->spinner));

  detected_pm = detect_pm ();

  /* Load games config */
  load_games_config (page);
  refresh_recent_games (page);

  /* Hardware card — plain GtkBox (no AdwPreferencesGroup, avoids GtkListView cleanup issues) */
  GtkWidget *hw_card = GTK_WIDGET (gtk_builder_get_object (builder, "hw-card"));
  GtkWidget *hw_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (hw_box, "dashboard-card-group");
  gtk_box_append (GTK_BOX (hw_card), hw_box);
  page->hw_box = hw_box;

  page->cpu_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->cpu_row, "title", tr ("CPU"), "icon-name", "cpu-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->cpu_row));

  page->ram_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->ram_row, "title", tr ("RAM"), "icon-name", "memory-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->ram_row));

  page->gpu_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->gpu_row, "title", tr ("GPU"), "icon-name", "pci-card-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->gpu_row));

  page->vram_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->vram_row, "title", tr ("VRAM"), "icon-name", "memory-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->vram_row));

  page->vulkan_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->vulkan_row, "title", tr ("Vulkan / OpenGL"), "icon-name", "resource-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->vulkan_row));

  page->driver_row = ADW_ACTION_ROW (adw_action_row_new ());
  g_object_set (page->driver_row, "title", tr ("Driver"), "icon-name", "executable-symbolic", NULL);
  gtk_box_append (GTK_BOX (hw_box), GTK_WIDGET (page->driver_row));

  /* Gaming resources card — plain GtkBox */
  GtkWidget *gr_card = GTK_WIDGET (gtk_builder_get_object (builder, "gr-card"));
  GtkWidget *gr_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (gr_box, "dashboard-card-group");
  gtk_box_append (GTK_BOX (gr_card), gr_box);

  for (int i = 0; i < DASHBOARD_N_RESOURCES; i++)
    {
      page->resources[i].row = ADW_ACTION_ROW (adw_action_row_new ());
      g_object_set (page->resources[i].row, "title", RESOURCES[i].name, NULL);
      gtk_box_append (GTK_BOX (gr_box), GTK_WIDGET (page->resources[i].row));

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
      GtkWidget *inst_lbl = gtk_label_new (tr ("Installed"));
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
      GtkWidget *ibtn = gtk_button_new_with_label (tr ("Install"));
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

  /* Show spinner, hide scroll until data is ready */
  gtk_widget_set_visible (page->scroll, FALSE);

  GThread *scan_thread = g_thread_new ("hw-scan", gather_thread, page);
  g_thread_unref (scan_thread);

  g_object_ref_sink (page->box);
  g_object_unref (builder);

  return page;
}
