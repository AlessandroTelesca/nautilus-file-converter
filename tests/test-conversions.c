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
    { "avif", "image/avif" }, { "ico", "image/vnd.microsoft.icon|image/x-icon" },
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
  check_many ("test.mov", k_mov);
  check_many ("test.mkv", k_mkv);
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
