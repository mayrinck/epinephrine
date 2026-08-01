#include "epi-privilege.h"

static GSubprocess *helper = NULL;
static GOutputStream *helper_stdin = NULL;
static GDataInputStream *helper_stdout = NULL;
static gboolean init_attempted = FALSE;
static char *last_error = NULL;

gboolean
epi_privilege_is_available (void)
{
  return helper != NULL;
}

const char *
epi_privilege_get_error (void)
{
  return last_error;
}

gboolean
epi_privilege_init (GError **error)
{
  if (helper) return TRUE;

  /* The admin session is only requested once, at application start.
     A cancelled/denied prompt is never retried during the session. */
  if (init_attempted)
    {
      if (error && last_error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", last_error);
      return FALSE;
    }
  init_attempted = TRUE;

  if (!g_find_program_in_path ("pkexec"))
    {
      last_error = g_strdup ("pkexec is not installed");
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", last_error);
      return FALSE;
    }

  helper = g_subprocess_new (
    G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
    error,
    "pkexec", "sh", "-c",
    "while IFS= read -r line; do "
      "case $line in "
        "ASYNC:*) eval \"${line#ASYNC:}\" >/dev/null 2>&1 ;; "
        "*) eval \"$line\" 2>&1; echo \"EXIT:$?\"; "
      "esac; "
    "done",
    NULL);

  if (!helper)
    {
      if (error && *error)
        last_error = g_strdup ((*error)->message);
      else
        last_error = g_strdup ("pkexec failed to start");
      return FALSE;
    }

  helper_stdin = g_subprocess_get_stdin_pipe (helper);
  GInputStream *in = g_subprocess_get_stdout_pipe (helper);
  helper_stdout = g_data_input_stream_new (in);
  g_object_unref (in);

  return TRUE;
}

static gboolean
read_until_exit_marker (int *out_exit_code, char **out_stdout, GError **error)
{
  GString *output = g_string_new ("");
  int exit_code = -1;

  while (TRUE)
    {
      gsize len;
      char *line = g_data_input_stream_read_line (helper_stdout, &len, NULL, error);
      if (!line)
        {
          if (error && *error) break;
          g_string_free (output, TRUE);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
            "helper process died unexpectedly");
          return FALSE;
        }

      if (g_str_has_prefix (line, "EXIT:"))
        {
          exit_code = atoi (line + 5);
          g_free (line);
          break;
        }

      g_string_append (output, line);
      g_string_append_c (output, '\n');
      g_free (line);
    }

  if (out_stdout)
    *out_stdout = g_string_free (output, FALSE);
  else
    g_string_free (output, TRUE);

  if (out_exit_code) *out_exit_code = exit_code;
  return TRUE;
}

gboolean
epi_privilege_exec_sync (const char *cmd, int *out_exit_code,
                         char **out_stdout, GError **error)
{
  if (!helper && !epi_privilege_init (error))
    return FALSE;

  char *buf = g_strdup_printf ("%s\n", cmd);
  gboolean ok = g_output_stream_write_all (helper_stdin, buf, strlen (buf),
    NULL, NULL, error);
  g_free (buf);

  if (!ok)
    {
      epi_privilege_reset ();
      return FALSE;
    }

  return read_until_exit_marker (out_exit_code, out_stdout, error);
}

gboolean
epi_privilege_exec_async (const char *cmd)
{
  GError *err = NULL;
  if (!helper && !epi_privilege_init (&err))
    {
      if (err) g_error_free (err);
      return FALSE;
    }

  char *buf = g_strdup_printf ("ASYNC:%s\n", cmd);
  gboolean ok = g_output_stream_write_all (helper_stdin, buf, strlen (buf),
    NULL, NULL, NULL);
  g_free (buf);

  if (!ok)
    {
      epi_privilege_reset ();
      return FALSE;
    }

  return TRUE;
}

void
epi_privilege_reset (void)
{
  /* Tear the helper down but keep init_attempted set: the admin session
     is requested only once per application run. */
  if (helper)
    {
      if (helper_stdin)
        {
          const char *exit_cmd = "exit\n";
          g_output_stream_write_all (helper_stdin, exit_cmd, strlen (exit_cmd),
            NULL, NULL, NULL);
        }
      g_subprocess_wait (helper, NULL, NULL);
    }

  if (helper_stdin) { g_object_unref (helper_stdin); helper_stdin = NULL; }
  if (helper_stdout) { g_object_unref (helper_stdout); helper_stdout = NULL; }
  if (helper) { g_object_unref (helper); helper = NULL; }
}

void
epi_privilege_shutdown (void)
{
  epi_privilege_reset ();
  init_attempted = FALSE;
  g_clear_pointer (&last_error, g_free);
}
