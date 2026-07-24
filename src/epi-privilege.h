#pragma once

#include <gio/gio.h>

gboolean epi_privilege_init (GError **error);

gboolean epi_privilege_exec_sync (const char *cmd, int *out_exit_code,
                                  char **out_stdout, GError **error);

gboolean epi_privilege_exec_async (const char *cmd);

void epi_privilege_reset (void);

void epi_privilege_shutdown (void);
