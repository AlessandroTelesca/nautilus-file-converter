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
  g_assert_true (argv_has (argv, "icon:auto-resize=256,48,32,16")); /* size unknown: all */
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
  /* WEBP: lossless for lossless sources, lossy q90 + sharp YUV for photos;
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
  g_assert_cmpstr (argv_after (argv, "-quality"), ==, "90");
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

static void
expect_msg (const char *text, const char *fmt, const char *want)
{
  char *got = nfc_failure_message (text, fmt);

  g_assert_cmpstr (got, ==, want);
  g_free (got);
}

/* The line shown in the failure toast: the cause, not the last line, in words
 * where it is known. The inputs are the real stderr texts seen in the field. */
static void
test_failure_message (void)
{
  char *long_line, *got;

  /* HEIC without an HEVC decoder (glycin): the last stderr line is the loader's
   * own chatter, the cause is earlier */
  expect_msg ("cannot decode /x/IMG_1.heic: Failed to load image: "
              "org.gnome.glycin.Error.LoadingError: glycin-loaders/glycin-heif/src/main.rs:103:24: "
              "DecoderPluginError(Unspecified)\nSetting process memory limit\n", "heic",
              "can't decode HEIC: the HEVC decoder is missing "
              "(install libheif-freeworld from RPM Fusion)");
  /* ... but the same text for another source format is not claimed to be HEVC */
  got = nfc_failure_message ("org.gnome.glycin.Error.LoadingError: x\nSetting process memory limit\n",
                             "avif");
  g_assert_nonnull (strstr (got, "LoadingError"));
  g_assert_null (strstr (got, "HEVC"));
  g_free (got);
  /* ffmpeg with a broken decoder: the last line is "Unknown error occurred" */
  expect_msg ("Input #0, mov,mp4, from 'a.mp4':\n"
              "[vist#0:0/h264 @ 0x55d] [dec:libopenh264 @ 0x55e] Error while opening decoder: Generic error in an external library\n"
              "[vost#0:0/libx264 @ 0x55f] Error initializing a simple filtergraph\n"
              "Error opening output files: Unknown error occurred\n", NULL,
              "the video decoder failed (libopenh264 decoder unavailable)");
  expect_msg ("[dec:libopenh264] Error while opening decoder\n"
              "Error opening output files: Unknown error occurred\n", NULL,
              "the video decoder failed (libopenh264 decoder unavailable)");
  expect_msg ("Error while opening decoder for input stream #0:0 : Function not implemented\n", NULL,
              "the video decoder failed (decoder unavailable)");
  /* ImageMagick: program prefix and "@ error/..." tail are dropped */
  expect_msg ("magick: Improper image header `/x/a.jpg' @ error/jpeg.c/EmitMessage/232.\n", NULL,
              "Improper image header `/x/a.jpg'");
  expect_msg ("/usr/bin/magick: Not a JPEG file: starts with 0x52 0x49 `/x/a.jpg' @ error/jpeg.c/EmitMessage/232.\n"
              "magick: no images defined `/o/.a.jpg' @ error/convert.c/ConvertImageCommand/3500.\n", NULL,
              "Not a JPEG file: starts with 0x52 0x49 `/x/a.jpg'");
  expect_msg ("magick: unable to open image '/x/a.png': No such file or directory @ error/blob.c/OpenBlob/3601.\n",
              NULL, "unable to open image '/x/a.png': No such file or directory");
  expect_msg ("magick: cache resources exhausted `/x/a.png' @ error/cache.c/OpenPixelCache/4119.\n"
              "magick: no images defined `/o/x.jpg' @ error/convert.c/ConvertImageCommand/3500.\n", NULL,
              "image too large for available memory");
  expect_msg ("av_interleaved_write_frame(): No space left on device\n"
              "Error writing trailer of /o/x.mp4: No space left on device\n", NULL, "disk full");
  expect_msg ("/x/a.mp4: Permission denied\n", NULL, "/x/a.mp4: Permission denied");
  expect_msg ("/x/a.mp3: Invalid data found when processing input\n", NULL,
              "/x/a.mp3: Invalid data found when processing input");
  /* no known cause: the last line that says error/failed/unable, else the last line */
  expect_msg ("starting\nLoad failed: bad thing\nsome trailing note\n", NULL,
              "Load failed: bad thing");
  expect_msg ("first\nsecond line\n\n  \n", NULL, "second line");
  g_assert_null (nfc_failure_message ("", NULL));
  g_assert_null (nfc_failure_message ("  \n\n", NULL));
  g_assert_null (nfc_failure_message (NULL, NULL));
  /* an endless line is cut on a character boundary */
  long_line = g_strnfill (400, 'x');
  got = nfc_failure_message (long_line, NULL);
  g_assert_true (g_str_has_suffix (got, "…"));
  g_assert_cmpuint (strlen (got), <=, 300 + 3);
  g_assert_true (g_utf8_validate (got, -1, NULL));
  g_free (got);
  g_free (long_line);
  {
    GString *s = g_string_new (NULL);

    for (int i = 0; i < 200; i++)
      g_string_append (s, "é");
    got = nfc_failure_message (s->str, NULL);
    g_assert_true (g_utf8_validate (got, -1, NULL));
    g_assert_cmpuint (strlen (got), <=, 300 + 3);
    g_free (got);
    g_string_free (s, TRUE);
  }
}

/* ICO: the 256/48/32/16 entries that do not enlarge the source, plus its own
 * size; at least one entry */
static void
check_ico_sizes (const char *fmt, guint64 w, guint64 h, const char *want)
{
  NfcImageInfo info = { w, h, FALSE, 0, 0 };
  gboolean dec;
  GPtrArray *at = nfc_build_attempts_info (NFC_CAT_IMAGE, fmt, "ico", "/s/a.x", "/o/x.ico", "/w",
                                           w ? &info : NULL, &dec);
  char *def = g_strconcat ("icon:auto-resize=", want, NULL);

  g_assert_nonnull (at);
  g_assert_true (argv_has (((GPtrArray *) at->pdata[0])->pdata[0], def));
  g_free (def);
  g_ptr_array_unref (at);
}

static void
test_ico_sizes (void)
{
  nfc_tools_init ();
  g_free (nfc_tools.magick);
  nfc_tools.magick = g_strdup ("/bin/magick");

  check_ico_sizes ("png", 16, 16, "16");
  check_ico_sizes ("png", 87, 98, "98,48,32,16");
  check_ico_sizes ("png", 98, 87, "98,48,32,16");
  check_ico_sizes ("png", 8, 8, "8");
  check_ico_sizes ("png", 15, 40, "40,32,16");
  check_ico_sizes ("png", 32, 32, "32,16");
  check_ico_sizes ("png", 48, 48, "48,32,16");
  check_ico_sizes ("png", 200, 100, "200,48,32,16");
  check_ico_sizes ("png", 256, 256, "256,48,32,16");
  check_ico_sizes ("png", 400, 100, "256,48,32,16");   /* shrunk to 256 first */
  check_ico_sizes ("png", 4000, 3000, "256,48,32,16");
  check_ico_sizes ("png", 0, 0, "256,48,32,16");       /* size unknown */
  check_ico_sizes ("svg", 16, 16, "256,48,32,16");     /* vector: no enlarging issue */
}

/* `ffmpeg -encoders` lines, as ffmpeg prints them */
#define ENC_HEADER " Encoders:\n V..... = Video\n ------\n"
#define ENC_X264 " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)\n"
#define ENC_OH264 " V....D libopenh264          OpenH264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)\n"
#define ENC_OTHER " V....D h264_vaapi           H.264/AVC (VAAPI) (codec h264)\n A....D aac                  AAC (Advanced Audio Coding)\n"

static gboolean
offers (NfcCategory cat, const char *fmt, const char *key)
{
  GPtrArray *t = nfc_targets_for (cat, fmt);
  gboolean found = FALSE;

  for (guint i = 0; i < t->len; i++)
    found = found || g_str_equal (((const NfcTarget *) t->pdata[i])->key, key);
  g_ptr_array_unref (t);
  return found;
}

static char **
attempt_argv (GPtrArray *at, guint i)
{
  return ((GPtrArray *) at->pdata[i])->pdata[0];
}

/* MP4 and the MKV re-encode: libx264 when ffmpeg has it, libopenh264 (bitrate
 * mode, bitrate from the picture size) otherwise or as the next attempt; MP4
 * and MKV are offered if any H.264 encoder exists. */
static void
test_h264_encoders (void)
{
  NfcImageInfo hd = { 1920, 1080, FALSE, 0, 0 }, hd720 = { 1280, 720, FALSE, 0, 0 };
  NfcImageInfo uhd = { 3840, 2160, FALSE, 0, 0 }, tiny = { 160, 120, FALSE, 0, 0 };
  NfcImageInfo huge = { 7680, 4320, FALSE, 0, 0 };
  GPtrArray *at;
  gboolean dec;

  nfc_tools_init ();
  g_free (nfc_tools.ffmpeg);
  nfc_tools.ffmpeg = g_strdup ("/bin/ffmpeg");

  /* bitrate: 3 bit per pixel per second, 0.5 .. 40 Mbit/s, 1080p when unknown */
  g_assert_cmpuint (nfc_h264_bitrate (&hd), ==, 6220800);
  g_assert_cmpuint (nfc_h264_bitrate (&hd720), ==, 2764800);
  g_assert_cmpuint (nfc_h264_bitrate (&uhd), ==, 24883200);
  g_assert_cmpuint (nfc_h264_bitrate (&huge), ==, 40000000);
  g_assert_cmpuint (nfc_h264_bitrate (&tiny), ==, 500000);
  g_assert_cmpuint (nfc_h264_bitrate (NULL), ==, 6220800);

  /* both: libx264 first, libopenh264 as the next attempt */
  nfc_test_set_encoders (ENC_HEADER ENC_X264 ENC_OH264 ENC_OTHER);
  g_assert_true (offers (NFC_CAT_VIDEO, "mkv", "mp4"));
  g_assert_true (offers (NFC_CAT_VIDEO, "mp4", "mkv"));
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mkv", "mp4", "/s/a.mkv", "/o/x.mp4", "/w", &hd720, &dec);
  g_assert_cmpuint (at->len, ==, 2);
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-c:v"), ==, "libx264");
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-crf"), ==, "18");
  g_assert_false (argv_has (attempt_argv (at, 0), "-b:v"));
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-c:v"), ==, "libopenh264");
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-b:v"), ==, "2764800");
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-rc_mode"), ==, "bitrate");
  g_assert_false (argv_has (attempt_argv (at, 1), "-crf"));
  g_assert_false (argv_has (attempt_argv (at, 1), "-preset"));
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-pix_fmt"), ==, "yuv420p");
  g_assert_true (argv_has (attempt_argv (at, 1), "+faststart"));
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-vf"), ==, "crop=trunc(iw/2)*2:trunc(ih/2)*2");
  g_assert_cmpstr (attempt_argv (at, 1)[g_strv_length (attempt_argv (at, 1)) - 1], ==, "/o/x.mp4");
  g_ptr_array_unref (at);
  /* MKV: stream copy, then x264, then openh264; no faststart in MKV */
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mp4", "mkv", "/s/a.mp4", "/o/x.mkv", "/w", &uhd, &dec);
  g_assert_cmpuint (at->len, ==, 3);
  g_assert_true (argv_has (attempt_argv (at, 0), "copy"));
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-c:v"), ==, "libx264");
  g_assert_cmpstr (argv_after (attempt_argv (at, 2), "-c:v"), ==, "libopenh264");
  g_assert_cmpstr (argv_after (attempt_argv (at, 2), "-b:v"), ==, "24883200");
  g_assert_false (argv_has (attempt_argv (at, 2), "+faststart"));
  g_ptr_array_unref (at);

  /* stock Fedora: libopenh264 only */
  nfc_test_set_encoders (ENC_HEADER ENC_OH264 ENC_OTHER);
  g_assert_true (offers (NFC_CAT_VIDEO, "mkv", "mp4"));
  g_assert_true (offers (NFC_CAT_VIDEO, "mp4", "mkv"));
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mkv", "mp4", "/s/a.mkv", "/o/x.mp4", "/w", &hd, &dec);
  g_assert_cmpuint (at->len, ==, 1);
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-c:v"), ==, "libopenh264");
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-b:v"), ==, "6220800");
  g_ptr_array_unref (at);
  /* no size known: 1080p */
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mkv", "mp4", "/s/a.mkv", "/o/x.mp4", "/w", NULL, &dec);
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-b:v"), ==, "6220800");
  g_ptr_array_unref (at);
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mp4", "mkv", "/s/a.mp4", "/o/x.mkv", "/w", &hd, &dec);
  g_assert_cmpuint (at->len, ==, 2);
  g_assert_true (argv_has (attempt_argv (at, 0), "copy"));
  g_assert_cmpstr (argv_after (attempt_argv (at, 1), "-c:v"), ==, "libopenh264");
  g_ptr_array_unref (at);

  /* libx264 only */
  nfc_test_set_encoders (ENC_HEADER ENC_X264 ENC_OTHER);
  g_assert_true (offers (NFC_CAT_VIDEO, "mkv", "mp4"));
  at = nfc_build_attempts_info (NFC_CAT_VIDEO, "mkv", "mp4", "/s/a.mkv", "/o/x.mp4", "/w", &hd, &dec);
  g_assert_cmpuint (at->len, ==, 1);
  g_assert_cmpstr (argv_after (attempt_argv (at, 0), "-c:v"), ==, "libx264");
  g_ptr_array_unref (at);

  /* no H.264 encoder (a hardware wrapper alone does not count): not offered */
  nfc_test_set_encoders (ENC_HEADER ENC_OTHER);
  g_assert_false (offers (NFC_CAT_VIDEO, "mkv", "mp4"));
  g_assert_false (offers (NFC_CAT_VIDEO, "mp4", "mkv"));
  nfc_test_set_encoders ("");

  /* the ffprobe size probe: absolute path after -i, never an option */
  g_free (nfc_tools.ffprobe);
  nfc_tools.ffprobe = g_strdup ("/bin/ffprobe");
  {
    char **argv = nfc_build_video_probe_argv ("-odd name.mp4");

    g_assert_nonnull (argv);
    g_assert_cmpstr (argv[0], ==, "/bin/ffprobe");
    g_assert_cmpstr (argv_after (argv, "-select_streams"), ==, "v:0");
    g_assert_true (g_str_has_suffix (argv_after (argv, "-i"), "/-odd name.mp4"));
    g_assert_true (g_path_is_absolute (argv_after (argv, "-i")));
    g_strfreev (argv);
    g_free (nfc_tools.ffprobe);
    nfc_tools.ffprobe = NULL;
    g_assert_null (nfc_build_video_probe_argv ("/s/a.mp4"));
  }
}


/* ---- Best == the argv of 729ab89 (before the quality presets existed) ----
 *
 * The strings are what nfc_build_attempts_info produced then (fake tool paths,
 * the "-limit" options removed because they depend on the machine's RAM):
 * attempts separated by " ## ", the steps of an attempt by " && ". Best must
 * never change: it is what existing users get. */
static const struct {
  const char *label;
  NfcCategory cat;
  const char *fmt, *key;
  int enc;          /* 0: libx264, 1: libopenh264, 2: both */
  gboolean hd;      /* 1920x1080 probe info */
  const char *argv;
} golden[] = {
  { "jpg-png", NFC_CAT_IMAGE, "png", "jpg", 0, FALSE,
    "/bin/magick -quiet PNG:/s/a.png[0] -auto-orient -background white -alpha remove -alpha off -quality 99 -sampling-factor 4:4:4 JPEG:/o/x.jpg"
  },
  { "jpg-jpg", NFC_CAT_IMAGE, "jpg", "jpg", 0, FALSE,
    "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -background white -alpha remove -alpha off -quality 95 -sampling-factor 4:4:4 JPEG:/o/x.jpg"
  },
  { "jpg-heic", NFC_CAT_IMAGE, "heic", "jpg", 0, FALSE,
    "/bin/nfc-decode /s/a.heic /w/decoded.png && /bin/magick -quiet PNG:/w/decoded.png[0] -auto-orient -background white -alpha remove -alpha off -quality 95 -sampling-factor 4:4:4 JPEG:/o/x.jpg"
  },
  { "jpg-svg", NFC_CAT_IMAGE, "svg", "jpg", 0, FALSE,
    "/bin/magick -quiet -background none -density 192 MSVG:/s/a.svg[0] -auto-orient -background white -alpha remove -alpha off -quality 99 -sampling-factor 4:4:4 JPEG:/o/x.jpg"
  },
  { "webp-png", NFC_CAT_IMAGE, "png", "webp", 0, FALSE,
    "/bin/magick -quiet PNG:/s/a.png[0] -auto-orient -resize 16383x16383> -define webp:lossless=true -define webp:method=4 WEBP:/o/x.webp"
  },
  { "webp-jpg", NFC_CAT_IMAGE, "jpg", "webp", 0, FALSE,
    "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -resize 16383x16383> -quality 90 -define webp:method=6 -define webp:sharp-yuv=true WEBP:/o/x.webp"
  },
  { "avif-png", NFC_CAT_IMAGE, "png", "avif", 0, FALSE,
    "/bin/magick -quiet PNG:/s/a.png[0] -auto-orient -background white -alpha remove -alpha off PNG:/w/flat.png && /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /w/flat.png -frames:v 1 -vf scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv444p -c:v libaom-av1 -crf 9 -b:v 0 -cpu-used 6 -row-mt 1 -still-picture 1 -colorspace bt709 -color_primaries bt709 -color_range pc -f avif /o/x.avif"
    " ## /bin/magick -quiet PNG:/s/a.png[0] -auto-orient -background white -alpha remove -alpha off PNG:/w/flat.png && /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /w/flat.png -frames:v 1 -vf scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv420p -c:v libaom-av1 -crf 12 -b:v 0 -cpu-used 6 -row-mt 1 -still-picture 1 -colorspace bt709 -color_primaries bt709 -color_range pc -f avif /o/x.avif"
  },
  { "avif-jpg", NFC_CAT_IMAGE, "jpg", "avif", 0, FALSE,
    "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -background white -alpha remove -alpha off PNG:/w/flat.png && /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /w/flat.png -frames:v 1 -vf scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv420p -c:v libaom-av1 -crf 12 -b:v 0 -cpu-used 6 -row-mt 1 -still-picture 1 -colorspace bt709 -color_primaries bt709 -color_range pc -f avif /o/x.avif"
  },
  { "avif-heic", NFC_CAT_IMAGE, "heic", "avif", 0, FALSE,
    "/bin/nfc-decode /s/a.heic /w/decoded.png && /bin/magick -quiet PNG:/w/decoded.png[0] -auto-orient -background white -alpha remove -alpha off PNG:/w/flat.png && /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /w/flat.png -frames:v 1 -vf scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv420p -c:v libaom-av1 -crf 12 -b:v 0 -cpu-used 6 -row-mt 1 -still-picture 1 -colorspace bt709 -color_primaries bt709 -color_range pc -f avif /o/x.avif"
  },
  { "mp4-x264", NFC_CAT_VIDEO, "mkv", "mp4", 0, TRUE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mkv -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4"
  },
  { "webm", NFC_CAT_VIDEO, "mp4", "webm", 0, FALSE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -map 0:a:0? -sn -dn -c:v libvpx-vp9 -crf 28 -b:v 0 -row-mt 1 -deadline good -cpu-used 2 -pix_fmt yuv420p -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:a libopus -b:a 128k /o/x.webm"
  },
  { "gif", NFC_CAT_VIDEO, "mp4", "gif", 0, FALSE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -an -sn -dn -vf fps=15,scale='min(960,iw)':-1:flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle -loop 0 /o/x.gif"
  },
  { "mkv-x264", NFC_CAT_VIDEO, "mp4", "mkv", 0, TRUE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -map 0:a:0? -sn -dn -c copy /o/x.mkv"
    " ## /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k /o/x.mkv"
  },
  { "mp4-oh264", NFC_CAT_VIDEO, "mkv", "mp4", 1, TRUE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mkv -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libopenh264 -rc_mode bitrate -b:v 6220800 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4"
  },
  { "mp4-oh264-noinfo", NFC_CAT_VIDEO, "mkv", "mp4", 1, FALSE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mkv -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libopenh264 -rc_mode bitrate -b:v 6220800 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4"
  },
  { "mp4-both", NFC_CAT_VIDEO, "mkv", "mp4", 2, TRUE,
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mkv -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4"
    " ## /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mkv -map 0:v:0 -map 0:a:0? -sn -dn -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libopenh264 -rc_mode bitrate -b:v 6220800 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4"
  },
};

/* ------------------------------------------------------------------------
 * Quality presets
 * --------------------------------------------------------------------- */

/* attempts as one string: " ## " between attempts, " && " between steps, the
 * "-limit <what> <value>" options removed (they depend on the machine's RAM) */
static char *
attempts_string (GPtrArray *at)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < at->len; i++)
    {
      GPtrArray *a = at->pdata[i];

      if (i)
        g_string_append (s, " ## ");
      for (guint j = 0; j < a->len; j++)
        {
          char **argv = a->pdata[j];

          if (j)
            g_string_append (s, " && ");
          for (guint k = 0; argv[k]; k++)
            {
              if (g_str_equal (argv[k], "-limit"))
                {
                  k += 2;
                  continue;
                }
              g_string_append_printf (s, "%s%s", k ? " " : "", argv[k]);
            }
        }
    }
  return g_string_free (s, FALSE);
}

static void
use_fake_tools (void)
{
  nfc_tools_init ();
  g_free (nfc_tools.magick); g_free (nfc_tools.ffmpeg); g_free (nfc_tools.decode);
  nfc_tools.magick = g_strdup ("/bin/magick");
  nfc_tools.ffmpeg = g_strdup ("/bin/ffmpeg");
  nfc_tools.decode = g_strdup ("/bin/nfc-decode");
}

static void
set_encoders (int enc)
{
  nfc_test_set_encoders (enc == 1 ? ENC_HEADER ENC_OH264 ENC_OTHER
                         : enc == 2 ? ENC_HEADER ENC_X264 ENC_OH264 ENC_OTHER
                         : ENC_HEADER ENC_X264 ENC_OTHER);
}

static GPtrArray *
build (NfcCategory cat, const char *fmt, const char *key, NfcPreset preset, gboolean hd)
{
  NfcImageInfo hd_info = { 1920, 1080, FALSE, 0, 0 };
  char *src = g_strdup_printf ("/s/a.%s", fmt), *out = g_strdup_printf ("/o/x.%s", key);
  gboolean dec;
  GPtrArray *at = nfc_build_attempts_preset (cat, fmt, key, preset, src, out, "/w",
                                             hd ? &hd_info : NULL, &dec);

  g_free (src);
  g_free (out);
  g_assert_nonnull (at);
  return at;
}

/* the argv string of the first attempt */
static char *
build_str (NfcCategory cat, const char *fmt, const char *key, NfcPreset preset, gboolean hd)
{
  GPtrArray *at = build (cat, fmt, key, preset, hd);
  char *s = attempts_string (at);

  g_ptr_array_unref (at);
  return s;
}

/* the last step of attempt i (the ImageMagick / ffmpeg encode command) */
static char **
last_step (GPtrArray *at, guint i)
{
  GPtrArray *a = at->pdata[i];

  return a->pdata[a->len - 1];
}

static gboolean
argv_has_prefix (char **argv, const char *prefix)
{
  for (; *argv; argv++)
    if (g_str_has_prefix (*argv, prefix))
      return TRUE;
  return FALSE;
}

#define BEST NFC_PRESET_BEST
#define HIGH NFC_PRESET_HIGH
#define BAL NFC_PRESET_BALANCED
#define SMALL NFC_PRESET_SMALL

/* Best is exactly what 729ab89 produced, through the old and the new API */
static void
test_best_argv_unchanged (void)
{
  use_fake_tools ();
  for (guint i = 0; i < G_N_ELEMENTS (golden); i++)
    {
      NfcImageInfo hd_info = { 1920, 1080, FALSE, 0, 0 };
      char *src = g_strdup_printf ("/s/a.%s", golden[i].fmt);
      char *out = g_strdup_printf ("/o/x.%s", golden[i].key), *s;
      gboolean dec;
      GPtrArray *at;

      set_encoders (golden[i].enc);
      at = nfc_build_attempts_info (golden[i].cat, golden[i].fmt, golden[i].key, src, out, "/w",
                                    golden[i].hd ? &hd_info : NULL, &dec);
      g_assert_nonnull (at);
      s = attempts_string (at);
      g_assert_cmpstr (s, ==, golden[i].argv);
      g_free (s);
      g_ptr_array_unref (at);

      at = nfc_build_attempts_preset (golden[i].cat, golden[i].fmt, golden[i].key, BEST, src, out,
                                      "/w", golden[i].hd ? &hd_info : NULL, &dec);
      g_assert_nonnull (at);
      s = attempts_string (at);
      g_assert_cmpstr (s, ==, golden[i].argv);
      g_free (s);
      g_ptr_array_unref (at);
      g_free (src);
      g_free (out);
    }
  nfc_test_set_encoders ("");
}

static void
test_preset_names (void)
{
  static const char *const keys[] = { "best", "high", "balanced", "small" };
  static const char *const labels[] = { "Best", "High", "Balanced", "Small" };
  NfcPreset p;

  g_assert_cmpuint (NFC_PRESET_N, ==, 4);
  for (guint i = 0; i < 4; i++)
    {
      g_assert_cmpstr (nfc_preset_key ((NfcPreset) i), ==, keys[i]);
      g_assert_cmpstr (nfc_preset_label ((NfcPreset) i), ==, labels[i]);
      g_assert_true (nfc_preset_from_key (keys[i], &p));
      g_assert_cmpint (p, ==, (int) i);
    }
  g_assert_false (nfc_preset_from_key ("ultra", &p));
  g_assert_false (nfc_preset_from_key (NULL, &p));
  g_assert_false (nfc_preset_from_key ("Best", &p)); /* the key, not the label */

  /* which targets have presets */
  {
    const char *const image_yes[] = { "jpg", "webp", "avif" };
    const char *const image_no[] = { "png", "bmp", "tiff", "ico", "pdf" };
    const char *const video_yes[] = { "mp4", "webm", "gif" };
    const char *const video_no[] = { "mkv", "mp3", "m4a" };
    const char *const audio_no[] = { "mp3", "flac", "wav", "ogg", "opus", "m4a" };
    const char *const doc_no[] = { "pdf", "docx", "odt", "xlsx", "ods", "csv", "pptx", "odp" };

    for (guint i = 0; i < G_N_ELEMENTS (image_yes); i++)
      g_assert_true (nfc_target_has_presets (NFC_CAT_IMAGE, image_yes[i]));
    for (guint i = 0; i < G_N_ELEMENTS (image_no); i++)
      g_assert_false (nfc_target_has_presets (NFC_CAT_IMAGE, image_no[i]));
    for (guint i = 0; i < G_N_ELEMENTS (video_yes); i++)
      g_assert_true (nfc_target_has_presets (NFC_CAT_VIDEO, video_yes[i]));
    for (guint i = 0; i < G_N_ELEMENTS (video_no); i++)
      g_assert_false (nfc_target_has_presets (NFC_CAT_VIDEO, video_no[i]));
    for (guint i = 0; i < G_N_ELEMENTS (audio_no); i++)
      g_assert_false (nfc_target_has_presets (NFC_CAT_AUDIO, audio_no[i]));
    for (guint i = 0; i < G_N_ELEMENTS (doc_no); i++)
      {
        g_assert_false (nfc_target_has_presets (NFC_CAT_DOC, doc_no[i]));
        g_assert_false (nfc_target_has_presets (NFC_CAT_SHEET, doc_no[i]));
        g_assert_false (nfc_target_has_presets (NFC_CAT_SLIDE, doc_no[i]));
      }
    g_assert_false (nfc_target_has_presets (NFC_CAT_IMAGE, NULL));
    /* a format key of the other category is not a target of this one */
    g_assert_false (nfc_target_has_presets (NFC_CAT_IMAGE, "mp4"));
    g_assert_false (nfc_target_has_presets (NFC_CAT_VIDEO, "jpg"));

    /* tooltips: one per preset, starting with its label; Small says what it scales */
    for (guint i = 0; i < G_N_ELEMENTS (image_yes); i++)
      for (guint p2 = 0; p2 < NFC_PRESET_N; p2++)
        {
          const char *tip = nfc_preset_tooltip (NFC_CAT_IMAGE, image_yes[i], (NfcPreset) p2);

          g_assert_true (g_str_has_prefix (tip, nfc_preset_label ((NfcPreset) p2)));
          g_assert_true (p2 != NFC_PRESET_SMALL || strstr (tip, "2560") != NULL);
        }
    g_assert_nonnull (strstr (nfc_preset_tooltip (NFC_CAT_VIDEO, "mp4", SMALL), "1080p"));
    g_assert_nonnull (strstr (nfc_preset_tooltip (NFC_CAT_VIDEO, "webm", SMALL), "1080p"));
    g_assert_nonnull (strstr (nfc_preset_tooltip (NFC_CAT_VIDEO, "gif", SMALL), "480 px"));
    g_assert_nonnull (strstr (nfc_preset_tooltip (NFC_CAT_VIDEO, "gif", HIGH), "720 px"));
    g_assert_cmpstr (nfc_preset_tooltip (NFC_CAT_IMAGE, "png", HIGH), ==, "");
  }
}

static void
test_preset_jpg (void)
{
  static const char *const q_graphics[] = { "99", "95", "85", "75" };
  static const char *const s_graphics[] = { "4:4:4", "4:4:4", "4:4:4", "4:2:0" };
  static const char *const q_photo[] = { "95", "92", "80", "70" };
  static const char *const s_photo[] = { "4:4:4", "4:2:0", "4:2:0", "4:2:0" };

  use_fake_tools ();
  for (int p = 0; p < NFC_PRESET_N; p++)
    for (int photo = 0; photo < 2; photo++)
      {
        GPtrArray *at = build (NFC_CAT_IMAGE, photo ? "jpg" : "png", "jpg", (NfcPreset) p, FALSE);
        char **argv;

        g_assert_cmpuint (at->len, ==, 1);
        argv = last_step (at, 0);
        g_assert_cmpstr (argv_after (argv, "-quality"), ==, (photo ? q_photo : q_graphics)[p]);
        g_assert_cmpstr (argv_after (argv, "-sampling-factor"), ==,
                         (photo ? s_photo : s_graphics)[p]);
        g_assert_cmpint (argv_has (argv, "-interlace"), ==, p != BEST);
        g_assert_cmpint (argv_has (argv, "-resize"), ==, p == SMALL);
        if (p == SMALL)
          g_assert_cmpstr (argv_after (argv, "-resize"), ==, "2560x2560>");
        g_assert_true (argv_has (argv, "-alpha")); /* still flattened onto white */
        g_ptr_array_unref (at);
      }
}

/* does any step of the attempt start with this program/marker? */
static char **
find_step (GPtrArray *attempt, const char *first_arg, const char *needle)
{
  for (guint i = 0; i < attempt->len; i++)
    {
      char **step = attempt->pdata[i];

      if (first_arg != NULL && !g_str_equal (step[0], first_arg))
        continue;
      if (needle == NULL || argv_has (step, needle))
        return step;
    }
  return NULL;
}

static void
test_preset_webp (void)
{
  static const char *const q_photo[] = { "90", "85", "78", "65" };
  static const char *const q_graphics[] = { "100", "95", "88", "75" };
  NfcImageInfo small_info = { 1920, 1080, FALSE, 0, 0 };
  NfcImageInfo big_info = { 6000, 5000, FALSE, 0, 0 }; /* 30 MP */
  NfcImageInfo edge_info = { 5000, 4999, FALSE, 0, 0 }; /* just below 25 MP */
  NfcImageInfo edge2_info = { 5000, 5000, FALSE, 0, 0 }; /* exactly 25 MP */

  use_fake_tools ();
  for (int p = 0; p < NFC_PRESET_N; p++)
    {
      GPtrArray *at = build (NFC_CAT_IMAGE, "jpg", "webp", (NfcPreset) p, FALSE);
      char **argv = last_step (at, 0);

      /* photos: lossy, lower quality per preset, method 6; one encode only.
       * (webp:sharp-yuv is not an ImageMagick option and does nothing: Best's
       * argv keeps it; the graphics presets use webp:use-sharp-yuv) */
      g_assert_cmpuint (at->len, ==, 1);
      g_assert_cmpuint (((GPtrArray *) at->pdata[0])->len, ==, 1);
      g_assert_cmpstr (argv_after (argv, "-quality"), ==, q_photo[p]);
      g_assert_true (argv_has (argv, "webp:method=6"));
      g_assert_true (argv_has (argv, "webp:sharp-yuv=true"));
      g_assert_false (argv_has (argv, "webp:lossless=true"));
      g_assert_cmpstr (argv_after (argv, "-resize"), ==, p == SMALL ? "2560x2560>" : "16383x16383>");
      g_ptr_array_unref (at);
    }

  /* graphics, Best: lossless method 4, one encode, as always */
  {
    GPtrArray *at = build (NFC_CAT_IMAGE, "png", "webp", BEST, TRUE);
    char **argv = last_step (at, 0);

    g_assert_cmpuint (at->len, ==, 1);
    g_assert_cmpuint (((GPtrArray *) at->pdata[0])->len, ==, 1);
    g_assert_true (argv_has (argv, "webp:lossless=true"));
    g_assert_true (argv_has (argv, "webp:method=4"));
    g_assert_false (argv_has (argv, "-quality"));
    g_assert_false (argv_has (argv, "-depth"));
    g_assert_false (argv_has (argv, "-colors"));
    g_ptr_array_unref (at);
  }

  /* graphics, High/Balanced/Small: a lossy encode (q95/88/75, method 6, sharp YUV,
   * alpha q100) AND the lossless one; the smaller file stays. Attempt 1 is
   * [lossless -> out.alt, lossy -> out, keep the smallest of out and out.alt],
   * attempt 2 (if attempt 1 fails) the lossy encode alone. No posterising. */
  for (int p = HIGH; p < NFC_PRESET_N; p++)
    {
      NfcImageInfo *infos[] = { &small_info, &edge_info };

      for (guint k = 0; k < G_N_ELEMENTS (infos); k++)
        {
          NfcImageInfo info = *infos[k];
          char *src = g_strdup ("/s/a.png"), *out = g_strdup ("/o/x.webp");
          gboolean dec;
          GPtrArray *at = nfc_build_attempts_preset (NFC_CAT_IMAGE, "png", "webp", (NfcPreset) p,
                                                     src, out, "/w", &info, &dec);
          GPtrArray *a0, *a1;
          char **lossless, **lossy, **keep;

          g_assert_nonnull (at);
          g_assert_cmpuint (at->len, ==, 2);
          a0 = at->pdata[0];
          a1 = at->pdata[1];
          g_assert_cmpuint (a0->len, ==, 3);
          g_assert_cmpuint (a1->len, ==, 1);
          lossless = a0->pdata[0];
          lossy = a0->pdata[1];
          keep = a0->pdata[2];

          g_assert_true (argv_has (lossless, "webp:lossless=true"));
          g_assert_true (argv_has (lossless, "webp:method=4"));
          g_assert_false (argv_has (lossless, "-quality"));
          g_assert_cmpstr (lossless[g_strv_length (lossless) - 1], ==, "WEBP:/o/x.webp.alt");

          g_assert_cmpstr (argv_after (lossy, "-quality"), ==, q_graphics[p]);
          g_assert_true (argv_has (lossy, "webp:method=6"));
          g_assert_true (argv_has (lossy, "webp:use-sharp-yuv=true"));
          g_assert_true (argv_has (lossy, "webp:alpha-quality=100"));
          g_assert_false (argv_has (lossy, "webp:lossless=true"));
          g_assert_cmpstr (lossy[g_strv_length (lossy) - 1], ==, "WEBP:/o/x.webp");
          for (guint i = 0; i < 2; i++)
            {
              char **v = i ? lossy : lossless;

              /* no posterising any more, and the same resize for both candidates */
              g_assert_false (argv_has (v, "-depth"));
              g_assert_false (argv_has (v, "-colors"));
              g_assert_false (argv_has (v, "+dither"));
              g_assert_cmpstr (argv_after (v, "-resize"), ==,
                               p == SMALL ? "2560x2560>" : "16383x16383>");
            }

          g_assert_cmpstr (keep[0], ==, NFC_STEP_KEEP_SMALLEST);
          g_assert_cmpstr (keep[1], ==, "/o/x.webp");
          g_assert_cmpstr (keep[2], ==, "/o/x.webp.alt");
          g_assert_null (keep[3]);

          /* the fallback: the lossy encode alone */
          g_assert_cmpstr (argv_after (a1->pdata[0], "-quality"), ==, q_graphics[p]);
          g_assert_false (find_step (a1, NFC_STEP_KEEP_SMALLEST, NULL) != NULL);
          g_ptr_array_unref (at);
          g_free (src);
          g_free (out);
        }

      /* 25 MP and up, or a size nobody knows: the lossy encode alone */
      {
        NfcImageInfo *lossy_only[] = { &big_info, &edge2_info, NULL };

        for (guint k = 0; k < G_N_ELEMENTS (lossy_only); k++)
          {
            gboolean dec;
            GPtrArray *at = nfc_build_attempts_preset (NFC_CAT_IMAGE, "png", "webp", (NfcPreset) p,
                                                       "/s/a.png", "/o/x.webp", "/w",
                                                       lossy_only[k], &dec);
            char **argv = last_step (at, 0);

            g_assert_cmpuint (at->len, ==, 1);
            g_assert_cmpuint (((GPtrArray *) at->pdata[0])->len, ==, 1);
            g_assert_cmpstr (argv_after (argv, "-quality"), ==, q_graphics[p]);
            g_assert_false (argv_has (argv, "webp:lossless=true"));
            g_ptr_array_unref (at);
          }
      }
    }
}

/* nfc_is_smaller: the one rule of "keep only if smaller" */
static void
test_is_smaller (void)
{
  g_assert_true (nfc_is_smaller (99, 100));
  g_assert_true (nfc_is_smaller (1, 2));
  g_assert_false (nfc_is_smaller (100, 100)); /* equal is not smaller: discarded */
  g_assert_false (nfc_is_smaller (101, 100));
  g_assert_false (nfc_is_smaller (0 + 5, 0));
  g_assert_false (nfc_is_smaller (-1, 100)); /* unknown sizes never win */
  g_assert_false (nfc_is_smaller (10, -1));
}

/* Compress targets: which source formats, which key/extension */
static void
test_compress_targets (void)
{
  static const struct { NfcCategory cat; const char *fmt, *key, *ext; NfcCategory tcat; } yes[] = {
    { NFC_CAT_IMAGE, "jpg",  "jpg",  "jpg",  NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "webp", "webp", "webp", NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "avif", "avif", "avif", NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "gif",  "gif",  "gif",  NFC_CAT_VIDEO }, /* the GIF presets are the video ones */
    { NFC_CAT_VIDEO, "mp4",  "mp4",  "mp4",  NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "m4v",  "mp4",  "mp4",  NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "webm", "webm", "webm", NFC_CAT_VIDEO },
  };
  static const struct { NfcCategory cat; const char *fmt; } no[] = {
    { NFC_CAT_IMAGE, "png" }, { NFC_CAT_IMAGE, "bmp" }, { NFC_CAT_IMAGE, "tiff" },
    { NFC_CAT_IMAGE, "heic" }, { NFC_CAT_IMAGE, "svg" }, { NFC_CAT_IMAGE, "ico" },
    { NFC_CAT_VIDEO, "mkv" }, { NFC_CAT_VIDEO, "mov" }, { NFC_CAT_VIDEO, "avi" },
    { NFC_CAT_AUDIO, "mp3" }, { NFC_CAT_AUDIO, "flac" }, { NFC_CAT_AUDIO, "m4a" },
    { NFC_CAT_DOC, "docx" }, { NFC_CAT_SHEET, "csv" }, { NFC_CAT_SLIDE, "pptx" },
    { NFC_CAT_VIDEO, "jpg" }, { NFC_CAT_IMAGE, "mp4" }, /* the wrong category */
  };
  NfcFormat f;

  for (guint i = 0; i < G_N_ELEMENTS (yes); i++)
    {
      NfcCompressTarget t;

      g_assert_true (nfc_compress_target (yes[i].cat, yes[i].fmt, &t));
      g_assert_cmpstr (t.key, ==, yes[i].key);
      g_assert_cmpstr (t.ext, ==, yes[i].ext);
      g_assert_cmpint (t.cat, ==, yes[i].tcat);
      g_assert_true (nfc_target_has_presets (t.cat, t.key));
    }
  for (guint i = 0; i < G_N_ELEMENTS (no); i++)
    g_assert_false (nfc_compress_target (no[i].cat, no[i].fmt, NULL));
  g_assert_false (nfc_compress_target (NFC_CAT_IMAGE, NULL, NULL));
  /* .jpeg, .jpe and .m4v files classify to the keys above */
  g_assert_true (nfc_classify ("/x/a.JPEG", NULL, &f));
  g_assert_true (nfc_compress_target (f.cat, f.fmt, NULL));
  g_assert_true (nfc_classify ("/x/a.m4v", NULL, &f));
  g_assert_true (nfc_compress_target (f.cat, f.fmt, NULL));
  g_assert_cmpstr (nfc_compress_tooltip (HIGH), !=, "");
  g_assert_true (g_str_has_prefix (nfc_compress_tooltip (HIGH), "High"));
  g_assert_true (g_str_has_prefix (nfc_compress_tooltip (BAL), "Balanced"));
  g_assert_nonnull (strstr (nfc_compress_tooltip (SMALL), "2560"));
  g_assert_cmpstr (nfc_compress_tooltip (BEST), ==, "");
}

/* what Compress builds: the preset's argv for the source's own format */
static void
test_compress_argv (void)
{
  use_fake_tools ();
  set_encoders (0);
  /* a JPG compressed: the photo table of JPG (q92/80/70) */
  {
    GPtrArray *at = build (NFC_CAT_IMAGE, "jpg", "jpg", HIGH, FALSE);

    g_assert_cmpstr (argv_after (last_step (at, 0), "-quality"), ==, "92");
    g_ptr_array_unref (at);
  }
  /* an MP4 with libx264, a WebM with VP9, a GIF with the GIF palette chain */
  {
    GPtrArray *at = build (NFC_CAT_VIDEO, "mp4", "mp4", BAL, FALSE);

    g_assert_cmpstr (argv_after (last_step (at, 0), "-crf"), ==, "28");
    g_assert_true (argv_has (last_step (at, 0), "libx264"));
    g_ptr_array_unref (at);
    at = build (NFC_CAT_VIDEO, "webm", "webm", BAL, FALSE);
    g_assert_cmpstr (argv_after (last_step (at, 0), "-crf"), ==, "44");
    g_assert_true (argv_has (last_step (at, 0), "libvpx-vp9"));
    g_ptr_array_unref (at);
    at = build (NFC_CAT_VIDEO, "gif", "gif", SMALL, FALSE);
    g_assert_nonnull (strstr (argv_after (last_step (at, 0), "-vf"), "min(480,iw)"));
    g_ptr_array_unref (at);
  }
  nfc_test_set_encoders ("");
}

static void
test_preset_avif (void)
{
  static const char *const crf_graphics[] = { "9", "18", "26", "34" };
  static const char *const crf_photo[] = { "12", "20", "28", "36" };

  use_fake_tools ();
  for (int p = 0; p < NFC_PRESET_N; p++)
    {
      GPtrArray *at = build (NFC_CAT_IMAGE, "png", "avif", (NfcPreset) p, FALSE);
      GPtrArray *a0, *a1;
      char **flat, **enc;

      /* graphics: 4:4:4 at the graphics crf, then the photo settings as the fallback */
      g_assert_cmpuint (at->len, ==, 2);
      a0 = at->pdata[0];
      a1 = at->pdata[1];
      g_assert_cmpuint (a0->len, ==, 2);
      flat = a0->pdata[0];
      enc = a0->pdata[1];
      g_assert_cmpstr (argv_after (enc, "-crf"), ==, crf_graphics[p]);
      g_assert_true (argv_has_prefix (enc, "scale=out_color_matrix=bt709"));
      g_assert_nonnull (strstr (argv_after (enc, "-vf"), "format=yuv444p"));
      g_assert_cmpint (argv_has (flat, "-resize"), ==, p == SMALL);
      if (p == SMALL)
        g_assert_cmpstr (argv_after (flat, "-resize"), ==, "2560x2560>");
      enc = ((GPtrArray *) a1)->pdata[1];
      g_assert_cmpstr (argv_after (enc, "-crf"), ==, crf_photo[p]);
      g_assert_nonnull (strstr (argv_after (enc, "-vf"), "format=yuv420p"));
      g_ptr_array_unref (at);

      at = build (NFC_CAT_IMAGE, "jpg", "avif", (NfcPreset) p, FALSE);
      g_assert_cmpuint (at->len, ==, 1);
      a0 = at->pdata[0];
      enc = a0->pdata[1];
      g_assert_cmpstr (argv_after (enc, "-crf"), ==, crf_photo[p]);
      g_assert_nonnull (strstr (argv_after (enc, "-vf"), "format=yuv420p"));
      g_assert_cmpint (argv_has (a0->pdata[0], "-resize"), ==, p == SMALL);
      g_ptr_array_unref (at);
    }
}

static void
test_preset_video (void)
{
  static const char *const x264_crf[] = { "18", "23", "28", "34" };
  static const char *const vp9_crf[] = { "28", "36", "44", "52" };
  static const char *const oh264_bps[] = { "6220800", "3732480", "2488320", "1555200" };
  static const struct { const char *fps, *width, *colors; } gif[] = {
    { "fps=15,", "min(960,iw)", NULL }, { "fps=15,", "min(720,iw)", NULL },
    { "fps=12,", "min(640,iw)", NULL }, { "fps=10,", "min(480,iw)", "max_colors=128" },
  };

  use_fake_tools ();
  for (int p = 0; p < NFC_PRESET_N; p++)
    {
      GPtrArray *at;
      char **argv;
      const char *vf;

      /* MP4, libx264 */
      set_encoders (0);
      at = build (NFC_CAT_VIDEO, "mkv", "mp4", (NfcPreset) p, TRUE);
      g_assert_cmpuint (at->len, ==, 1);
      argv = last_step (at, 0);
      g_assert_cmpstr (argv_after (argv, "-c:v"), ==, "libx264");
      g_assert_cmpstr (argv_after (argv, "-crf"), ==, x264_crf[p]);
      g_assert_cmpstr (argv_after (argv, "-preset"), ==, p == SMALL ? "medium" : "slow");
      vf = argv_after (argv, "-vf");
      g_assert_true (g_str_has_prefix (vf, "crop=trunc(iw/2)*2:trunc(ih/2)*2"));
      g_assert_cmpint (strstr (vf, "scale=") != NULL, ==, p == SMALL);
      g_assert_cmpstr (argv_after (argv, "-pix_fmt"), ==, "yuv420p");
      g_assert_cmpstr (argv_after (argv, "-b:a"), ==, "192k");
      g_assert_true (argv_has (argv, "+faststart"));
      g_ptr_array_unref (at);

      /* MP4, libopenh264: the bitrate scales with the preset (100/60/40/25 %) */
      set_encoders (1);
      at = build (NFC_CAT_VIDEO, "mkv", "mp4", (NfcPreset) p, TRUE);
      g_assert_cmpuint (at->len, ==, 1);
      argv = last_step (at, 0);
      g_assert_cmpstr (argv_after (argv, "-c:v"), ==, "libopenh264");
      g_assert_cmpstr (argv_after (argv, "-b:v"), ==, oh264_bps[p]);
      g_assert_cmpint (strstr (argv_after (argv, "-vf"), "scale=") != NULL, ==, p == SMALL);
      g_ptr_array_unref (at);
      /* ... never below the 0.5 Mbit/s floor */
      {
        NfcImageInfo tiny = { 160, 120, FALSE, 0, 0 };
        gboolean dec;

        at = nfc_build_attempts_preset (NFC_CAT_VIDEO, "mkv", "mp4", SMALL, "/s/a.mkv", "/o/x.mp4",
                                        "/w", &tiny, &dec);
        g_assert_cmpstr (argv_after (last_step (at, 0), "-b:v"), ==, "500000");
        g_ptr_array_unref (at);
      }

      /* both encoders: two attempts, each at the preset */
      set_encoders (2);
      at = build (NFC_CAT_VIDEO, "mkv", "mp4", (NfcPreset) p, TRUE);
      g_assert_cmpuint (at->len, ==, 2);
      g_assert_cmpstr (argv_after (last_step (at, 0), "-crf"), ==, x264_crf[p]);
      g_assert_cmpstr (argv_after (last_step (at, 1), "-b:v"), ==, oh264_bps[p]);
      g_ptr_array_unref (at);

      /* WebM: VP9 crf; Small shrinks above 1080p */
      at = build (NFC_CAT_VIDEO, "mp4", "webm", (NfcPreset) p, FALSE);
      argv = last_step (at, 0);
      g_assert_cmpstr (argv_after (argv, "-c:v"), ==, "libvpx-vp9");
      g_assert_cmpstr (argv_after (argv, "-crf"), ==, vp9_crf[p]);
      g_assert_cmpstr (argv_after (argv, "-deadline"), ==, "good");
      g_assert_cmpint (strstr (argv_after (argv, "-vf"), "scale=") != NULL, ==, p == SMALL);
      g_assert_cmpstr (argv_after (argv, "-c:a"), ==, "libopus");
      g_ptr_array_unref (at);

      /* GIF: width cap, frame rate, palette size */
      at = build (NFC_CAT_VIDEO, "mp4", "gif", (NfcPreset) p, FALSE);
      vf = argv_after (last_step (at, 0), "-vf");
      g_assert_true (g_str_has_prefix (vf, gif[p].fps));
      g_assert_nonnull (strstr (vf, gif[p].width));
      g_assert_cmpint (strstr (vf, "max_colors=128") != NULL, ==, gif[p].colors != NULL);
      g_assert_nonnull (strstr (vf, "palettegen=stats_mode=diff"));
      g_assert_nonnull (strstr (vf, "paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle"));
      g_ptr_array_unref (at);
    }
  nfc_test_set_encoders ("");
}

/* Every preset of a preset-capable target gives a different command, and
 * targets without presets ignore the preset altogether (one click, one result) */
static void
test_preset_distinct_and_ignored (void)
{
  static const struct { NfcCategory cat; const char *fmt, *key; } with[] = {
    { NFC_CAT_IMAGE, "png", "jpg" }, { NFC_CAT_IMAGE, "jpg", "jpg" },
    { NFC_CAT_IMAGE, "png", "webp" }, { NFC_CAT_IMAGE, "jpg", "webp" },
    { NFC_CAT_IMAGE, "png", "avif" }, { NFC_CAT_IMAGE, "jpg", "avif" },
    { NFC_CAT_VIDEO, "mkv", "mp4" }, { NFC_CAT_VIDEO, "mp4", "webm" },
    { NFC_CAT_VIDEO, "mp4", "gif" },
  };
  static const struct { NfcCategory cat; const char *fmt, *key; } without[] = {
    { NFC_CAT_IMAGE, "jpg", "png" }, { NFC_CAT_IMAGE, "png", "bmp" },
    { NFC_CAT_IMAGE, "png", "tiff" }, { NFC_CAT_IMAGE, "png", "ico" },
    { NFC_CAT_IMAGE, "png", "pdf" }, { NFC_CAT_VIDEO, "mp4", "mkv" },
    { NFC_CAT_VIDEO, "mp4", "mp3" }, { NFC_CAT_VIDEO, "mp4", "m4a" },
    { NFC_CAT_AUDIO, "wav", "mp3" }, { NFC_CAT_AUDIO, "wav", "flac" },
    { NFC_CAT_AUDIO, "mp3", "wav" }, { NFC_CAT_AUDIO, "wav", "ogg" },
    { NFC_CAT_AUDIO, "wav", "opus" }, { NFC_CAT_AUDIO, "wav", "m4a" },
  };

  use_fake_tools ();
  set_encoders (0);
  for (guint i = 0; i < G_N_ELEMENTS (with); i++)
    {
      char *s[NFC_PRESET_N];

      for (int p = 0; p < NFC_PRESET_N; p++)
        s[p] = build_str (with[i].cat, with[i].fmt, with[i].key, (NfcPreset) p, TRUE);
      for (int p = 0; p < NFC_PRESET_N; p++)
        for (int q = p + 1; q < NFC_PRESET_N; q++)
          g_assert_cmpstr (s[p], !=, s[q]);
      for (int p = 0; p < NFC_PRESET_N; p++)
        g_free (s[p]);
    }
  for (guint i = 0; i < G_N_ELEMENTS (without); i++)
    {
      char *best = build_str (without[i].cat, without[i].fmt, without[i].key, BEST, TRUE);

      for (int p = 1; p < NFC_PRESET_N; p++)
        {
          char *s = build_str (without[i].cat, without[i].fmt, without[i].key, (NfcPreset) p, TRUE);

          g_assert_cmpstr (s, ==, best);
          g_free (s);
        }
      g_free (best);
    }
  /* an out-of-range preset is Best */
  {
    char *best = build_str (NFC_CAT_IMAGE, "png", "jpg", BEST, FALSE);
    char *bad = build_str (NFC_CAT_IMAGE, "png", "jpg", (NfcPreset) 99, FALSE);

    g_assert_cmpstr (best, ==, bad);
    g_free (best);
    g_free (bad);
  }
  /* the MKV re-encode fallback stays at Best's quality for every preset */
  {
    char *best = build_str (NFC_CAT_VIDEO, "mp4", "mkv", BEST, TRUE);
    char *small = build_str (NFC_CAT_VIDEO, "mp4", "mkv", SMALL, TRUE);

    g_assert_cmpstr (best, ==, small);
    g_assert_nonnull (strstr (best, "-crf 18"));
    g_free (best);
    g_free (small);
  }
  nfc_test_set_encoders ("");
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
  g_test_add_func ("/core/ico-sizes", test_ico_sizes);
  g_test_add_func ("/core/h264-encoders", test_h264_encoders); /* last: fakes the encoder probe */
  g_test_add_func ("/core/preset-names", test_preset_names);
  g_test_add_func ("/core/best-argv-unchanged", test_best_argv_unchanged);
  g_test_add_func ("/core/preset-jpg", test_preset_jpg);
  g_test_add_func ("/core/preset-webp", test_preset_webp);
  g_test_add_func ("/core/is-smaller", test_is_smaller);
  g_test_add_func ("/core/compress-targets", test_compress_targets);
  g_test_add_func ("/core/compress-argv", test_compress_argv);
  g_test_add_func ("/core/preset-avif", test_preset_avif);
  g_test_add_func ("/core/preset-video", test_preset_video);
  g_test_add_func ("/core/preset-distinct-and-ignored", test_preset_distinct_and_ignored);
  g_test_add_func ("/core/failure-message", test_failure_message);
  return g_test_run ();
}
