/* File names that are not UTF-8 (a Latin-1 "café.mp4" from an old zip, a name with
 * 0xff in it): a Linux file name is any bytes, but everything that is shown (the
 * toasts, the tray tooltip and menu) is a GLib/D-Bus string and must be valid
 * UTF-8. The bytes stay bytes where they are a path (the output is named after the
 * source, byte for byte) and become a display name (invalid sequences shown as
 * U+FFFD) in anything shown.
 *
 *  - a conversion of such a file works, the output keeps the raw bytes of the
 *    name and a new extension, and every toast (title, body: what notify-send is
 *    given) is valid UTF-8 and names the files
 *  - the same for a failed conversion whose stderr holds invalid bytes, and for
 *    Compress
 *  - nfc_activity_get with two such jobs running (it sorts their lines: a
 *    g_utf8_collate of invalid UTF-8 crashed) gives valid lines and tooltip
 *  - nfc_failure_message never returns invalid UTF-8
 *
 * Hermetic: a fake ffmpeg (a /bin/sh script) and a fake notify-send that logs its
 * arguments. Run with G_DEBUG=fatal-criticals (meson does): any GLib critical
 * fails it. No D-Bus here (the tray's own side is in test-tray.c).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "nfc-convert.h"
#include "nfc-core.h"

#define BAD1 "bad\xff\xfename.mp4"   /* not UTF-8 at all */
#define CAFE "caf\xe9.mp4"           /* Latin-1 */
#define FFFD "\xef\xbf\xbd"

static char *base, *bindir, *datadir, *notify_log;

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

static gboolean
timed_out_cb (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

#define SPIN_UNTIL(cond, ms) \
  G_STMT_START { \
    gboolean to_ = FALSE; \
    guint id_ = g_timeout_add ((ms), timed_out_cb, &to_); \
    while (!(cond) && !to_) \
      g_main_context_iteration (NULL, TRUE); \
    if (!to_) \
      g_source_remove (id_); \
    g_assert_true ((cond)); \
  } G_STMT_END

/* a fake ffmpeg: runs body (sh), the output file is "$last" */
static void
set_ffmpeg (const char *body)
{
  char *script = B (bindir, "ffmpeg");
  char *full = g_strconcat ("for a; do last=\"$a\"; done\n", body, NULL);

  write_script (script, full);
  g_free (script);
  g_free (full);
}

/* everything notify-send was given so far (title and body of each toast) */
static char *
notify_text (void)
{
  char *t = NULL;

  if (!g_file_get_contents (notify_log, &t, NULL, NULL))
    return g_strdup ("");
  return t;
}

static gboolean
notify_has (const char *needle)
{
  char *t = notify_text ();
  gboolean has = strstr (t, needle) != NULL;

  g_free (t);
  return has;
}

/* every toast handed to notify-send is valid UTF-8 */
static void
assert_notify_valid (void)
{
  char *t = notify_text ();

  g_assert_true (g_utf8_validate (t, -1, NULL));
  g_free (t);
}

typedef struct {
  gboolean done;
  guint n, ok;
  GPtrArray *errors;  /* char *, one per failed file */
  char **outputs;     /* one per file, NULL for a failed one */
} BatchResult;

static void
batch_done (GPtrArray *results, gpointer data)
{
  BatchResult *b = data;

  b->n = results->len;
  b->errors = g_ptr_array_new_with_free_func (g_free);
  b->outputs = g_new0 (char *, results->len + 1);
  for (guint i = 0; i < results->len; i++)
    {
      NfcResult *r = results->pdata[i];

      if (r->ok)
        {
          b->ok++;
          b->outputs[i] = g_strdup (r->output);
        }
      else
        g_ptr_array_add (b->errors, g_strdup (r->error));
    }
  b->done = TRUE;
}

static void
batch_clear (BatchResult *b)
{
  g_clear_pointer (&b->errors, g_ptr_array_unref);
  g_strfreev (b->outputs);
}

/* write the sources datadir/<names[i]> (size bytes of "x" each) and return their paths */
static char **
make_sources (const char *const *names, guint n, gsize size)
{
  char **paths = g_new0 (char *, n + 1);
  char *content = g_strnfill (size, 'x');

  for (guint i = 0; i < n; i++)
    {
      paths[i] = B (datadir, names[i]);
      g_assert_true (g_file_set_contents (paths[i], content, -1, NULL));
    }
  g_free (content);
  return paths;
}

static void
reset_notify (void)
{
  g_unlink (notify_log);
}

/* ---- tests ---- */

/* converts, keeps the raw bytes in the output name, and the toasts are valid */
static void
test_convert_keeps_bytes (void)
{
  const char *names[] = { BAD1, CAFE };
  const char *fmts[] = { "mp4", "mp4", NULL };
  char **paths = make_sources (names, 2, 1);
  BatchResult r = { 0 };
  char *out0, *out1, *b0, *b1;

  reset_notify ();
  set_ffmpeg ("printf done > \"$last\"\n");
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", (const char *const *) paths, fmts, 2,
                 batch_done, &r);
  SPIN_UNTIL (r.done, 30000);
  g_assert_cmpuint (r.ok, ==, 2);
  out0 = g_strdup (r.outputs[0]);
  out1 = g_strdup (r.outputs[1]);
  b0 = g_path_get_basename (out0);
  b1 = g_path_get_basename (out1);
  /* the bytes of the name are those of the source, the extension is the new one */
  g_assert_cmpstr (b0, ==, "bad\xff\xfename.mkv");
  g_assert_cmpstr (b1, ==, "caf\xe9.mkv");
  g_assert_true (g_file_test (out0, G_FILE_TEST_IS_REGULAR));
  g_assert_true (g_file_test (out1, G_FILE_TEST_IS_REGULAR));
  g_free (b0);
  g_free (b1);
  /* the "Conversion complete" toast, whenever notify-send has been run for it */
  SPIN_UNTIL (notify_has ("Conversion complete"), 10000);
  assert_notify_valid ();
  g_assert_true (notify_has ("Converted 2 files to MKV"));
  batch_clear (&r);

  /* one file: the body names it and its output, as display names */
  g_strfreev (paths);
  paths = make_sources (names, 1, 1);
  reset_notify ();
  memset (&r, 0, sizeof r);
  g_unlink (out0); /* so that the next output has the plain name again */
  g_unlink (out1);
  g_free (out0);
  g_free (out1);
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", (const char *const *) paths, fmts, 1,
                 batch_done, &r);
  SPIN_UNTIL (r.done, 30000);
  g_assert_cmpuint (r.ok, ==, 1);
  SPIN_UNTIL (notify_has ("bad" FFFD FFFD "name.mp4 \xe2\x86\x92 bad" FFFD FFFD "name.mkv"), 10000);
  assert_notify_valid ();
  g_assert_true (notify_has ("bad" FFFD FFFD "name.mp4 \xe2\x86\x92 bad" FFFD FFFD "name.mkv"));
  batch_clear (&r);
  g_strfreev (paths);
}

/* a failure whose stderr holds invalid bytes (the converter echoes the name) */
static void
test_failure_text_valid (void)
{
  const char *names[] = { CAFE, BAD1 };
  const char *fmts[] = { "mp4", "mp4", NULL };
  char **paths = make_sources (names, 2, 1);
  BatchResult r = { 0 };

  reset_notify ();
  set_ffmpeg ("printf 'caf\\351.mp4: Invalid data \\377\\376 found when processing input\\n' >&2\n"
              "exit 1\n");
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", (const char *const *) paths, fmts, 2,
                 batch_done, &r);
  SPIN_UNTIL (r.done, 60000);
  g_assert_cmpuint (r.ok, ==, 0);
  g_assert_cmpuint (r.errors->len, ==, 2);
  for (guint i = 0; i < r.errors->len; i++)
    g_assert_true (g_utf8_validate (r.errors->pdata[i], -1, NULL));
  SPIN_UNTIL (notify_has ("Conversion failed"), 10000);
  assert_notify_valid ();
  g_assert_true (notify_has ("Failed: caf" FFFD ".mp4"));
  g_assert_true (notify_has ("Failed: bad" FFFD FFFD "name.mp4"));
  batch_clear (&r);
  g_strfreev (paths);
}

/* Reduce file size: a smaller copy (named after the source), the toast valid */
static void
test_compress_valid (void)
{
  const char *names[] = { CAFE };
  const char *fmts[] = { "mp4", NULL };
  char **paths = make_sources (names, 1, 4000);
  BatchResult r = { 0 };
  char *b;

  reset_notify ();
  set_ffmpeg ("printf small > \"$last\"\n");
  nfc_batch_run_compress (NFC_CAT_VIDEO, NFC_PRESET_BALANCED, (const char *const *) paths, fmts, 1,
                          batch_done, &r);
  SPIN_UNTIL (r.done, 30000);
  g_assert_cmpuint (r.ok, ==, 1);
  b = g_path_get_basename (r.outputs[0]);
  g_assert_true (g_str_has_prefix (b, "caf\xe9 (compressed)"));
  g_free (b);
  SPIN_UNTIL (notify_has ("File size reduced"), 10000);
  assert_notify_valid ();
  g_assert_true (notify_has ("caf" FFFD ".mp4 \xe2\x86\x92 caf" FFFD " (compressed)"));
  batch_clear (&r);
  g_strfreev (paths);
}

static void
assert_valid_activity (const NfcActivity *a)
{
  g_assert_nonnull (a->summary);
  g_assert_true (g_utf8_validate (a->summary, -1, NULL));
  for (guint i = 0; i < a->lines->len; i++)
    g_assert_true (g_utf8_validate (a->lines->pdata[i], -1, NULL));
}

/* two bad names running at once: the activity sorts their lines */
static void
test_activity_valid (void)
{
  const char *names[] = { BAD1, CAFE, "plain.mp4", "z\303\251e.mp4" };
  const char *fmts[] = { "mp4", "mp4", "mp4", "mp4", NULL };
  char **paths = make_sources (names, 4, 1);
  BatchResult r = { 0 }, one = { 0 };
  NfcActivity a;
  gboolean got = FALSE;

  reset_notify ();
  set_ffmpeg ("sleep 30\nprintf done > \"$last\"\n");
  /* a batch of four files runs 2 at a time; a second batch adds a job of its own */
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", (const char *const *) paths, fmts, 4,
                 batch_done, &r);
  for (int i = 0; i < 300 && !got; i++)
    {
      got = nfc_activity_get (&a) && a.lines->len >= 2;
      if (!got)
        {
          nfc_activity_clear (&a);
          g_usleep (20000);
          g_main_context_iteration (NULL, FALSE);
        }
    }
  g_assert_true (got);
  assert_valid_activity (&a);
  g_assert_true (strstr (a.summary, "Converting 4 files to MKV") != NULL);
  {
    gboolean bad_shown = FALSE;

    for (guint i = 0; i < a.lines->len; i++)
      if (strstr (a.lines->pdata[i], FFFD) != NULL)
        bad_shown = TRUE;
    g_assert_true (bad_shown); /* the first two files run first, both bad */
  }
  nfc_activity_clear (&a);
  nfc_cancel_all ();
  SPIN_UNTIL (r.done, 30000);
  batch_clear (&r);
  g_strfreev (paths);

  /* a batch of one: the tooltip names the file */
  paths = make_sources (names, 1, 1);
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", (const char *const *) paths, fmts, 1,
                 batch_done, &one);
  g_assert_true (nfc_activity_get (&a));
  assert_valid_activity (&a);
  g_assert_cmpstr (a.summary, ==, "Converting bad" FFFD FFFD "name.mp4 to MKV\xe2\x80\xa6");
  nfc_activity_clear (&a);
  nfc_cancel_all ();
  SPIN_UNTIL (one.done, 30000);
  batch_clear (&one);
  g_strfreev (paths);
  SPIN_UNTIL (!nfc_activity_get (&a), 5000);
}

static void
test_failure_message_valid (void)
{
  const char *texts[] = {
    "caf\xe9.mp4: Invalid data found when processing input\n",
    "error: \xff\xfe\xfd\n",
    "\xe9\xe9\xe9",
    "x\xc3", /* cut in the middle of a character */
    NULL
  };

  for (guint i = 0; texts[i] != NULL; i++)
    {
      char *m = nfc_failure_message (texts[i], "mp4");

      if (m != NULL)
        g_assert_true (g_utf8_validate (m, -1, NULL));
      g_free (m);
    }
  {
    /* a long line cut at the limit stays valid when the bytes are not */
    GString *s = g_string_new ("error: ");

    for (guint i = 0; i < 400; i++)
      g_string_append (s, "\xe9");
    {
      char *m = nfc_failure_message (s->str, "mp4");

      g_assert_nonnull (m);
      g_assert_true (g_utf8_validate (m, -1, NULL));
      g_free (m);
    }
    g_string_free (s, TRUE);
  }
}

int
main (int argc, char **argv)
{
  const char *orig = g_getenv ("TMPDIR");
  char *tmpl, *path, *script;
  int rc;

  g_test_init (&argc, &argv, NULL);
  /* every GLib critical and warning fails the test (g_test_init does it too; being
   * explicit: this is what the tests are about) */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);

  tmpl = g_strdup_printf ("%s/nfc-utf8-XXXXXX", orig && *orig ? orig : "/tmp");
  base = g_mkdtemp (tmpl);
  g_assert_nonnull (base);
  bindir = B (base, "bin");
  datadir = B (base, "data");
  notify_log = B (base, "notify.log");
  g_assert_cmpint (g_mkdir_with_parents (bindir, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (datadir, 0755), ==, 0);
  g_setenv ("TMPDIR", base, TRUE);
  path = B (base, "cache");
  g_setenv ("XDG_CACHE_HOME", path, TRUE);
  g_free (path);
  path = B (base, "config");
  g_setenv ("XDG_CONFIG_HOME", path, TRUE);
  g_free (path);
  g_setenv ("NFC_TEST_TIMEOUT_SECS", "600", TRUE);

  /* the fakes first in PATH; no ffprobe, so no size probe of the video */
  path = B (bindir, "notify-send");
  script = g_strdup_printf ("for a; do printf '%%s\\n' \"$a\"; done >> '%s'\necho 1\n", notify_log);
  write_script (path, script);
  g_free (script);
  g_free (path);
  set_ffmpeg ("printf done > \"$last\"\n");
  path = g_strconcat (bindir, ":/usr/bin:/bin", NULL);
  g_setenv ("PATH", path, TRUE);
  g_free (path);
  nfc_tools_init ();
  g_clear_pointer (&nfc_tools.ffprobe, g_free);
  g_assert_true (g_str_has_prefix (nfc_tools.ffmpeg, bindir));
  g_assert_true (g_str_has_prefix (nfc_tools.notify, bindir));

  g_test_add_func ("/utf8/convert-keeps-bytes", test_convert_keeps_bytes);
  g_test_add_func ("/utf8/failure-text-valid", test_failure_text_valid);
  g_test_add_func ("/utf8/compress-valid", test_compress_valid);
  g_test_add_func ("/utf8/activity-valid", test_activity_valid);
  g_test_add_func ("/utf8/failure-message-valid", test_failure_message_valid);
  rc = g_test_run ();

  {
    char *rm[] = { "rm", "-rf", base, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (bindir);
  g_free (datadir);
  g_free (notify_log);
  g_free (base);
  return rc;
}
