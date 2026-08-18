#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct
{
  const char *title;
  const char *object_id;
  const char *shop;
  const char *exec_path;
  const char *icon_url;
} HydraGame;

/* Parse one LevelDB .ldb file's bytes. Calls cb for each game found.
 * Returns the number of games emitted, or 0 on parse failure. */
typedef void (*HydraGameCb) (const HydraGame *game, void *user_data);

int hydra_ldb_parse (const uint8_t *data, size_t len,
                     HydraGameCb cb, void *user_data);
