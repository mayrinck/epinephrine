#include "page-dashboard.h"
#include "game-artwork.h"
#include "../window.h"
#include "../epi-i18n.h"
#include <string.h>
#include <stdio.h>
#define tr(x) epi_i18n_get(x)
static const char *GAMES_CONFIG_PATH;
void
init_config_path (void)
{
  char *dir = g_build_filename (g_get_user_config_dir (), "epinephrine", NULL);
  g_mkdir_with_parents (dir, 0755);
  GAMES_CONFIG_PATH = g_build_filename (dir, "games.ini", NULL);
  g_free (dir);
}

static void
free_games (DashboardPage *page)
{
  for (int i = 0; i < page->n_games; i++)
    {
      g_free (page->games[i].id);
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

void
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
              page->games[page->n_games].id = g_key_file_get_string (keyfile, groups[i], "id", NULL);
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
  if (page->n_games) save_games_config (page); /* Persist legacy ID migration before loading art. */
}

void
save_games_config (DashboardPage *page)
{
  if (!GAMES_CONFIG_PATH)
    init_config_path ();

  GKeyFile *keyfile = g_key_file_new ();
  g_autoptr(GKeyFile) art = game_artwork_config_load ();

  for (int i = 0; i < page->n_games; i++)
    {
      char group[64];
      snprintf (group, sizeof (group), "game_%d", i);

      GameShortcut *game = &page->games[i];
      if (!game->id || !g_uuid_string_is_valid (game->id)) { g_free (game->id); game->id = g_uuid_string_random (); }
      g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, game->command, -1);
      g_autofree char *old = g_key_file_get_string (art, "identities", hash, NULL);
      if (!old) old = g_strdup (hash);
      if (g_strcmp0 (old, game->id) != 0 && g_key_file_has_group (art, old))
        {
          g_auto(GStrv) keys = g_key_file_get_keys (art, old, NULL, NULL);
          for (int k=0; keys[k]; k++) {
            g_autofree char *value = g_key_file_get_value (art, old, keys[k], NULL);
            g_key_file_set_value (art, game->id, keys[k], value);
          }
        }
      g_key_file_set_string (art, "identities", hash, game->id);
      g_key_file_set_string (keyfile, group, "id", game->id);
      g_key_file_set_string (keyfile, group, "name", page->games[i].name);
      g_key_file_set_string (keyfile, group, "command", page->games[i].command);
      g_key_file_set_string (keyfile, group, "icon", page->games[i].icon ? page->games[i].icon : "input-gaming-symbolic");
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
      if (!g_file_set_contents_full (GAMES_CONFIG_PATH, data, -1,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &err))
        {
          dashboard_show_error_toast (tr ("Could not save games."), err->message);
          g_clear_error (&err);
        }
      g_free (data);
    }
  else
    {
      g_warning ("Failed to save games config: %s", err->message);
      g_error_free (err);
    }

  game_artwork_config_save (art);
  g_key_file_free (keyfile);
}

void
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
  g->id = g_uuid_string_random ();
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
