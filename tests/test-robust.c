/* Robustness tests: nothing may block the main thread, and nothing may hang or
 * leak when a tool misbehaves.
 *
 * Hermetic: every external tool is a fake shell script created here. The whole
 * process runs with TMPDIR set to a directory whose name contains a space and
 * a '%' ("sp ace%20"), so every temp-path use is also tested with a hostile
 * name. No Python; fake tools are /bin/sh.
 *
 *  - probe-freeze: a fake ffmpeg that sleeps 20 s must not slow the menu logic
 *    (nfc_analyze_selection returns in < 100 ms for every selection), must not
 *    hide image targets, and is killed (with its whole process group) by the
 *    probe timeout and logged.
 *  - soffice-timeout: a fake soffice `sh -c 'sleep 30 & exec sleep 30'` (the
 *    grandchild holds the pipes) fails within timeout + 2 s; nothing from its
 *    process group survives; the work directory is removed.
 *  - ffmpeg timeout: partial output and hidden temp files are cleaned up, and a
 *    timeout does not start the fallback attempt.
 *  - shutdown: in-flight jobs are killed and cleaned up.
 *  - log: mode 0600, rotation at 1 MiB.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nfc-convert.h"
#include "nfc-core.h"

#define TEST_TIMEOUT 2        /* NFC_TEST_TIMEOUT_SECS for steps */
#define TEST_PROBE_TIMEOUT 3  /* NFC_TEST_PROBE_TIMEOUT_SECS */

static char *base;     /* private TMPDIR, "... sp ace%20 ..." */
static char *bindir;   /* fake tools first in PATH */
static char *datadir;  /* user files */

static char *
B (const char *dir, const char *name)
{
  return g_build_filename (dir, name, NULL);
}

static void
write_script (const char *path, const char *body)
{
  char *content = g_strconcat ("#!/bin/sh\n", body, NULL);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  g_assert_cmpint (g_chmod (path, 0755), ==, 0);
  g_free (content);
}

static void
touch (const char *dir, const char *name)
{
  char *p = B (dir, name);

  g_assert_true (g_file_set_contents (p, "x", -1, NULL));
  g_free (p);
}

/* is the process gone (or a zombie awaiting its reaper)? */
static gboolean
pid_gone (long pid)
{
  char *path = g_strdup_printf ("/proc/%ld/stat", pid);
  char *data = NULL;
  gboolean gone;

  if (!g_file_get_contents (path, &data, NULL, NULL))
    gone = TRUE;
  else
    {
      const char *rp = strrchr (data, ')');

      gone = rp != NULL && rp[1] == ' ' && rp[2] == 'Z';
    }
  g_free (data);
  g_free (path);
  return gone;
}

static gboolean
wait_pid_gone (long pid, int secs)
{
  for (int i = 0; i < secs * 20; i++)
    {
      if (pid_gone (pid))
        return TRUE;
      g_usleep (50000);
    }
  return pid_gone (pid);
}

static GArray *
read_pids (const char *file)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (long));
  char *data = NULL;

  if (g_file_get_contents (file, &data, NULL, NULL))
    {
      char **lines = g_strsplit (data, "\n", -1);

      for (char **l = lines; *l; l++)
        if (**l)
          {
            long v = strtol (*l, NULL, 10);

            g_array_append_val (a, v);
          }
      g_strfreev (lines);
    }
  g_free (data);
  return a;
}

static guint
count_entries (const char *dir, const char *prefix)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *n;
  guint c = 0;

  while (d && (n = g_dir_read_name (d)))
    if (g_str_has_prefix (n, prefix))
      c++;
  if (d)
    g_dir_close (d);
  return c;
}

/* ---- synchronous wrapper around the async API, with a watchdog ---- */
typedef struct {
  GMainLoop *loop;
  char *out;
  GError *error;
} Wait;

static void
on_done (GObject *o G_GNUC_UNUSED, GAsyncResult *res, gpointer data)
{
  Wait *w = data;

  w->out = nfc_convert_file_finish (res, &w->error);
  g_main_loop_quit (w->loop);
}

static gboolean
watchdog (gpointer data G_GNUC_UNUSED)
{
  g_error ("watchdog: a conversion did not finish in time (the main loop was stuck)");
  return G_SOURCE_REMOVE;
}

static char *
convert_sync (NfcCategory cat, const char *fmt, const char *key, const char *src,
              char **err, gint64 *elapsed_us)
{
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  gint64 t0 = g_get_monotonic_time ();
  guint wd = g_timeout_add_seconds (40, watchdog, NULL);
  char *out;

  nfc_convert_file_async (cat, fmt, key, key, src, on_done, &w);
  g_main_loop_run (w.loop);
  g_source_remove (wd);
  g_main_loop_unref (w.loop);
  if (elapsed_us)
    *elapsed_us = g_get_monotonic_time () - t0;
  out = w.out;
  if (err)
    *err = w.error ? g_strdup (w.error->message) : NULL;
  g_clear_error (&w.error);
  return out;
}

/* ------------------------------------------------------------------------ */

static gboolean
analyze (const char *name, const char *mime, gint64 *us, GString *labels)
{
  char *p = B (datadir, name);
  char *uri = g_filename_to_uri (p, NULL, NULL);
  NfcSelItem it = { uri, mime, FALSE };
  NfcSelection sel;
  gint64 t0 = g_get_monotonic_time ();
  gboolean ok = nfc_analyze_selection (&it, 1, &sel);

  *us = g_get_monotonic_time () - t0;
  if (labels != NULL)
    {
      g_string_truncate (labels, 0);
      if (ok)
        for (guint i = 0; i < sel.targets->len; i++)
          g_string_append_printf (labels, "%s,", ((const NfcTarget *) sel.targets->pdata[i])->label);
    }
  nfc_selection_clear (&sel);
  g_free (uri);
  g_free (p);
  return ok;
}

static char *
read_log (void)
{
  char *p = B (g_get_user_cache_dir (), "nautilus-file-converter.log");
  char *data = NULL;

  if (!g_file_get_contents (p, &data, NULL, NULL))
    data = g_strdup ("");
  g_free (p);
  return data;
}

/* QC finding 1: a hung `ffmpeg -encoders` used to freeze the right-click menu
 * for the whole hang (20 s in the QC probe, forever for a truly hung tool). */
static void
test_probe_freeze (void)
{
  char *ff_pids = B (base, "ffmpeg.pids");
  char *body;
  GString *labels = g_string_new (NULL);
  gint64 us, t_start;
  gboolean ok;
  GArray *pids;
  char *log;

  /* fake tools, first in PATH (the tool lookup happens in nfc_tools_init) */
  body = g_strdup_printf ("echo $$ > '%s'\nsleep 20 &\necho $! >> '%s'\nwait\n", ff_pids, ff_pids);
  { char *p = B (bindir, "ffmpeg"); write_script (p, body); g_free (p); }
  g_free (body);
  {
    char *p = B (bindir, "magick");

    write_script (p,
                  "if [ \"$1\" = -list ]; then\n"
                  "echo '   Format  Module    Mode  Description'\n"
                  "echo '-----------------------------------------'\n"
                  "for f in 'PNG* PNG' 'JPEG* JPEG' 'WEBP* WEBP' 'ICO* ICO' 'BMP* BMP' 'TIFF* TIFF' 'PDF* PDF' 'RSVG* SVG'; do\n"
                  "  echo \"      $f       rw-   fake\"\n"
                  "done\nfi\n");
    g_free (p);
    p = B (bindir, "soffice");
    write_script (p, "exit 0\n");
    g_free (p);
  }
  touch (datadir, "a.txt");
  touch (datadir, "b.png");
  touch (datadir, "c.mp4");
  touch (datadir, "h.heic");

  t_start = g_get_monotonic_time ();
  nfc_tools_init ();
  g_assert_nonnull (nfc_tools.ffmpeg);
  g_assert_true (g_str_has_prefix (nfc_tools.ffmpeg, bindir));

  /* documents need no probe: a menu at once, and the very first call (which
   * starts the probes) does not wait for anything */
  ok = analyze ("a.txt", NULL, &us, labels);
  g_print ("txt: %s in %" G_GINT64_FORMAT " us (%s)\n", ok ? "menu" : "no menu", us, labels->str);
  g_assert_true (ok);
  g_assert_cmpint (us, <, 100000);
  g_assert_nonnull (strstr (labels->str, "PDF"));

  /* images: whatever the state of the probes, never slow */
  analyze ("b.png", NULL, &us, labels);
  g_assert_cmpint (us, <, 100000);
  analyze ("c.mp4", NULL, &us, labels);
  g_assert_cmpint (us, <, 100000);

  /* the magick probe finishes quickly and must not depend on the hung ffmpeg */
  for (int i = 0; i < 200 && !nfc_probe_done (NFC_PROBE_MAGICK); i++)
    {
      analyze ("c.mp4", NULL, &us, labels);
      g_assert_cmpint (us, <, 100000);
      g_usleep (10000);
    }
  g_assert_true (nfc_probe_done (NFC_PROBE_MAGICK));
  g_assert_false (nfc_probe_done (NFC_PROBE_FFMPEG)); /* still hanging */
  ok = analyze ("b.png", NULL, &us, labels);
  g_print ("png while ffmpeg hangs: %s in %" G_GINT64_FORMAT " us (%s)\n", ok ? "menu" : "no menu",
           us, labels->str);
  g_assert_true (ok);
  g_assert_cmpint (us, <, 100000);
  g_assert_nonnull (strstr (labels->str, "JPG"));
  g_assert_null (strstr (labels->str, "AVIF")); /* needs ffmpeg: not known yet */
  /* video: ffmpeg not known yet -> no menu, immediately */
  ok = analyze ("c.mp4", NULL, &us, labels);
  g_assert_false (ok);
  g_assert_cmpint (us, <, 100000);
  /* document menus are unaffected the whole time */
  g_assert_true (analyze ("a.txt", NULL, &us, labels));
  g_assert_cmpint (us, <, 100000);

  /* the probe timeout kills the hung ffmpeg (its whole process group) */
  for (int i = 0; i < 20 * (TEST_PROBE_TIMEOUT + 5) && !nfc_probe_done (NFC_PROBE_FFMPEG); i++)
    g_usleep (50000);
  g_assert_true (nfc_probe_done (NFC_PROBE_FFMPEG));
  us = g_get_monotonic_time () - t_start;
  g_print ("ffmpeg probe gave up after %.1f s\n", us / 1e6);
  g_assert_cmpint (us, >=, (TEST_PROBE_TIMEOUT - 1) * G_USEC_PER_SEC);
  g_assert_cmpint (us, <, (TEST_PROBE_TIMEOUT + 4) * G_USEC_PER_SEC);
  pids = read_pids (ff_pids);
  g_assert_cmpuint (pids->len, ==, 2);
  for (guint i = 0; i < pids->len; i++)
    g_assert_true (wait_pid_gone (g_array_index (pids, long, i), 3));
  g_array_free (pids, TRUE);

  /* timed-out probe == tool unavailable for the session, and it was logged */
  g_assert_false (nfc_have_encoder ("libx264"));
  g_assert_false (analyze ("c.mp4", NULL, &us, labels));
  g_assert_cmpint (us, <, 100000);
  ok = analyze ("b.png", NULL, &us, labels);
  g_assert_true (ok);
  g_assert_nonnull (strstr (labels->str, "JPG"));
  g_assert_null (strstr (labels->str, "AVIF"));
  log = read_log ();
  g_assert_nonnull (strstr (log, "probe ffmpeg"));
  g_assert_nonnull (strstr (log, "did not answer within 3 s"));
  g_free (log);

  /* HEIC/AVIF input needs the helper: without it nothing is offered */
  nfc_probe_ensure ();
  /* SVG input goes through librsvg in-process (RSVG:), never the plain SVG: coder
   * (which may delegate to Inkscape) */
  {
    gboolean dec;
    GPtrArray *at = nfc_build_attempts (NFC_CAT_IMAGE, "svg", "png", "/s/a.svg", "/o/x.png", "/w", &dec);
    char **argv;

    g_assert_true (nfc_magick_can_read ("RSVG"));
    g_assert_nonnull (at);
    argv = ((GPtrArray *) at->pdata[0])->pdata[0];
    g_assert_true (g_strv_contains ((const char * const *) argv, "RSVG:/s/a.svg[0]"));
    g_assert_false (g_strv_contains ((const char * const *) argv, "SVG:/s/a.svg[0]"));
    g_ptr_array_unref (at);
  }
  {
    GPtrArray *t = nfc_targets_for (NFC_CAT_IMAGE, "heic");
    guint with_helper = t->len;
    char *saved = nfc_tools.decode;

    g_ptr_array_unref (t);
    if (saved != NULL)
      {
        g_assert_true (nfc_pixbuf_can_read ("heic"));
        g_assert_cmpuint (with_helper, >, 0);
      }
    nfc_tools.decode = NULL;
    t = nfc_targets_for (NFC_CAT_IMAGE, "heic");
    g_assert_cmpuint (t->len, ==, 0);
    g_ptr_array_unref (t);
    t = nfc_targets_for (NFC_CAT_IMAGE, "avif");
    g_assert_cmpuint (t->len, ==, 0);
    g_ptr_array_unref (t);
    nfc_tools.decode = saved;
  }
  g_string_free (labels, TRUE);
  g_free (ff_pids);
}

/* QC finding 2: the soffice wrapper script leaves soffice.bin holding our
 * pipes; the timeout must still end the job, and kill the whole tree. */
static void
test_soffice_timeout (void)
{
  char *pidfile = B (base, "soffice.pids");
  char *script = B (bindir, "soffice-hang");
  char *body, *src, *err = NULL, *out;
  char *saved = nfc_tools.soffice;
  gint64 us;
  GArray *pids;
  guint before = count_entries (base, "nfc-");

  body = g_strdup_printf ("sleep 30 &\necho $! > '%s'\necho $$ >> '%s'\nexec sleep 30\n", pidfile, pidfile);
  write_script (script, body);
  g_free (body);
  nfc_tools.soffice = script;
  touch (datadir, "doc.txt");
  src = B (datadir, "doc.txt");

  out = convert_sync (NFC_CAT_DOC, "txt", "pdf", src, &err, &us);
  g_print ("soffice hang: failed after %.2f s: %s\n", us / 1e6, err ? err : "(no error)");
  g_assert_null (out);
  g_assert_cmpstr (err, ==, "timed out");
  g_assert_cmpint (us, >=, (TEST_TIMEOUT - 1) * G_USEC_PER_SEC);
  g_assert_cmpint (us, <, (TEST_TIMEOUT + 2) * G_USEC_PER_SEC);
  pids = read_pids (pidfile);
  g_assert_cmpuint (pids->len, ==, 2); /* the grandchild and the script's own pid */
  for (guint i = 0; i < pids->len; i++)
    g_assert_true (wait_pid_gone (g_array_index (pids, long, i), 3));
  g_array_free (pids, TRUE);
  g_assert_cmpuint (count_entries (base, "nfc-"), ==, before); /* workdir removed */
  g_assert_cmpuint (count_entries (datadir, "doc."), ==, 1);   /* only the source */

  nfc_tools.soffice = saved;
  g_free (err); g_free (out); g_free (src); g_free (script); g_free (pidfile);
}

/* a hung ffmpeg that already wrote partial output */
static void
test_ffmpeg_timeout_cleanup (void)
{
  char *script = B (bindir, "ffmpeg-partial");
  char *saved = nfc_tools.ffmpeg, *err = NULL, *out, *src, *dir;
  gint64 us;
  char *outdir;

  write_script (script, "for a; do last=\"$a\"; done\nprintf partial > \"$last\"\nsleep 30 &\nwait\n");
  nfc_tools.ffmpeg = script;
  outdir = B (datadir, "vids");
  g_assert_cmpint (g_mkdir_with_parents (outdir, 0755), ==, 0);
  touch (outdir, "clip.mp4");
  src = B (outdir, "clip.mp4");
  dir = outdir;

  /* mp4 -> mkv has two attempts (stream copy, then re-encode): a timeout is
   * final, so the fallback must not run (that would take 2 x timeout) */
  out = convert_sync (NFC_CAT_VIDEO, "mp4", "mkv", src, &err, &us);
  g_print ("ffmpeg hang: failed after %.2f s: %s\n", us / 1e6, err ? err : "(no error)");
  g_assert_null (out);
  g_assert_cmpstr (err, ==, "timed out");
  g_assert_cmpint (us, <, (TEST_TIMEOUT + 2) * G_USEC_PER_SEC);
  g_assert_cmpuint (count_entries (dir, "."), ==, 0);        /* no hidden .nfc- file */
  g_assert_cmpuint (count_entries (dir, "clip.mkv"), ==, 0); /* no partial output */
  g_assert_cmpuint (count_entries (dir, "clip.mp4"), ==, 1);

  /* a failing (not hanging) tool: same cleanup */
  write_script (script, "for a; do last=\"$a\"; done\nprintf partial > \"$last\"\necho boom >&2\nexit 3\n");
  g_free (err);
  out = convert_sync (NFC_CAT_AUDIO, "mp4", "mp3", src, &err, &us);
  g_assert_null (out);
  g_assert_cmpstr (err, ==, "boom");
  g_assert_cmpuint (count_entries (dir, "."), ==, 0);
  g_assert_cmpuint (count_entries (dir, "clip.mp3"), ==, 0);

  nfc_tools.ffmpeg = saved;
  g_free (err); g_free (out); g_free (src); g_free (script); g_free (outdir);
}

static gsize
file_size (const char *p)
{
  struct stat st;

  return stat (p, &st) == 0 ? (gsize) st.st_size : 0;
}

/* peak resident memory of this process in KiB (VmHWM), 0 if unknown */
static long
peak_rss_kib (void)
{
  char *data = NULL, *p;
  long v = 0;

  if (g_file_get_contents ("/proc/self/status", &data, NULL, NULL) &&
      (p = strstr (data, "VmHWM:")) != NULL)
    v = strtol (p + 6, NULL, 10);
  g_free (data);
  return v;
}

/* a tool that prints 10 MB to stderr (and to stdout) and fails: the job fails
 * with the LAST stderr line as the message, memory stays bounded, and the log
 * grows by at most one capped entry (about 64 KiB), not by 10 MB. */
static void
test_output_flood (void)
{
  char *log = B (g_get_user_cache_dir (), "nautilus-file-converter.log");
  char *script = B (bindir, "ffmpeg-flood"), *src, *err = NULL, *out;
  char *saved = nfc_tools.ffmpeg;
  gsize before, grown;
  long rss0, rss1;
  gint64 us;

  /* many short lines, then one endless line, then the real last line */
  write_script (script,
                "yes 'noisy stderr line, not the last one' | head -c 5242880 >&2\n"
                "head -c 5242880 /dev/zero | tr '\\0' 'x' >&2\n"
                "echo >&2\n"
                "yes 'noisy stdout line' | head -c 5242880\n"
                "echo 'the very last stderr line' >&2\n"
                "exit 1\n");
  nfc_tools.ffmpeg = script;
  touch (datadir, "flood.mp4");
  src = B (datadir, "flood.mp4");
  before = file_size (log);
  rss0 = peak_rss_kib ();

  out = convert_sync (NFC_CAT_AUDIO, "mp4", "mp3", src, &err, &us);
  rss1 = peak_rss_kib ();
  grown = file_size (log) - before;
  g_print ("flood: failed in %.2f s: %s; log grew by %" G_GSIZE_FORMAT " bytes; peak RSS +%ld KiB\n",
           us / 1e6, err ? err : "(no error)", grown, rss1 - rss0);
  g_assert_null (out);
  g_assert_cmpstr (err, ==, "the very last stderr line"); /* the toast text */
  g_assert_cmpuint (grown, >, 1000);                      /* it was logged ... */
  g_assert_cmpuint (grown, <=, 64 * 1024);                /* ... capped */
  {
    char *data = NULL, *tail;

    g_assert_true (g_file_get_contents (log, &data, NULL, NULL));
    tail = strstr (data + before, "the very last stderr line");
    g_assert_nonnull (tail);
    g_assert_nonnull (strstr (data + before, "earlier bytes omitted"));
    g_free (data);
  }
  if (rss0 > 0 && g_getenv ("NFC_TEST_SANITIZED") == NULL)
    g_assert_cmpint (rss1 - rss0, <, 16 * 1024); /* 10 MB of output kept in memory would exceed it */

  /* a single very long line is shortened in the toast text too */
  write_script (script, "head -c 2000000 /dev/zero | tr '\\0' 'y' >&2\nexit 1\n");
  g_free (err);
  out = convert_sync (NFC_CAT_AUDIO, "mp4", "mp3", src, &err, &us);
  g_assert_null (out);
  g_assert_nonnull (err);
  g_assert_cmpuint (strlen (err), <, 400);

  nfc_tools.ffmpeg = saved;
  g_free (err); g_free (out); g_free (src); g_free (script); g_free (log);
}

/* module shutdown: in-flight jobs are killed and their files removed */
static void
test_cancel_on_shutdown (void)
{
  char *pidfile = B (base, "shutdown.pids");
  char *script = B (bindir, "soffice-hang2");
  char *body, *src;
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  Wait w2 = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  char *ffscript = B (bindir, "ffmpeg-partial2"), *shutdir = B (datadir, "shut"), *vsrc;
  char *saved_ff = nfc_tools.ffmpeg;
  guint wd = g_timeout_add_seconds (40, watchdog, NULL);
  GArray *pids;
  guint before = count_entries (base, "nfc-");
  gint64 t0 = g_get_monotonic_time ();

  body = g_strdup_printf ("sleep 30 &\necho $! > '%s'\necho $$ >> '%s'\nexec sleep 30\n", pidfile, pidfile);
  write_script (script, body);
  g_free (body);
  g_free (nfc_tools.soffice);
  nfc_tools.soffice = script;
  src = B (datadir, "doc.txt");

  nfc_convert_file_async (NFC_CAT_DOC, "txt", "pdf", "pdf", src, on_done, &w);
  /* let the fake soffice start */
  for (int i = 0; i < 100; i++)
    {
      g_main_context_iteration (NULL, FALSE);
      pids = read_pids (pidfile);
      if (pids->len == 2)
        {
          g_array_free (pids, TRUE);
          break;
        }
      g_array_free (pids, TRUE);
      g_usleep (20000);
    }
  /* a second job in flight: a video whose fake ffmpeg wrote partial output to
   * the hidden temp file next to the source and then hangs */
  nfc_tools.ffmpeg = ffscript;
  write_script (ffscript, "for a; do last=\"$a\"; done\nprintf partial > \"$last\"\nsleep 30 &\nwait\n");
  g_assert_cmpint (g_mkdir_with_parents (shutdir, 0755), ==, 0);
  touch (shutdir, "clip.mp4");
  vsrc = B (shutdir, "clip.mp4");
  nfc_convert_file_async (NFC_CAT_VIDEO, "mp4", "mkv", "mkv", vsrc, on_done, &w2);
  for (int i = 0; i < 100 && count_entries (shutdir, ".") == 0; i++)
    {
      g_main_context_iteration (NULL, FALSE);
      g_usleep (20000);
    }
  g_assert_cmpuint (count_entries (shutdir, "."), ==, 1); /* the hidden temp file exists */
  g_assert_cmpuint (count_entries (base, "nfc-"), ==, before + 2); /* both workdirs exist */
  nfc_shutdown ();
  g_assert_cmpuint (count_entries (base, "nfc-"), ==, before);     /* ... and are gone */
  g_assert_cmpuint (count_entries (shutdir, "."), ==, 0);          /* hidden temp file removed */
  while ((w.out == NULL && w.error == NULL) || (w2.out == NULL && w2.error == NULL))
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (wd);
  g_assert_null (w.out);
  g_assert_nonnull (w.error);
  g_assert_cmpstr (w.error->message, ==, "cancelled");
  g_assert_null (w2.out);
  g_assert_nonnull (w2.error);
  g_assert_cmpstr (w2.error->message, ==, "cancelled");
  g_assert_cmpuint (count_entries (shutdir, "."), ==, 0);
  g_assert_cmpuint (count_entries (shutdir, "clip.mkv"), ==, 0);
  g_clear_error (&w2.error);
  g_main_loop_unref (w2.loop);
  nfc_tools.ffmpeg = saved_ff;
  g_free (vsrc); g_free (shutdir); g_free (ffscript);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 10 * G_USEC_PER_SEC);
  pids = read_pids (pidfile);
  g_assert_cmpuint (pids->len, ==, 2);
  for (guint i = 0; i < pids->len; i++)
    g_assert_true (wait_pid_gone (g_array_index (pids, long, i), 3));
  g_array_free (pids, TRUE);
  g_assert_cmpuint (count_entries (base, "nfc-"), ==, before);

  /* after shutdown no new job starts */
  {
    char *err = NULL, *out = convert_sync (NFC_CAT_DOC, "txt", "pdf", src, &err, NULL);

    g_assert_null (out);
    g_assert_nonnull (err);
    g_free (err);
  }
  g_clear_error (&w.error);
  g_main_loop_unref (w.loop);
  g_free (src);
  nfc_tools.soffice = NULL; /* the script path is freed below */
  g_free (script);
  g_free (pidfile);
}

static mode_t
file_mode (const char *p)
{
  struct stat st;

  g_assert_cmpint (stat (p, &st), ==, 0);
  return st.st_mode & 07777;
}

static void
test_log_mode_and_rotation (void)
{
  char *dir = g_strdup (g_get_user_cache_dir ());
  char *log = B (dir, "nautilus-file-converter.log");
  char *bak = g_strconcat (log, ".1", NULL);
  char *data = NULL;
  gsize len;
  GString *big = g_string_new (NULL);

  g_unlink (log);
  g_unlink (bak);
  /* new file: 0600 */
  nfc_log ("first line %d", 1);
  g_assert_cmpuint (file_mode (log), ==, 0600);
  g_assert_true (g_file_get_contents (log, &data, &len, NULL));
  g_assert_nonnull (strstr (data, "first line 1"));
  g_free (data);
  /* pre-existing wide permissions are narrowed */
  g_assert_cmpint (g_chmod (log, 0666), ==, 0);
  nfc_log ("second line");
  g_assert_cmpuint (file_mode (log), ==, 0600);
  /* above 1 MiB: rotated to .log.1 (single backup), a fresh log is started */
  for (int i = 0; i < 4000; i++) /* 281 bytes each: > 1 MiB */
    g_string_append_len (big, "0123456789012345678901234567890123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789012345678901234567890123456789"
                              "0123456789012345678901234567890123456789012345678901234567890123456789\n",
                         4 * 70 + 1);
  g_assert_true (g_file_set_contents (log, big->str, -1, NULL));
  g_assert_cmpint (g_chmod (log, 0644), ==, 0);
  nfc_log ("after rotation");
  g_assert_true (g_file_test (bak, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpuint (file_mode (bak), ==, 0600);
  g_assert_true (g_file_get_contents (bak, &data, &len, NULL));
  g_assert_cmpuint (len, ==, big->len);
  g_free (data);
  g_assert_true (g_file_get_contents (log, &data, &len, NULL));
  g_assert_nonnull (strstr (data, "after rotation"));
  g_assert_cmpuint (len, <, 200);
  g_assert_cmpuint (file_mode (log), ==, 0600);
  g_free (data);
  /* a second rotation replaces the backup, never accumulates ".2" */
  g_assert_true (g_file_set_contents (log, big->str, -1, NULL));
  nfc_log ("again");
  {
    char *bak2 = g_strconcat (log, ".2", NULL);

    g_assert_false (g_file_test (bak2, G_FILE_TEST_EXISTS));
    g_free (bak2);
  }
  /* one entry is never larger than 64 KiB, and an entry that would push the
   * file past 1 MiB rotates it first (the file is just below the limit) */
  {
    char *huge = g_strnfill (300 * 1024, 'z');
    char *fill = g_strnfill (1024 * 1024 - 100, 'f');
    gsize blen;

    g_assert_true (g_file_set_contents (log, fill, -1, NULL));
    nfc_log ("huge entry: %s", huge);
    g_assert_true (g_file_get_contents (bak, &data, &blen, NULL));
    g_assert_cmpuint (blen, ==, 1024 * 1024 - 100); /* the old content was rotated away */
    g_free (data);
    g_assert_true (g_file_get_contents (log, &data, &len, NULL));
    g_assert_cmpuint (len, <=, 64 * 1024);
    g_assert_cmpuint (len, >, 60 * 1024);
    g_assert_nonnull (strstr (data, "huge entry"));
    g_assert_nonnull (strstr (data, "entry truncated"));
    g_free (data);
    g_free (huge);
    g_free (fill);
  }
  g_string_free (big, TRUE);
  g_free (dir); g_free (log); g_free (bak);
}

int
main (int argc, char **argv)
{
  const char *orig = g_getenv ("TMPDIR");
  char *tmpl, *path;
  int rc;

  g_test_init (&argc, &argv, NULL);

  /* Before anything caches the temp/cache directories: a private TMPDIR with a
   * space and a '%' in its name, our own XDG_CACHE_HOME, fakes first in PATH. */
  tmpl = g_strdup_printf ("%s/nfc-robust-sp ace%%20-XXXXXX", orig && *orig ? orig : "/tmp");
  base = g_mkdtemp (tmpl);
  g_assert_nonnull (base);
  bindir = B (base, "bin");
  datadir = B (base, "data");
  g_assert_cmpint (g_mkdir_with_parents (bindir, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (datadir, 0755), ==, 0);
  g_setenv ("TMPDIR", base, TRUE);
  path = B (base, "cache");
  g_setenv ("XDG_CACHE_HOME", path, TRUE);
  g_free (path);
  path = g_strconcat (bindir, ":", g_getenv ("PATH"), NULL);
  g_setenv ("PATH", path, TRUE);
  g_free (path);
  g_setenv ("NFC_TEST_TIMEOUT_SECS", G_STRINGIFY (TEST_TIMEOUT), TRUE);
  g_setenv ("NFC_TEST_PROBE_TIMEOUT_SECS", G_STRINGIFY (TEST_PROBE_TIMEOUT), TRUE);

  /* order matters: the probe test needs fresh probe state, shutdown goes last */
  g_test_add_func ("/robust/probe-freeze", test_probe_freeze);
  g_test_add_func ("/robust/soffice-timeout", test_soffice_timeout);
  g_test_add_func ("/robust/ffmpeg-timeout-cleanup", test_ffmpeg_timeout_cleanup);
  g_test_add_func ("/robust/log-mode-and-rotation", test_log_mode_and_rotation);
  g_test_add_func ("/robust/output-flood", test_output_flood);
  g_test_add_func ("/robust/cancel-on-shutdown", test_cancel_on_shutdown);
  rc = g_test_run ();

  {
    char *rm[] = { "rm", "-rf", base, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (bindir); g_free (datadir); g_free (base);
  return rc;
}
