/* Unit tests for the pure logic: classification, argv builders, naming. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "nfc-core.h"

static gboolean
argv_has (char **argv, const char *s)
{
  for (; *argv; argv++)
    if (g_str_equal (*argv, s))
      return TRUE;
  return FALSE;
}

/* the argument that follows the first occurrence of opt, or NULL */
static const char *
argv_after (char **argv, const char *opt)
{
  for (; *argv; argv++)
    if (g_str_equal (*argv, opt))
      return argv[1];
  return NULL;
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
        /* memory: a quarter of the RAM, 1..8 GiB (never below the old 1 GiB);
         * map: twice that; disk: generous, the old 4 GiB made a 207 megapixel
         * screenshot fail with "cache resources exhausted" */
        {
          guint64 mem = g_ascii_strtoull (argv_after (argv, "memory"), NULL, 10);
          guint64 map = g_ascii_strtoull (argv_after (argv, "map"), NULL, 10);

          g_assert_true (g_str_has_suffix (argv_after (argv, "memory"), "MiB"));
          g_assert_cmpuint (mem, >=, 1024);
          g_assert_cmpuint (mem, <=, 8192);
          g_assert_cmpuint (map, ==, 2 * mem);
        }
        g_assert_cmpstr (argv_after (argv, "disk"), ==, "16GiB");
        g_assert_cmpstr (argv_after (argv, "time"), ==, "600");
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
  /* ICO: 256 max, aspect kept (square transparent padding), several sizes */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "ico", "/s/a.png", "/o/x.ico", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "256x256>"));
  g_assert_true (argv_has (argv, "icon:auto-resize=256,48,32,16"));
  g_assert_true (argv_has (argv, "-extent"));
  g_ptr_array_unref (at);
  /* JPG: explicit 4:4:4 always; q99 for graphics (screenshots), q95 for photos */
  {
    const struct { const char *fmt, *src, *q; } j[] = {
      { "png", "/s/a.png", "99" }, { "bmp", "/s/a.bmp", "99" }, { "svg", "/s/a.svg", "99" },
      { "heic", "/s/a.heic", "95" }, { "webp", "/s/a.webp", "95" },
    };

    for (guint i = 0; i < G_N_ELEMENTS (j); i++)
      {
        at = nfc_build_attempts (NFC_CAT_IMAGE, j[i].fmt, "jpg", j[i].src, "/o/x.jpg", "/w", &dec);
        a0 = at->pdata[0];
        argv = a0->pdata[a0->len - 1];
        g_assert_cmpstr (argv_after (argv, "-sampling-factor"), ==, "4:4:4");
        g_assert_cmpstr (argv_after (argv, "-quality"), ==, j[i].q);
        g_ptr_array_unref (at);
      }
  }
  /* WEBP: lossless for lossless sources, lossy q94 + sharp YUV for photos;
   * never wider than the 16383 pixels WEBP can hold */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "webp", "/s/a.png", "/o/x.webp", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "webp:lossless=true"));
  g_assert_false (argv_has (argv, "-quality"));
  g_assert_true (argv_has (argv, "16383x16383>"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_IMAGE, "heic", "webp", "/s/a.heic", "/o/x.webp", "/w", &dec);
  a0 = at->pdata[0];
  argv = a0->pdata[a0->len - 1];
  g_assert_false (argv_has (argv, "webp:lossless=true"));
  g_assert_cmpstr (argv_after (argv, "-quality"), ==, "94");
  g_assert_true (argv_has (argv, "webp:sharp-yuv=true"));
  g_ptr_array_unref (at);
  /* AVIF: graphics 4:4:4 crf 9 with a fallback attempt (8-bit 4:2:0), photos
   * 4:2:0 crf 12; both tagged BT.709 full range, even-size scale gone */
  at = nfc_build_attempts (NFC_CAT_IMAGE, "png", "avif", "/s/a.png", "/o/x.avif", "/w", &dec);
  check_attempts (at, 2);
  a0 = at->pdata[0];
  argv = a0->pdata[1];
  g_assert_true (g_str_has_suffix (argv_after (argv, "-vf"), "format=yuv444p"));
  g_assert_cmpstr (argv_after (argv, "-crf"), ==, "9");
  g_assert_cmpstr (argv_after (argv, "-colorspace"), ==, "bt709");
  g_assert_cmpstr (argv_after (argv, "-color_range"), ==, "pc");
  g_assert_false (argv_has (argv, "-tiles"));
  g_assert_null (strstr (argv_after (argv, "-vf"), "trunc"));
  argv = ((GPtrArray *) at->pdata[1])->pdata[1];
  g_assert_true (g_str_has_suffix (argv_after (argv, "-vf"), "format=yuv420p"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_IMAGE, "jpg", "avif", "/s/a.jpg", "/o/x.avif", "/w", &dec);
  check_attempts (at, 1);
  argv = ((GPtrArray *) at->pdata[0])->pdata[1];
  g_assert_true (g_str_has_suffix (argv_after (argv, "-vf"), "format=yuv420p"));
  g_assert_cmpstr (argv_after (argv, "-crf"), ==, "12");
  g_ptr_array_unref (at);
  /* video: x264 crf 18 slow (audio optional, 192k), VP9 crf 28, GIF 15 fps up to
   * 960 px with the diff palette; the audio maps are all optional */
  at = nfc_build_attempts (NFC_CAT_VIDEO, "mkv", "mp4", "/s/a.mkv", "/o/x.mp4", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_cmpstr (argv_after (argv, "-crf"), ==, "18");
  g_assert_cmpstr (argv_after (argv, "-preset"), ==, "slow");
  g_assert_cmpstr (argv_after (argv, "-b:a"), ==, "192k");
  g_assert_true (argv_has (argv, "0:a:0?"));
  g_assert_false (argv_has (argv, "0:a:0"));
  g_assert_true (argv_has (argv, "+faststart"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_VIDEO, "mp4", "webm", "/s/a.mp4", "/o/x.webm", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_cmpstr (argv_after (argv, "-crf"), ==, "28");
  g_assert_cmpstr (argv_after (argv, "-deadline"), ==, "good");
  g_assert_true (argv_has (argv, "0:a:0?"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts (NFC_CAT_VIDEO, "mp4", "gif", "/s/a.mp4", "/o/x.gif", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_nonnull (strstr (argv_after (argv, "-vf"), "fps=15,scale='min(960,iw)'"));
  g_assert_nonnull (strstr (argv_after (argv, "-vf"), "palettegen=stats_mode=diff"));
  g_assert_nonnull (strstr (argv_after (argv, "-vf"), "diff_mode=rectangle"));
  g_ptr_array_unref (at);
  /* audio targets map the first audio stream (required: a video without audio
   * fails, and nfc-convert turns that into a plain message) */
  at = nfc_build_attempts (NFC_CAT_VIDEO, "mp4", "mp3", "/s/a.mp4", "/o/x.mp3", "/w", &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_true (argv_has (argv, "0:a:0"));
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

  /* --info: the size from the header (the png is 1 x 1) */
  g_assert_true (run_decode (helper, "--info", in, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 0);
  g_assert_cmpstr (so, ==, "1 1\n");
  g_free (so); g_free (se);

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
  g_assert_true (run_decode (helper, "--info", in, &so, &se, &rc));
  g_assert_cmpint (rc, ==, 1);
  g_assert_cmpstr (so, ==, "");
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

/* ---- image size, memory estimate, admission, PDF page size ---- */

static void
test_image_info_parse (void)
{
  NfcImageInfo i;

  g_assert_true (nfc_image_info_parse ("1920 1080|72 72 Undefined\n", &i));
  g_assert_cmpuint (i.width, ==, 1920);
  g_assert_cmpuint (i.height, ==, 1080);
  g_assert_false (i.has_density);
  /* units undefined: the numbers are only an aspect ratio, whatever they say */
  g_assert_true (nfc_image_info_parse ("800 600|96 96 Undefined", &i));
  g_assert_false (i.has_density);
  /* a stamped 72 dpi, written in either unit, is no information */
  g_assert_true (nfc_image_info_parse ("1920 1080|72 72 PixelsPerInch", &i));
  g_assert_false (i.has_density);
  g_assert_true (nfc_image_info_parse ("1920 1080|28.34 28.34 PixelsPerCentimeter", &i));
  g_assert_false (i.has_density);
  /* real resolutions */
  g_assert_true (nfc_image_info_parse ("1920 1080|118.11 118.11 PixelsPerCentimeter", &i));
  g_assert_true (i.has_density);
  g_assert_cmpfloat_with_epsilon (i.xdpi, 300.0, 0.05);
  g_assert_true (nfc_image_info_parse ("4000 3000|300 300 PixelsPerInch", &i));
  g_assert_true (i.has_density);
  g_assert_true (nfc_image_info_parse ("4000 3000|72 300 PixelsPerInch", &i));
  g_assert_true (i.has_density);
  /* nfc-decode --info prints the size only */
  g_assert_true (nfc_image_info_parse ("4032 3024\n", &i));
  g_assert_cmpuint (i.width, ==, 4032);
  g_assert_false (i.has_density);
  /* junk */
  g_assert_false (nfc_image_info_parse ("", &i));
  g_assert_false (nfc_image_info_parse (NULL, &i));
  g_assert_false (nfc_image_info_parse ("0 5|72 72 Undefined", &i));
  g_assert_false (nfc_image_info_parse ("identify: no decode delegate", &i));
  g_assert_false (nfc_image_info_parse ("12", &i));
}

static void
test_pdf_fit_dpi (void)
{
  /* a 1920 x 1080 screenshot: 165 dpi = 11.64 x 6.55 in, inside A4 landscape */
  g_assert_cmpuint (nfc_pdf_fit_dpi (1920, 1080), ==, 165);
  g_assert_cmpuint (nfc_pdf_fit_dpi (1080, 1920), ==, 165); /* portrait: same, rotated */
  g_assert_cmpuint (nfc_pdf_fit_dpi (3840, 2160), ==, 329);
  /* never below 96 dpi: a small icon is not blown up to a page */
  g_assert_cmpuint (nfc_pdf_fit_dpi (64, 64), ==, 96);
  g_assert_cmpuint (nfc_pdf_fit_dpi (1, 1), ==, 96);
  /* a tall phone screenshot fits the A4 height: 1170 x 2532 */
  g_assert_cmpuint (nfc_pdf_fit_dpi (1170, 2532), ==, 217);
  /* whatever it is, the page is never larger than A4 (or 96 dpi small) */
  for (guint64 w = 1; w < 20000; w += 997)
    for (guint64 h = 1; h < 20000; h += 1511)
      {
        double dpi = nfc_pdf_fit_dpi (w, h);
        double lo = MIN (w, h) / dpi, hi = MAX (w, h) / dpi;

        g_assert_cmpfloat (lo, <=, 8.27);
        g_assert_cmpfloat (hi, <=, 11.69);
      }
}

static void
test_estimate (void)
{
  NfcImageInfo big = { 19200, 10800, FALSE, 0, 0 }; /* the measured 207 MP image */
  NfcImageInfo none = { 0, 0, FALSE, 0, 0 };
  guint64 base = nfc_estimate_peak_bytes ("png", "jpg", &big);
  guint64 webp = nfc_estimate_peak_bytes ("png", "webp", &big);
  guint64 avif = nfc_estimate_peak_bytes ("png", "avif", &big);

  /* calibration: measured 5.7 GB (jpg, ico, bmp, tiff, pdf), 7.6 GB (avif),
   * 8.4 GB (webp); the estimate must be above, within 15 % *
   */
  g_assert_cmpuint (base, >=, 5700000000ULL);
  g_assert_cmpuint (base, <=, 5700000000ULL * 115 / 100);
  g_assert_cmpuint (avif, >=, 7600000000ULL);
  g_assert_cmpuint (avif, <=, 7600000000ULL * 115 / 100);
  g_assert_cmpuint (webp, >=, 8400000000ULL);
  g_assert_cmpuint (webp, <=, 8400000000ULL * 115 / 100);
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "pdf", &big), ==, base);
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "ico", &big), ==, base);
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "bmp", &big), ==, base);
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "tiff", &big), ==, base);
  /* linear in the pixel count */
  {
    NfcImageInfo half = { 19200, 5400, FALSE, 0, 0 };

    g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "jpg", &half), ==, base / 2);
  }
  /* SVG renders at 192 dpi: 64/9 of the nominal pixels */
  {
    NfcImageInfo s = { 900, 900, FALSE, 0, 0 };

    g_assert_cmpuint (nfc_estimate_peak_bytes ("svg", "png", &s), ==, 900ULL * 900 * 64 / 9 * 30);
  }
  /* no size known: no estimate; absurd sizes do not overflow */
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "jpg", NULL), ==, 0);
  g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "jpg", &none), ==, 0);
  {
    NfcImageInfo huge = { G_MAXUINT64, G_MAXUINT64, FALSE, 0, 0 };

    g_assert_cmpuint (nfc_estimate_peak_bytes ("png", "webp", &huge), >, 8400000000ULL);
  }
}

static void
test_admission (void)
{
  const guint64 G = 1000ULL * 1000 * 1000;
  const guint64 big_px = 100 * 1000 * 1000, small_px = 24 * 1000 * 1000;

  /* nothing running: a single huge file always runs, even above the budget */
  g_assert_true (nfc_admission_allows (big_px, 100 * G, 0, 0, 1 * G));
  /* something runs: it must fit */
  g_assert_true (nfc_admission_allows (big_px, 3 * G, 5 * G, 1, 8 * G));
  g_assert_true (nfc_admission_allows (big_px, 3 * G, 5 * G, 1, 8 * G + 1)); /* boundary: equal fits */
  g_assert_false (nfc_admission_allows (big_px, 3 * G + 1, 5 * G, 1, 8 * G));
  g_assert_false (nfc_admission_allows (big_px, 6 * G, 5 * G, 3, 8 * G));
  g_assert_false (nfc_admission_allows (big_px, 9 * G, 0, 1, 8 * G)); /* larger than all of it */
  /* small images are never held back */
  g_assert_true (nfc_admission_allows (small_px, 100 * G, 100 * G, 7, 1 * G));
  g_assert_true (nfc_admission_allows (0, 0, 100 * G, 7, 0));
  g_assert_false (nfc_admission_allows (NFC_SMALL_IMAGE_PIXELS, 2 * G, 100 * G, 7, 1 * G));
  /* a budget of 0 (MemAvailable unreadable): huge files run one at a time */
  g_assert_true (nfc_admission_allows (big_px, 3 * G, 0, 0, 0));
  g_assert_false (nfc_admission_allows (big_px, 3 * G, 3 * G, 1, 0));

  /* the memory figure: NFC_TEST_MEMAVAILABLE_KB replaces /proc/meminfo */
  g_setenv ("NFC_TEST_MEMAVAILABLE_KB", "10000000", TRUE);
  g_assert_cmpuint (nfc_mem_available_bytes (), ==, 10000000ULL * 1024);
  g_assert_cmpuint (nfc_admission_budget_bytes (), ==, 10000000ULL * 1024 / 10 * 6);
  g_unsetenv ("NFC_TEST_MEMAVAILABLE_KB");
  g_assert_cmpuint (nfc_mem_available_bytes (), >, 0); /* the real one */
  g_assert_cmpuint (nfc_admission_budget_bytes (), <, nfc_mem_available_bytes ());
}

static void
test_probe_argv_and_pdf_density (void)
{
  NfcImageInfo shot = { 1920, 1080, FALSE, 0, 0 };
  NfcImageInfo scan = { 2480, 3508, TRUE, 300, 300 };
  GPtrArray *at;
  char **argv;
  gboolean dec;

  nfc_tools_init ();
  g_free (nfc_tools.magick);
  nfc_tools.magick = g_strdup ("/bin/magick");
  g_free (nfc_tools.decode);
  nfc_tools.decode = g_strdup ("/bin/nfc-decode");

  /* the size probe: header only (-ping), explicit coder, first frame, % escaped */
  argv = nfc_build_probe_argv ("png", "/s/100%d.png");
  g_assert_nonnull (argv);
  g_assert_cmpstr (argv[0], ==, "/bin/magick");
  g_assert_cmpstr (argv[1], ==, "identify");
  g_assert_true (argv_has (argv, "-ping"));
  g_assert_cmpstr (argv_after (argv, "-format"), ==, "%w %h|%x %y %U");
  g_assert_cmpstr (argv[g_strv_length (argv) - 1], ==, "PNG:/s/100%%d.png[0]");
  g_strfreev (argv);
  argv = nfc_build_probe_argv ("heic", "/s/a.heic");
  g_assert_nonnull (argv);
  g_assert_cmpstr (argv[0], ==, "/bin/nfc-decode");
  g_assert_cmpstr (argv[1], ==, "--info");
  g_assert_cmpstr (argv[2], ==, "/s/a.heic");
  g_assert_null (argv[3]);
  g_strfreev (argv);
  g_assert_null (nfc_build_probe_argv ("mp4", "/s/a.mp4"));

  /* PDF without a resolution: a density that fits A4, nothing resampled */
  at = nfc_build_attempts_info (NFC_CAT_IMAGE, "png", "pdf", "/s/a.png", "/o/.x.pdf", "/w", &shot, &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_cmpstr (argv_after (argv, "-units"), ==, "PixelsPerInch");
  g_assert_cmpstr (argv_after (argv, "-density"), ==, "165");
  g_assert_false (argv_has (argv, "-resize"));
  g_assert_false (argv_has (argv, "-resample"));
  g_assert_false (argv_has (argv, "-scale"));
  g_ptr_array_unref (at);
  /* a real resolution is kept (no -density), and so is "no probe result" */
  at = nfc_build_attempts_info (NFC_CAT_IMAGE, "png", "pdf", "/s/a.png", "/o/.x.pdf", "/w", &scan, &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_false (argv_has (argv, "-density"));
  g_ptr_array_unref (at);
  at = nfc_build_attempts_info (NFC_CAT_IMAGE, "png", "pdf", "/s/a.png", "/o/.x.pdf", "/w", NULL, &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_false (argv_has (argv, "-density"));
  g_ptr_array_unref (at);
  /* SVG keeps its own 192 dpi rendering; other targets never get the fit */
  at = nfc_build_attempts_info (NFC_CAT_IMAGE, "svg", "pdf", "/s/a.svg", "/o/.x.pdf", "/w", &shot, &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_cmpstr (argv_after (argv, "-density"), ==, "192");
  g_ptr_array_unref (at);
  at = nfc_build_attempts_info (NFC_CAT_IMAGE, "png", "jpg", "/s/a.png", "/o/.x.jpg", "/w", &shot, &dec);
  argv = ((GPtrArray *) at->pdata[0])->pdata[0];
  g_assert_false (argv_has (argv, "-density"));
  g_ptr_array_unref (at);
}

/* the spill directory: under the cache dir, private, and old leftovers go */
static void
test_spill_dir (void)
{
  char *cache = g_dir_make_tmp ("nfc-spill-XXXXXX", NULL);
  char *want = g_build_filename (cache, "nautilus-file-converter", "tmp", NULL);
  char *old = g_build_filename (want, "magick-old", NULL);
  char *young = g_build_filename (want, "magick-young", NULL);
  char *dir;
  GStatBuf st;
  struct utimbuf ut;

  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  g_assert_cmpint (g_mkdir_with_parents (want, 0755), ==, 0); /* too open on purpose */
  g_assert_true (g_file_set_contents (old, "x", -1, NULL));
  g_assert_true (g_file_set_contents (young, "x", -1, NULL));
  ut.actime = ut.modtime = time (NULL) - 2 * 24 * 3600;
  g_assert_cmpint (utime (old, &ut), ==, 0);

  dir = nfc_spill_dir ();
  g_assert_cmpstr (dir, ==, want);
  g_assert_cmpint (g_stat (dir, &st), ==, 0);
  g_assert_cmpuint (st.st_mode & 07777, ==, 0700);
  for (int i = 0; i < 100 && g_file_test (old, G_FILE_TEST_EXISTS); i++)
    g_usleep (50000);
  g_assert_false (g_file_test (old, G_FILE_TEST_EXISTS));   /* older than a day: removed */
  g_assert_true (g_file_test (young, G_FILE_TEST_EXISTS));  /* recent: kept */
  g_free (dir);

  g_unlink (young);
  g_rmdir (want);
  {
    char *p = g_build_filename (cache, "nautilus-file-converter", NULL);

    g_rmdir (p);
    g_free (p);
  }
  g_rmdir (cache);
  g_free (cache); g_free (want); g_free (old); g_free (young);
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
  g_test_add_func ("/core/image-info-parse", test_image_info_parse);
  g_test_add_func ("/core/pdf-fit-dpi", test_pdf_fit_dpi);
  g_test_add_func ("/core/estimate", test_estimate);
  g_test_add_func ("/core/admission", test_admission);
  g_test_add_func ("/core/probe-argv-and-pdf-density", test_probe_argv_and_pdf_density);
  g_test_add_func ("/core/spill-dir", test_spill_dir);
  return g_test_run ();
}
