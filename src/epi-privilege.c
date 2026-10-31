#include "epi-privilege.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

static GSubprocess *helper = NULL;
static GRecMutex session_lock;
static GCancellable *io_cancel;
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
  g_autoptr(GRecMutexLocker) lock = g_rec_mutex_locker_new (&session_lock);
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

  char *pkexec_path = g_find_program_in_path ("pkexec");
  if (!pkexec_path)
    {
      last_error = g_strdup ("pkexec is not installed");
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s", last_error);
      return FALSE;
    }

  /* Only the installed, root-owned helper may be elevated. Development
     checkouts and writable installation prefixes are never used as root. */
  char *check = g_strdup (LIBEXECDIR "/epinephrine/epi-admin");
  gboolean trusted = TRUE;
  while (TRUE)
    {
      GStatBuf info;
      if (g_file_test (check, G_FILE_TEST_IS_SYMLINK) || g_stat (check, &info) != 0 ||
          info.st_uid != 0 || (info.st_mode & 0022)) { trusted = FALSE; break; }
      if (g_strcmp0 (check, "/") == 0) break;
      char *parent = g_path_get_dirname (check); g_free (check); check = parent;
    }
  g_free (check);
  if (!trusted)
    {
      g_free (pkexec_path);
      last_error = g_strdup ("Install the root-owned epi-admin helper with meson install before using administrative features.");
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, last_error);
      return FALSE;
    }

  helper = g_subprocess_new (
    G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
    error,
    pkexec_path, LIBEXECDIR "/epinephrine/epi-admin",
    NULL);
  g_free (pkexec_path);

  if (!helper)
    {
      if (error && *error)
        last_error = g_strdup ((*error)->message);
      else
        last_error = g_strdup ("pkexec failed to start");
      return FALSE;
    }

  io_cancel = g_cancellable_new ();
  helper_stdin = g_object_ref (g_subprocess_get_stdin_pipe (helper));
  GInputStream *in = g_subprocess_get_stdout_pipe (helper);
  helper_stdout = g_data_input_stream_new (in);
  /* get_stdout_pipe returns a borrowed reference owned by helper. */

  return TRUE;
}

static gboolean
read_until_exit_marker (const char *tag, int *out_exit_code,
                        char **out_stdout, GError **error)
{
  GString *output = g_string_new ("");
  int exit_code = -1;
  char *marker = g_strdup_printf ("EXIT:%s:", tag);

  while (TRUE)
    {
      gsize len;
      char *line = g_data_input_stream_read_line (helper_stdout, &len, io_cancel, error);
      if (!line)
        {
          if (error && *error)
            { g_string_free (output, TRUE); g_free (marker); return FALSE; }
          g_string_free (output, TRUE);
          g_free (marker);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
            "helper process died unexpectedly");
          return FALSE;
        }

      if (g_str_has_prefix (line, marker))
        {
          exit_code = atoi (line + strlen (marker));
          g_free (line);
          break;
        }

      g_string_append (output, line);
      g_string_append_c (output, '\n');
      g_free (line);
    }

  g_free (marker);

  if (out_stdout)
    *out_stdout = g_string_free (output, FALSE);
  else
    g_string_free (output, TRUE);

  if (out_exit_code) *out_exit_code = exit_code;
  return TRUE;
}

static gboolean
privilege_send (const char *cmd, GError **error)
{
  if (strchr (cmd, '\n'))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "privileged command must not contain newlines");
      return FALSE;
    }

  char *buf = g_strdup_printf ("%s\n", cmd);
  gboolean ok = g_output_stream_write_all (helper_stdin, buf, strlen (buf),
    NULL, io_cancel, error);
  g_free (buf);

  if (!ok)
    {
      epi_privilege_reset ();
      return FALSE;
    }
  return TRUE;
}

gboolean
epi_privilege_exec_sync (const char *cmd, int *out_exit_code,
                         char **out_stdout, GError **error)
{
  g_autoptr(GRecMutexLocker) lock = g_rec_mutex_locker_new (&session_lock);
  if (!helper && !epi_privilege_init (error))
    return FALSE;

  char *tag = g_strdup_printf ("%08lX", (unsigned long) g_random_int ());
  char *buf = g_strdup_printf ("SYNC:%s %s", tag, cmd);
  if (!privilege_send (buf, error))
    {
      g_free (tag);
      g_free (buf);
      return FALSE;
    }
  g_free (buf);

  gboolean ok = read_until_exit_marker (tag, out_exit_code, out_stdout, error);
  g_free (tag);
  if (!ok) epi_privilege_reset ();
  return ok;
}

gboolean
epi_privilege_exec_async (const char *cmd)
{
  g_autoptr(GRecMutexLocker) lock = g_rec_mutex_locker_new (&session_lock);
  GError *err = NULL;
  if (!helper && !epi_privilege_init (&err))
    {
      if (err) g_error_free (err);
      return FALSE;
    }

  char *buf = g_strdup_printf ("ASYNC:%s", cmd);
  gboolean ok = privilege_send (buf, NULL);
  g_free (buf);
  return ok;
}

void
epi_privilege_reset (void)
{
  g_autoptr(GRecMutexLocker) lock = g_rec_mutex_locker_new (&session_lock);
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
      g_subprocess_force_exit (helper);
    }

  if (helper_stdin) { g_object_unref (helper_stdin); helper_stdin = NULL; }
  if (helper_stdout) { g_object_unref (helper_stdout); helper_stdout = NULL; }
  if (helper) { g_object_unref (helper); helper = NULL; }
}

void
epi_privilege_shutdown (void)
{
  if (io_cancel) g_cancellable_cancel (io_cancel);
  g_autoptr(GRecMutexLocker) lock = g_rec_mutex_locker_new (&session_lock);
  epi_privilege_reset ();
  init_attempted = FALSE;
  g_clear_object (&io_cancel);
  g_clear_pointer (&last_error, g_free);
}
