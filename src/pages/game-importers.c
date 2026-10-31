#include "page-dashboard.h"
#include "page-games.h"
#include "../hydra-ldb.h"
#include "../epi-i18n.h"
#include <string.h>
#include <unistd.h>
#define tr(x) epi_i18n_get(x)
/* ---- Import Games ---- */

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

/* ---- Hydra game icon download ----
 *
 * Hydra stores an "iconUrl" per game. Downloads go to the user cache and are
 * attached to the matching game entry as a file path, so the cards show the
 * same kind of artwork as Steam/Heroic (which resolve themed icon names).
 * Everything runs on the main loop through GSubprocess: never blocking. */

#define HYDRA_ICON_MAX_PARALLEL 3

typedef struct
{
  DashboardPage *page;
  char *command; /* key: the game's launch command */
  char *url;
  char *dest;
} HydraIconJob;

static GQueue *hydra_icon_queue = NULL;
static int hydra_icon_active = 0;
static guint hydra_icon_flush_id = 0;

static void hydra_icon_start_next (void);

static void
hydra_icon_job_free (HydraIconJob *job)
{
  if (!job) return;
  g_free (job->command);
  g_free (job->url);
  g_free (job->dest);
  g_free (job);
}

static gboolean
hydra_icon_file_ok (const char *path)
{
  if (!path || !path[0] || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return FALSE;

  /* Reject anything that is not a loadable image (404 pages, empty files). */
  GError *err = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file (path, &err);
  if (err) g_error_free (err);
  if (!pixbuf) return FALSE;
  g_object_unref (pixbuf);
  return TRUE;
}

static char *
hydra_icon_dest_path (const char *url)
{
  static const char *exts[] = {
    ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".svg", NULL
  };

  const char *q = strchr (url, '?');
  size_t len = q ? (size_t) (q - url) : strlen (url);

  const char *ext = ".png";
  for (int i = 0; exts[i]; i++)
    {
      size_t n = strlen (exts[i]);
      if (len > n && g_ascii_strncasecmp (url + len - n, exts[i], n) == 0)
        { ext = exts[i]; break; }
    }

  char *dir = g_build_filename (g_get_user_cache_dir (),
    "epinephrine", "hydra-icons", NULL);
  g_mkdir_with_parents (dir, 0755);

  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, url, -1);
  char *dest = g_strdup_printf ("%s/%s%s", dir, sum, ext);
  g_free (dir);
  return dest;
}

static gboolean
hydra_icon_flush (gpointer user_data)
{
  DashboardPage *page = user_data;
  hydra_icon_flush_id = 0;
  save_games_config (page);
  refresh_games_grid (page);
  return G_SOURCE_REMOVE;
}

static void
hydra_icon_schedule_flush (DashboardPage *page)
{
  if (hydra_icon_flush_id)
    return;
  hydra_icon_flush_id = g_timeout_add (400, hydra_icon_flush, page);
}

static void
hydra_icon_apply (HydraIconJob *job, gboolean ok)
{
  DashboardPage *page = job->page;

  if (ok)
    {
      for (int i = 0; i < page->n_games; i++)
        {
          if (g_strcmp0 (page->games[i].command, job->command) != 0)
            continue;

          const char *cur = page->games[i].icon;
          if (cur && cur[0] && g_strcmp0 (cur, "input-gaming-symbolic") != 0)
            break; /* the user picked another icon already */

          g_free (page->games[i].icon);
          page->games[i].icon = g_strdup (job->dest);
          hydra_icon_schedule_flush (page);
          break;
        }
    }

  hydra_icon_job_free (job);
}

static void
hydra_icon_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  HydraIconJob *job = user_data;

  g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
  g_object_unref (G_SUBPROCESS (source));
  hydra_icon_active--;

  gboolean ok = hydra_icon_file_ok (job->dest);
  if (!ok)
    unlink (job->dest); /* remove partial/error output, if any */

  hydra_icon_apply (job, ok);
  hydra_icon_start_next ();
}

static void
hydra_icon_start_next (void)
{
  if (!hydra_icon_queue) return;

  while (hydra_icon_active < HYDRA_ICON_MAX_PARALLEL)
    {
      HydraIconJob *job = g_queue_pop_head (hydra_icon_queue);
      if (!job) return;

      if (hydra_icon_file_ok (job->dest))
        {
          hydra_icon_apply (job, TRUE);
          continue;
        }

      if (!g_find_program_in_path ("curl"))
        {
          hydra_icon_job_free (job);
          continue;
        }

      const char *argv[] = { "curl", "-sfL", "--max-time", "20",
                             "-o", job->dest, job->url, NULL };

      GError *err = NULL;
      GSubprocessLauncher *launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
      GSubprocess *proc = g_subprocess_launcher_spawnv (launcher,
        (const char * const *) argv, &err);
      g_object_unref (launcher);

      if (!proc)
        {
          if (err) g_error_free (err);
          hydra_icon_job_free (job);
          continue;
        }

      hydra_icon_active++;
      g_subprocess_wait_async (proc, NULL, hydra_icon_done, job);
      return; /* resuming happens in hydra_icon_done() */
    }
}

/* Queue the icon of a Hydra game (by its launch command) for download. */
static void
hydra_queue_game_icon (DashboardPage *page, const char *command,
                       const char *icon_url)
{
  if (!icon_url || !g_str_has_prefix (icon_url, "http"))
    return;
  if (!command || !command[0])
    return;

  if (!hydra_icon_queue)
    hydra_icon_queue = g_queue_new ();

  /* Skip games that already show a real icon, and jobs already queued. */
  for (int i = 0; i < page->n_games; i++)
    {
      if (g_strcmp0 (page->games[i].command, command) != 0)
        continue;
      const char *cur = page->games[i].icon;
      if (cur && cur[0] && g_strcmp0 (cur, "input-gaming-symbolic") != 0)
        return;
    }
  for (GList *l = hydra_icon_queue->head; l; l = l->next)
    {
      HydraIconJob *j = l->data;
      if (g_strcmp0 (j->command, command) == 0)
        return;
    }

  HydraIconJob *job = g_new0 (HydraIconJob, 1);
  job->page = page;
  job->command = g_strdup (command);
  job->url = g_strdup (icon_url);
  job->dest = hydra_icon_dest_path (icon_url);
  g_queue_push_tail (hydra_icon_queue, job);

  hydra_icon_start_next ();
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
  if (g->icon_url && g->icon_url[0])
    hydra_queue_game_icon (ctx->page, cmd, g->icon_url);
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
              if (g_strv_length (f) >= 5 && f[4][0])
                hydra_queue_game_icon (page, cmd, f[4]);
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
