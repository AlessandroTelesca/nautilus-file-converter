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
          strstr (*l, "corrupt.png") == NULL)
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
