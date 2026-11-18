/* Unit tests for the pure logic: classification, argv builders, naming. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nfc-core.h"

static gboolean
argv_has (char **argv, const char *s)
{
  for (; *argv; argv++)
    if (g_str_equal (*argv, s))
      return TRUE;
  return FALSE;
}

static void
test_classify (void)
{
  NfcFormat f;

  g_assert_true (nfc_classify ("/x/a.JPEG", NULL, &f));
  g_assert_cmpint (f.cat, ==, NFC_CAT_IMAGE);
  g_assert_cmpstr (f.fmt, ==, "jpg");
  g_assert_true (nfc_classify ("/x/a.heif", NULL, &f));
  g_assert_cmpstr (f.fmt, ==, "heic");
  g_assert_true (nfc_classify ("/x/a.tif", NULL, &f));
  g_assert_cmpstr (f.fmt, ==, "tiff");
  g_assert_true (nfc_classify ("/x/a.Mkv", NULL, &f));
  g_assert_cmpint (f.cat, ==, NFC_CAT_VIDEO);
  g_assert_true (nfc_classify ("/x/a.csv", NULL, &f));
  g_assert_cmpint (f.cat, ==, NFC_CAT_SHEET);
  g_assert_true (nfc_classify ("/x/a.odp", NULL, &f));
  g_assert_cmpint (f.cat, ==, NFC_CAT_SLIDE);
  /* no extension: mime fallback */
  g_assert_true (nfc_classify ("/x/noext", "image/png", &f));
  g_assert_cmpstr (f.fmt, ==, "png");
  /* extension wins over mime */
  g_assert_true (nfc_classify ("/x/a.mp3", "image/png", &f));
  g_assert_cmpint (f.cat, ==, NFC_CAT_AUDIO);
  g_assert_false (nfc_classify ("/x/a.xyz", "application/octet-stream", &f));
  g_assert_false (nfc_classify ("/x/.png", NULL, &f)); /* hidden file named ".png" has no ext */
  g_assert_false (nfc_classify (NULL, NULL, &f));
}

static void
test_naming (void)
{
  char *s, *dir, *tmp, *a, *b, *c;
  GError *error = NULL;

  s = nfc_stem_of ("/a/b/photo.final.png");
  g_assert_cmpstr (s, ==, "photo.final");
  g_free (s);
  s = nfc_stem_of ("/a/b/.hidden");
  g_assert_cmpstr (s, ==, ".hidden");
  g_free (s);

  s = nfc_candidate_name ("x", "pdf", 0);
  g_assert_cmpstr (s, ==, "x.pdf");
  g_free (s);
  s = nfc_candidate_name ("x", "pdf", 2);
  g_assert_cmpstr (s, ==, "x (2).pdf");
  g_free (s);

  /* hidden temp name: bounded length, valid UTF-8 even for long names */
  {
    GString *long_stem = g_string_new (NULL);

    for (int i = 0; i < 100; i++)
      g_string_append (long_stem, "\xc3\xa9"); /* e-acute, 2 bytes */
    s = nfc_hidden_temp_name ("/d", long_stem->str, "png");
    g_assert_true (g_utf8_validate (s, -1, NULL));
    g_assert_cmpuint (strlen (s), <, 120);
    g_assert_true (g_str_has_suffix (s, ".png"));
    g_free (s);
    g_string_free (long_stem, TRUE);
  }

  dir = g_dir_make_tmp ("nfc-core-XXXXXX", &error);
  g_assert_no_error (error);
  for (int i = 0; i < 3; i++)
    {
      tmp = g_build_filename (dir, "t.tmp", NULL);
      g_file_set_contents (tmp, "data", -1, NULL);
      s = nfc_publish (tmp, dir, "name", "jpg", &error);
      g_assert_no_error (error);
      g_assert_nonnull (s);
      g_assert_false (g_file_test (tmp, G_FILE_TEST_EXISTS)); /* temp unlinked */
      if (i == 0)
        a = s;
      else if (i == 1)
        b = s;
      else
        c = s;
      g_free (tmp);
    }
  {
    char *e0 = g_build_filename (dir, "name.jpg", NULL);
    char *e1 = g_build_filename (dir, "name (1).jpg", NULL);
    char *e2 = g_build_filename (dir, "name (2).jpg", NULL);

    g_assert_cmpstr (a, ==, e0);
    g_assert_cmpstr (b, ==, e1);
    g_assert_cmpstr (c, ==, e2);
    g_unlink (a); g_unlink (b); g_unlink (c);
    g_free (e0); g_free (e1); g_free (e2);
  }
  g_free (a); g_free (b); g_free (c);
  g_rmdir (dir);
  g_free (dir);
}

static void
check_attempts (GPtrArray *at, guint n_attempts)
{
  g_assert_nonnull (at);
  g_assert_cmpuint (at->len, ==, n_attempts);
}

static void
test_argv (void)
{
  gboolean dec;
  GPtrArray *at;
  GPtrArray *a0;
  char **argv;

  nfc_tools_init ();
  /* use fake tool paths so the test does not depend on what is installed */
  g_free (nfc_tools.magick); g_free (nfc_tools.ffmpeg); g_free (nfc_tools.soffice);
  nfc_tools.magick = g_strdup ("/bin/magick");
  nfc_tools.ffmpeg = g_strdup ("/bin/ffmpeg");
  nfc_tools.soffice = g_strdup ("/bin/soffice");
  g_free (nfc_tools.decode);
  nfc_tools.decode = g_strdup ("/bin/nfc-decode");

  /* every ImageMagick output carries an explicit format prefix */
  {
    const struct { const char *key, *prefix; } im[] = {
      { "png", "PNG:" }, { "jpg", "JPEG:" }, { "webp", "WEBP:" }, { "ico", "ICO:" },
      { "bmp", "BMP:" }, { "tiff", "TIFF:" }, { "pdf", "PDF:" },
    };

    for (guint i = 0; i < G_N_ELEMENTS (im); i++)
      {
        char *out = g_strdup_printf ("/o/.x.nfc-1.%s", im[i].key);
        char *want = g_strconcat (im[i].prefix, out, NULL);

        at = nfc_build_attempts (NFC_CAT_IMAGE, "png", im[i].key, "/s/a.png", out, "/w", &dec);
        check_attempts (at, 1);
        g_assert_false (dec);
        a0 = at->pdata[0];
        g_assert_cmpuint (a0->len, ==, 1);
        argv = a0->pdata[0];
        g_assert_true (argv_has (argv, want));
        g_assert_true (argv_has (argv, "PNG:/s/a.png[0]")); /* explicit input coder */
        g_assert_true (argv_has (argv, "-limit"));       /* resource limits */
        g_assert_true (argv_has (argv, "1GiB"));
        g_assert_true (argv_has (argv, "4GiB"));
        g_ptr_array_unref (at);
        g_free (out);
        g_free (want);
      }
  }
  /* transparency flattened for jpg and pdf, not for png */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "jpg", "/s/a.png", "/o/x.jpg", "/w", &dec);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "remove"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "png", "/s/a.png", "/o/x.png", "/w", &dec);
  g_assert_false (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "remove"));
  g_ptr_array_unref (at);
  /* ICO is limited to 256 */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "ico", "/s/a.png", "/o/x.ico", "/w", &dec);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "256x256>"));
  g_ptr_array_unref (at);
  /* svg input gets density */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "svg", "png", "/s/a.svg", "/o/x.png", "/w", &dec);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "192"));
  g_ptr_array_unref (at);
  /* HEIC/AVIF input: step 1 is the out-of-process nfc-decode, step 2 magick reads
   * the decoded png */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "heic", "jpg", "/s/a.heic", "/o/x.jpg", "/w", &dec);
  g_assert_true (dec);
  a0 = at->pdata[0];
  g_assert_cmpuint (a0->len, ==, 2);
  argv = a0->pdata[0];
  g_assert_cmpstr (argv[0], ==, "/bin/nfc-decode");
  g_assert_cmpstr (argv[1], ==, "/s/a.heic");
  g_assert_cmpstr (argv[2], ==, "/w/decoded.png");
  g_assert_null (argv[3]);
  g_assert_true (argv_has (a0->pdata[1], "PNG:/w/decoded.png[0]"));
  g_ptr_array_unref (at);
  /* no helper: HEIC/AVIF input cannot be converted at all */
  {
    char *saved = nfc_tools.decode;

    nfc_tools.decode = NULL;
    g_assert_null (nfc_build_attempts (NFC_CAT_IMAGE, "avif", "jpg", "/s/a.avif", "/o/x.jpg", "/w", &dec));
    nfc_tools.decode = saved;
  }
  /* AVIF output: magick -> PNG:flat, then ffmpeg libaom (never libsvtav1) */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "avif", "/s/a.png", "/o/x.avif", "/w", &dec);
  a0 = at->pdata[0];
  g_assert_cmpuint (a0->len, ==, 2);
  g_assert_true (argv_has (a0->pdata[0], "PNG:/w/flat.png"));
  g_assert_true (argv_has (a0->pdata[0], "-limit"));
  g_assert_true (argv_has (a0->pdata[1], "libaom-av1"));
  g_assert_false (argv_has (a0->pdata[1], "libsvtav1"));
  g_assert_true (argv_has (a0->pdata[1], "/o/x.avif"));
  g_ptr_array_unref (at);
  /* MKV: stream copy first, re-encode as fallback */
  at = nfc_build_attempts (NFC_CAT_VIDEO, "mp4", "mkv", "/s/a.mp4", "/o/x.mkv", "/w", &dec);
  check_attempts (at, 2);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "copy"));
  g_assert_true (argv_has (((GPtrArray *) at->pdata[1])->pdata[0], "libx264"));
  g_ptr_array_unref (at);
  /* (this process never runs the probes, so librsvg is unknown: the svg fallback
   * MSVG applies; test-robust covers RSVG with a fake magick listing it) */
  /* every input type gets its own explicit coder, never content sniffing */
  {
    const struct { const char *fmt, *in; } c[] = {
      { "jpg", "JPEG:/s/a.x[0]" }, { "webp", "WEBP:/s/a.x[0]" }, { "bmp", "BMP:/s/a.x[0]" },
      { "gif", "GIF:/s/a.x[0]" }, { "ico", "ICO:/s/a.x[0]" }, { "svg", "MSVG:/s/a.x[0]" },
      { "tiff", "TIFF:/s/a.x[0]" },
    };

    for (guint i = 0; i < G_N_ELEMENTS (c); i++)
      {
        at = nfc_build_attempts (NFC_CAT_IMAGE, c[i].fmt, "png", "/s/a.x", "/o/x.png", "/w", &dec);
        g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], c[i].in));
        g_ptr_array_unref (at);
      }
  }
  /* '%' in any path handed to ImageMagick is escaped as "%%" (input incl. the
   * frame suffix, output, and the work dir files) */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "jpg", "/s/100%/a%d.png", "/o/.a%d.nfc-1.jpg",
                           "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "PNG:/s/100%%/a%%d.png[0]"));
  g_assert_true (argv_has (argv, "JPEG:/o/.a%%d.nfc-1.jpg"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_IMAGE, "heic", "avif", "/s/a%d.heic", "/o/x.avif", "/w%x", &dec);
  a0 = at->pdata[0];
  g_assert_cmpuint (a0->len, ==, 3);
  g_assert_cmpstr (((char **) a0->pdata[0])[1], ==, "/s/a%d.heic"); /* nfc-decode: no escaping */
  g_assert_true (argv_has (a0->pdata[1], "PNG:/w%%x/decoded.png[0]"));
  g_assert_true (argv_has (a0->pdata[1], "PNG:/w%%x/flat.png"));
  g_assert_true (argv_has (a0->pdata[2], "/w%x/flat.png")); /* ffmpeg gets it as is */
  g_ptr_array_unref (at);
  /* documents: private profile and outdir */
  at = nfc_build_attempts (NFC_CAT_DOC, "txt", "pdf", "/s/a.txt", "/w/out/a.pdf", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "-env:UserInstallation=file:///w/lo_profile"));
  g_assert_true (argv_has (argv, "--outdir"));
  g_assert_true (argv_has (argv, "file:///w/out")); /* outdir as a URI: LibreOffice decodes %XX in paths */
  g_ptr_array_unref (at);
  /* the profile is a real file URI: spaces and '%' are escaped */
  at = nfc_build_attempts (NFC_CAT_DOC, "txt", "pdf", "/s/a.txt", "/w sp%20/out/a.pdf", "/w sp%20", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "-env:UserInstallation=file:///w%20sp%2520/lo_profile"));
  g_assert_true (argv_has (argv, "file:///w%20sp%2520/out"));
  g_ptr_array_unref (at);
  /* missing tool: no commands */
  g_clear_pointer (&nfc_tools.ffmpeg, g_free);
  g_assert_null (nfc_build_attempts (NFC_CAT_AUDIO, "wav", "mp3", "/s/a.wav", "/o/x.mp3", "/w", &dec));
}

/* publishing never overwrites anything, in every strategy: link(), renameat2
 * (RENAME_NOREPLACE) and the O_EXCL placeholder + rename fallback */
static void
test_publish_noreplace (void)
{
  const NfcPublishMode modes[] = { NFC_PUBLISH_AUTO, NFC_PUBLISH_NO_LINK,
                                   NFC_PUBLISH_NO_LINK_NO_RENAME2 };

  for (guint m = 0; m < G_N_ELEMENTS (modes); m++)
    {
      GError *error = NULL;
      char *dir = g_dir_make_tmp ("nfc-pub-XXXXXX", &error);
      char *p0 = g_build_filename (dir, "name.jpg", NULL);
      char *p1 = g_build_filename (dir, "name (1).jpg", NULL);
      char *p2 = g_build_filename (dir, "name (2).jpg", NULL);
      char *p3 = g_build_filename (dir, "name (3).jpg", NULL);
      char *tmp = g_build_filename (dir, ".name.nfc-1.jpg", NULL);
      char *res, *data = NULL;

      g_assert_no_error (error);
      g_file_set_contents (p0, "old0", -1, NULL);
      g_file_set_contents (p1, "old1", -1, NULL);
      /* a dangling symlink is an existing name too: never write through it */
      g_assert_cmpint (symlink ("/nonexistent/target", p2), ==, 0);
      g_file_set_contents (tmp, "new", -1, NULL);

      res = nfc_publish_with_mode (tmp, dir, "name", "jpg", modes[m], &error);
      g_assert_no_error (error);
      g_assert_cmpstr (res, ==, p3);
      g_assert_false (g_file_test (tmp, G_FILE_TEST_EXISTS));
      g_assert_true (g_file_get_contents (p3, &data, NULL, NULL));
      g_assert_cmpstr (data, ==, "new");
      g_free (data);
      g_assert_true (g_file_get_contents (p0, &data, NULL, NULL));
      g_assert_cmpstr (data, ==, "old0");
      g_free (data);
      g_assert_true (g_file_get_contents (p1, &data, NULL, NULL));
      g_assert_cmpstr (data, ==, "old1");
      g_free (data);
      g_assert_true (g_file_test (p2, G_FILE_TEST_IS_SYMLINK));
      g_assert_false (g_file_test ("/nonexistent/target", G_FILE_TEST_EXISTS));
      g_free (res);

      /* a missing destination directory is an error, not a hang or a loop */
      g_file_set_contents (tmp, "new", -1, NULL);
      {
        char *gone = g_build_filename (dir, "no-such-dir", NULL);

        res = nfc_publish_with_mode (tmp, gone, "name", "jpg", modes[m], &error);
        g_assert_null (res);
        g_assert_nonnull (error);
        g_clear_error (&error);
        g_assert_true (g_file_test (tmp, G_FILE_TEST_EXISTS));
        g_free (gone);
      }
      g_unlink (p0); g_unlink (p1); g_unlink (p2); g_unlink (p3); g_unlink (tmp);
      g_rmdir (dir);
      g_free (p0); g_free (p1); g_free (p2); g_free (p3); g_free (tmp); g_free (dir);
    }
}

static gboolean
run_decode (const char *helper, const char *a, const char *b, char **out, char **err, int *rc)
{
  char *argv[4] = { (char *) helper, (char *) a, (char *) b, NULL };
  gint status = 0;
  gboolean ok = g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, out, err, &status, NULL);

  *rc = ok && WIFEXITED (status) ? WEXITSTATUS (status) : -1;
  return ok;
}

/* the out-of-process decoder: formats, a good decode, a bad file, usage */
static void
test_decode_helper (void)
{
  const char *helper = g_getenv ("NFC_DECODE_HELPER");
  char *dir, *in, *outp, *so = NULL, *se = NULL, *data = NULL;
  const char *png_b64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAMAAAAoyzS7AAAAA1BMVEX/AAAZ4gk3AAAACklEQVQI"
                        "12NgAAAAAgAB4iG8MwAAAABJRU5ErkJggg==";
  gsize len;
  guchar *png;
  int rc;

  if (helper == NULL)
    {
      g_test_skip ("NFC_DECODE_HELPER not set");
      return;
    }
  g_assert_true (run_decode (helper, "--formats", NULL, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 0);
  {
    char **v = g_strsplit (so, "\n", -1);

    g_assert_true (g_strv_contains ((const char *const *) v, "png"));
    g_assert_true (g_strv_contains ((const char *const *) v, "jpeg"));
    g_strfreev (v);
  }
  g_free (so); g_free (se);

  dir = g_dir_make_tmp ("nfc-dec-XXXXXX", NULL);
  in = g_build_filename (dir, "in%d.png", NULL);
  outp = g_build_filename (dir, "out.png", NULL);
  png = g_base64_decode (png_b64, &len);
  g_assert_true (g_file_set_contents (in, (const char *) png, len, NULL));
  g_free (png);
  g_assert_true (run_decode (helper, in, outp, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 0);
  g_assert_true (g_file_get_contents (outp, &data, &len, NULL));
  g_assert_cmpuint (len, >, 8);
  g_assert_cmpint (memcmp (data, "\x89PNG\r\n\x1a\n", 8), ==, 0);
  g_free (data); g_free (so); g_free (se);

  /* HEIC sample, when provided: decodes to a PNG too */
  {
    const char *heic = g_getenv ("NFC_HEIC_SAMPLE");

    if (heic != NULL && g_file_test (heic, G_FILE_TEST_IS_REGULAR))
      {
        g_unlink (outp);
        g_assert_true (run_decode (helper, heic, outp, &so, &se, &rc));
        g_assert_cmpint (rc, ==, 0);
        g_assert_true (g_file_get_contents (outp, &data, &len, NULL));
        g_assert_cmpint (memcmp (data, "\x89PNG\r\n\x1a\n", 8), ==, 0);
        g_free (data); g_free (so); g_free (se);
      }
  }

  /* garbage: exit 1, message on stderr, no output */
  g_unlink (outp);
  g_assert_true (g_file_set_contents (in, "this is not an image", -1, NULL));
  g_assert_true (run_decode (helper, in, outp, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 1);
  g_assert_nonnull (strstr (se, "cannot decode"));
  g_assert_false (g_file_test (outp, G_FILE_TEST_EXISTS));
  g_free (so); g_free (se);
  /* usage errors */
  g_assert_true (run_decode (helper, "-x", "y", &so, &se, &rc));
  g_assert_cmpint (rc, ==, 2);
  g_free (so); g_free (se);
  g_assert_true (run_decode (helper, "/nonexistent.png", outp, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 1);
  g_free (so); g_free (se);

  g_unlink (in); g_unlink (outp); g_rmdir (dir);
  g_free (in); g_free (outp); g_free (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/core/classify", test_classify);
  g_test_add_func ("/core/naming", test_naming);
  g_test_add_func ("/core/argv", test_argv);
  g_test_add_func ("/core/publish-noreplace", test_publish_noreplace);
  g_test_add_func ("/core/decode-helper", test_decode_helper);
  return g_test_run ();
}
