/* Unit tests for the pure logic: classification, argv builders, naming. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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
        g_assert_true (argv_has (argv, "/s/a.png[0]"));
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
  /* HEIC/AVIF input is decoded first and magick reads the decoded png */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "heic", "jpg", "/s/a.heic", "/o/x.jpg", "/w", &dec);
  g_assert_true (dec);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], "/w/decoded.png[0]"));
  g_ptr_array_unref (at);
  /* AVIF output: magick -> PNG:flat, then ffmpeg libaom (never libsvtav1) */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "avif", "/s/a.png", "/o/x.avif", "/w", &dec);
  a0 = at->pdata[0];
  g_assert_cmpuint (a0->len, ==, 2);
  g_assert_true (argv_has (a0->pdata[0], "PNG:/w/flat.png"));
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
  /* documents: private profile and outdir */
  at = nfc_build_attempts (NFC_CAT_DOC, "txt", "pdf", "/s/a.txt", "/w/out/a.pdf", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "-env:UserInstallation=file:///w/lo_profile"));
  g_assert_true (argv_has (argv, "--outdir"));
  g_assert_true (argv_has (argv, "/w/out"));
  g_ptr_array_unref (at);
  /* missing tool: no commands */
  g_clear_pointer (&nfc_tools.ffmpeg, g_free);
  g_assert_null (nfc_build_attempts (NFC_CAT_AUDIO, "wav", "mp3", "/s/a.wav", "/o/x.mp3", "/w", &dec));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/core/classify", test_classify);
  g_test_add_func ("/core/naming", test_naming);
  g_test_add_func ("/core/argv", test_argv);
  return g_test_run ();
}
