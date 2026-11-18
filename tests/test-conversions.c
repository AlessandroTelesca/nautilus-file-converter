/* Run every conversion path for real and print a pass/fail table.
 *
 * Self-contained: fixtures are generated at run time with ImageMagick, ffmpeg
 * and LibreOffice (no Python). The conversions go through the same code the
 * extension uses (nfc_convert_file_async / nfc_batch_run). Every output is
 * checked for existence, non-zero size and its REAL type (magic bytes via
 * `file`), because ImageMagick happily writes PNG data under a .avif name.
 *
 * Optional: NFC_HEIC_SAMPLE=/path/to/photo.heic also tests HEIC input.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc-convert.h"
#include "nfc-core.h"

static char *work;
static GPtrArray *rows; /* char *[4] rows: src, target, PASS/FAIL, detail */
static int n_fail;

static void
add_row (const char *src, const char *tgt, gboolean pass, const char *detail)
{
  char **r = g_new0 (char *, 5);

  r[0] = g_strdup (src);
  r[1] = g_strdup (tgt);
  r[2] = g_strdup (pass ? "PASS" : "FAIL");
  r[3] = g_strdup (detail ? detail : "");
  g_ptr_array_add (rows, r);
  if (!pass)
    n_fail++;
}

/* run argv in dir, return exit status ok; optionally capture stdout */
static gboolean
run (const char *dir, char **out, const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new_null_terminated (16, g_free, TRUE);
  va_list ap;
  const char *s = first;
  gint status = -1;
  gboolean ok;
  char *so = NULL;

  va_start (ap, first);
  while (s != NULL)
    {
      g_ptr_array_add (a, g_strdup (s));
      s = va_arg (ap, const char *);
    }
  va_end (ap);
  ok = g_spawn_sync (dir, (char **) a->pdata, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL |
                     G_SPAWN_CHILD_INHERITS_STDIN,
                     NULL, NULL, &so, NULL, &status, NULL);
  ok = ok && g_spawn_check_wait_status (status, NULL);
  if (out)
    *out = so;
  else
    g_free (so);
  g_ptr_array_unref (a);
  return ok;
}

static void
fixture (const char *what, gboolean ok)
{
  if (!ok)
    {
      g_printerr ("fixture failed: %s\n", what);
      add_row ("(fixture)", what, FALSE, "could not generate");
    }
}

static void
write_file (const char *name, const char *content)
{
  char *p = g_build_filename (work, name, NULL);

  g_file_set_contents (p, content, -1, NULL);
  g_free (p);
}

static char *
P (const char *name)
{
  return g_build_filename (work, name, NULL);
}

/* ---- synchronous wrapper around the async API ---- */
typedef struct {
  GMainLoop *loop;
  char *out;
  GError *error;
} Wait;

static void
on_done (GObject *o, GAsyncResult *res, gpointer data)
{
  Wait *w = data;

  (void) o;
  w->out = nfc_convert_file_finish (res, &w->error);
  g_main_loop_quit (w->loop);
}

static char *
convert_sync (NfcCategory cat, const char *fmt, const char *key, const char *ext,
              const char *src, char **err)
{
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  char *out;

  nfc_convert_file_async (cat, fmt, key, ext, src, on_done, &w);
  g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  out = w.out;
  if (err)
    *err = w.error ? g_strdup (w.error->message) : NULL;
  g_clear_error (&w.error);
  return out;
}

/* ---- type checks ---- */
static char *
file_mime (const char *path)
{
  char *out = NULL;

  if (!run (NULL, &out, "file", "--mime-type", "-b", path, NULL) || out == NULL)
    return g_strdup ("(file failed)");
  return g_strstrip (out);
}

static gboolean
contains (const char *path, const char *needle)
{
  char *data;
  gsize len;
  gboolean r;

  if (!g_file_get_contents (path, &data, &len, NULL))
    return FALSE;
  r = memmem (data, len, needle, strlen (needle)) != NULL;
  g_free (data);
  return r;
}

/* returns NULL if the real type is as expected, else a description */
static char *
check_type (const char *ext, const char *path)
{
  static const struct { const char *ext, *mimes; } want[] = {
    { "png", "image/png" }, { "jpg", "image/jpeg" }, { "webp", "image/webp" },
    { "avif", "image/avif" },
    /* libmagic reads the header of a multi-size ICO (00 00 01 00, n > 1 images)
     * as a SIMH tape image: for .ico the header is checked directly below */
    { "ico", "image/vnd.microsoft.icon|image/x-icon|application/SIMH-tape-data" },
    { "bmp", "image/bmp|image/x-ms-bmp" }, { "tiff", "image/tiff" },
    { "pdf", "application/pdf" }, { "gif", "image/gif" },
    { "mp3", "audio/mpeg" }, { "flac", "audio/flac|audio/x-flac" },
    { "wav", "audio/x-wav|audio/vnd.wave|audio/wav" },
    { "ogg", "audio/ogg|application/ogg" },
    { "opus", "audio/ogg|audio/opus|audio/x-opus+ogg|application/ogg" },
    { "m4a", "audio/mp4|audio/x-m4a|video/mp4" },
    { "mp4", "video/mp4" }, { "mkv", "video/x-matroska" }, { "webm", "video/webm" },
    { "odt", "application/vnd.oasis.opendocument.text" },
    { "ods", "application/vnd.oasis.opendocument.spreadsheet" },
    { "odp", "application/vnd.oasis.opendocument.presentation" },
    { "csv", "text/csv|text/plain" },
  };
  char *mime = file_mime (path), *res = NULL;
  gboolean zip_ok = FALSE;

  /* OOXML: a zip containing the format's main part */
  if (g_str_equal (ext, "docx") || g_str_equal (ext, "xlsx") || g_str_equal (ext, "pptx"))
    {
      const char *part = g_str_equal (ext, "docx") ? "word/document.xml"
                         : g_str_equal (ext, "xlsx") ? "xl/workbook.xml"
                         : "ppt/presentation.xml";

      zip_ok = contains (path, "PK") && contains (path, part);
      if (!zip_ok)
        res = g_strdup_printf ("not a real %s (file says %s)", ext, mime);
      g_free (mime);
      return res;
    }
  for (guint i = 0; i < G_N_ELEMENTS (want); i++)
    if (g_str_equal (want[i].ext, ext))
      {
        char **alts = g_strsplit (want[i].mimes, "|", -1);

        if (!g_strv_contains ((const char *const *) alts, mime))
          res = g_strdup_printf ("real type is %s, expected %s", mime, want[i].mimes);
        if (res == NULL && g_str_equal (ext, "ico"))
          {
            /* ICONDIR: reserved 0, type 1 (icon), image count >= 1 */
            char *d = NULL;
            gsize n = 0;

            if (!g_file_get_contents (path, &d, &n, NULL) || n < 6 ||
                memcmp (d, "\0\0\1\0", 4) != 0 || (guchar) d[4] == 0)
              res = g_strdup ("not an ICO header");
            g_free (d);
          }
        g_strfreev (alts);
        g_free (mime);
        return res;
      }
  g_free (mime);
  return g_strdup_printf ("no type check for .%s", ext);
}

static gboolean
non_empty (const char *path)
{
  GStatBuf st;

  return g_stat (path, &st) == 0 && st.st_size > 0;
}

/* convert src_name -> key through the real code path and verify the result */
static char *
check (const char *src_name, const char *key)
{
  char *src = P (src_name), *err = NULL, *out = NULL, *res = NULL;
  NfcFormat f;
  GPtrArray *tg;
  const NfcTarget *t = NULL;

  if (!g_file_test (src, G_FILE_TEST_EXISTS))
    {
      add_row (src_name, key, FALSE, "fixture missing");
      goto done;
    }
  if (!nfc_classify (src, NULL, &f))
    {
      add_row (src_name, key, FALSE, "not classified");
      goto done;
    }
  tg = nfc_targets_for (f.cat, f.fmt);
  for (guint i = 0; i < tg->len; i++)
    if (g_str_equal (((const NfcTarget *) tg->pdata[i])->key, key))
      t = tg->pdata[i];
  g_ptr_array_unref (tg);
  if (t == NULL)
    {
      add_row (src_name, key, FALSE, "target not offered");
      goto done;
    }
  out = convert_sync (f.cat, f.fmt, t->key, t->ext, src, &err);
  if (out == NULL)
    add_row (src_name, key, FALSE, err ? err : "failed");
  else if (!non_empty (out))
    add_row (src_name, key, FALSE, "output empty");
  else
    {
      char *bad = check_type (t->ext, out);
      char *base = g_path_get_basename (out);

      add_row (src_name, key, bad == NULL, bad ? bad : base);
      g_free (bad);
      g_free (base);
      res = g_strdup (out);
    }
done:
  g_free (src);
  g_free (out);
  g_free (err);
  return res; /* path of the good output, for further checks */
}

static void
check_many (const char *src, const char *const *keys)
{
  for (; *keys; keys++)
    g_free (check (src, *keys));
}

static gboolean
leftover_temp_files (const char *dir, char **list)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *n;
  GString *s = g_string_new (NULL);

  while (d && (n = g_dir_read_name (d)))
    if (n[0] == '.' && strstr (n, ".nfc-") != NULL)
      g_string_append_printf (s, "%s ", n);
  if (d)
    g_dir_close (d);
  *list = g_string_free (s, FALSE);
  return (*list)[0] != '\0';
}

/* ---- fake notify-send (shell) to exercise the replace-id logic ---- */
static void
test_notifications (void)
{
  char *dir = g_dir_make_tmp ("nfc-notify-XXXXXX", NULL);
  char *script = g_build_filename (dir, "notify-send", NULL);
  char *log = g_build_filename (dir, "calls.log", NULL);
  /* behaves like the real notify-send for option parsing: any argument that
   * starts with '-' before a "--" must be a known option, else "Unknown
   * option" and exit status 1 (getopt permutes, so this also catches a
   * leading-dash title or body) */
  char *content = g_strdup_printf ("#!/bin/sh\n"
                                   "echo \"$@\" >> '%s'\n"
                                   "while [ $# -gt 0 ]; do\n"
                                   "  case \"$1\" in\n"
                                   "    --) break ;;\n"
                                   "    -a|-i|-u|-h|-r|-t|-c) shift 2 ;;\n"
                                   "    -p) shift ;;\n"
                                   "    -*) echo \"Unknown option $1\" >&2; echo REJECTED >> '%s'; exit 1 ;;\n"
                                   "    *) shift ;;\n"
                                   "  esac\n"
                                   "done\n"
                                   "echo 4242\n", log, log);
  char *paths[2], *fmts[] = { (char *) "png", NULL };
  char *saved = nfc_tools.notify, *logged = NULL, *src = P ("-dash.png"), *orig = P ("test.png");
  gboolean pass;

  g_file_set_contents (script, content, -1, NULL);
  g_chmod (script, 0755);
  /* a file whose name starts with '-': it ends up in the notification body */
  {
    char *data;
    gsize len;

    g_file_get_contents (orig, &data, &len, NULL);
    g_file_set_contents (src, data, len, NULL);
    g_free (data);
  }
  nfc_tools.notify = script;
  paths[0] = src;
  paths[1] = NULL;
  {
    GMainContext *ctx = g_main_context_default ();
    gboolean done = FALSE;

    nfc_batch_run (NFC_CAT_IMAGE, "bmp", "BMP", "bmp", (const char *const *) paths,
                   (const char *const *) fmts, 1, NULL, NULL);
    /* give the notifier time to finish its two exchanges */
    for (int i = 0; i < 400 && !done; i++)
      {
        g_main_context_iteration (ctx, FALSE);
        g_usleep (20000);
        if (g_file_get_contents (log, &logged, NULL, NULL))
          {
            int lines = 0;

            for (const char *p = logged; *p; p++)
              lines += *p == '\n';
            done = lines >= 2;
            if (!done)
              g_clear_pointer (&logged, g_free);
          }
      }
  }
  pass = logged != NULL;
  if (pass)
    {
      char **lines = g_strsplit (logged, "\n", -1);

      pass = g_str_has_prefix (lines[0], "-a File Converter") &&
             strstr (lines[0], "Converting") != NULL && strstr (lines[0], "-r ") == NULL &&
             /* every banner is urgency normal (a critical one would block the later
              * ones), and the success icon is one Adwaita 50 has */
             strstr (lines[0], "-i view-refresh-symbolic -u normal ") != NULL &&
             strstr (lines[1], "-i object-select-symbolic -u normal ") != NULL &&
             strstr (logged, "critical") == NULL && strstr (logged, "emblem-ok") == NULL &&
             strstr (lines[1], "-r 4242") != NULL &&
             strstr (lines[1], "Conversion complete") != NULL &&
             /* "--" comes right before the positionals (title, body) ... */
             strstr (lines[0], " -- Converting… -dash.png to BMP") != NULL &&
             strstr (lines[1], " -- Conversion complete -dash.png → -dash.bmp") != NULL &&
             /* ... and after every option */
             strstr (lines[1], "-r 4242 -- ") != NULL &&
             strstr (logged, "REJECTED") == NULL;
      g_strfreev (lines);
    }
  add_row ("(notifications)", "-p / -r id", pass, pass ? "-- before title/body; second call replaces id 4242" : (logged ? logged : "no calls"));
  /* a failed conversion: urgency normal as well, error icon, and the toast shows
   * the cause. The "image" is plain text, which ImageMagick refuses. */
  {
    char *bad = P ("notify-bad.png"), *logged2 = NULL;
    char *bpaths[2] = { bad, NULL };
    GMainContext *ctx = g_main_context_default ();
    gboolean ok2 = FALSE;

    g_file_set_contents (bad, "this is not an image\n", -1, NULL);
    nfc_batch_run (NFC_CAT_IMAGE, "bmp", "BMP", "bmp", (const char *const *) bpaths,
                   (const char *const *) fmts, 1, NULL, NULL);
    for (int i = 0; i < 500 && !ok2; i++)
      {
        g_main_context_iteration (ctx, FALSE);
        g_usleep (20000);
        if (g_file_get_contents (log, &logged2, NULL, NULL))
          {
            ok2 = strstr (logged2, "Conversion failed") != NULL;
            if (!ok2)
              g_clear_pointer (&logged2, g_free);
          }
      }
    pass = ok2 && strstr (logged2, "-i dialog-error-symbolic -u normal ") != NULL &&
           strstr (logged2, "critical") == NULL &&
           strstr (logged2, "Failed: notify-bad.png — ") != NULL &&
           strstr (logged2, "REJECTED") == NULL;
    add_row ("(notifications)", "failure banner normal urgency", pass,
             pass ? "dialog-error-symbolic, urgency normal" : (logged2 ? logged2 : "no failure banner"));
    g_free (logged2);
    g_remove (bad);
    g_free (bad);
  }
  nfc_tools.notify = saved;
  /* the converted bmp is left in work; remove it so naming tests stay exact */
  g_free (logged);
  g_free (dir);
  g_free (script);
  g_free (log);
  g_free (content);
  g_free (src);
  g_free (orig);
}

/* ---- batch ---- */
static void
batch_done (GPtrArray *results, gpointer data)
{
  GPtrArray *copy = data;

  for (guint i = 0; i < results->len; i++)
    {
      NfcResult *r = results->pdata[i];

      g_ptr_array_add (copy, g_strdup_printf ("%s|%s", r->ok ? "ok" : "fail",
                                              r->ok ? r->output : r->error));
    }
  g_main_context_wakeup (NULL);
}

static void
test_batch (void)
{
  const char *names[] = { "b1.png", "b2.png", "b3.png", "b4.png", "b5.png" };
  const char *paths[6], *fmts[6];
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  char *saved = nfc_tools.notify;
  gboolean pass = TRUE;
  GString *detail = g_string_new (NULL);

  nfc_tools.notify = NULL; /* no real notifications from the test */
  for (guint i = 0; i < 5; i++)
    {
      char *src = P ("test.png"), *dst = P (names[i]);
      gchar *data;
      gsize len;

      g_file_get_contents (src, &data, &len, NULL);
      g_file_set_contents (dst, data, len, NULL);
      paths[i] = dst;
      fmts[i] = "png";
      g_free (data);
      g_free (src);
    }
  paths[5] = fmts[5] = NULL;
  nfc_batch_run (NFC_CAT_IMAGE, "webp", "WEBP", "webp", paths, fmts, 5, batch_done, out);
  while (out->len < 5)
    g_main_context_iteration (NULL, TRUE);
  for (guint i = 0; i < out->len; i++)
    {
      const char *s = out->pdata[i];

      if (!g_str_has_prefix (s, "ok|"))
        pass = FALSE;
      else
        {
          char *bad = check_type ("webp", s + 3);

          if (bad)
            pass = FALSE;
          g_free (bad);
        }
      g_string_append_printf (detail, "%s ", s);
    }
  /* a failing item must be reported with the file name and not hide the others */
  {
    GPtrArray *out2 = g_ptr_array_new_with_free_func (g_free);
    char *bad = P ("corrupt.png");
    char *good = P ("test.png");
    const char *p2[] = { bad, good, NULL }, *f2[] = { "png", "png", NULL };
    char *left;
    gboolean p;

    write_file ("corrupt.png", "this is not a png");
    nfc_batch_run (NFC_CAT_IMAGE, "jpg", "JPG", "jpg", p2, f2, 2, batch_done, out2);
    while (out2->len < 2)
      g_main_context_iteration (NULL, TRUE);
    p = g_str_has_prefix (out2->pdata[0], "fail|") && g_str_has_prefix (out2->pdata[1], "ok|") &&
        strlen ((char *) out2->pdata[0]) > 6;
    add_row ("corrupt.png + test.png", "jpg (batch)", p,
             p ? (char *) out2->pdata[0] : "expected [fail, ok]");
    p = !leftover_temp_files (work, &left);
    add_row ("(failure cleanup)", "no temp files", p, left);
    g_free (left);
    g_ptr_array_unref (out2);
    g_free (bad);
    g_free (good);
  }
  add_row ("5x png (parallel batch)", "webp", pass, pass ? "all 5 converted" : detail->str);
  g_string_free (detail, TRUE);
  for (guint i = 0; i < 5; i++)
    g_free ((char *) paths[i]);
  g_ptr_array_unref (out);
  nfc_tools.notify = saved;
}

/* ---- file names that ImageMagick / ffmpeg / LibreOffice could misread ---- */
static void
check_special (const char *rel, const char *src_fmt_ext, const char *key)
{
  char *src = P (rel), *out, *dir = g_path_get_dirname (src), *stem = nfc_stem_of (src);
  char *want, *got, *left = NULL;
  guint n = 0;
  GDir *d;
  gboolean ok;
  GString *entries = g_string_new (NULL);
  const char *name;

  (void) src_fmt_ext;
  out = check (rel, key);
  want = g_strdup_printf ("%s.%s", stem, key);
  got = out ? g_path_get_basename (out) : g_strdup ("(failed)");
  /* exactly the source and the output: nothing like "a0.jpg" next to them */
  d = g_dir_open (dir, 0, NULL);
  while (d && (name = g_dir_read_name (d)))
    {
      n++;
      g_string_append_printf (entries, "%s | ", name);
    }
  if (d)
    g_dir_close (d);
  ok = out != NULL && g_str_equal (got, want) && n == 2 && !leftover_temp_files (dir, &left);
  g_free (left);
  add_row (rel, key, ok, ok ? "output named after the source, no stray files" : entries->str);
  g_string_free (entries, TRUE);
  g_free (src); g_free (out); g_free (dir); g_free (stem); g_free (want); g_free (got);
}

static void
copy_to (const char *from_name, const char *rel)
{
  char *from = P (from_name), *to = P (rel), *todir = g_path_get_dirname (to), *data;
  gsize len;

  g_mkdir_with_parents (todir, 0755);
  g_assert_true (g_file_get_contents (from, &data, &len, NULL));
  g_assert_true (g_file_set_contents (to, data, len, NULL));
  g_free (data); g_free (from); g_free (to); g_free (todir);
}

static void
test_special_names (void)
{
  /* ImageMagick expands %-escapes in file names; also [ ] * @ : have meaning to it */
  static const char *png_names[] = {
    "a%d.png", "%[filename].png", "100%.png", "a%%d.png", "%s%n.png", "a[1].png",
    "a*b.png", "@list.png", "a:b.png", "a b (1).png", "dir%d/x.png", "dir%[filename]/x.png",
    "-dash.png", NULL
  };
  static const char *keys[] = { "jpg", "avif", "pdf", NULL };
  char *rel;

  for (guint i = 0; png_names[i]; i++)
    for (guint k = 0; keys[k]; k++)
      {
        /* every file in its own directory so stray output is visible */
        rel = g_strdup_printf ("special/p%u_%u/%s", i, k, png_names[i]);
        copy_to ("test.png", rel);
        check_special (rel, "png", keys[k]);
        g_free (rel);
      }
  /* other tools and other input types with a '%' in the name */
  rel = g_strdup ("special/svg/100%d.svg"); copy_to ("test.svg", rel); check_special (rel, "svg", "png"); g_free (rel);
  rel = g_strdup ("special/gif/a%d.gif"); copy_to ("test.gif", rel); check_special (rel, "gif", "png"); g_free (rel);
  rel = g_strdup ("special/ico/a%d.ico"); copy_to ("test.ico", rel); check_special (rel, "ico", "png"); g_free (rel);
  rel = g_strdup ("special/tiff/a%d.tiff"); copy_to ("test.tiff", rel); check_special (rel, "tiff", "jpg"); g_free (rel);
  rel = g_strdup ("special/wav/a%d.wav"); copy_to ("test.wav", rel); check_special (rel, "wav", "mp3"); g_free (rel);
  rel = g_strdup ("special/mp4/v%d.mp4"); copy_to ("test.mp4", rel); check_special (rel, "mp4", "gif"); g_free (rel);
  rel = g_strdup ("special/mp4b/v%d.mp4"); copy_to ("test.mp4", rel); check_special (rel, "mp4", "webm"); g_free (rel);
  rel = g_strdup ("special/txt/100%.txt"); copy_to ("test.txt", rel); check_special (rel, "txt", "pdf"); g_free (rel);
  rel = g_strdup ("special/txt2/sp ace%20.txt"); copy_to ("test.txt", rel); check_special (rel, "txt", "docx"); g_free (rel);
}

/* SVG input must neither embed local files through hrefs (file://, text:,
 * relative paths) nor spawn Inkscape: it is read by librsvg in-process
 * (RSVG:). The secret is red, the SVG's own rect is blue: any red in the
 * output means the reference was followed. A fake `inkscape` first in PATH
 * leaves a marker file if ImageMagick ever delegates to it. */
static void
test_svg_hardening (void)
{
  char *dir = P ("svgref"), *bin = P ("svgref/bin"), *marker = P ("svgref/inkscape-ran");
  char *secret = P ("svgref/secret.png"), *svg = P ("svgref/ref.svg"), *out, *px = NULL;
  char *fake, *content, *old_path = g_strdup (g_getenv ("PATH")), *new_path, *err = NULL;
  gboolean ok;

  g_mkdir_with_parents (bin, 0755);
  fake = g_build_filename (bin, "inkscape", NULL);
  content = g_strdup_printf ("#!/bin/sh\ntouch '%s'\nexit 1\n", marker);
  g_file_set_contents (fake, content, -1, NULL);
  g_chmod (fake, 0755);
  g_free (content);
  fixture ("svgref/secret.png", run (dir, NULL, "magick", "-size", "40x30", "xc:red", "secret.png", NULL));
  content = g_strdup_printf (
    "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "width=\"40\" height=\"30\"><rect width=\"40\" height=\"30\" fill=\"blue\"/>"
    "<image href=\"file://%s\" width=\"40\" height=\"30\"/>"
    "<image xlink:href=\"text:%s\" width=\"40\" height=\"30\"/>"
    "<image href=\"secret.png\" width=\"40\" height=\"30\"/>"
    "<image href=\"../svgref/secret.png\" width=\"40\" height=\"30\"/></svg>",
    secret, secret);
  g_file_set_contents (svg, content, -1, NULL);
  g_free (content);

  /* 1. the secret must not appear (real PATH, so a plain SVG: coder would embed it) */
  out = convert_sync (NFC_CAT_IMAGE, "svg", "png", "png", svg, &err);
  ok = out != NULL && run (NULL, &px, "magick", out, "-format", "%[pixel:p{5,5}]", "info:", NULL) &&
       px != NULL && strstr (px, "0,0,255") != NULL;
  add_row ("svgref/ref.svg (file://, text:, relative hrefs)", "png", ok,
           ok ? "no reference embedded (pixel is the SVG's own blue)"
              : (out ? (px ? px : "magick identify failed") : (err ? err : "conversion failed")));
  /* 2. no Inkscape: a second conversion with the fake first in PATH */
  new_path = g_strconcat (bin, ":", old_path, NULL);
  g_setenv ("PATH", new_path, TRUE);
  g_free (out);
  out = convert_sync (NFC_CAT_IMAGE, "svg", "png", "png", svg, NULL);
  ok = out != NULL;
  add_row ("svgref/ref.svg (fake inkscape first in PATH)", "png", ok, ok ? "converted" : "conversion failed");
  ok = !g_file_test (marker, G_FILE_TEST_EXISTS);
  add_row ("svgref/ref.svg", "inkscape not spawned", ok, ok ? "no marker" : "fake inkscape was run");

  g_setenv ("PATH", old_path, TRUE);
  g_free (out); g_free (px); g_free (err); g_free (new_path); g_free (old_path);
  g_free (fake); g_free (dir); g_free (bin); g_free (marker); g_free (secret); g_free (svg);
}

static void
test_heic (void)
{
  const char *heic = g_getenv ("NFC_HEIC_SAMPLE");
  char *dst, *data, *dims;
  gsize len;

  if (heic == NULL || !g_file_test (heic, G_FILE_TEST_IS_REGULAR))
    {
      g_print ("HEIC sample: NOT FOUND (set NFC_HEIC_SAMPLE) - HEIC tests skipped\n");
      return;
    }
  g_print ("HEIC sample: %s\n", heic);
  dst = P ("photo.heic");
  g_file_get_contents (heic, &data, &len, NULL);
  g_file_set_contents (dst, data, len, NULL);
  g_free (data);
  g_free (dst);
  {
    char *o = check ("photo.heic", "jpg");

    if (o)
      {
        dims = NULL;
        run (NULL, &dims, "magick", "identify", "-format", "%wx%h", o, NULL);
        g_print ("photo.heic -> jpg dimensions: %s (orientation applied)\n", dims);
        g_free (dims);
        g_free (o);
      }
  }
  {
    const char *keys[] = { "png", "webp", "avif", "pdf", NULL };

    check_many ("photo.heic", keys);
  }
}

/* A conversion that must FAIL with a message containing `want` (the toast
 * text), and leave nothing behind. */
static void
check_error (const char *src_name, const char *key, const char *want)
{
  char *src = P (src_name), *err = NULL, *out;
  NfcFormat f;
  GPtrArray *tg;
  const NfcTarget *t = NULL;
  char *left = NULL;
  gboolean ok;

  g_assert_true (nfc_classify (src, NULL, &f));
  tg = nfc_targets_for (f.cat, f.fmt);
  for (guint i = 0; i < tg->len; i++)
    if (g_str_equal (((const NfcTarget *) tg->pdata[i])->key, key))
      t = tg->pdata[i];
  g_ptr_array_unref (tg);
  g_assert_nonnull (t);
  out = convert_sync (f.cat, f.fmt, t->key, t->ext, src, &err);
  ok = out == NULL && err != NULL && strstr (err, want) != NULL &&
       !leftover_temp_files (work, &left);
  add_row (src_name, key, ok, out != NULL ? "unexpectedly succeeded"
                              : ok ? err : err ? err : "no message");
  g_free (left);
  g_free (src);
  g_free (err);
  g_free (out);
}

/* a stream's property through ffprobe ("width", "pix_fmt", ...) */
static char *
probe_stream (const char *path, const char *entry)
{
  char *out = NULL, *what = g_strdup_printf ("stream=%s", entry);

  if (!run (NULL, &out, "ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries", what, "-of", "csv=p=0", path, NULL))
    out = g_strdup ("(ffprobe failed)");
  g_free (what);
  return g_strstrip (out);
}

/* Quality settings that must survive in the real output files. */
static void
test_quality (void)
{
  char *out, *src;

  /* JPG: 4:4:4, ImageMagick's own default switches to 4:2:0 below quality 90 */
  out = check ("test.png", "jpg");
  if (out != NULL)
    {
      char *sf = NULL;
      gboolean ok = run (NULL, &sf, "magick", "identify", "-format", "%[jpeg:sampling-factor]", out, NULL);

      add_row ("test.png", "jpg 4:4:4", ok && sf != NULL && g_str_equal (sf, "1x1,1x1,1x1"),
               sf ? sf : "(identify failed)");
      g_free (sf);
    }
  g_free (out);
  /* AVIF: an odd-sized image keeps its exact size (no 1-pixel rescale), 4:4:4 for
   * a screenshot-type source, BT.709 full range tagged */
  fixture ("odd.png", run (work, NULL, "magick", "-size", "101x51", "gradient:red-blue", "-depth", "8", "odd.png", NULL));
  src = P ("odd.png");
  out = check ("odd.png", "avif");
  if (out != NULL)
    {
      char *w = probe_stream (out, "width"), *h = probe_stream (out, "height");
      char *pf = probe_stream (out, "pix_fmt"), *cs = probe_stream (out, "color_space");
      char *cr = probe_stream (out, "color_range");
      char *det = g_strdup_printf ("%sx%s %s %s %s", w, h, pf, cs, cr);

      add_row ("odd.png (101x51)", "avif size/format",
               g_str_equal (w, "101") && g_str_equal (h, "51") && g_str_equal (pf, "yuv444p") &&
               g_str_equal (cs, "bt709") && g_str_equal (cr, "pc"), det);
      g_free (det); g_free (w); g_free (h); g_free (pf); g_free (cs); g_free (cr);
    }
  g_free (out);
  /* WEBP of a PNG is lossless (pixel identical) */
  out = check ("odd.png", "webp");
  if (out != NULL)
    {
      char *m = NULL;
      gboolean ok = run (NULL, &m, "magick", "compare", "-metric", "AE", src, out, "null:", NULL);

      /* compare exits 1 when the images differ, 0 when identical */
      add_row ("odd.png", "webp lossless", ok, "pixel identical");
      g_free (m);
    }
  g_free (out);
  /* ICO of a non-square image: square entries, several sizes, not stretched */
  fixture ("wide400.png", run (work, NULL, "magick", "-size", "400x100", "gradient:red-blue", "-depth", "8", "wide400.png", NULL));
  out = check ("wide400.png", "ico");
  if (out != NULL)
    {
      char *sizes = NULL;
      gboolean ok = run (NULL, &sizes, "magick", "identify", "-format", "%wx%h ", out, NULL);

      add_row ("wide400.png (400x100)", "ico sizes",
               ok && sizes != NULL && g_str_equal (g_strstrip (sizes), "256x256 48x48 32x32 16x16"),
               sizes ? sizes : "(identify failed)");
      g_free (sizes);
    }
  g_free (out);
  /* ICO never enlarges: a 16x16 source gives one 16 entry, an 87x98 one 98,48,32,16
   * (the ICONDIR entries are read back with identify) */
  {
    const struct { const char *name, *size, *want; } ic[] = {
      { "ico16.png", "16x16", "16x16" },
      { "ico87x98.png", "87x98", "98x98 48x48 32x32 16x16" },
      { "ico48.png", "48x48", "48x48 32x32 16x16" },
    };

    for (guint i = 0; i < G_N_ELEMENTS (ic); i++)
      {
        char *sizes = NULL, *label = g_strdup_printf ("%s (%s)", ic[i].name, ic[i].size);

        fixture (ic[i].name, run (work, NULL, "magick", "-size", ic[i].size, "gradient:red-blue",
                                  "-depth", "8", ic[i].name, NULL));
        out = check (ic[i].name, "ico");
        if (out != NULL)
          {
            gboolean ok = run (NULL, &sizes, "magick", "identify", "-format", "%wx%h ", out, NULL);

            add_row (label, "ico entries (no upscaling)",
                     ok && sizes != NULL && g_str_equal (g_strstrip (sizes), ic[i].want),
                     sizes ? sizes : "(identify failed)");
            g_free (sizes);
          }
        g_free (out);
        g_free (label);
      }
  }
  g_free (src);
}

/* A system whose ffmpeg has no libx264 but libopenh264 (stock Fedora ffmpeg-free):
 * `ffmpeg -encoders` is replaced by the real list without libx264, then MKV -> MP4
 * and MP4 -> MKV must still be offered and must produce H.264 made by libopenh264. */
static void
test_openh264_only (void)
{
  char *enc = NULL, *out;
  GString *filtered = g_string_new (NULL);
  char **lines;
  gboolean have_oh;

  if (!run (NULL, &enc, "ffmpeg", "-hide_banner", "-encoders", NULL) || enc == NULL)
    return;
  have_oh = strstr (enc, "libopenh264") != NULL;
  if (!have_oh)
    {
      add_row ("test.mkv", "mp4 (libopenh264 only)", TRUE, "skipped: this ffmpeg has no libopenh264");
      g_free (enc);
      g_string_free (filtered, TRUE);
      return;
    }
  lines = g_strsplit (enc, "\n", -1);
  for (char **l = lines; *l; l++)
    if (strstr (*l, " libx264") == NULL)
      g_string_append_printf (filtered, "%s\n", *l);
  nfc_test_set_encoders (filtered->str);
  g_assert_false (nfc_have_encoder ("libx264"));
  /* test.mkv -> mp4 (re-encode) and test.mp4 -> mkv (stream copy first, then the
   * re-encode only if needed) */
  out = check ("test.mkv", "mp4");
  if (out != NULL)
    {
      char *tag = NULL;
      gboolean ok = run (NULL, &tag, "ffprobe", "-v", "error", "-select_streams", "v:0",
                         "-show_entries", "stream_tags=encoder:stream=codec_name,width,height",
                         "-of", "default=nw=1", out, NULL);

      add_row ("test.mkv", "mp4 made by libopenh264", ok && tag != NULL &&
               strstr (tag, "libopenh264") != NULL && strstr (tag, "codec_name=h264") != NULL &&
               strstr (tag, "width=320") != NULL, tag ? tag : "(ffprobe failed)");
      g_free (tag);
      g_free (out);
    }
  g_free (check ("test.mp4", "mkv"));
  g_free (check ("noaudio.mp4", "mkv"));
  /* back to the real list */
  nfc_test_set_encoders (enc);
  g_strfreev (lines);
  g_string_free (filtered, TRUE);
  g_free (enc);
}

/* Free memory in GiB (MemAvailable), 0 if unknown. */
static guint64
available_gib (void)
{
  char *text = NULL;
  guint64 kb = 0;

  if (g_file_get_contents ("/proc/meminfo", &text, NULL, NULL))
    {
      const char *p = strstr (text, "MemAvailable:");

      if (p != NULL)
        kb = g_ascii_strtoull (p + strlen ("MemAvailable:"), NULL, 10);
    }
  g_free (text);
  return kb / (1024 * 1024);
}

/* The user's real failure: a 19200 x 10800 RGBA screenshot (207 megapixels,
 * 3.3 GB of pixel cache) died with "cache resources exhausted" under the old
 * 1 GiB / 2 GiB / 4 GiB limits. The generated image compresses to under a
 * megabyte, but converting it needs ~6-8 GB of RAM: skipped (and said so) when
 * less than 16 GiB is available. NFC_TEST_SKIP_HUGE=1 skips it too. */
static void
test_large_image (void)
{
  const char *k[] = { "jpg", "webp", "avif", "ico", "bmp", "tiff", "pdf", NULL };
  guint64 gib = available_gib ();
  char *big = P ("huge/big.png");
  char *dir = P ("huge");

  if (g_getenv ("NFC_TEST_SKIP_HUGE") != NULL || gib < 16)
    {
      char *why = g_strdup_printf ("skipped: %" G_GUINT64_FORMAT " GiB available, 16 needed", gib);

      g_print ("large image: %s\n", why);
      add_row ("big.png (19200x10800)", "(all targets)", TRUE, why);
      g_free (why);
      g_free (big);
      g_free (dir);
      return;
    }
  g_mkdir_with_parents (dir, 0755);
  fixture ("big.png (19200x10800)",
           run (dir, NULL, "magick", "-size", "19200x10800", "gradient:#ff8000-#0040ff", "-alpha", "set",
                "-depth", "8", "-define", "png:compression-level=1", "PNG:big.png", NULL));
  for (const char **t = k; *t; t++)
    {
      char *out = check ("huge/big.png", *t);

      if (out != NULL)
        g_unlink (out); /* bmp and tiff are 800 MB each */
      g_free (out);
    }
  g_free (big);
  g_free (dir);
}

/* PDF page size. A screenshot has no resolution, ImageMagick then writes 72 dpi
 * and a 1920 px wide capture becomes a 26 inch (1920 pt) page; it must be an
 * A4-sized page instead, with the pixels untouched. Images that carry a real
 * resolution keep it. */
static gboolean
pdf_page_pts (const char *pdf, double *w, double *h)
{
  char *out = NULL, *p;
  gboolean ok = run (NULL, &out, "pdfinfo", pdf, NULL) && out != NULL &&
                (p = strstr (out, "Page size:")) != NULL &&
                sscanf (p + strlen ("Page size:"), " %lf x %lf", w, h) == 2;

  g_free (out);
  return ok;
}

/* size in pixels of the one image embedded in the PDF */
static char *
pdf_image_size (const char *pdf)
{
  char *out = NULL, *res = NULL;

  if (run (NULL, &out, "pdfimages", "-list", pdf, NULL) && out != NULL)
    {
      char **lines = g_strsplit (out, "\n", -1);

      if (g_strv_length (lines) > 2)
        {
          int page, num, w, h;
          char type[32];

          if (sscanf (lines[2], " %d %d %31s %d %d", &page, &num, type, &w, &h) == 5)
            res = g_strdup_printf ("%dx%d", w, h);
        }
      g_strfreev (lines);
    }
  g_free (out);
  return res != NULL ? res : g_strdup ("?");
}

static void
check_pdf_page (const char *name, const char *size, const char *density_args, double want_w,
                double want_h, double tol, const char *want_px)
{
  char *dir = P ("pdfsize"), *rel = g_strdup_printf ("pdfsize/%s", name), *out;
  double w = 0, h = 0;
  char *px, *detail;
  gboolean ok;

  g_mkdir_with_parents (dir, 0755);
  {
    char *file = g_strdup_printf ("PNG:%s", name);
    char *argv_dens[] = { "magick", "-size", (char *) size, "gradient:#ff8000-#0040ff", NULL, NULL, NULL, NULL, NULL };

    /* optional "-units PixelsPerInch -density N" (density_args = "N") */
    if (density_args != NULL)
      {
        argv_dens[4] = "-units"; argv_dens[5] = "PixelsPerInch";
        argv_dens[6] = "-density"; argv_dens[7] = (char *) density_args;
      }
    {
      GPtrArray *a = g_ptr_array_new ();
      gint status = -1;

      for (char **s = argv_dens; *s; s++)
        g_ptr_array_add (a, *s);
      g_ptr_array_add (a, file);
      g_ptr_array_add (a, NULL);
      fixture (name, g_spawn_sync (dir, (char **) a->pdata, NULL, G_SPAWN_SEARCH_PATH |
                                   G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL) &&
                     g_spawn_check_wait_status (status, NULL));
      g_ptr_array_unref (a);
    }
    g_free (file);
  }
  out = check (rel, "pdf");
  if (out == NULL || !pdf_page_pts (out, &w, &h))
    {
      add_row (rel, "pdf page size", FALSE, "no output or pdfinfo failed");
      g_free (out); g_free (dir); g_free (rel);
      return;
    }
  px = pdf_image_size (out);
  ok = w >= want_w - tol && w <= want_w + tol && h >= want_h - tol && h <= want_h + tol &&
       g_str_equal (px, want_px);
  detail = g_strdup_printf ("page %.1f x %.1f pt (%.2f x %.2f in), image %s px", w, h, w / 72, h / 72, px);
  add_row (rel, "pdf page size", ok, detail);
  g_print ("%s -> %s\n", name, detail);
  g_free (detail); g_free (px); g_free (out); g_free (dir); g_free (rel);
}

static void
test_pdf_page_size (void)
{
  char *pdfinfo = g_find_program_in_path ("pdfinfo"), *pdfimages = g_find_program_in_path ("pdfimages");

  if (pdfinfo == NULL || pdfimages == NULL)
    {
      add_row ("(pdf page size)", "skipped", TRUE, "pdfinfo/pdfimages not installed");
      g_free (pdfinfo); g_free (pdfimages);
      return;
    }
  /* no density: 165 dpi, 837.8 x 471.3 pt = A4 landscape wide (842 pt), pixels intact */
  check_pdf_page ("shot.png", "1920x1080", NULL, 837.8, 471.3, 1.0, "1920x1080");
  /* a tall phone screenshot: 217 dpi, A4 tall (842 pt) at most */
  check_pdf_page ("phone.png", "1170x2532", NULL, 388.2, 840.2, 1.0, "1170x2532");
  /* a small icon: never blown up beyond 96 dpi (64 px = 48 pt) */
  check_pdf_page ("icon.png", "64x64", NULL, 48.0, 48.0, 0.5, "64x64");
  /* a real 300 dpi resolution is kept: 1920 px = 6.4 in = 460.8 pt */
  check_pdf_page ("scan300.png", "1920x1080", "300", 460.8, 259.2, 0.5, "1920x1080");
  /* and a real 96 dpi (screen) too: 1920 px = 20 in = 1440 pt, not "fixed" */
  check_pdf_page ("dpi96.png", "1920x1080", "96", 1440.0, 810.0, 0.5, "1920x1080");
  g_free (pdfinfo); g_free (pdfimages);
}

/* ---- quality presets ---- */

static char *
convert_preset_sync (NfcCategory cat, const char *fmt, const char *key, NfcPreset preset,
                     const char *ext, const char *src, char **err)
{
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  char *out;

  nfc_convert_file_preset_async (cat, fmt, key, preset, ext, src, on_done, &w);
  g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  out = w.out;
  if (err)
    *err = w.error ? g_strdup (w.error->message) : NULL;
  g_clear_error (&w.error);
  return out;
}

static gint64
file_size (const char *path)
{
  GStatBuf st;

  return path != NULL && g_stat (path, &st) == 0 ? (gint64) st.st_size : -1;
}

/* "<w>x<h>" of the first frame / video stream of a file, or NULL */
static char *
dims_of (const char *path, gboolean video)
{
  char *out = NULL, *r = NULL;

  if (video)
    run (NULL, &out, "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=s=x:p=0", path, NULL);
  else
    {
      char *p0 = g_strconcat (path, "[0]", NULL);

      run (NULL, &out, "magick", "identify", "-format", "%wx%h", p0, NULL);
      g_free (p0);
    }
  if (out != NULL)
    r = g_strdup (g_strstrip (out));
  g_free (out);
  return r;
}

/* One real conversion per preset of src_name -> key (src in dir `dir`): the real
 * type, and Best >= High >= Balanced >= Small in size (equal allowed, for inputs
 * where a preset changes nothing). If small_dims is set, the picture size of the
 * Small result is checked, and that of the Best result against other_dims. */
static void
check_presets (const char *dir, const char *src_name, const char *key, const char *small_dims,
               const char *other_dims)
{
  char *src = g_build_filename (dir, src_name, NULL), *label;
  NfcFormat f;
  GPtrArray *tg;
  const NfcTarget *t = NULL;
  gint64 size[NFC_PRESET_N] = { 0 };
  gboolean pass = TRUE;
  GString *detail = g_string_new (NULL);

  label = g_strdup_printf ("%s presets", key);
  if (!nfc_classify (src, NULL, &f))
    {
      add_row (src_name, label, FALSE, "not classified");
      goto out;
    }
  tg = nfc_targets_for (f.cat, f.fmt);
  for (guint i = 0; i < tg->len; i++)
    if (g_str_equal (((const NfcTarget *) tg->pdata[i])->key, key))
      t = tg->pdata[i];
  g_ptr_array_unref (tg);
  if (t == NULL)
    {
      /* e.g. no libx264 / libvpx / libaom in this ffmpeg: nothing to test */
      add_row (src_name, label, TRUE, "not offered by this system: skipped");
      goto out;
    }
  g_assert_true (nfc_target_has_presets (f.cat, key));
  for (int p = 0; p < NFC_PRESET_N; p++)
    {
      char *err = NULL, *bad = NULL;
      char *o = convert_preset_sync (f.cat, f.fmt, t->key, (NfcPreset) p, t->ext, src, &err);

      if (o == NULL)
        {
          g_string_append_printf (detail, "%s: %s; ", nfc_preset_label ((NfcPreset) p),
                                  err ? err : "failed");
          pass = FALSE;
        }
      else
        {
          bad = check_type (t->ext, o);
          size[p] = file_size (o);
          if (bad != NULL || size[p] <= 0)
            {
              g_string_append_printf (detail, "%s: %s; ", nfc_preset_label ((NfcPreset) p),
                                      bad ? bad : "empty");
              pass = FALSE;
            }
          else
            g_string_append_printf (detail, "%s %" G_GINT64_FORMAT " ", nfc_preset_label ((NfcPreset) p),
                                    size[p]);
          if (pass && small_dims != NULL && (p == NFC_PRESET_BEST || p == NFC_PRESET_SMALL))
            {
              char *d = dims_of (o, f.cat == NFC_CAT_VIDEO);
              const char *want = p == NFC_PRESET_SMALL ? small_dims : other_dims;

              if (g_strcmp0 (d, want) != 0)
                {
                  g_string_append_printf (detail, "(%s is %s, expected %s) ",
                                          nfc_preset_label ((NfcPreset) p), d ? d : "?", want);
                  pass = FALSE;
                }
              g_free (d);
            }
        }
      g_free (bad);
      g_free (err);
      g_free (o);
    }
  if (pass)
    for (int p = 1; p < NFC_PRESET_N; p++)
      if (size[p] > size[p - 1])
        {
          g_string_append_printf (detail, "(%s is larger than %s) ",
                                  nfc_preset_label ((NfcPreset) p),
                                  nfc_preset_label ((NfcPreset) (p - 1)));
          pass = FALSE;
        }
  add_row (src_name, label, pass, detail->str);
out:
  g_string_free (detail, TRUE);
  g_free (label);
  g_free (src);
}

/* Notification text: a preset other than Best is named, Best is not. */
static void
test_preset_notifications (const char *dir)
{
  char *ndir = g_dir_make_tmp ("nfc-notify-XXXXXX", NULL);
  char *script = g_build_filename (ndir, "notify-send", NULL);
  char *log = g_build_filename (ndir, "calls.log", NULL);
  char *content = g_strdup_printf ("#!/bin/sh\necho \"$@\" >> '%s'\necho 4242\n", log);
  char *saved = nfc_tools.notify;
  struct { NfcPreset preset; guint n; const char *want_start, *want_done; } c[] = {
    { NFC_PRESET_BALANCED, 2, " -- Converting… 2 files to JPG (Balanced)",
      " -- Conversion complete Converted 2 files to JPG (Balanced)" },
    { NFC_PRESET_SMALL, 1, " -- Converting… n1.png to JPG (Small)",
      " -- Conversion complete n1.png → n1.jpg (Small)" },
    { NFC_PRESET_BEST, 2, " -- Converting… 2 files to JPG\n",
      " -- Conversion complete Converted 2 files to JPG\n" },
  };

  g_file_set_contents (script, content, -1, NULL);
  g_chmod (script, 0755);
  nfc_tools.notify = script;
  for (guint k = 0; k < G_N_ELEMENTS (c); k++)
    {
      char *sub = g_strdup_printf ("%s/n%u", dir, k);
      char *paths[3] = { NULL, NULL, NULL }, *fmts[] = { (char *) "png", (char *) "png", NULL };
      char *src = g_build_filename (dir, "n.png", NULL), *data = NULL, *logged = NULL, *label;
      gsize len;
      gboolean pass = FALSE;

      g_mkdir_with_parents (sub, 0755);
      g_file_get_contents (src, &data, &len, NULL);
      for (guint i = 0; i < c[k].n; i++)
        {
          char *name = g_strdup_printf ("n%u.png", i + 1);

          paths[i] = g_build_filename (sub, name, NULL);
          g_file_set_contents (paths[i], data, len, NULL);
          g_free (name);
        }
      g_remove (log);
      nfc_batch_run_preset (NFC_CAT_IMAGE, "jpg", c[k].preset, "JPG", "jpg",
                            (const char *const *) paths, (const char *const *) fmts, c[k].n,
                            NULL, NULL);
      for (int i = 0; i < 500 && !pass; i++)
        {
          g_main_context_iteration (NULL, FALSE);
          g_usleep (20000);
          g_clear_pointer (&logged, g_free);
          if (g_file_get_contents (log, &logged, NULL, NULL))
            pass = strstr (logged, "Conversion complete") != NULL ||
                   strstr (logged, "Conversion failed") != NULL;
        }
      pass = pass && logged != NULL && strstr (logged, c[k].want_start) != NULL &&
             strstr (logged, c[k].want_done) != NULL && strstr (logged, "(Best)") == NULL;
      label = g_strdup_printf ("batch of %u, %s", c[k].n, nfc_preset_label (c[k].preset));
      add_row ("(notifications)", label, pass, pass ? "the preset is named unless it is Best"
                                                   : (logged ? logged : "no calls"));
      /* the multi-file batch converted every file: same type each */
      if (pass)
        for (guint i = 0; i < c[k].n; i++)
          {
            char *want = g_strdup_printf ("n%u.jpg", i + 1);
            char *o = g_build_filename (sub, want, NULL);
            char *bad = check_type ("jpg", o);

            add_row ("(notifications)", want, bad == NULL && non_empty (o), bad ? bad : "converted");
            g_free (bad);
            g_free (o);
            g_free (want);
          }
      g_free (label);
      g_free (logged);
      g_free (data);
      g_free (src);
      g_free (paths[0]);
      g_free (paths[1]);
      g_free (sub);
    }
  nfc_tools.notify = saved;
  g_free (ndir);
  g_free (script);
  g_free (log);
  g_free (content);
}

static void
test_presets (void)
{
  char *dir = P ("presets");
  char *saved_notify = nfc_tools.notify;

  g_mkdir_with_parents (dir, 0755);
  nfc_tools.notify = NULL; /* no real notifications from the test */
  /* A screenshot-like image (smooth plasma with text and a hard edge) as PNG and,
   * for the photo class, as JPEG; a 3200x1800 image to see Small shrink; clips. */
  fixture ("presets/gfx.png",
           run (dir, NULL, "magick", "-seed", "7", "-size", "640x400", "plasma:fractal",
                "-fill", "white", "-pointsize", "28", "-annotate", "+20+50", "Hello presets 12345",
                "-fill", "black", "-draw", "rectangle 300,200 500,300", "gfx.png", NULL));
  fixture ("presets/photo.jpg", run (dir, NULL, "magick", "gfx.png", "-quality", "92", "photo.jpg", NULL));
  fixture ("presets/n.png", run (dir, NULL, "magick", "-size", "32x32", "xc:red", "n.png", NULL));
  fixture ("presets/big.png", run (dir, NULL, "magick", "-size", "3200x1800", "gradient:red-blue", "big.png", NULL));
  fixture ("presets/clip.mp4",
           run (dir, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                "-i", "testsrc2=d=2:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=2", "-shortest",
                "-pix_fmt", "yuv420p", "clip.mp4", NULL));
  fixture ("presets/clip.mkv", run (dir, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "clip.mp4", "-c", "copy", "clip.mkv", NULL));
  fixture ("presets/uhd.mkv",
           run (dir, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                "-i", "testsrc2=d=1:s=2560x1440:r=5", "-pix_fmt", "yuv420p", "uhd.mkv", NULL));
  fixture ("presets/tall.mkv",
           run (dir, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                "-i", "testsrc2=d=1:s=1440x2560:r=5", "-pix_fmt", "yuv420p", "tall.mkv", NULL));

  /* every preset-capable target, both source classes: type and size order */
  check_presets (dir, "gfx.png", "jpg", NULL, NULL);
  check_presets (dir, "photo.jpg", "webp", NULL, NULL);
  check_presets (dir, "gfx.png", "webp", NULL, NULL);
  check_presets (dir, "gfx.png", "avif", NULL, NULL);
  check_presets (dir, "photo.jpg", "avif", NULL, NULL);
  check_presets (dir, "clip.mkv", "mp4", NULL, NULL);
  check_presets (dir, "clip.mp4", "webm", NULL, NULL);
  check_presets (dir, "clip.mp4", "gif", "480x270", "960x540");
  /* Small shrinks what is larger than 2560 px / 1080p, the others never do */
  check_presets (dir, "big.png", "jpg", "2560x1440", "3200x1800");
  check_presets (dir, "uhd.mkv", "mp4", "1920x1080", "2560x1440");
  check_presets (dir, "tall.mkv", "mp4", "1080x1920", "1440x2560");

  test_preset_notifications (dir);
  nfc_tools.notify = saved_notify;
  g_free (dir);
}

/* ---- Compress and the keep-the-smaller rule ---- */

typedef struct {
  GMainLoop *loop;
  GPtrArray *results; /* NfcResult copies: "ok|skipped|fail", path, output, error */
  gboolean done;
} CompressWait;

static void
compress_batch_done (GPtrArray *results, gpointer data)
{
  CompressWait *w = data;

  for (guint i = 0; i < results->len; i++)
    {
      NfcResult *r = results->pdata[i];

      g_ptr_array_add (w->results, g_strdup_printf ("%s|%s|%s|%s",
                                                    r->ok ? "ok" : r->skipped ? "skipped" : "fail",
                                                    r->path, r->output ? r->output : "",
                                                    r->error ? r->error : ""));
    }
  w->done = TRUE;
  g_main_loop_quit (w->loop);
}

/* run nfc_batch_run_compress and wait; the results as "state|path|output|error" */
static GPtrArray *
compress_batch_sync (NfcCategory cat, NfcPreset preset, char **paths, char **fmts, guint n)
{
  CompressWait w = { g_main_loop_new (NULL, FALSE), g_ptr_array_new_with_free_func (g_free), FALSE };

  nfc_batch_run_compress (cat, preset, (const char *const *) paths, (const char *const *) fmts, n,
                          compress_batch_done, &w);
  if (!w.done)
    g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  return w.results;
}

/* nfc_convert_file_opts_async with the Compress options (what the batch does per
 * file). *skipped: the output was not smaller and was discarded. */
static char *
compress_sync (NfcCategory cat, const char *fmt, NfcPreset preset, const char *src,
               gboolean *skipped, char **err)
{
  NfcCompressTarget t;
  NfcConvertOptions o = { preset, " (compressed)", NFC_REFERENCE_SOURCE, NULL };
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  char *out;

  *skipped = FALSE;
  if (err != NULL)
    *err = NULL;
  if (!nfc_compress_target (cat, fmt, &t))
    {
      g_main_loop_unref (w.loop);
      if (err != NULL)
        *err = g_strdup ("not compressible");
      return NULL;
    }
  nfc_convert_file_opts_async (t.cat, fmt, t.key, t.ext, src, &o, on_done, &w);
  g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  out = w.out;
  *skipped = w.error != NULL && g_error_matches (w.error, NFC_CONVERT_ERROR,
                                                 NFC_CONVERT_ERROR_NOT_SMALLER);
  if (err != NULL)
    *err = w.error ? g_strdup (w.error->message) : NULL;
  g_clear_error (&w.error);
  return out;
}

static char *
checksum_of (const char *path)
{
  char *data;
  gsize len;
  char *sum = NULL;

  if (g_file_get_contents (path, &data, &len, NULL))
    {
      sum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) data, len);
      g_free (data);
    }
  return sum;
}

/* every entry of dir, space separated, minus the ones in `known` (NULL-ended) */
static char *
unexpected_entries (const char *dir, const char *const *known)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *n;
  GString *s = g_string_new (NULL);

  while (d != NULL && (n = g_dir_read_name (d)) != NULL)
    if (!g_strv_contains (known, n))
      g_string_append_printf (s, "%s ", n);
  if (d != NULL)
    g_dir_close (d);
  return g_string_free (s, FALSE);
}

/* Compress src_name (in dir) with High, Balanced and Small: every result is the
 * same format, named "<stem> (compressed).<ext>", a real file of that type,
 * smaller than the source; the source is untouched and nothing is left behind. */
static void
check_compress (const char *dir, const char *src_name)
{
  char *src = g_build_filename (dir, src_name, NULL), *stem = nfc_stem_of (src);
  char *label = g_strdup ("compress"), *before = checksum_of (src), *left;
  gint64 src_size = file_size (src), size[NFC_PRESET_N] = { 0 };
  GString *detail = g_string_new (NULL);
  gboolean pass = TRUE;
  NfcFormat f;
  NfcCompressTarget t;

  if (!nfc_classify (src, NULL, &f) || !nfc_compress_target (f.cat, f.fmt, &t))
    {
      add_row (src_name, label, FALSE, "not compressible");
      goto out;
    }
  if (!nfc_compress_available (f.cat, f.fmt))
    {
      /* e.g. no libvpx / libaom / libx264 in this ffmpeg: nothing to test */
      add_row (src_name, label, TRUE, "not offered by this system: skipped");
      goto out;
    }
  for (int p = NFC_PRESET_HIGH; p < NFC_PRESET_N && pass; p++)
    {
      gboolean skipped;
      char *err = NULL;
      char *o = compress_sync (f.cat, f.fmt, (NfcPreset) p, src, &skipped, &err);
      char *want = g_strdup_printf ("%s (compressed).%s", stem, t.ext), *base = NULL, *bad;

      if (o == NULL)
        {
          g_string_append_printf (detail, "%s: %s%s; ", nfc_preset_label ((NfcPreset) p),
                                  skipped ? "SKIPPED, not smaller: " : "", err ? err : "failed");
          pass = FALSE;
        }
      else
        {
          base = g_path_get_basename (o);
          size[p] = file_size (o);
          bad = check_type (t.ext, o);
          if (g_strcmp0 (base, want) != 0)
            {
              g_string_append_printf (detail, "%s: named %s, expected %s; ",
                                      nfc_preset_label ((NfcPreset) p), base, want);
              pass = FALSE;
            }
          else if (bad != NULL || size[p] <= 0)
            {
              g_string_append_printf (detail, "%s: %s; ", nfc_preset_label ((NfcPreset) p),
                                      bad ? bad : "empty");
              pass = FALSE;
            }
          else if (size[p] >= src_size)
            {
              g_string_append_printf (detail, "%s: %" G_GINT64_FORMAT " bytes is not smaller than the source %"
                                      G_GINT64_FORMAT "; ", nfc_preset_label ((NfcPreset) p), size[p],
                                      src_size);
              pass = FALSE;
            }
          else
            g_string_append_printf (detail, "%s %" G_GINT64_FORMAT "%% ", nfc_preset_label ((NfcPreset) p),
                                    size[p] * 100 / src_size);
          g_free (bad);
          g_unlink (o); /* the next preset publishes under the same name */
        }
      g_free (want);
      g_free (base);
      g_free (err);
      g_free (o);
    }
  if (pass && (size[NFC_PRESET_BALANCED] > size[NFC_PRESET_HIGH] ||
               size[NFC_PRESET_SMALL] > size[NFC_PRESET_BALANCED]))
    {
      g_string_append (detail, "(sizes do not decrease High > Balanced > Small) ");
      pass = FALSE;
    }
  {
    char *after = checksum_of (src);

    if (g_strcmp0 (before, after) != 0)
      {
        g_string_append (detail, "(the original changed) ");
        pass = FALSE;
      }
    g_free (after);
  }
  left = unexpected_entries (dir, (const char *const[]) { src_name, NULL });
  if (left[0] != '\0')
    {
      g_string_append_printf (detail, "(left behind: %s)", left);
      pass = FALSE;
    }
  g_free (left);
  add_row (src_name, label, pass, detail->str);
out:
  g_string_free (detail, TRUE);
  g_free (label);
  g_free (before);
  g_free (stem);
  g_free (src);
}

/* The keep-the-smaller rule: an output that is not smaller than the reference is
 * discarded, with nothing left behind */
static void
test_keep_smaller (const char *dir)
{
  const char *sub = "keep";
  char *d = g_build_filename (dir, sub, NULL), *tiny, *gfx;
  gboolean skipped;
  char *err = NULL, *o, *left;
  NfcConvertOptions opts;
  Wait w;

  g_mkdir_with_parents (d, 0755);
  fixture ("compress/keep/tiny.jpg",
           run (d, NULL, "magick", "-seed", "3", "-size", "200x150", "plasma:fractal",
                "-quality", "40", "tiny.jpg", NULL));
  fixture ("compress/keep/gfx.png",
           run (d, NULL, "magick", "-seed", "7", "-size", "320x200", "plasma:fractal", "gfx.png", NULL));
  tiny = g_build_filename (d, "tiny.jpg", NULL);
  gfx = g_build_filename (d, "gfx.png", NULL);

  /* a tiny q40 JPG compressed with High (q92) comes out larger: skipped, nothing left */
  for (int p = NFC_PRESET_HIGH; p < NFC_PRESET_N; p++)
    {
      char *label = g_strdup_printf ("tiny.jpg compress %s", nfc_preset_label ((NfcPreset) p));

      o = compress_sync (NFC_CAT_IMAGE, "jpg", (NfcPreset) p, tiny, &skipped, &err);
      left = unexpected_entries (d, (const char *const[]) { "tiny.jpg", "gfx.png", NULL });
      /* Small shrinks nothing here (200 px) and may or may not be smaller: only
       * the invariant is checked there (skipped XOR a smaller file) */
      if (p == NFC_PRESET_SMALL && o != NULL)
        {
          add_row ("keep-smaller", label, file_size (o) < file_size (tiny), "smaller: kept");
          g_unlink (o);
        }
      else
        add_row ("keep-smaller", label, o == NULL && skipped && left[0] == '\0' &&
                 g_strcmp0 (err, "already as small as it gets") == 0,
                 o ? o : (left[0] ? left : (err ? err : "")));
      if (p == NFC_PRESET_SMALL && o != NULL)
        g_free (left), left = g_strdup ("");
      g_free (label);
      g_free (left);
      g_free (err);
      g_free (o);
    }

  /* the general mechanism: an explicit reference size, no name suffix */
  opts = (NfcConvertOptions) { NFC_PRESET_BEST, NULL, 1, NULL };
  w = (Wait) { g_main_loop_new (NULL, FALSE), NULL, NULL };
  nfc_convert_file_opts_async (NFC_CAT_IMAGE, "png", "jpg", "jpg", gfx, &opts, on_done, &w);
  g_main_loop_run (w.loop);
  left = unexpected_entries (d, (const char *const[]) { "tiny.jpg", "gfx.png", NULL });
  add_row ("keep-smaller", "reference 1 byte: discarded", w.out == NULL && left[0] == '\0' &&
           g_error_matches (w.error, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_NOT_SMALLER),
           w.error ? w.error->message : (left[0] ? left : "kept"));
  g_free (left);
  g_clear_error (&w.error);
  g_free (w.out);

  opts.reference_size = G_GINT64_CONSTANT (1) << 40;
  w.out = NULL;
  nfc_convert_file_opts_async (NFC_CAT_IMAGE, "png", "jpg", "jpg", gfx, &opts, on_done, &w);
  g_main_loop_run (w.loop);
  {
    char *base = w.out ? g_path_get_basename (w.out) : NULL;

    add_row ("keep-smaller", "reference 1 TiB: kept, plain name",
             g_strcmp0 (base, "gfx.jpg") == 0 && non_empty (w.out), w.error ? w.error->message : base);
    g_free (base);
  }
  if (w.out)
    g_unlink (w.out);
  g_free (w.out);
  g_clear_error (&w.error);

  /* NFC_REFERENCE_SOURCE: smaller than the PNG source itself -> kept */
  opts.reference_size = NFC_REFERENCE_SOURCE;
  w.out = NULL;
  nfc_convert_file_opts_async (NFC_CAT_IMAGE, "png", "jpg", "jpg", gfx, &opts, on_done, &w);
  g_main_loop_run (w.loop);
  add_row ("keep-smaller", "reference = source", w.out != NULL && file_size (w.out) < file_size (gfx),
           w.error ? w.error->message : "kept a smaller file");
  if (w.out)
    g_unlink (w.out);
  g_free (w.out);
  g_clear_error (&w.error);
  g_main_loop_unref (w.loop);

  g_free (tiny);
  g_free (gfx);
  g_free (d);
}

/* a name that is taken: "(compressed) (1)", never overwrites anything */
static void
test_compress_naming (const char *dir)
{
  char *d = g_build_filename (dir, "naming", NULL), *src, *o1, *o2, *o3, *b1, *b2, *b3;
  char *before1, *sums_ok;
  gboolean skipped;

  g_mkdir_with_parents (d, 0755);
  fixture ("compress/naming/pic.jpg",
           run (d, NULL, "magick", "-seed", "5", "-size", "800x500", "plasma:fractal", "-quality", "96",
                "pic.jpg", NULL));
  src = g_build_filename (d, "pic.jpg", NULL);
  o1 = compress_sync (NFC_CAT_IMAGE, "jpg", NFC_PRESET_HIGH, src, &skipped, NULL);
  before1 = o1 ? checksum_of (o1) : NULL;
  o2 = compress_sync (NFC_CAT_IMAGE, "jpg", NFC_PRESET_SMALL, src, &skipped, NULL);
  o3 = compress_sync (NFC_CAT_IMAGE, "jpg", NFC_PRESET_BALANCED, src, &skipped, NULL);
  b1 = o1 ? g_path_get_basename (o1) : NULL;
  b2 = o2 ? g_path_get_basename (o2) : NULL;
  b3 = o3 ? g_path_get_basename (o3) : NULL;
  {
    char *after1 = o1 ? checksum_of (o1) : NULL;

    sums_ok = g_strcmp0 (before1, after1) == 0 ? NULL : g_strdup ("the first output was overwritten");
    g_free (after1);
  }
  add_row ("pic.jpg", "compress naming",
           g_strcmp0 (b1, "pic (compressed).jpg") == 0 &&
           g_strcmp0 (b2, "pic (compressed) (1).jpg") == 0 &&
           g_strcmp0 (b3, "pic (compressed) (2).jpg") == 0 && sums_ok == NULL,
           sums_ok ? sums_ok : (b3 ? b3 : "failed"));
  g_free (sums_ok);
  g_free (before1);
  g_free (b1); g_free (b2); g_free (b3);
  g_free (o1); g_free (o2); g_free (o3);
  g_free (src);
  g_free (d);
}

/* Several files at once, in several formats, through the batch + notifications */
static void
test_compress_batch (const char *dir)
{
  char *d = g_build_filename (dir, "batch", NULL);
  char *paths[4], *fmts[4];
  GPtrArray *res;
  gboolean pass = TRUE;
  GString *detail = g_string_new (NULL);
  const char *want[] = { "m1 (compressed).jpg", "m2 (compressed).webp", "m3 (compressed).gif" };

  g_mkdir_with_parents (d, 0755);
  fixture ("compress/batch/m1.jpg",
           run (d, NULL, "magick", "-seed", "5", "-size", "800x500", "plasma:fractal", "-quality", "96",
                "m1.jpg", NULL));
  fixture ("compress/batch/m2.webp",
           run (d, NULL, "magick", "m1.jpg", "-quality", "98", "WEBP:m2.webp", NULL));
  fixture ("compress/batch/m3.gif",
           run (d, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                "testsrc2=d=2:s=800x600:r=20", "m3.gif", NULL));
  paths[0] = g_build_filename (d, "m1.jpg", NULL);
  paths[1] = g_build_filename (d, "m2.webp", NULL);
  paths[2] = g_build_filename (d, "m3.gif", NULL);
  paths[3] = NULL;
  fmts[0] = (char *) "jpg"; fmts[1] = (char *) "webp"; fmts[2] = (char *) "gif"; fmts[3] = NULL;

  /* mixed formats of the image category: each in its own format (a GIF is
   * compressed with the GIF presets) */
  nfc_tools.notify = NULL;
  res = compress_batch_sync (NFC_CAT_IMAGE, NFC_PRESET_BALANCED, paths, fmts, 3);
  g_assert_cmpuint (res->len, ==, 3);
  for (guint i = 0; i < 3; i++)
    {
      char **v = g_strsplit (res->pdata[i], "|", 4);
      char *base = g_path_get_basename (v[2]);
      char *bad = v[2][0] ? check_type (strrchr (want[i], '.') + 1, v[2]) : g_strdup ("no output");

      if (!g_str_equal (v[0], "ok") || g_strcmp0 (base, want[i]) != 0 || bad != NULL ||
          file_size (v[2]) >= file_size (paths[i]))
        {
          g_string_append_printf (detail, "%s: %s %s %s; ", want[i], v[0], base, bad ? bad : "");
          pass = FALSE;
        }
      else
        g_string_append_printf (detail, "%s %" G_GINT64_FORMAT "%% ", want[i],
                                file_size (v[2]) * 100 / file_size (paths[i]));
      g_free (bad);
      g_free (base);
      g_strfreev (v);
    }
  add_row ("jpg + webp + gif", "compress batch (Balanced)", pass, detail->str);
  g_ptr_array_unref (res);
  g_string_free (detail, TRUE);
  for (guint i = 0; i < 3; i++)
    g_free (paths[i]);
  g_free (d);
}

/* The notification texts of Compress */
static void
test_compress_notifications (const char *dir)
{
  char *ndir = g_dir_make_tmp ("nfc-notify-XXXXXX", NULL);
  char *script = g_build_filename (ndir, "notify-send", NULL);
  char *log = g_build_filename (ndir, "calls.log", NULL);
  char *content = g_strdup_printf ("#!/bin/sh\necho \"$@\" >> '%s'\necho 4242\n", log);
  char *saved = nfc_tools.notify;
  struct {
    const char *label;
    NfcPreset preset;
    const char *files[4];   /* "big", "tiny" or "bad" each */
    const char *want[4];    /* all of these must be in the log */
    const char *unwanted;
  } c[] = {
    { "one file", NFC_PRESET_HIGH, { "big", NULL },
      { " -- Compressing… f0.jpg (High)", " -- Compression complete f0.jpg → f0 (compressed).jpg (High), −", "%", NULL },
      "already as small" },
    { "three files", NFC_PRESET_BALANCED, { "big", "big", "big", NULL },
      { " -- Compressing… 3 files (Balanced)", " -- Compression complete Compressed 3 files (Balanced), −", "%", NULL },
      "already as small" },
    { "already as small as it gets", NFC_PRESET_HIGH, { "tiny", NULL },
      { " -- Compressing… f0.jpg (High)", " -- Nothing to compress f0.jpg is already as small as it gets", NULL, NULL },
      "Compression failed" },
    { "one good, one skipped", NFC_PRESET_BALANCED, { "big", "tiny", NULL },
      { " -- Compression complete f0.jpg → f0 (compressed).jpg (Balanced), −",
        "\nf1.jpg is already as small as it gets", NULL, NULL },
      "Compression failed" },
    { "one good, one broken", NFC_PRESET_BALANCED, { "big", "bad", NULL },
      { " -- Compression failed f0.jpg → f0 (compressed).jpg (Balanced), −", "\nFailed: f1.jpg — ", NULL, NULL },
      "already as small" },
  };

  g_file_set_contents (script, content, -1, NULL);
  g_chmod (script, 0755);
  nfc_tools.notify = script;
  for (guint k = 0; k < G_N_ELEMENTS (c); k++)
    {
      char *sub = g_strdup_printf ("%s/nt%u", dir, k);
      char *paths[5] = { NULL }, *fmts[5] = { NULL }, *logged = NULL;
      guint n = 0;
      gboolean pass = FALSE;
      GString *miss = g_string_new (NULL);
      char *big = g_build_filename (dir, "keep", "gfx.png", NULL);

      g_mkdir_with_parents (sub, 0755);
      for (; c[k].files[n] != NULL; n++)
        {
          char *name = g_strdup_printf ("f%u.jpg", n);

          paths[n] = g_build_filename (sub, name, NULL);
          fmts[n] = (char *) "jpg";
          if (g_str_equal (c[k].files[n], "big"))
            run (sub, NULL, "magick", "-seed", "9", "-size", "640x400", "plasma:fractal", "-quality", "96", name, NULL);
          else if (g_str_equal (c[k].files[n], "tiny"))
            run (sub, NULL, "magick", "-seed", "3", "-size", "200x150", "plasma:fractal", "-quality", "40", name, NULL);
          else
            g_file_set_contents (paths[n], "this is not a jpeg", -1, NULL);
          g_free (name);
        }
      g_remove (log);
      {
        GPtrArray *res = compress_batch_sync (NFC_CAT_IMAGE, c[k].preset, paths, fmts, n);

        g_ptr_array_unref (res);
      }
      for (int i = 0; i < 500 && !pass; i++)
        {
          g_main_context_iteration (NULL, FALSE);
          g_usleep (20000);
          g_clear_pointer (&logged, g_free);
          if (g_file_get_contents (log, &logged, NULL, NULL))
            pass = strstr (logged, "Compression complete") != NULL ||
                   strstr (logged, "Compression failed") != NULL ||
                   strstr (logged, "Nothing to compress") != NULL;
        }
      if (!pass)
        g_string_append (miss, "no final notification; ");
      for (guint i = 0; pass && i < 4 && c[k].want[i] != NULL; i++)
        if (strstr (logged, c[k].want[i]) == NULL)
          {
            g_string_append_printf (miss, "missing '%s'; ", c[k].want[i]);
            pass = FALSE;
          }
      if (pass && c[k].unwanted != NULL && strstr (logged, c[k].unwanted) != NULL)
        {
          g_string_append_printf (miss, "unexpected '%s'; ", c[k].unwanted);
          pass = FALSE;
        }
      add_row ("(notifications)", c[k].label, pass,
               pass ? "Compress notification texts" : (miss->len ? miss->str : "?"));
      if (!pass && logged != NULL)
        g_printerr ("--- notify log of '%s':\n%s\n", c[k].label, logged);
      g_string_free (miss, TRUE);
      g_free (logged);
      g_free (big);
      for (guint i = 0; i < n; i++)
        g_free (paths[i]);
      g_free (sub);
    }
  nfc_tools.notify = saved;
  g_free (ndir);
  g_free (script);
  g_free (log);
  g_free (content);
}

/* WEBP graphics presets: lossy encode vs lossless, the smaller wins */
static void
test_webp_graphics_presets (const char *dir)
{
  char *d = g_build_filename (dir, "webpgfx", NULL), *flat, *plasma;
  gint64 best_flat = 0, best_plasma = 0, flat_size[NFC_PRESET_N] = { 0 }, plasma_size[NFC_PRESET_N] = { 0 };
  gboolean pass = TRUE;
  GString *detail = g_string_new (NULL);
  char *left;

  g_mkdir_with_parents (d, 0755);
  /* flat text on a plain background: lossless is smaller than any lossy q */
  fixture ("compress/webpgfx/flat.png",
           run (d, NULL, "magick", "-size", "800x500", "xc:#1e1e1e", "-fill", "#d0d0d0", "-pointsize", "16",
                "-annotate", "+10+30", "user@host:~$ ls -la /usr/share/doc | head -n 20\n"
                "drwxr-xr-x 2 root root 4096 Nov 18 12:00 alpha\n"
                "drwxr-xr-x 2 root root 4096 Nov 18 12:00 beta\n", "flat.png", NULL));
  /* smooth gradients and noise: a lossy encode is far smaller */
  fixture ("compress/webpgfx/plasma.png",
           run (d, NULL, "magick", "-seed", "7", "-size", "640x400", "plasma:fractal",
                "-fill", "white", "-pointsize", "28", "-annotate", "+20+50", "Hello presets 12345",
                "plasma.png", NULL));
  flat = g_build_filename (d, "flat.png", NULL);
  plasma = g_build_filename (d, "plasma.png", NULL);
  for (int p = 0; p < NFC_PRESET_N; p++)
    {
      for (int k = 0; k < 2; k++)
        {
          const char *src = k ? plasma : flat;
          char *err = NULL;
          char *o = convert_preset_sync (NFC_CAT_IMAGE, "png", "webp", (NfcPreset) p, "webp", src, &err);
          gint64 sz = file_size (o);
          char *bad = o ? check_type ("webp", o) : g_strdup (err ? err : "failed");

          if (bad != NULL || sz <= 0)
            {
              g_string_append_printf (detail, "%s %s: %s; ", k ? "plasma" : "flat",
                                      nfc_preset_label ((NfcPreset) p), bad ? bad : "empty");
              pass = FALSE;
            }
          (k ? plasma_size : flat_size)[p] = sz;
          if (p == 0)
            (k ? &best_plasma : &best_flat)[0] = sz;
          if (o != NULL)
            g_unlink (o);
          g_free (bad);
          g_free (err);
          g_free (o);
        }
    }
  for (int p = 1; pass && p < NFC_PRESET_N; p++)
    {
      /* flat: lossless wins every time: exactly Best's size, never larger */
      if (flat_size[p] != best_flat)
        {
          g_string_append_printf (detail, "flat %s is %" G_GINT64_FORMAT ", Best (lossless) %" G_GINT64_FORMAT "; ",
                                  nfc_preset_label ((NfcPreset) p), flat_size[p], best_flat);
          pass = FALSE;
        }
      /* plasma: lossy wins by far, and shrinks with the preset */
      if (plasma_size[p] * 2 > best_plasma || plasma_size[p] > plasma_size[p - 1])
        {
          g_string_append_printf (detail, "plasma %s is %" G_GINT64_FORMAT " (Best %" G_GINT64_FORMAT ", previous %"
                                  G_GINT64_FORMAT "); ", nfc_preset_label ((NfcPreset) p),
                                  plasma_size[p], best_plasma, plasma_size[p - 1]);
          pass = FALSE;
        }
    }
  left = unexpected_entries (d, (const char *const[]) { "flat.png", "plasma.png", NULL });
  if (left[0] != '\0')
    {
      g_string_append_printf (detail, "(left behind: %s)", left);
      pass = FALSE;
    }
  if (pass)
    g_string_append_printf (detail, "flat %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
                            " (Best %" G_GINT64_FORMAT "), plasma %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
                            "/%" G_GINT64_FORMAT " (Best %" G_GINT64_FORMAT ")",
                            flat_size[1], flat_size[2], flat_size[3], best_flat,
                            plasma_size[1], plasma_size[2], plasma_size[3], best_plasma);
  add_row ("flat.png, plasma.png", "webp graphics: smaller of lossy and lossless", pass, detail->str);
  g_free (left);
  g_string_free (detail, TRUE);
  g_free (flat);
  g_free (plasma);
  g_free (d);
}

static void
test_compress (void)
{
  char *dir = P ("compress");
  char *saved_notify = nfc_tools.notify;

  g_mkdir_with_parents (dir, 0755);
  nfc_tools.notify = NULL; /* no real notifications from the test */
  {
    /* one directory per source file: the "nothing left behind" check lists it */
    static const char *const names[] = { "photo.jpg", "photo.jpeg", "photo.webp", "photo.avif",
                                         "clip.mp4", "clip.m4v", "clip.webm", "anim.gif" };
    char *sub[G_N_ELEMENTS (names)];
    char *src = NULL;

    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
      {
        sub[i] = g_strdup_printf ("%s/c%u", dir, i);
        g_mkdir_with_parents (sub[i], 0755);
      }
    /* a high-quality photograph-like JPG (q97), WEBP and AVIF made from it */
    fixture ("photo.jpg", run (sub[0], NULL, "magick", "-seed", "11", "-size", "1280x800", "plasma:fractal",
                               "-attenuate", "0.3", "+noise", "Gaussian", "-quality", "97", "photo.jpg", NULL));
    src = g_build_filename (sub[1], "photo.jpeg", NULL);
    {
      char *from = g_build_filename (sub[0], "photo.jpg", NULL);
      gchar *data;
      gsize len;

      if (g_file_get_contents (from, &data, &len, NULL))
        {
          g_file_set_contents (src, data, len, NULL);
          g_free (data);
        }
      g_free (from);
    }
    g_free (src);
    fixture ("photo.webp", run (sub[2], NULL, "magick", "../c0/photo.jpg", "-quality", "98", "WEBP:photo.webp", NULL));
    fixture ("photo.avif", run (sub[3], NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
                                "../c0/photo.jpg", "-frames:v", "1", "-c:v", "libaom-av1", "-crf", "10",
                                "-cpu-used", "8", "-still-picture", "1", "-f", "avif", "photo.avif", NULL));
    /* video: an MPEG-4 part 2 encode at q2 is large for what it holds */
    fixture ("clip.mp4", run (sub[4], NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                              "-i", "testsrc2=d=2:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=2", "-shortest",
                              "-c:v", "mpeg4", "-q:v", "2", "-pix_fmt", "yuv420p", "clip.mp4", NULL));
    fixture ("clip.m4v", run (sub[5], NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
                              "../c4/clip.mp4", "-c", "copy", "-f", "mp4", "clip.m4v", NULL));
    fixture ("clip.webm", run (sub[6], NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                               "-i", "testsrc2=d=2:s=1280x720:r=25", "-c:v", "libvpx-vp9", "-crf", "12",
                               "-b:v", "0", "-deadline", "realtime", "-cpu-used", "8", "clip.webm", NULL));
    fixture ("anim.gif", run (sub[7], NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                              "-i", "testsrc2=d=2:s=1000x750:r=25", "anim.gif", NULL));
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
      {
        check_compress (sub[i], names[i]);
        g_free (sub[i]);
      }
  }
  test_keep_smaller (dir);
  test_compress_naming (dir);
  test_compress_batch (dir);
  test_webp_graphics_presets (dir);
  nfc_tools.notify = saved_notify;
  test_compress_notifications (dir);
  nfc_tools.notify = saved_notify;
  g_free (dir);
}

/* ---- Resize ---- */

typedef struct {
  GMainLoop *loop;
  char *out;
  GError *error;
  guint64 w, h;
} ResizeWait;

static void
resize_on_done (GObject *o, GAsyncResult *res, gpointer data)
{
  ResizeWait *w = data;

  (void) o;
  nfc_convert_result_dims (res, &w->w, &w->h);
  w->out = nfc_convert_file_finish (res, &w->error);
  g_main_loop_quit (w->loop);
}

/* what the batch does for one file: the target of the resize, the name suffix, the
 * preset. *skipped: the preset would not make the source smaller (nothing made).
 * src_w, src_h: the size the job probed. */
static char *
resize_sync (NfcCategory cat, const char *fmt, const char *rs_key, const char *src,
             gboolean *skipped, char **err, guint64 *src_w, guint64 *src_h)
{
  NfcResizeTarget t;
  const NfcResizePreset *rs = nfc_resize_preset_from_key (cat, rs_key);
  char *suffix = rs != NULL ? g_strdup_printf (" (%s)", rs->name) : NULL;
  NfcConvertOptions o = { NFC_PRESET_BEST, suffix, NFC_REFERENCE_NONE, rs };
  ResizeWait w = { g_main_loop_new (NULL, FALSE), NULL, NULL, 0, 0 };
  char *out;

  *skipped = FALSE;
  if (err != NULL)
    *err = NULL;
  if (rs == NULL || !nfc_resize_target (cat, fmt, &t))
    {
      g_main_loop_unref (w.loop);
      g_free (suffix);
      if (err != NULL)
        *err = g_strdup ("not resizable");
      return NULL;
    }
  nfc_convert_file_opts_async (t.cat, fmt, t.key, t.ext, src, &o, resize_on_done, &w);
  g_main_loop_run (w.loop);
  g_main_loop_unref (w.loop);
  g_free (suffix);
  out = w.out;
  *skipped = w.error != NULL && g_error_matches (w.error, NFC_CONVERT_ERROR,
                                                 NFC_CONVERT_ERROR_NOT_SMALLER);
  if (err != NULL)
    *err = w.error ? g_strdup (w.error->message) : NULL;
  if (src_w != NULL)
    *src_w = w.w;
  if (src_h != NULL)
    *src_h = w.h;
  g_clear_error (&w.error);
  return out;
}

/* "WxH" of the first frame of an image or the first video stream of path (g_free) */
static char *
size_of (const char *path)
{
  char *out = NULL, *r;

  if (!run (NULL, &out, "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
            "stream=width,height", "-of", "csv=p=0:s=x", path, NULL) || out == NULL)
    {
      g_free (out);
      return g_strdup ("(ffprobe failed)");
    }
  r = g_strdup (g_strstrip (out));
  g_free (out);
  return r;
}

/* one ffprobe field of the first stream of a kind ("v:0" / "a:0"), e.g. codec_name */
static char *
stream_field (const char *path, const char *stream, const char *field)
{
  char *out = NULL, *entries = g_strdup_printf ("stream=%s", field), *r;

  if (!run (NULL, &out, "ffprobe", "-v", "error", "-select_streams", stream, "-show_entries",
            entries, "-of", "csv=p=0", path, NULL) || out == NULL)
    r = g_strdup ("");
  else
    r = g_strdup (g_strstrip (out));
  g_free (out);
  g_free (entries);
  return r;
}

/* frames of an image or animated GIF (all of them, no decoding) */
static char *
frames_of (const char *path)
{
  char *out = NULL, *r;

  if (!run (NULL, &out, "magick", "identify", "-ping", "-format", "%n\n", path, NULL) || out == NULL)
    {
      g_free (out);
      return g_strdup ("?");
    }
  r = g_strdup (strtok (out, "\n"));
  g_free (out);
  return r;
}

/* Resize `src_name` (in `dir`, alone there) with preset rs_key: the output is named
 * `want_name`, is a real file of the same type, has exactly `want_dims` ("WxH"), the
 * source is untouched and nothing else is left behind. extra: more to check on the
 * output (may be NULL), returns an error text or NULL. */
static void
check_resize (const char *dir, const char *src_name, const char *rs_key, const char *want_name,
              const char *want_dims, char *(*extra) (const char *src, const char *out))
{
  char *src = g_build_filename (dir, src_name, NULL), *before = checksum_of (src), *label;
  char *err = NULL, *out, *detail = NULL, *left;
  gboolean skipped;
  guint64 sw = 0, sh = 0;
  NfcFormat f;
  NfcResizeTarget t;
  gboolean pass = FALSE;

  label = g_strdup_printf ("resize %s", rs_key);
  if (!nfc_classify (src, NULL, &f) || !nfc_resize_target (f.cat, f.fmt, &t))
    {
      add_row (src_name, label, FALSE, "not resizable");
      goto out;
    }
  if (!nfc_resize_available (f.cat, f.fmt))
    {
      add_row (src_name, label, TRUE, "not offered by this system: skipped");
      goto out;
    }
  out = resize_sync (f.cat, f.fmt, rs_key, src, &skipped, &err, &sw, &sh);
  if (out == NULL)
    detail = g_strdup_printf ("%s%s", skipped ? "SKIPPED: " : "", err ? err : "failed");
  else
    {
      char *base = g_path_get_basename (out), *bad = NULL, *d = size_of (out), *x = NULL;
      char *after = checksum_of (src);

      if (!g_str_equal (base, want_name))
        bad = g_strdup_printf ("named %s, expected %s", base, want_name);
      else if (!g_str_equal (d, want_dims))
        bad = g_strdup_printf ("is %s, expected %s", d, want_dims);
      else if (!non_empty (out))
        bad = g_strdup ("empty");
      else if (g_str_equal (t.ext, "mov") || g_str_equal (t.ext, "avi"))
        {
          char *fmt_name = NULL;

          run (NULL, &fmt_name, "ffprobe", "-v", "error", "-show_entries", "format=format_name",
               "-of", "csv=p=0", out, NULL);
          if (fmt_name == NULL || strstr (fmt_name, g_str_equal (t.ext, "mov") ? "mov" : "avi") == NULL)
            bad = g_strdup_printf ("container is %s", fmt_name ? g_strstrip (fmt_name) : "?");
          g_free (fmt_name);
        }
      else
        bad = check_type (t.ext, out);
      if (bad == NULL && g_strcmp0 (before, after) != 0)
        bad = g_strdup ("the original changed");
      if (bad == NULL && extra != NULL)
        bad = extra (src, out);
      if (bad == NULL && (sw == 0 || sh == 0))
        bad = g_strdup ("the job reported no source size");
      if (bad == NULL)
        {
          char *sd = g_strdup_printf ("%" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT, sw, sh);

          x = g_strdup_printf ("%s -> %s %s, %" G_GINT64_FORMAT " -> %" G_GINT64_FORMAT " bytes", sd, d, base,
                               file_size (src), file_size (out));
          g_free (sd);
        }
      pass = bad == NULL;
      detail = bad != NULL ? bad : x;
      if (bad != NULL)
        g_free (x);
      g_free (base);
      g_free (d);
      g_free (after);
    }
  left = unexpected_entries (dir, (const char *const[]) { src_name, want_name, NULL });
  if (left[0] != '\0')
    {
      char *d2 = g_strdup_printf ("%s (left behind: %s)", detail ? detail : "", left);

      g_free (detail);
      detail = d2;
      pass = FALSE;
    }
  g_free (left);
  add_row (src_name, label, pass, detail);
  g_free (detail);
  g_free (out);
out:
  g_free (err);
  g_free (label);
  g_free (before);
  g_free (src);
}

/* a transparent pixel must survive: the output is not opaque */
static char *
extra_alpha_kept (const char *src, const char *out)
{
  char *op = NULL, *r = NULL;

  (void) src;
  if (!run (NULL, &op, "magick", "identify", "-format", "%[opaque]", out, NULL) || op == NULL ||
      !g_str_equal (g_strstrip (op), "False"))
    r = g_strdup_printf ("the alpha channel was lost (opaque=%s)", op ? op : "?");
  g_free (op);
  return r;
}

/* an animated GIF keeps its frame count and frame rate */
static char *
extra_gif_same_frames (const char *src, const char *out)
{
  char *fs = frames_of (src), *fo = frames_of (out);
  char *rs = stream_field (src, "v:0", "avg_frame_rate"), *ro = stream_field (out, "v:0", "avg_frame_rate");
  char *r = NULL;

  if (!g_str_equal (fs, fo))
    r = g_strdup_printf ("%s frames became %s", fs, fo);
  else if (!g_str_equal (rs, ro))
    r = g_strdup_printf ("frame rate %s became %s", rs, ro);
  g_free (fs); g_free (fo); g_free (rs); g_free (ro);
  return r;
}

/* a video keeps its audio untouched (stream copy: same codec, sample rate) and gets
 * H.264 / VP9 as before */
static char *
extra_audio_copied (const char *src, const char *out)
{
  char *a = stream_field (src, "a:0", "codec_name"), *b = stream_field (out, "a:0", "codec_name");
  char *ra = stream_field (src, "a:0", "sample_rate"), *rb = stream_field (out, "a:0", "sample_rate");
  char *r = NULL;

  if (a[0] == '\0')
    r = g_strdup ("the fixture has no audio");
  else if (!g_str_equal (a, b) || !g_str_equal (ra, rb))
    r = g_strdup_printf ("audio %s/%s became %s/%s", a, ra, b, rb);
  g_free (a); g_free (b); g_free (ra); g_free (rb);
  return r;
}

static char *
extra_h264_copy (const char *src, const char *out)
{
  char *c = stream_field (out, "v:0", "codec_name"), *r = extra_audio_copied (src, out);

  if (r == NULL && !g_str_equal (c, "h264"))
    r = g_strdup_printf ("video codec is %s", c);
  g_free (c);
  return r;
}

static char *
extra_vp9_copy (const char *src, const char *out)
{
  char *c = stream_field (out, "v:0", "codec_name"), *r = extra_audio_copied (src, out);

  if (r == NULL && !g_str_equal (c, "vp9"))
    r = g_strdup_printf ("video codec is %s", c);
  g_free (c);
  return r;
}

static char *
extra_not_enlarged (const char *src, const char *out)
{
  return file_size (out) < file_size (src) * 3 ? NULL : g_strdup ("the output is much larger");
}

/* a source the preset would not make smaller: nothing is made, nothing is left */
static void
check_resize_skipped (const char *dir, const char *src_name, const char *rs_key, const char *want_msg)
{
  char *src = g_build_filename (dir, src_name, NULL), *err = NULL, *out, *left, *detail;
  gboolean skipped;
  guint64 sw = 0, sh = 0;
  NfcFormat f;
  gboolean pass;
  char *label = g_strdup_printf ("resize %s (already smaller)", rs_key);

  g_assert_true (nfc_classify (src, NULL, &f));
  out = resize_sync (f.cat, f.fmt, rs_key, src, &skipped, &err, &sw, &sh);
  left = unexpected_entries (dir, (const char *const[]) { src_name, "small.png", "exact.png", "hd.mp4",
                                                          "tiny.png", NULL });
  pass = out == NULL && skipped && g_strcmp0 (err, want_msg) == 0 && left[0] == '\0' && sw > 0;
  detail = g_strdup_printf ("%s%s%s", skipped ? "skipped: " : "NOT skipped: ", err ? err : "(no error)",
                            left[0] ? left : "");
  add_row (src_name, label, pass, detail);
  g_free (detail); g_free (label); g_free (left); g_free (out); g_free (err); g_free (src);
}

/* the frame rate of a GIF made from a 10 fps and from a 30 fps source with the GIF
 * presets: min(source, preset) */
static void
check_gif_fps (const char *dir)
{
  /* frames of the 3 s / 2 s sources: a GIF stores delays in hundredths of a second, so
   * 15 fps is not exactly representable and the frame COUNT is what is checked */
  static const struct { const char *src, *preset_label; NfcPreset preset; int want; } c[] = {
    { "ten.gif", "Best (15 max)", NFC_PRESET_BEST, 30 },       /* 10 fps source: no doubled frames */
    { "ten.gif", "Small (10 max)", NFC_PRESET_SMALL, 30 },
    { "ten.gif", "Balanced (12 max)", NFC_PRESET_BALANCED, 30 },
    { "thirty.mp4", "Best (15 max)", NFC_PRESET_BEST, 30 },    /* 30 fps source: capped at 15 */
    { "thirty.mp4", "Balanced (12 max)", NFC_PRESET_BALANCED, 24 },
    { "thirty.mp4", "Small (10 max)", NFC_PRESET_SMALL, 20 },
    { "twelve.mp4", "Best (15 max)", NFC_PRESET_BEST, 24 },    /* 12 fps source under the cap */
  };

  for (guint i = 0; i < G_N_ELEMENTS (c); i++)
    {
      char *src = g_build_filename (dir, c[i].src, NULL), *err = NULL, *out, *rate = NULL, *label;
      int got = 0;
      NfcFormat f;
      gboolean pass = FALSE;
      char *detail;

      g_assert_true (nfc_classify (src, NULL, &f));
      out = convert_preset_sync (NFC_CAT_VIDEO, f.fmt, "gif", c[i].preset, "gif", src, &err);
      if (out != NULL)
        {
          rate = frames_of (out);
          got = atoi (rate);
          pass = ABS (got - c[i].want) <= 1;
          g_unlink (out);
        }
      detail = g_strdup_printf ("%s: %s frames (expected %d)", err ? err : "ok", rate ? rate : "-", c[i].want);
      label = g_strdup_printf ("gif frames, %s", c[i].preset_label);
      add_row (c[i].src, label, pass, detail);
      g_free (label); g_free (detail); g_free (rate); g_free (out); g_free (err); g_free (src);
    }
}

/* the notifications of Resize and the size in the Small toast */
static void
test_resize_notifications (const char *dir)
{
  char *ndir = g_dir_make_tmp ("nfc-notify-XXXXXX", NULL);
  char *script = g_build_filename (ndir, "notify-send", NULL);
  char *log = g_build_filename (ndir, "calls.log", NULL);
  char *content = g_strdup_printf ("#!/bin/sh\necho \"$@\" >> '%s'\necho 4242\n", log);
  char *saved = nfc_tools.notify;
  struct {
    const char *label, *preset;
    const char *files[4];   /* "4k", "small" (320x200) or "bad" each */
    const char *want[4];
    const char *unwanted;
  } c[] = {
    { "one file", "1080p", { "4k", NULL },
      { " -- Resizing… f0.png (1080p)",
        " -- Resize complete f0.png → f0 (1080p).png, 3840×2160 → 1920×1080", NULL, NULL },
      "already smaller" },
    { "two files", "720p", { "4k", "4k", NULL },
      { " -- Resizing… 2 files to 720p", " -- Resize complete Resized 2 files to 720p", NULL, NULL },
      "already smaller" },
    { "50%", "50pct", { "4k", NULL },
      { " -- Resizing… f0.png (50%)",
        " -- Resize complete f0.png → f0 (50%).png, 3840×2160 → 1920×1080", NULL, NULL },
      "already smaller" },
    { "already smaller", "1080p", { "small", NULL },
      { " -- Resizing… f0.png (1080p)", " -- Nothing to resize f0.png is already smaller than 1080p", NULL, NULL },
      "Resize failed" },
    { "two already smaller", "1080p", { "small", "small", NULL },
      { " -- Nothing to resize 2 files are already smaller than 1080p", NULL, NULL, NULL },
      "Resize failed" },
    { "one good, one skipped", "1080p", { "4k", "small", NULL },
      { " -- Resize complete f0.png → f0 (1080p).png, 3840×2160 → 1920×1080",
        "\nf1.png is already smaller than 1080p", NULL, NULL },
      "Resize failed" },
    { "one good, one broken", "1080p", { "4k", "bad", NULL },
      { " -- Resize failed f0.png → f0 (1080p).png, 3840×2160 → 1920×1080", "\nFailed: f1.png — ", NULL, NULL },
      "already smaller" },
    { "too small for a percentage", "25pct", { "tiny", NULL },
      { " -- Nothing to resize f0.png is too small to resize to 25%", NULL, NULL, NULL }, "Resize failed" },
  };

  g_file_set_contents (script, content, -1, NULL);
  g_chmod (script, 0755);
  nfc_tools.notify = script;
  for (guint k = 0; k < G_N_ELEMENTS (c); k++)
    {
      char *sub = g_strdup_printf ("%s/rn%u", dir, k);
      char *paths[5] = { NULL }, *fmts[5] = { NULL }, *logged = NULL;
      guint n = 0;
      gboolean pass = FALSE;
      GString *miss = g_string_new (NULL);

      g_mkdir_with_parents (sub, 0755);
      for (; c[k].files[n] != NULL; n++)
        {
          char *name = g_strdup_printf ("f%u.png", n);

          paths[n] = g_build_filename (sub, name, NULL);
          fmts[n] = (char *) "png";
          if (g_str_equal (c[k].files[n], "4k"))
            run (sub, NULL, "magick", "-size", "3840x2160", "gradient:red-blue", name, NULL);
          else if (g_str_equal (c[k].files[n], "small"))
            run (sub, NULL, "magick", "-size", "320x200", "gradient:red-blue", name, NULL);
          else if (g_str_equal (c[k].files[n], "tiny"))
            run (sub, NULL, "magick", "-size", "30x30", "gradient:red-blue", name, NULL);
          else
            g_file_set_contents (paths[n], "this is not a png", -1, NULL);
          g_free (name);
        }
      g_remove (log);
      nfc_batch_run_resize (NFC_CAT_IMAGE, c[k].preset, (const char *const *) paths,
                            (const char *const *) fmts, n, NULL, NULL);
      for (int i = 0; i < 1000 && !pass; i++)
        {
          g_main_context_iteration (NULL, FALSE);
          g_usleep (20000);
          g_clear_pointer (&logged, g_free);
          if (g_file_get_contents (log, &logged, NULL, NULL))
            pass = strstr (logged, "Resize complete") != NULL ||
                   strstr (logged, "Resize failed") != NULL ||
                   strstr (logged, "Nothing to resize") != NULL;
        }
      if (!pass)
        g_string_append (miss, "no final notification; ");
      for (guint i = 0; pass && i < 4 && c[k].want[i] != NULL; i++)
        if (strstr (logged, c[k].want[i]) == NULL)
          {
            g_string_append_printf (miss, "missing '%s'; ", c[k].want[i]);
            pass = FALSE;
          }
      if (pass && c[k].unwanted != NULL && strstr (logged, c[k].unwanted) != NULL)
        {
          g_string_append_printf (miss, "unexpected '%s'; ", c[k].unwanted);
          pass = FALSE;
        }
      add_row ("(notifications)", c[k].label, pass, pass ? "Resize notification texts" : (miss->len ? miss->str : "?"));
      if (!pass && logged != NULL)
        g_printerr ("--- notify log of '%s':\n%s\n", c[k].label, logged);
      g_string_free (miss, TRUE);
      g_free (logged);
      for (guint i = 0; i < n; i++)
        g_free (paths[i]);
      g_free (sub);
    }

  /* the Small preset names the size it scaled to, only when it did scale */
  {
    static const struct { const char *label; const char *size; gboolean compress; const char *want; } s[] = {
      { "Small, convert: scaled", "3840x2560", FALSE, " -- Conversion complete f0.png → f0.jpg (Small, 2560×1707)" },
      { "Small, convert: not scaled", "1000x700", FALSE, " -- Conversion complete f0.png → f0.jpg (Small)\n" },
      { "Small, compress: scaled", "3840x2560", TRUE, " (Small, 2560×1707)" },
    };

    for (guint k = 0; k < G_N_ELEMENTS (s); k++)
      {
        char *sub = g_strdup_printf ("%s/sm%u", dir, k), *logged = NULL, *path;
        char *paths[2], *fmts[2];
        gboolean pass = FALSE;
        const char *src_name = s[k].compress ? "f0.jpg" : "f0.png";

        g_mkdir_with_parents (sub, 0755);
        if (s[k].compress)
          run (sub, NULL, "magick", "-seed", "9", "-size", s[k].size, "plasma:fractal", "-quality", "97",
               src_name, NULL);
        else
          run (sub, NULL, "magick", "-size", s[k].size, "gradient:red-blue", src_name, NULL);
        path = g_build_filename (sub, src_name, NULL);
        paths[0] = path; paths[1] = NULL;
        fmts[0] = (char *) (s[k].compress ? "jpg" : "png"); fmts[1] = NULL;
        g_remove (log);
        if (s[k].compress)
          nfc_batch_run_compress (NFC_CAT_IMAGE, NFC_PRESET_SMALL, (const char *const *) paths,
                                  (const char *const *) fmts, 1, NULL, NULL);
        else
          nfc_batch_run_preset (NFC_CAT_IMAGE, "jpg", NFC_PRESET_SMALL, "JPG", "jpg",
                                (const char *const *) paths, (const char *const *) fmts, 1, NULL, NULL);
        for (int i = 0; i < 1000 && !pass; i++)
          {
            g_main_context_iteration (NULL, FALSE);
            g_usleep (20000);
            g_clear_pointer (&logged, g_free);
            if (g_file_get_contents (log, &logged, NULL, NULL))
              pass = strstr (logged, "complete") != NULL || strstr (logged, "failed") != NULL;
          }
        pass = pass && strstr (logged, s[k].want) != NULL;
        add_row ("(notifications)", s[k].label, pass, pass ? "the size is named when Small scaled" : (logged ? logged : "no calls"));
        g_free (logged);
        g_free (path);
        g_free (sub);
      }
  }
  nfc_tools.notify = saved;
  g_free (ndir);
  g_free (script);
  g_free (log);
  g_free (content);
}

static void
test_resize (void)
{
  char *dir = P ("resize");
  char *saved_notify = nfc_tools.notify;
  struct {
    const char *sub, *name, *rs, *want_name, *want_dims, *category;
    char *(*extra) (const char *, const char *);
  } c[] = {
    /* images */
    { "r0", "pic.png", "1080p", "pic (1080p).png", "1920x1080", "image", NULL },
    { "r1", "photo.jpg", "50pct", "photo (50%).jpg", "800x600", "image", extra_not_enlarged },
    { "r2", "sticker.webp", "720p", "sticker (720p).webp", "1280x720", "image", extra_alpha_kept },
    { "r3", "anim.gif", "480p", "anim (480p).gif", "854x641", "image", extra_gif_same_frames },
    { "r4", "tall.jpg", "480p", "tall (480p).jpg", "641x854", "image", NULL },
    { "r5", "pic.bmp", "50pct", "pic (50%).bmp", "500x400", "image", NULL },
    { "r6", "pic.tiff", "25pct", "pic (25%).tiff", "250x200", "image", NULL },
    { "r7", "pic.avif", "50pct", "pic (50%).avif", "400x300", "image", NULL },
    { "r8", "alpha.png", "25pct", "alpha (25%).png", "200x150", "image", extra_alpha_kept },
    /* video: the SHORT side, audio stream copied */
    { "v0", "clip.mp4", "720p", "clip (720p).mp4", "1280x720", "video", extra_h264_copy },
    { "v1", "clip.webm", "480p", "clip (480p).webm", "854x480", "video", extra_vp9_copy },
    { "v2", "clip.mkv", "720p", "clip (720p).mkv", "1280x720", "video", extra_h264_copy },
    { "v3", "clip.mov", "480p", "clip (480p).mov", "854x480", "video", extra_h264_copy },
    { "v4", "clip.avi", "480p", "clip (480p).avi", "854x480", "video", NULL },
    { "v5", "clip.m4v", "480p", "clip (480p).mp4", "854x480", "video", extra_h264_copy },
    { "v6", "phone.mp4", "480p", "phone (480p).mp4", "480x854", "video", NULL },
    { "v7", "silent.mp4", "480p", "silent (480p).mp4", "854x480", "video", NULL },
  };

  g_mkdir_with_parents (dir, 0755);
  nfc_tools.notify = NULL; /* no real notifications from the test */
  for (guint i = 0; i < G_N_ELEMENTS (c); i++)
    {
      char *sub = g_build_filename (dir, c[i].sub, NULL);

      g_mkdir_with_parents (sub, 0755);
      g_free (sub);
    }
#define SUB(i) g_build_filename (dir, c[i].sub, NULL)
#define MK(i, what, ...) do { char *d_ = SUB (i); fixture (what, run (d_, NULL, __VA_ARGS__, NULL)); g_free (d_); } while (0)
  MK (0, "r0/pic.png", "magick", "-size", "3840x2160", "gradient:red-blue", "-fill", "white", "-pointsize", "80",
      "-annotate", "+100+200", "Resize me", "pic.png");
  MK (1, "r1/photo.jpg", "magick", "-seed", "11", "-size", "1600x1200", "plasma:fractal", "-quality", "97", "photo.jpg");
  MK (2, "r2/sticker.webp", "magick", "-size", "1600x900", "xc:none", "-fill", "red", "-draw", "circle 800,450 800,100",
      "WEBP:sticker.webp");
  MK (3, "r3/anim.gif", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=3:s=1000x750:r=10", "anim.gif");
  MK (4, "r4/tall.jpg", "magick", "-seed", "2", "-size", "900x1200", "plasma:fractal", "-quality", "95", "tall.jpg");
  MK (5, "r5/pic.bmp", "magick", "-size", "1000x800", "gradient:red-blue", "BMP:pic.bmp");
  MK (6, "r6/pic.tiff", "magick", "-size", "1000x800", "gradient:red-blue", "TIFF:pic.tiff");
  MK (7, "r7/pic.avif", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=800x600:r=1", "-frames:v", "1", "-c:v", "libaom-av1", "-cpu-used", "8", "-still-picture", "1",
      "-f", "avif", "pic.avif");
  MK (8, "r8/alpha.png", "magick", "-size", "800x600", "xc:none", "-fill", "blue", "-draw", "circle 400,300 400,50",
      "alpha.png");
  MK (9, "v0/clip.mp4", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=2:s=1920x1080:r=25", "-f", "lavfi", "-i", "sine=d=2", "-shortest", "-c:v", "libx264", "-preset",
      "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "clip.mp4");
  MK (10, "v1/clip.webm", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=1", "-shortest", "-c:v", "libvpx-vp9", "-b:v", "1M",
      "-deadline", "realtime", "-cpu-used", "8", "-c:a", "libopus", "clip.webm");
  MK (11, "v2/clip.mkv", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=2:s=1920x1080:r=25", "-f", "lavfi", "-i", "sine=d=2", "-shortest", "-c:v", "libx264", "-preset",
      "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "clip.mkv");
  MK (12, "v3/clip.mov", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=1", "-shortest", "-c:v", "libx264", "-preset",
      "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "clip.mov");
  MK (13, "v4/clip.avi", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=1", "-shortest", "-c:v", "mpeg4", "-q:v", "3",
      "-c:a", "libmp3lame", "clip.avi");
  MK (14, "v5/clip.m4v", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1280x720:r=25", "-f", "lavfi", "-i", "sine=d=1", "-shortest", "-c:v", "libx264", "-preset",
      "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-f", "mp4", "clip.m4v");
  MK (15, "v6/phone.mp4", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1080x1920:r=25", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "phone.mp4");
  MK (16, "v7/silent.mp4", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      "testsrc2=d=1:s=1280x720:r=25", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "silent.mp4");
  for (guint i = 0; i < G_N_ELEMENTS (c); i++)
    {
      char *sub = SUB (i);

      check_resize (sub, c[i].name, c[i].rs, c[i].want_name, c[i].want_dims, c[i].extra);
      g_free (sub);
    }
#undef MK
#undef SUB

  /* naming: a second resize does not overwrite the first */
  {
    char *sub = g_build_filename (dir, "r0", NULL), *src = g_build_filename (sub, "pic.png", NULL);
    gboolean skipped;
    char *o = resize_sync (NFC_CAT_IMAGE, "png", "480p", src, &skipped, NULL, NULL, NULL);
    char *o2 = resize_sync (NFC_CAT_IMAGE, "png", "480p", src, &skipped, NULL, NULL, NULL);
    char *b1 = o ? g_path_get_basename (o) : NULL, *b2 = o2 ? g_path_get_basename (o2) : NULL;

    add_row ("pic.png", "resize naming", g_strcmp0 (b1, "pic (480p).png") == 0 &&
             g_strcmp0 (b2, "pic (480p) (1).png") == 0, b2 ? b2 : "failed");
    g_free (b1); g_free (b2); g_free (o); g_free (o2); g_free (src); g_free (sub);
  }

  /* an image or a video the preset would not shrink: nothing made, a plain message */
  {
    char *sub = g_build_filename (dir, "s0", NULL);

    g_mkdir_with_parents (sub, 0755);
    fixture ("s0/small.png", run (sub, NULL, "magick", "-size", "800x600", "gradient:red-blue", "small.png", NULL));
    fixture ("s0/exact.png", run (sub, NULL, "magick", "-size", "1920x1080", "gradient:red-blue", "exact.png", NULL));
    fixture ("s0/hd.mp4", run (sub, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                               "testsrc2=d=1:s=1280x720:r=25", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                               "yuv420p", "hd.mp4", NULL));
    fixture ("s0/tiny.png", run (sub, NULL, "magick", "-size", "30x30", "gradient:red-blue", "tiny.png", NULL));
    check_resize_skipped (sub, "small.png", "1080p", "already smaller than 1080p");
    check_resize_skipped (sub, "exact.png", "1080p", "already smaller than 1080p"); /* equal is not smaller */
    check_resize_skipped (sub, "hd.mp4", "720p", "already smaller than 720p");     /* 720p -> 720p */
    check_resize_skipped (sub, "tiny.png", "50pct", "too small to resize to 50%");
    g_free (sub);
  }

  /* the GIF frame rate is min(source, preset) */
  {
    char *sub = g_build_filename (dir, "fps", NULL);

    g_mkdir_with_parents (sub, 0755);
    fixture ("fps/ten.gif", run (sub, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                 "testsrc2=d=3:s=400x300:r=10", "ten.gif", NULL));
    fixture ("fps/thirty.mp4", run (sub, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                                    "-i", "testsrc2=d=2:s=400x300:r=30", "-pix_fmt", "yuv420p", "thirty.mp4", NULL));
    fixture ("fps/twelve.mp4", run (sub, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                                    "-i", "testsrc2=d=2:s=400x300:r=12", "-pix_fmt", "yuv420p", "twelve.mp4", NULL));
    check_gif_fps (sub);
    g_free (sub);
  }
  nfc_tools.notify = saved_notify;
  test_resize_notifications (dir);
  nfc_tools.notify = saved_notify;
  g_free (dir);
}

int
main (void)
{
  const char *k_png[] = { "jpg", "webp", "avif", "ico", "bmp", "tiff", "pdf", NULL };
  const char *k_svg[] = { "png", "jpg", "webp", "avif", "pdf", NULL };
  const char *k_gif[] = { "png", "jpg", "pdf", NULL };
  const char *k_avif[] = { "png", "jpg", "webp", "ico", "bmp", "tiff", "pdf", NULL };
  const char *k_other_img[] = { "png", "jpg", NULL };
  const char *k_jpg_src[] = { "png", "webp", "pdf", NULL };
  const char *k_wav[] = { "mp3", "flac", "ogg", "opus", "m4a", NULL };
  const char *k_flac[] = { "mp3", "wav", "ogg", "opus", "m4a", NULL };
  const char *k_toWav[] = { "wav", NULL };
  const char *k_mp4[] = { "mkv", "webm", "gif", "mp3", "m4a", NULL };
  const char *k_mov[] = { "mp4", "mkv", NULL };
  const char *k_mkv[] = { "mp4", NULL };
  const char *k_noaudio[] = { "mkv", "webm", "gif", NULL };
  const char *k_txt[] = { "pdf", "docx", "odt", NULL };
  const char *k_odt[] = { "pdf", "docx", NULL };
  const char *k_docx[] = { "pdf", "odt", NULL };
  const char *k_rtf[] = { "pdf", NULL };
  const char *k_csv[] = { "pdf", "xlsx", "ods", NULL };
  const char *k_xlsx[] = { "pdf", "ods", "csv", NULL };
  const char *k_odp[] = { "pdf", "pptx", NULL };
  const char *k_pptx[] = { "pdf", "odp", NULL };
  char *profile, *envarg, *left;
  char *tmpdir;

  {
    /* not g_dir_make_tmp(): it would cache the temp dir before we change it */
    const char *orig = g_getenv ("TMPDIR");
    char *tmpl = g_strdup_printf ("%s/nfc-test-XXXXXX", orig && *orig ? orig : "/tmp");

    work = g_mkdtemp (tmpl);
  }
  g_assert_nonnull (work);
  /* Run everything with a hostile TMPDIR ("sp ace%20"): LibreOffice profile
   * URL, ImageMagick work files, nfc-decode output, all live in there. Must be
   * set before anything caches g_get_tmp_dir(). */
  tmpdir = P ("tmp sp ace%20");
  g_assert_cmpint (g_mkdir_with_parents (tmpdir, 0755), ==, 0);
  g_setenv ("TMPDIR", tmpdir, TRUE);
  g_free (tmpdir);
  /* keep the log out of the user's real ~/.cache */
  tmpdir = P ("xdg-cache");
  g_setenv ("XDG_CACHE_HOME", tmpdir, TRUE);
  g_free (tmpdir);
  rows = g_ptr_array_new ();
  nfc_tools_init ();
  g_print ("tools: magick=%s ffmpeg=%s soffice=%s notify-send=%s\n",
           nfc_tools.magick, nfc_tools.ffmpeg, nfc_tools.soffice, nfc_tools.notify);
  nfc_probe_ensure ();

  /* ---------------- fixtures ---------------- */
  fixture ("test.png", run (work, NULL, "magick", "-size", "64x64", "xc:red", "test.png", NULL));
  fixture ("wide.png", run (work, NULL, "magick", "-size", "300x100", "gradient:red-blue", "wide.png", NULL));
  fixture ("test.gif", run (work, NULL, "magick", "test.png", "test.gif", NULL));
  fixture ("test.jpg", run (work, NULL, "magick", "test.png", "JPEG:test.jpg", NULL));
  fixture ("test.webp", run (work, NULL, "magick", "test.png", "WEBP:test.webp", NULL));
  fixture ("test.bmp", run (work, NULL, "magick", "test.png", "BMP:test.bmp", NULL));
  fixture ("test.tiff", run (work, NULL, "magick", "test.png", "TIFF:test.tiff", NULL));
  fixture ("test.ico", run (work, NULL, "magick", "test.png", "ICO:test.ico", NULL));
  write_file ("test.svg",
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"30\">"
              "<rect width=\"40\" height=\"30\" fill=\"blue\"/></svg>");
  fixture ("test.wav", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=d=2", "test.wav", NULL));
  fixture ("test.flac", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=d=2", "test.flac", NULL));
  fixture ("test.mp3", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.wav", "test.mp3", NULL));
  fixture ("test.ogg", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.wav", "-c:a", "libvorbis", "test.ogg", NULL));
  fixture ("test.opus", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.wav", "-c:a", "libopus", "test.opus", NULL));
  fixture ("test.m4a", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.wav", "-c:a", "aac", "test.m4a", NULL));
  fixture ("test.mp4", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc=d=2:s=320x240", "-f", "lavfi", "-i", "sine=d=2", "-shortest", "-pix_fmt", "yuv420p", "test.mp4", NULL));
  fixture ("test.mov", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.mp4", "-c", "copy", "test.mov", NULL));
  fixture ("test.mkv", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.mp4", "-c", "copy", "test.mkv", NULL));
  /* GNOME screencasts have no audio track at all */
  fixture ("noaudio.mp4", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc=d=2:s=321x241", "-pix_fmt", "yuv420p", "noaudio.mp4", NULL));
  fixture ("noaudio.mkv", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "noaudio.mp4", "-c", "copy", "noaudio.mkv", NULL));
  fixture ("test.avif", run (work, NULL, "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", "test.png", "-frames:v", "1", "-c:v", "libaom-av1", "-cpu-used", "8", "-still-picture", "1", "-f", "avif", "test.avif", NULL));
  write_file ("test.txt", "Hello\nWorld\n");
  write_file ("test.csv", "a,b\n1,2\n3,4\n");
  write_file ("test.fodp",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
              "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
              "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
              "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" "
              "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
              "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
              "office:version=\"1.3\" office:mimetype=\"application/vnd.oasis.opendocument.presentation\">"
              "<office:body><office:presentation><draw:page draw:name=\"p1\"><draw:frame "
              "svg:width=\"10cm\" svg:height=\"3cm\" svg:x=\"1cm\" svg:y=\"1cm\"><draw:text-box><text:p>Hi"
              "</text:p></draw:text-box></draw:frame></draw:page></office:presentation></office:body>"
              "</office:document>");
  profile = P ("p0");
  {
    char *uri = g_filename_to_uri (profile, NULL, NULL);

    envarg = g_strconcat ("-env:UserInstallation=", uri, NULL);
    g_free (uri);
  }
  fixture ("test.odt", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "odt", "test.txt", NULL));
  fixture ("test.docx", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "docx", "test.txt", NULL));
  fixture ("test.rtf", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "rtf", "test.txt", NULL));
  fixture ("test.xlsx", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "xlsx", "test.csv", NULL));
  fixture ("test.odp", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "odp", "test.fodp", NULL));
  fixture ("test.pptx", run (work, NULL, "soffice", "--headless", envarg, "--convert-to", "pptx", "test.fodp", NULL));
  g_free (profile);
  g_free (envarg);

  /* ---------------- conversions ---------------- */
  check_many ("test.png", k_png);
  g_free (check ("wide.png", "ico"));
  {
    /* wide.png is 300 wide: the ICO must be resized to at most 256 */
    char *e = NULL, *w = NULL, *p = P ("wide.ico");
    gboolean pass = FALSE;

    if (g_file_test (p, G_FILE_TEST_EXISTS) && run (NULL, &w, "magick", "identify", "-format", "%w ", p, NULL))
      {
        char **v = g_strsplit (g_strstrip (w), " ", -1);

        pass = TRUE;
        for (char **x = v; *x; x++)
          if (atoi (*x) > 256 || atoi (*x) <= 0)
            pass = FALSE;
        e = g_strdup (w);
        g_strfreev (v);
      }
    add_row ("wide.png (300x100)", "ico max 256", pass, e ? e : "wide.ico not found");
    g_free (e);
    g_free (w);
    g_free (p);
  }
  check_many ("test.svg", k_svg);
  test_svg_hardening ();
  check_many ("test.gif", k_gif);
  check_many ("test.avif", k_avif);
  check_many ("test.jpg", k_jpg_src);
  check_many ("test.webp", k_other_img);
  check_many ("test.bmp", k_other_img);
  check_many ("test.tiff", k_other_img);
  check_many ("test.ico", k_other_img);
  test_heic ();
  test_special_names ();
  check_many ("test.wav", k_wav);
  check_many ("test.flac", k_flac);
  check_many ("test.mp3", k_toWav);
  check_many ("test.ogg", k_toWav);
  check_many ("test.opus", k_toWav);
  check_many ("test.m4a", k_toWav);
  check_many ("test.mp4", k_mp4);
  /* audio-less video: every video target works (odd frame size 321x241 too), the
   * audio targets fail fast with a plain message instead of ffmpeg's text */
  check_many ("noaudio.mp4", k_noaudio);
  check_many ("noaudio.mkv", k_mkv);
  check_error ("noaudio.mp4", "mp3", "no audio track");
  check_error ("noaudio.mp4", "m4a", "no audio track");
  test_quality ();
  test_presets ();
  test_compress ();
  test_resize ();
  test_large_image ();
  test_pdf_page_size ();
  check_many ("test.mov", k_mov);
  check_many ("test.mkv", k_mkv);
  test_openh264_only ();
  check_many ("test.txt", k_txt);
  check_many ("test.odt", k_odt);
  check_many ("test.docx", k_docx);
  check_many ("test.rtf", k_rtf);
  check_many ("test.csv", k_csv);
  check_many ("test.xlsx", k_xlsx);
  check_many ("test.odp", k_odp);
  check_many ("test.pptx", k_pptx);

  /* ---------------- naming / cleanup ---------------- */
  {
    char *d = P ("naming"), *t = P ("test.png"), *s, *data, *b[3] = { NULL, NULL, NULL };
    gsize len;
    gboolean ok;
    const char *expect[3] = { "test.bmp", "test (1).bmp", "test (2).bmp" };

    g_mkdir_with_parents (d, 0755);
    s = g_build_filename (d, "test.png", NULL);
    g_file_get_contents (t, &data, &len, NULL);
    g_file_set_contents (s, data, len, NULL);
    for (int i = 0; i < 3; i++)
      {
        char *o3 = convert_sync (NFC_CAT_IMAGE, "png", "bmp", "bmp", s, NULL);

        b[i] = o3 ? g_path_get_basename (o3) : g_strdup ("(failed)");
        g_free (o3);
      }
    ok = g_str_equal (b[0], expect[0]) && g_str_equal (b[1], expect[1]) &&
         g_str_equal (b[2], expect[2]);
    {
      char *det = g_strjoin (", ", b[0], b[1], b[2], NULL);

      add_row ("test.png (same dir, x3)", "bmp", ok, det);
      g_free (det);
    }
    {
      char *left2;

      ok = !leftover_temp_files (d, &left2);
      add_row ("(naming dir)", "no temp files", ok, left2);
      g_free (left2);
    }
    for (int i = 0; i < 3; i++)
      g_free (b[i]);
    g_free (data); g_free (s); g_free (t); g_free (d);
  }
  {
    char *d = P ("fresh"), *src, *out, *b;
    char *data;
    gsize len;
    char *t = P ("test.png");

    g_mkdir_with_parents (d, 0755);
    src = g_build_filename (d, "my pic (x) \xc3\xa9.png", NULL);
    g_file_get_contents (t, &data, &len, NULL);
    g_file_set_contents (src, data, len, NULL);
    out = convert_sync (NFC_CAT_IMAGE, "png", "jpg", "jpg", src, NULL);
    b = out ? g_path_get_basename (out) : g_strdup ("(failed)");
    add_row ("my pic (x) \xc3\xa9.png", "jpg", g_str_equal (b, "my pic (x) \xc3\xa9.jpg"), b);
    g_free (b); g_free (out); g_free (src); g_free (d); g_free (data); g_free (t);
  }
  {
    char *bad = P ("bad.png"), *err = NULL, *out;
    write_file ("bad.png", "not an image at all");
    out = convert_sync (NFC_CAT_IMAGE, "png", "jpg", "jpg", bad, &err);
    add_row ("bad.png (garbage)", "jpg", out == NULL && err != NULL && *err, err ? err : "unexpectedly succeeded");
    g_free (out); g_free (err); g_free (bad);
  }
  test_batch ();
  test_notifications ();
  /* the size probe (identify -ping / nfc-decode --info) must have worked for every
   * image of the run: only the garbage files may have failed it */
  {
    char *logp = g_build_filename (g_get_user_cache_dir (), "nautilus-file-converter.log", NULL);
    char *log = NULL, **lines;
    guint bad = 0;

    g_file_get_contents (logp, &log, NULL, NULL);
    lines = g_strsplit (log != NULL ? log : "", "\n", -1);
    for (char **l = lines; *l; l++)
      if (strstr (*l, "size probe failed") != NULL && strstr (*l, "bad.png") == NULL &&
          strstr (*l, "corrupt.png") == NULL && strstr (*l, "/nt4/") == NULL && strstr (*l, "/rn6/") == NULL)
        {
          g_printerr ("%s\n", *l);
          bad++;
        }
    add_row ("(size probe)", "worked for every image", bad == 0, bad ? "see stderr" : "");
    g_strfreev (lines);
    g_free (log);
    g_free (logp);
  }
  {
    gboolean none = !leftover_temp_files (work, &left);

    add_row ("(temp files)", "cleanup", none, left);
    g_free (left);
  }

  /* ---------------- table ---------------- */
  {
    int w0 = 0, w1 = 0, pass = 0;

    for (guint i = 0; i < rows->len; i++)
      {
        char **r = rows->pdata[i];

        w0 = MAX (w0, (int) strlen (r[0]));
        w1 = MAX (w1, (int) strlen (r[1]));
      }
    for (guint i = 0; i < rows->len; i++)
      {
        char **r = rows->pdata[i];

        printf ("%-*s  %-*s  %s  %s\n", w0, r[0], w1, r[1], r[2], r[3]);
        pass += g_str_equal (r[2], "PASS");
      }
    printf ("\n%d passed, %d failed\n", pass, n_fail);
  }
  {
    char *rm[] = { "rm", "-rf", work, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  return n_fail ? 1 : 0;
}
