/* nautilus-file-converter: logging and child process setup. See nfc-util.h. */
#include "nfc-util.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */

#define NFC_LOG_MAX_BYTES   (1024 * 1024)
#define NFC_LOG_ENTRY_BYTES (64 * 1024) /* longest single entry; the rest is cut */

static GMutex log_lock;

/* Open (creating with 0600) and tighten the permissions of an existing file. */
static int
log_open (const char *path)
{
  struct stat st;
  int fd = g_open (path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);

  if (fd < 0)
    return -1;
  if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
    {
      close (fd);
      return -1;
    }
  if ((st.st_mode & 07777 & ~0600) != 0)
    fchmod (fd, 0600);
  return fd;
}

void
nfc_log (const char *format, ...)
{
  va_list ap;
  char *msg, *dir, *path, *ts, *line;
  GDateTime *now;
  int fd;

  va_start (ap, format);
  msg = g_strdup_vprintf (format, ap);
  va_end (ap);

  now = g_date_time_new_now_local ();
  ts = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
  line = g_strdup_printf ("[%s] %s\n", ts, msg);
  if (strlen (line) > NFC_LOG_ENTRY_BYTES)
    {
      /* never write more than NFC_LOG_ENTRY_BYTES for one entry: keep the head
       * (what happened) and the tail (usually the useful end of a tool's output),
       * cutting the middle on UTF-8 character boundaries */
      const char *note = "\n[... entry truncated ...]\n";
      gsize total = strlen (line), head = 8 * 1024;
      gsize tail = NFC_LOG_ENTRY_BYTES - head - strlen (note) - 1;
      const char *tp = line + total - tail;
      char *cut;

      while (head > 0 && (line[head] & 0xC0) == 0x80)
        head--;
      while (*tp != '\0' && (*tp & 0xC0) == 0x80)
        tp++;
      cut = g_strdup_printf ("%.*s%s%s", (int) head, line, note, tp);
      g_free (line);
      line = cut;
    }
  dir = g_strdup (g_get_user_cache_dir ());
  path = g_build_filename (dir, "nautilus-file-converter.log", NULL);

  g_mutex_lock (&log_lock);
  g_mkdir_with_parents (dir, 0700);
  fd = log_open (path);
  if (fd >= 0)
    {
      struct stat st;

      /* rotate when this entry would push the file past the limit */
      if (fstat (fd, &st) == 0 && st.st_size > 0 &&
          (gint64) st.st_size + (gint64) strlen (line) > NFC_LOG_MAX_BYTES)
        {
          /* rotate: one backup, replaced each time */
          char *bak = g_strconcat (path, ".1", NULL);

          g_rename (path, bak);
          g_free (bak);
          close (fd);
          fd = log_open (path);
        }
    }
  if (fd >= 0)
    {
      size_t len = strlen (line), off = 0;

      while (off < len)
        {
          ssize_t w = write (fd, line + off, len - off);

          if (w < 0 && errno == EINTR)
            continue;
          if (w <= 0)
            break;
          off += (size_t) w;
        }
      close (fd);
    }
  g_mutex_unlock (&log_lock);

  g_date_time_unref (now);
  g_free (ts);
  g_free (line);
  g_free (path);
  g_free (dir);
  g_free (msg);
}

/* -------------------------------------------------------------------------
 * Child processes
 * ---------------------------------------------------------------------- */

/* Runs in the forked child before exec: only async-signal-safe calls. */
static void
child_setup (gpointer data)
{
  setsid (); /* own session and process group (pgid == pid) */
#ifdef __linux__
  /* Die with the parent. PDEATHSIG is tied to the parent THREAD that forked
   * us: GSubprocess forks from the calling thread, which for conversions is
   * the Nautilus main thread (lives as long as the process). */
  prctl (PR_SET_PDEATHSIG, SIGKILL);
  /* the parent may have died between fork and prctl */
  if (getppid () != (pid_t) GPOINTER_TO_INT (data))
    _exit (1);
#else
  (void) data;
#endif
}

GSubprocessLauncher *
nfc_launcher_new (GSubprocessFlags flags)
{
  GSubprocessLauncher *l = g_subprocess_launcher_new (flags);

  g_subprocess_launcher_set_stdin_file_path (l, "/dev/null");
  g_subprocess_launcher_set_child_setup (l, child_setup, GINT_TO_POINTER (getpid ()), NULL);
  return l;
}

pid_t
nfc_subprocess_pgid (GSubprocess *proc)
{
  const char *id = proc != NULL ? g_subprocess_get_identifier (proc) : NULL;
  gint64 v = id != NULL ? g_ascii_strtoll (id, NULL, 10) : 0;

  return v > 1 ? (pid_t) v : 0;
}

void
nfc_kill_group (pid_t pgid)
{
  if (pgid > 1)
    kill (-pgid, SIGKILL);
}

/* Recursive removal (best effort, never follows symlinks). */
void
nfc_rm_rf (const char *path)
{
  GDir *d;
  const char *name;

  if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    {
      d = g_dir_open (path, 0, NULL);
      if (d != NULL)
        {
          while ((name = g_dir_read_name (d)) != NULL)
            {
              char *child = g_build_filename (path, name, NULL);

              nfc_rm_rf (child);
              g_free (child);
            }
          g_dir_close (d);
        }
      g_rmdir (path);
    }
  else
    g_unlink (path);
}
