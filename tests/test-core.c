/* Unit tests for the pure logic: classification, argv builders, naming. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "nfc-core.h"
#include "nfc-util.h"
#include "nfc-header.h"
#include "header-fixtures.h"

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
  g_assert_true (argv_has (argv, "webp:use-sharp-yuv=true"));
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
  g_assert_nonnull (strstr (argv_after (argv, "-vf"), "fps=fps='min(source_fps,15)',scale='min(960,iw)'"));
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
    "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -resize 16383x16383> -quality 90 -define webp:method=6 -define webp:use-sharp-yuv=true WEBP:/o/x.webp"
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
    "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -an -sn -dn -vf fps=fps='min(source_fps,15)',scale='min(960,iw)':-1:flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle -loop 0 /o/x.gif"
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
       * (sharp YUV is webp:use-sharp-yuv: the former webp:sharp-yuv was not an
       * ImageMagick option and did nothing) */
      g_assert_cmpuint (at->len, ==, 1);
      g_assert_cmpuint (((GPtrArray *) at->pdata[0])->len, ==, 1);
      g_assert_cmpstr (argv_after (argv, "-quality"), ==, q_photo[p]);
      g_assert_true (argv_has (argv, "webp:method=6"));
      g_assert_true (argv_has (argv, "webp:use-sharp-yuv=true"));
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
    { "fps=fps='min(source_fps,15)',", "min(960,iw)", NULL }, { "fps=fps='min(source_fps,15)',", "min(720,iw)", NULL },
    { "fps=fps='min(source_fps,12)',", "min(640,iw)", NULL }, { "fps=fps='min(source_fps,10)',", "min(480,iw)", "max_colors=128" },
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

/* ------------------------------------------------------------------------
 * Image headers (nfc-header.h)
 * --------------------------------------------------------------------- */

static void
put_be16 (guint8 *p, guint v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void
put_be32 (guint8 *p, guint32 v) { put_be16 (p, v >> 16); put_be16 (p + 2, v & 0xffff); }
static void
put_le16 (guint8 *p, guint v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void
put_le32 (guint8 *p, guint32 v) { put_le16 (p, v & 0xffff); put_le16 (p + 2, v >> 16); }

/* parse a copy of data[0..len) held in an allocation of EXACTLY len bytes, so
 * that ASan sees any read beyond it */
static gboolean
parse_exact (const char *fmt, const guint8 *data, gsize len, guint32 *w, guint32 *h)
{
  guint8 *copy = g_malloc (len > 0 ? len : 1); /* exact: no slack byte to hide an over-read */
  gboolean ok;

  if (len > 0)
    memcpy (copy, data, len);
  ok = nfc_header_parse (fmt, copy, len, w, h);
  g_free (copy);
  return ok;
}

/* a JPEG made by hand: SOI, the given APP segments (n_app of seg bytes of
 * payload each), then an SOF0 of w x h and an SOS */
static GByteArray *
make_jpeg (guint n_app, guint seg, guint w, guint h)
{
  GByteArray *a = g_byte_array_new ();
  guint8 b[16];

  b[0] = 0xff; b[1] = 0xd8;
  g_byte_array_append (a, b, 2);
  for (guint i = 0; i < n_app; i++)
    {
      guint8 *payload = g_malloc0 (seg);

      b[0] = 0xff; b[1] = 0xe1; put_be16 (b + 2, seg + 2);
      g_byte_array_append (a, b, 4);
      g_byte_array_append (a, payload, seg);
      g_free (payload);
    }
  b[0] = 0xff; b[1] = 0xc0; put_be16 (b + 2, 17); b[4] = 8;
  put_be16 (b + 5, h); put_be16 (b + 7, w); b[9] = 3;
  g_byte_array_append (a, b, 10);
  {
    guint8 comps[9] = { 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };

    g_byte_array_append (a, comps, 9);
  }
  b[0] = 0xff; b[1] = 0xda; put_be16 (b + 2, 12);
  g_byte_array_append (a, b, 4);
  return a;
}
static guint32
be32_at (const guint8 *p)
{
  return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}

/* the first occurrence of the 4 characters cc in data (a box type), or NULL */
static guint8 *
find4 (const guint8 *data, gsize len, const char *cc)
{
  for (gsize i = 0; i + 4 <= len; i++)
    if (memcmp (data + i, cc, 4) == 0)
      return (guint8 *) data + i;
  return NULL;
}

/* AVIF / HEIC / HEIF (ISO base media boxes) made by hand */
static void
test_header_isobmff_handmade (void)
{
  guint32 w = 0, h = 0;
  GByteArray *a;
  HfHeif o;
  static const char *const brands[] = { "avif", "avis", "heic", "heix", "hevc", "heim", "heis",
                                        "mif1", "msf1" };

  /* every accepted major brand, with either format key: the primary item's ispe */
  for (guint i = 0; i < G_N_ELEMENTS (brands); i++)
    {
      HfHeif ob = HF_HEIF_DEFAULT (brands[i], 4032, 3024);

      a = hf_make_heif (&ob);
      g_assert_true (parse_exact ("avif", a->data, a->len, &w, &h));
      g_assert_cmpuint (w, ==, 4032); g_assert_cmpuint (h, ==, 3024);
      g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
      g_assert_cmpuint (w, ==, 4032); g_assert_cmpuint (h, ==, 3024);
      g_byte_array_free (a, TRUE);
    }
  /* an unknown major brand is fine when a compatible brand is known (mif1 is in the list),
   * and an unknown set of brands is not (the file is something else, an MP4) */
  o = (HfHeif) HF_HEIF_DEFAULT ("isom", 640, 480);
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("avif", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 640);
  memcpy (a->data + 8, "isom", 4);
  memcpy (a->data + 16, "iso2", 4);
  memcpy (a->data + 20, "iso2", 4);
  g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);

  /* several ispe: the primary item's, not the largest (a bigger one belongs to
   * another item) nor the first (a thumbnail comes first in ipco) */
  o = (HfHeif) HF_HEIF_DEFAULT ("heic", 3000, 2000);
  o.decoy = TRUE;
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 3000); g_assert_cmpuint (h, ==, 2000);
  g_byte_array_free (a, TRUE);
  /* ... also with the wide ipma (version 1, 32-bit ids, 15-bit indices) */
  o.wide_ipma = TRUE;
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 3000); g_assert_cmpuint (h, ==, 2000);
  g_byte_array_free (a, TRUE);
  /* no pitm: the largest ispe (the decoy) */
  o.wide_ipma = FALSE;
  o.pitm = FALSE;
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 6000); g_assert_cmpuint (h, ==, 4000);
  g_byte_array_free (a, TRUE);
  /* a pitm naming an item that is not in ipma: the largest too */
  o.pitm = TRUE;
  a = hf_make_heif (&o);
  {
    /* pitm payload: version/flags (4), item id (2) = 1: make it 9 */
    guint8 *pitm = (guint8 *) find4 (a->data, a->len, "pitm");

    g_assert_nonnull (pitm);
    pitm[4 + 4 + 1] = 9;
    g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
    g_assert_cmpuint (w, ==, 6000); g_assert_cmpuint (h, ==, 4000);
  }
  g_byte_array_free (a, TRUE);

  /* irot: 90 and 270 degrees swap, 0 and 180 do not (a portrait HEIC) */
  for (int r = 0; r < 4; r++)
    {
      o = (HfHeif) HF_HEIF_DEFAULT ("heic", 4032, 3024);
      o.irot = r;
      a = hf_make_heif (&o);
      g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
      g_assert_cmpuint (w, ==, (r & 1) ? 3024u : 4032u);
      g_assert_cmpuint (h, ==, (r & 1) ? 4032u : 3024u);
      g_byte_array_free (a, TRUE);
    }
  /* clap crops (applied before irot, in the order listed), a nonsense one is ignored */
  o = (HfHeif) HF_HEIF_DEFAULT ("heic", 4032, 3024);
  o.clap = TRUE; o.clap_w = 2268; o.clap_h = 3024; o.irot = 1;
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 3024); g_assert_cmpuint (h, ==, 2268);
  g_byte_array_free (a, TRUE);
  o.clap_w = 0;
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("heic", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 3024); g_assert_cmpuint (h, ==, 4032);
  g_byte_array_free (a, TRUE);

  /* the biggest sizes: 31 bits are valid, 2^31 and 0 are not */
  o = (HfHeif) HF_HEIF_DEFAULT ("avif", 0x7fffffff, 1);
  a = hf_make_heif (&o);
  g_assert_true (parse_exact ("avif", a->data, a->len, &w, &h));
  g_assert_cmpuint (w, ==, 0x7fffffff);
  g_byte_array_free (a, TRUE);
  o.w = 0x80000000u;
  a = hf_make_heif (&o);
  g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);
  o.w = 0;
  a = hf_make_heif (&o);
  g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);

  /* box sizes: a largesize (size 1, then 64 bits) for meta; size 0 (to the end)
   * for the last box; sizes below 8 (or 16 with a largesize), and sizes beyond
   * the buffer, are refused */
  o = (HfHeif) HF_HEIF_DEFAULT ("avif", 321, 123);
  a = hf_make_heif (&o);
  {
    guint8 *meta = (guint8 *) find4 (a->data, a->len, "meta") - 4;
    gsize meta_off = (gsize) (meta - a->data);
    guint32 msize = be32_at (meta);
    GByteArray *big = g_byte_array_new ();
    gsize tail = a->len - meta_off - msize;

    g_byte_array_append (big, a->data, meta_off);
    hf_u32 (big, 1);
    hf_cc (big, "meta");
    hf_u32 (big, 0); hf_u32 (big, msize + 8); /* 64-bit largesize, 8 more for itself */
    g_byte_array_append (big, a->data + meta_off + 8, msize - 8);
    g_byte_array_append (big, a->data + meta_off + msize, tail);
    g_assert_true (parse_exact ("avif", big->data, big->len, &w, &h));
    g_assert_cmpuint (w, ==, 321); g_assert_cmpuint (h, ==, 123);
    memset (big->data + meta_off + 8, 0, 8);
    big->data[meta_off + 15] = 15; /* largesize < 16 */
    g_assert_false (parse_exact ("avif", big->data, big->len, &w, &h));
    memset (big->data + meta_off + 8, 0xff, 8); /* 2^64 - 1: far beyond the buffer */
    g_assert_false (parse_exact ("avif", big->data, big->len, &w, &h));
    g_byte_array_free (big, TRUE);

    /* meta with size 0: to the end of the file (mdat is then inside it, as junk) */
    memset (meta, 0, 4);
    g_assert_true (parse_exact ("avif", a->data, a->len, &w, &h));
    g_assert_cmpuint (w, ==, 321);
    meta[3] = 7; /* size 7 < 8 */
    g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
    meta[3] = 1; /* size 1 with its largesize fields being "meta" bytes */
    parse_exact ("avif", a->data, a->len, &w, &h);
    meta[0] = 0xff; meta[1] = 0xff; meta[2] = 0xff; meta[3] = 0xff; /* beyond the buffer */
    g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
    /* a meta that is cut off by the end of the buffer is not read */
    g_assert_false (parse_exact ("avif", a->data, meta_off + msize - 1, &w, &h));
    g_assert_false (parse_exact ("avif", a->data, 12, &w, &h)); /* ftyp cut */
  }
  g_byte_array_free (a, TRUE);
  /* the first box must be ftyp; a PNG or JPEG with the key is no match */
  o = (HfHeif) HF_HEIF_DEFAULT ("avif", 321, 123);
  a = hf_make_heif (&o);
  memcpy (a->data + 4, "free", 4);
  g_assert_false (parse_exact ("avif", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);
  /* a meta that does not come within the buffer: an mdat of 70000 bytes in front of it */
  a = hf_make_heif (&o);
  {
    GByteArray *pre = g_byte_array_new (), *big;
    guint8 *z = g_malloc0 (70000);

    g_assert_cmpuint (be32_at (a->data), ==, 24); /* ftyp: 8 + major, minor, 2 brands */
    g_byte_array_append (pre, a->data, 24);
    big = g_byte_array_new ();
    hf_u32 (big, 70008); hf_cc (big, "mdat");
    g_byte_array_append (big, z, 70000);
    g_byte_array_append (pre, big->data, big->len);
    g_byte_array_append (pre, a->data + 24, a->len - 24);
    g_assert_false (parse_exact ("avif", pre->data, MIN (pre->len, NFC_HEADER_MAX_BYTES), &w, &h));
    g_assert_true (parse_exact ("avif", pre->data, pre->len, &w, &h)); /* the parser itself is fine */
    g_assert_cmpuint (w, ==, 321);
    g_byte_array_free (pre, TRUE); g_byte_array_free (big, TRUE);
    g_free (z);
  }
  g_byte_array_free (a, TRUE);
  g_assert_false (parse_exact ("avif", (const guint8 *) "", 0, &w, &h));
}

/* TIFF made by hand: II and MM, 32 and 64-bit (BigTIFF), SHORT and LONG sizes,
 * orientation */
static void
test_header_tiff_handmade (void)
{
  guint32 w = 0, h = 0;
  GByteArray *a;

  for (guint v = 0; v < 8; v++)
    {
      gboolean le = v & 1, big = v & 2, short_ok = !(v & 4);

      a = hf_make_tiff (le, big, 1425, 950, 0, short_ok);
      g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h));
      g_assert_cmpuint (w, ==, 1425); g_assert_cmpuint (h, ==, 950);
      g_byte_array_free (a, TRUE);
      a = hf_make_tiff (le, big, 100000, 70000, 0, short_ok); /* > 16 bits: LONG */
      g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h));
      g_assert_cmpuint (w, ==, 100000); g_assert_cmpuint (h, ==, 70000);
      g_byte_array_free (a, TRUE);
      /* orientation 5..8 swaps, 1..4 does not */
      for (guint o = 1; o <= 8; o++)
        {
          a = hf_make_tiff (le, big, 1425, 950, o, short_ok);
          g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h));
          g_assert_cmpuint (w, ==, o >= 5 ? 950u : 1425u);
          g_assert_cmpuint (h, ==, o >= 5 ? 1425u : 950u);
          g_byte_array_free (a, TRUE);
        }
    }
  /* a size of 0, of 2^31, a missing tag, a wrong magic, an IFD beyond the buffer */
  a = hf_make_tiff (TRUE, FALSE, 0, 950, 0, TRUE);
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);
  a = hf_make_tiff (TRUE, FALSE, 0x80000000u, 950, 0, FALSE);
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);
  a = hf_make_tiff (FALSE, FALSE, 800, 600, 0, TRUE);
  a->data[3] = 43; /* BigTIFF magic on a classic header: offset size field is junk */
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[3] = 41;
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[3] = 42;
  g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[0] = 'X';
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[0] = 'M';
  a->data[4] = 0x7f; /* IFD at 0x7f000008: beyond the buffer */
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[4] = 0; a->data[7] = 7; /* IFD inside the header */
  g_assert_false (parse_exact ("tiff", a->data, a->len, &w, &h));
  a->data[7] = 8;
  a->data[8] = 0xff; a->data[9] = 0xff; /* 65535 entries in a buffer of a few: bounded by it */
  g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h)); /* the count is bounded by the buffer */
  g_byte_array_free (a, TRUE);
  /* cuts of a valid one */
  a = hf_make_tiff (FALSE, TRUE, 3000, 2000, 6, TRUE);
  for (gsize len = 0; len < 16 + 8 + 20 * 2; len++) /* header, count, and not both sizes yet */
    g_assert_false (parse_exact ("tiff", a->data, len, &w, &h));
  g_assert_true (parse_exact ("tiff", a->data, a->len, &w, &h));
  g_byte_array_free (a, TRUE);
}


static void
test_header_handmade (void)
{
  guint32 w = 0, h = 0;
  guint8 png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
  guint8 gif[13] = { 'G', 'I', 'F', '8', '9', 'a' };
  guint8 bmp[54] = { 'B', 'M' };
  guint8 vp8[30] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ' };
  guint8 vp8l[30] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L' };
  guint8 vp8x[30] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X' };
  GByteArray *j;

  /* PNG */
  put_be32 (png + 16, 3840); put_be32 (png + 20, 2160);
  g_assert_true (parse_exact ("png", png, sizeof png, &w, &h));
  g_assert_cmpuint (w, ==, 3840); g_assert_cmpuint (h, ==, 2160);
  put_be32 (png + 16, 0);
  g_assert_false (parse_exact ("png", png, sizeof png, &w, &h)); /* zero width */
  put_be32 (png + 16, 0x80000000u);
  g_assert_false (parse_exact ("png", png, sizeof png, &w, &h)); /* beyond 2^31 - 1 */
  put_be32 (png + 16, 1); put_be32 (png + 20, 1);
  g_assert_true (parse_exact ("png", png, sizeof png, &w, &h));
  png[15] = 'X'; /* not IHDR */
  g_assert_false (parse_exact ("png", png, sizeof png, &w, &h));
  png[15] = 'R'; png[11] = 12; /* a length that is not 13 */
  g_assert_false (parse_exact ("png", png, sizeof png, &w, &h));

  /* GIF: logical screen, little endian */
  put_le16 (gif + 6, 600); put_le16 (gif + 8, 799);
  g_assert_true (parse_exact ("gif", gif, sizeof gif, &w, &h));
  g_assert_cmpuint (w, ==, 600); g_assert_cmpuint (h, ==, 799);
  gif[4] = '7';
  g_assert_true (parse_exact ("gif", gif, sizeof gif, &w, &h)); /* GIF87a */
  gif[3] = '9';
  g_assert_false (parse_exact ("gif", gif, sizeof gif, &w, &h));

  /* BMP: BITMAPINFOHEADER (40), top-down (negative height), OS/2 (12) */
  put_le32 (bmp + 14, 40); put_le32 (bmp + 18, 635); put_le32 (bmp + 22, 600);
  g_assert_true (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));
  g_assert_cmpuint (w, ==, 635); g_assert_cmpuint (h, ==, 600);
  put_le32 (bmp + 22, (guint32) -480);
  g_assert_true (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));
  g_assert_cmpuint (h, ==, 480);
  put_le32 (bmp + 22, 0x80000000u); /* INT_MIN: must not overflow the negation */
  g_assert_true (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));
  g_assert_cmpuint (h, ==, 0x80000000u);
  put_le32 (bmp + 18, (guint32) -5);
  g_assert_false (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));
  put_le32 (bmp + 14, 12); put_le16 (bmp + 18, 320); put_le16 (bmp + 20, 200);
  g_assert_true (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));
  g_assert_cmpuint (w, ==, 320); g_assert_cmpuint (h, ==, 200);
  put_le32 (bmp + 14, 7); /* an impossible header size */
  g_assert_false (parse_exact ("bmp", bmp, sizeof bmp, &w, &h));

  /* WebP: lossy VP8 (14-bit sizes after the start code), lossless VP8L
   * (14 bits minus one each), extended VP8X (24 bits minus one each) */
  vp8[23] = 0x9d; vp8[24] = 0x01; vp8[25] = 0x2a;
  put_le16 (vp8 + 26, 1920 | 0x4000); put_le16 (vp8 + 28, 1080); /* the scale bits are ignored */
  g_assert_true (parse_exact ("webp", vp8, sizeof vp8, &w, &h));
  g_assert_cmpuint (w, ==, 1920); g_assert_cmpuint (h, ==, 1080);
  vp8[24] = 0;
  g_assert_false (parse_exact ("webp", vp8, sizeof vp8, &w, &h));
  vp8l[20] = 0x2f;
  put_le32 (vp8l + 21, (512 - 1) | ((300 - 1) << 14));
  g_assert_true (parse_exact ("webp", vp8l, sizeof vp8l, &w, &h));
  g_assert_cmpuint (w, ==, 512); g_assert_cmpuint (h, ==, 300);
  vp8l[20] = 0x2e;
  g_assert_false (parse_exact ("webp", vp8l, sizeof vp8l, &w, &h));
  vp8x[24] = (16383 - 1) & 0xff; vp8x[25] = (16383 - 1) >> 8; vp8x[26] = 0;
  vp8x[27] = 100 - 1; vp8x[28] = 0; vp8x[29] = 0;
  g_assert_true (parse_exact ("webp", vp8x, sizeof vp8x, &w, &h));
  g_assert_cmpuint (w, ==, 16383); g_assert_cmpuint (h, ==, 100);

  /* JPEG: the size marker after APPn segments is found by their lengths */
  j = make_jpeg (0, 0, 4191, 6286);
  g_assert_true (parse_exact ("jpg", j->data, j->len, &w, &h));
  g_assert_cmpuint (w, ==, 4191); g_assert_cmpuint (h, ==, 6286);
  g_byte_array_free (j, TRUE);
  j = make_jpeg (1, 60000, 3024, 4032); /* one big EXIF/ICC: still inside 64 KiB */
  g_assert_true (parse_exact ("jpg", j->data, MIN (j->len, NFC_HEADER_MAX_BYTES), &w, &h));
  g_assert_cmpuint (w, ==, 3024); g_assert_cmpuint (h, ==, 4032);
  g_byte_array_free (j, TRUE);
  j = make_jpeg (2, 40000, 3024, 4032); /* the size marker lies beyond 64 KiB: unknown */
  g_assert_cmpuint (j->len, >, NFC_HEADER_MAX_BYTES);
  g_assert_false (parse_exact ("jpg", j->data, NFC_HEADER_MAX_BYTES, &w, &h));
  g_assert_true (parse_exact ("jpg", j->data, j->len, &w, &h)); /* the parser itself is fine */
  g_byte_array_free (j, TRUE);
  j = make_jpeg (0, 0, 10, 10);
  j->data[2 + 0] = 0xff; j->data[3] = 0xda; /* SOS before any frame header */
  g_assert_false (parse_exact ("jpg", j->data, j->len, &w, &h));
  g_byte_array_free (j, TRUE);
  j = make_jpeg (0, 0, 10, 0); /* height 0 (DNL): unknown */
  g_assert_false (parse_exact ("jpg", j->data, j->len, &w, &h));
  g_byte_array_free (j, TRUE);

  /* a mismatch between the extension and the content, unknown and NULL input */
  j = make_jpeg (0, 0, 10, 10);
  g_assert_false (parse_exact ("png", j->data, j->len, &w, &h));
  g_assert_false (parse_exact ("tiff", j->data, j->len, &w, &h)); /* JPEG data, TIFF key */
  g_assert_false (parse_exact ("avif", j->data, j->len, &w, &h));
  g_assert_false (parse_exact ("pdf", j->data, j->len, &w, &h));
  g_assert_false (parse_exact (NULL, j->data, j->len, &w, &h));
  g_assert_false (nfc_header_parse ("jpg", NULL, 0, &w, &h));
  g_assert_true (nfc_header_parse ("jpg", j->data, j->len, NULL, NULL)); /* out params optional */
  g_assert_true (nfc_header_format_known ("webp"));
  g_assert_true (nfc_header_format_known ("tiff"));
  g_assert_true (nfc_header_format_known ("avif"));
  g_assert_true (nfc_header_format_known ("heic"));
  g_assert_false (nfc_header_format_known ("pdf"));
  g_assert_false (nfc_header_format_known ("ico"));
  g_assert_false (nfc_header_format_known (NULL));
  g_byte_array_free (j, TRUE);
}

/* Real files made by ImageMagick, parsed from disk by nfc_header_read_file, with
 * the time that takes; then all of them under truncation and byte flips. */
static void
test_header_files_and_fuzz (void)
{
  char *magick = g_find_program_in_path ("magick");
  char *dir;
  static const struct {
    const char *name, *fmt, *spec2, *define, *out_prefix;
    guint w, h;
  } fx[] = {
    { "a.png",  "png",  "xc:gray",   NULL, "PNG:",  123, 45 },
    { "b.jpg",  "jpg",  "gradient:", NULL, "JPEG:", 321, 123 },
    { "c.gif",  "gif",  "xc:gray",   NULL, "GIF:",  64, 48 },
    { "d.webp", "webp", "gradient:", NULL, "WEBP:", 200, 100 }, /* lossy: VP8 */
    { "e.webp", "webp", "xc:gray",   "webp:lossless=true", "WEBP:", 77, 33 }, /* VP8L */
    { "f.webp", "webp", "xc:none",   NULL, "WEBP:", 150, 90 },  /* alpha: VP8X */
    { "g.bmp",  "bmp",  "xc:gray",   NULL, "BMP:",  635, 600 },
    { "h.png",  "png",  "xc:gray",   NULL, "PNG:",  3840, 2160 },
    { "i.jpg",  "jpg",  "xc:gray",   NULL, "JPEG:", 1, 1 },
    { "j.png",  "png",  "xc:gray",   NULL, "PNG:",  1, 7000 },
  };
  GPtrArray *bufs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_byte_array_unref);
  double total_us = 0;
  guint n_timed = 0;

  if (magick == NULL)
    {
      g_test_skip ("ImageMagick is not installed");
      g_free (magick);
      g_ptr_array_unref (bufs);
      return;
    }
  dir = g_dir_make_tmp ("nfc-hdr-XXXXXX", NULL);
  g_assert_nonnull (dir);
  for (guint i = 0; i < G_N_ELEMENTS (fx); i++)
    {
      char *path = g_build_filename (dir, fx[i].name, NULL);
      char *size = g_strdup_printf ("%ux%u", fx[i].w, fx[i].h);
      char *out = g_strconcat (fx[i].out_prefix, path, NULL);
      guint32 w = 0, h = 0;
      gint64 t0;
      gchar *contents = NULL;
      gsize len = 0;

      {
        char *argv[10];
        int status = -1, n = 0;

        argv[n++] = magick; argv[n++] = "-quiet"; argv[n++] = "-size"; argv[n++] = size;
        argv[n++] = (char *) fx[i].spec2;
        if (fx[i].define != NULL)
          {
            argv[n++] = "-define";
            argv[n++] = (char *) fx[i].define;
          }
        argv[n++] = out;
        argv[n] = NULL;
        g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL |
                                     G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL,
                                     &status, NULL) && status == 0);
      }
      t0 = g_get_monotonic_time ();
      g_assert_true (nfc_header_read_file (path, fx[i].fmt, &w, &h));
      total_us += (double) (g_get_monotonic_time () - t0);
      n_timed++;
      g_assert_cmpuint (w, ==, fx[i].w);
      g_assert_cmpuint (h, ==, fx[i].h);
      /* the wrong format key never matches */
      g_assert_false (nfc_header_read_file (path, g_str_equal (fx[i].fmt, "png") ? "gif" : "png",
                                            &w, &h));
      g_assert_true (g_file_get_contents (path, &contents, &len, NULL));
      g_ptr_array_add (bufs, g_byte_array_new_take ((guint8 *) contents, len));
      g_free (path); g_free (size); g_free (out);
    }
  /* AVIF from libaom (ffmpeg), TIFF in both byte orders from libtiff (its IFD is
   * after the pixels), the 16 x 16 HEIC of tests/data, and headers made by hand:
   * the same checks, and into the fuzz corpus */
  {
    char *ffmpeg = g_find_program_in_path ("ffmpeg");
    const char *data = g_getenv ("NFC_TEST_DATA");
    char *avif = g_build_filename (dir, "k.avif", NULL);
    char *heic = data != NULL ? g_build_filename (data, "data", "tiny-hevc.heic", NULL) : NULL;
    static const struct { const char *name, *endian; } tifs[] = { { "m.tif", "msb" }, { "l.tif", "lsb" } };
    gchar *contents = NULL;
    gsize len = 0;
    guint32 w = 0, h = 0;

    if (ffmpeg != NULL)
      {
        char *argv[] = { ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc=s=321x123",
                         "-frames:v", "1", "-c:v", "libaom-av1", "-still-picture", "1", "-y", avif, NULL };
        int status = -1;

        if (g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                          NULL, NULL, NULL, NULL, &status, NULL) && status == 0)
          {
            g_assert_true (nfc_header_read_file (avif, "avif", &w, &h));
            g_assert_cmpuint (w, ==, 321); g_assert_cmpuint (h, ==, 123);
            g_assert_true (nfc_header_read_file (avif, "heic", &w, &h)); /* same container */
            g_assert_false (nfc_header_read_file (avif, "png", &w, &h));
            g_assert_false (nfc_header_read_file (avif, "tiff", &w, &h));
            g_assert_true (g_file_get_contents (avif, &contents, &len, NULL));
            g_ptr_array_add (bufs, g_byte_array_new_take ((guint8 *) contents, len));
          }
        else
          g_test_message ("ffmpeg has no libaom-av1: no AVIF fixture");
      }
    for (guint i = 0; magick != NULL && i < G_N_ELEMENTS (tifs); i++)
      {
        char *path = g_build_filename (dir, tifs[i].name, NULL);
        char *out = g_strconcat ("TIFF:", path, NULL);
        char *def = g_strdup_printf ("tiff:endian=%s", tifs[i].endian);
        char *argv[] = { magick, "-quiet", "-size", "13x9", "gradient:", "-define", def, out, NULL };
        int status = -1;

        g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                     NULL, NULL, NULL, NULL, &status, NULL) && status == 0);
        g_assert_true (nfc_header_read_file (path, "tiff", &w, &h));
        g_assert_cmpuint (w, ==, 13); g_assert_cmpuint (h, ==, 9);
        g_assert_false (nfc_header_read_file (path, "avif", &w, &h));
        g_assert_true (g_file_get_contents (path, &contents, &len, NULL));
        g_assert_true (parse_exact ("tiff", (const guint8 *) contents, len, &w, &h)); /* small: IFD in the buffer */
        g_assert_cmpuint (w, ==, 13);
        g_ptr_array_add (bufs, g_byte_array_new_take ((guint8 *) contents, len));
        g_free (path); g_free (out); g_free (def);
      }
    if (heic != NULL && g_file_get_contents (heic, &contents, &len, NULL))
      {
        g_assert_true (parse_exact ("heic", (const guint8 *) contents, len, &w, &h));
        g_assert_cmpuint (w, ==, 16); g_assert_cmpuint (h, ==, 16);
        g_ptr_array_add (bufs, g_byte_array_new_take ((guint8 *) contents, len));
      }
    for (guint i = 0; i < 16; i++)
      {
        HfHeif o = HF_HEIF_DEFAULT (i % 2 ? "avif" : "heic", 321 + i, 123);

        o.decoy = i & 2; o.irot = (i & 4) ? 1 : -1; o.clap = (i & 8) != 0;
        o.clap_w = 100; o.clap_h = 50; o.wide_ipma = i % 3 == 0;
        g_ptr_array_add (bufs, hf_make_heif (&o));
      }
    for (guint i = 0; i < 8; i++)
      g_ptr_array_add (bufs, hf_make_tiff (i & 1, i & 2, 1000 + i, 500, (i & 4) ? 6 : 0, TRUE));
    g_free (ffmpeg); g_free (avif); g_free (heic);
  }
  g_test_message ("header read+parse of %u real files: %.1f us on average", n_timed,
                  total_us / n_timed);
  g_assert_cmpfloat (total_us / n_timed, <, 20000.0); /* far below the 1 ms a menu would feel */

  /* Fuzz: every prefix of every file (first 600 bytes one by one, then
   * strides), and 400 random byte flips each, through every parser: never a read
   * beyond the buffer (ASan), never a crash, and a success has non-zero sizes. */
  {
    GRand *rnd = g_rand_new_with_seed (42);
    static const char *const fmts[] = { "png", "jpg", "gif", "webp", "bmp", "tiff", "avif", "heic" };

    for (guint b = 0; b < bufs->len; b++)
      {
        GByteArray *a = bufs->pdata[b];
        gsize cap = MIN (a->len, (gsize) 3000);

        for (gsize len = 0; len <= cap; len += (len < 600 ? 1 : 37))
          for (guint f = 0; f < G_N_ELEMENTS (fmts); f++)
            {
              guint32 w = 0, h = 0;

              if (parse_exact (fmts[f], a->data, len, &w, &h))
                {
                  g_assert_cmpuint (w, >, 0);
                  g_assert_cmpuint (h, >, 0);
                }
            }
        for (guint round = 0; round < 400; round++)
          {
            guint8 *copy = g_memdup2 (a->data, cap);
            guint flips = 1 + g_rand_int_range (rnd, 0, 4);

            for (guint k = 0; k < flips; k++)
              copy[g_rand_int_range (rnd, 0, cap)] = (guint8) g_rand_int (rnd);
            for (guint f = 0; f < G_N_ELEMENTS (fmts); f++)
              {
                guint32 w = 0, h = 0;

                if (parse_exact (fmts[f], copy, cap, &w, &h))
                  {
                    g_assert_cmpuint (w, >, 0);
                    g_assert_cmpuint (h, >, 0);
                  }
              }
            g_free (copy);
          }
      }
    g_rand_free (rnd);
  }
  /* pure garbage, all lengths up to 100, from a fixed seed */
  {
    GRand *rnd = g_rand_new_with_seed (7);
    guint8 junk[100];
    static const char *const fmts[] = { "png", "jpg", "gif", "webp", "bmp", "tiff", "avif", "heic" };

    for (guint round = 0; round < 3000; round++)
      {
        gsize len = g_rand_int_range (rnd, 0, 100);

        for (gsize k = 0; k < len; k++)
          junk[k] = (guint8) g_rand_int (rnd);
        /* bias towards the magic numbers so the deeper code is reached */
        if (round % 3 == 0 && len > 12)
          memcpy (junk, round % 2 ? "RIFF\0\0\0\0WEBPVP8X" : "\xff\xd8\xff\xe1\x00\x10", 6);
        /* ... and towards an ftyp + meta box (the sizes in it random) and a TIFF header */
        if (round % 5 == 1 && len > 40)
          {
            static const guint8 ftyp[24] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0,
                                             'm', 'i', 'f', '1', 'a', 'v', 'i', 'f' };

            memcpy (junk, ftyp, sizeof ftyp);
            memcpy (junk + 28, "meta", 4);
            /* the size of meta: 0 (to the end) or 40..99, which may or may not fit */
            junk[24] = junk[25] = junk[26] = 0;
            junk[27] = round % 7 == 0 ? 0 : (guint8) (40 + round % 60);
          }
        else if (round % 5 == 2 && len > 16)
          memcpy (junk, round % 2 ? "II*\0\x08\0\0\0" : "MM\0*\0\0\0\x08", 8);
        for (guint f = 0; f < G_N_ELEMENTS (fmts); f++)
          {
            guint32 w, h;

            parse_exact (fmts[f], junk, len, &w, &h);
          }
      }
    g_rand_free (rnd);
  }
  {
    char *rm[] = { "rm", "-rf", dir, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (dir);
  g_free (magick);
  g_ptr_array_unref (bufs);
}

/* ------------------------------------------------------------------------
 * Resize
 * --------------------------------------------------------------------- */

static void
check_out (const char *cat_key, const NfcResizePreset *p, guint64 w, guint64 h, gboolean want,
           guint64 want_w, guint64 want_h)
{
  guint64 ow = 99, oh = 99;
  gboolean got = nfc_resize_output_size (p, w, h, &ow, &oh);

  if (got != want)
    g_error ("%s %s %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT ": got %d want %d", cat_key, p->key, w, h,
             got, want);
  if (want && (ow != want_w || oh != want_h))
    g_error ("%s %s %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT ": got %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT
             ", want %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT, cat_key, p->key, w, h, ow, oh, want_w, want_h);
}

/* which presets a w x h image offers, as "a,b,c" of keys */
static char *
offered (NfcCategory cat, guint64 w, guint64 h)
{
  GString *s = g_string_new (NULL);
  guint n;
  const NfcResizePreset *all = nfc_resize_presets (cat, &n);

  for (guint i = 0; i < n; i++)
    if (nfc_resize_output_size (&all[i], w, h, NULL, NULL))
      g_string_append_printf (s, "%s%s", s->len ? "," : "", all[i].key);
  return g_string_free (s, FALSE);
}

static void
test_resize_presets (void)
{
  guint n;
  const NfcResizePreset *im = nfc_resize_presets (NFC_CAT_IMAGE, &n);
  const NfcResizePreset *vid;
  char *s;

  /* the table: names, labels, order */
  g_assert_cmpuint (n, ==, 7);
  {
    static const char *const keys[] = { "4k", "1440p", "1080p", "720p", "480p", "50pct", "25pct" };
    static const char *const labels[] = { "4K (3840 px)", "1440p (2560 px)", "1080p (1920 px)",
                                          "720p (1280 px)", "480p (854 px)", "50%", "25%" };
    static const char *const names[] = { "4K", "1440p", "1080p", "720p", "480p", "50%", "25%" };

    for (guint i = 0; i < n; i++)
      {
        g_assert_cmpstr (im[i].key, ==, keys[i]);
        g_assert_cmpstr (im[i].label, ==, labels[i]);
        g_assert_cmpstr (im[i].name, ==, names[i]);
        g_assert_true (nfc_resize_preset_from_key (NFC_CAT_IMAGE, keys[i]) == &im[i]);
      }
  }
  vid = nfc_resize_presets (NFC_CAT_VIDEO, &n);
  g_assert_cmpuint (n, ==, 3);
  g_assert_cmpstr (vid[0].key, ==, "1080p");
  g_assert_true (vid[0].short_side);
  g_assert_cmpuint (vid[2].side, ==, 480);
  g_assert_null (nfc_resize_presets (NFC_CAT_AUDIO, &n));
  g_assert_cmpuint (n, ==, 0);
  g_assert_null (nfc_resize_presets (NFC_CAT_DOC, NULL));
  g_assert_null (nfc_resize_preset_from_key (NFC_CAT_IMAGE, "2k"));
  g_assert_null (nfc_resize_preset_from_key (NFC_CAT_IMAGE, NULL));
  g_assert_null (nfc_resize_preset_from_key (NFC_CAT_VIDEO, "50pct"));

  /* images: a preset is offered when it makes the LONG side strictly smaller; the
   * percentages when the short side stays at 16 px or more */
  s = offered (NFC_CAT_IMAGE, 3840, 2160);
  g_assert_cmpstr (s, ==, "1440p,1080p,720p,480p,50pct,25pct"); /* not 4K itself */
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 7680, 4320);
  g_assert_cmpstr (s, ==, "4k,1440p,1080p,720p,480p,50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 1000, 800); /* 1280 and 1920 px are larger than the 1000 px side */
  g_assert_cmpstr (s, ==, "480p,50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 800, 1000); /* portrait: the long side is the height */
  g_assert_cmpstr (s, ==, "480p,50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 1280, 720); /* equal to 720p: not smaller */
  g_assert_cmpstr (s, ==, "480p,50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 1281, 720);
  g_assert_cmpstr (s, ==, "720p,480p,50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 100, 100); /* 50% is 50 px, 25% is 25 px */
  g_assert_cmpstr (s, ==, "50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 64, 64); /* 25% would be 16: the limit is inclusive */
  g_assert_cmpstr (s, ==, "50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 63, 63); /* 25% is 15.75 -> 16 px (rounded): offered */
  g_assert_cmpstr (s, ==, "50pct,25pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 61, 61); /* 25% is 15 px: no */
  g_assert_cmpstr (s, ==, "50pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 32, 32); /* 50% is 16 */
  g_assert_cmpstr (s, ==, "50pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 31, 31); /* 50% is 16 after rounding (15.5) */
  g_assert_cmpstr (s, ==, "50pct");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 30, 30); /* 15 px: nothing at all */
  g_assert_cmpstr (s, ==, "");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 20, 20);
  g_assert_cmpstr (s, ==, "");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 4000, 20); /* a thin banner: the short side limits the percentages */
  g_assert_cmpstr (s, ==, "4k,1440p,1080p,720p,480p");
  g_free (s);
  s = offered (NFC_CAT_IMAGE, 0, 0); /* unknown */
  g_assert_cmpstr (s, ==, "");
  g_free (s);

  /* the sizes */
  for (guint i = 0; i < 7; i++)
    {
      const char *k = im[i].key;
      const NfcResizePreset *p = &im[i];

      if (g_str_equal (k, "4k"))        check_out ("img", p, 3840, 2160, FALSE, 0, 0);
      if (g_str_equal (k, "1440p"))     check_out ("img", p, 3840, 2160, TRUE, 2560, 1440);
      if (g_str_equal (k, "1080p"))     check_out ("img", p, 3840, 2160, TRUE, 1920, 1080);
      if (g_str_equal (k, "720p"))      check_out ("img", p, 3840, 2160, TRUE, 1280, 720);
      if (g_str_equal (k, "480p"))      check_out ("img", p, 3840, 2160, TRUE, 854, 480);
      if (g_str_equal (k, "50pct"))     check_out ("img", p, 3840, 2160, TRUE, 1920, 1080);
      if (g_str_equal (k, "25pct"))     check_out ("img", p, 3840, 2160, TRUE, 960, 540);
      if (g_str_equal (k, "1080p"))     check_out ("img", p, 2160, 3840, TRUE, 1080, 1920);
      if (g_str_equal (k, "480p"))      check_out ("img", p, 1000, 800, TRUE, 854, 683);
      if (g_str_equal (k, "50pct"))     check_out ("img", p, 101, 51, TRUE, 51, 26); /* nearest */
      if (g_str_equal (k, "720p"))      check_out ("img", p, 4000, 3, TRUE, 1280, 1); /* never 0 */
    }
  /* video: the SHORT side, even sizes, no percentages */
  for (guint i = 0; i < 3; i++)
    {
      const NfcResizePreset *p = &vid[i];

      if (i == 0)
        {
          check_out ("vid", p, 1920, 1080, FALSE, 0, 0);
          check_out ("vid", p, 3840, 2160, TRUE, 1920, 1080);
          check_out ("vid", p, 4096, 2160, TRUE, 2048, 1080);
          check_out ("vid", p, 2160, 3840, TRUE, 1080, 1920); /* portrait phone video */
          check_out ("vid", p, 1921, 1081, TRUE, 1920, 1080);
          check_out ("vid", p, 1080, 1920, FALSE, 0, 0);
        }
      if (i == 1)
        {
          check_out ("vid", p, 1920, 1080, TRUE, 1280, 720);
          check_out ("vid", p, 1280, 720, FALSE, 0, 0);
          check_out ("vid", p, 405, 864, FALSE, 0, 0);
          check_out ("vid", p, 1080, 1920, TRUE, 720, 1280);
          check_out ("vid", p, 1000, 722, TRUE, 998, 720); /* 721.2: even */
        }
      if (i == 2)
        {
          check_out ("vid", p, 1920, 1080, TRUE, 854, 480);
          check_out ("vid", p, 854, 480, FALSE, 0, 0);
          check_out ("vid", p, 640, 360, FALSE, 0, 0);
        }
    }
  s = offered (NFC_CAT_VIDEO, 1920, 1080);
  g_assert_cmpstr (s, ==, "720p,480p");
  g_free (s);
  s = offered (NFC_CAT_VIDEO, 1280, 720);
  g_assert_cmpstr (s, ==, "480p");
  g_free (s);
  g_assert_false (nfc_resize_output_size (NULL, 1920, 1080, NULL, NULL));
  g_assert_false (nfc_resize_output_size (&im[2], 0, 1080, NULL, NULL));
  /* absurd sizes cannot overflow */
  g_assert_true (nfc_resize_output_size (&im[6], G_MAXUINT64, G_MAXUINT64, NULL, NULL));
  g_assert_true (nfc_resize_output_size (&im[2], G_MAXUINT64, 1, NULL, NULL));

  /* tooltips: the exact size when known */
  s = nfc_resize_tooltip (&im[2], 3840, 2160);
  g_assert_nonnull (strstr (s, "1920 × 1080"));
  g_free (s);
  s = nfc_resize_tooltip (&im[2], 0, 0);
  g_assert_nonnull (strstr (s, "1920 px"));
  g_free (s);
  s = nfc_resize_tooltip (&im[5], 0, 0);
  g_assert_nonnull (strstr (s, "50%"));
  g_free (s);
  s = nfc_resize_tooltip (&vid[1], 0, 0);
  g_assert_nonnull (strstr (s, "shorter side is 720"));
  g_free (s);
}

static void
test_resize_targets (void)
{
  static const struct { NfcCategory cat; const char *fmt, *key, *ext; NfcCategory tcat; } yes[] = {
    { NFC_CAT_IMAGE, "png",  "png",  "png",  NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "jpg",  "jpg",  "jpg",  NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "webp", "webp", "webp", NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "avif", "avif", "avif", NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "bmp",  "bmp",  "bmp",  NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "tiff", "tiff", "tiff", NFC_CAT_IMAGE },
    { NFC_CAT_IMAGE, "gif",  "gif",  "gif",  NFC_CAT_VIDEO }, /* an animation: ffmpeg */
    { NFC_CAT_VIDEO, "mp4",  "mp4",  "mp4",  NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "m4v",  "mp4",  "mp4",  NFC_CAT_VIDEO }, /* .m4v would be raw H.264 */
    { NFC_CAT_VIDEO, "mkv",  "mkv",  "mkv",  NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "webm", "webm", "webm", NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "mov",  "mov",  "mov",  NFC_CAT_VIDEO },
    { NFC_CAT_VIDEO, "avi",  "avi",  "avi",  NFC_CAT_VIDEO },
  };
  static const struct { NfcCategory cat; const char *fmt; } no[] = {
    { NFC_CAT_IMAGE, "heic" }, { NFC_CAT_IMAGE, "svg" }, { NFC_CAT_IMAGE, "ico" },
    { NFC_CAT_VIDEO, "wmv" }, { NFC_CAT_VIDEO, "flv" }, { NFC_CAT_AUDIO, "mp3" },
    { NFC_CAT_DOC, "docx" }, { NFC_CAT_SHEET, "csv" }, { NFC_CAT_SLIDE, "pptx" },
    { NFC_CAT_VIDEO, "jpg" }, { NFC_CAT_IMAGE, "mp4" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (yes); i++)
    {
      NfcResizeTarget t;

      g_assert_true (nfc_resize_target (yes[i].cat, yes[i].fmt, &t));
      g_assert_cmpstr (t.key, ==, yes[i].key);
      g_assert_cmpstr (t.ext, ==, yes[i].ext);
      g_assert_cmpint (t.cat, ==, yes[i].tcat);
    }
  for (guint i = 0; i < G_N_ELEMENTS (no); i++)
    g_assert_false (nfc_resize_target (no[i].cat, no[i].fmt, NULL));
  g_assert_false (nfc_resize_target (NFC_CAT_IMAGE, NULL, NULL));
}

/* the argv of a resize of source fmt to output key with preset rs_key of the
 * source category, as one string ("##" between attempts) */
static char *
resize_str (NfcCategory cat, const char *fmt, const char *key, const char *rs_key, gboolean hd)
{
  NfcResizeTarget t;
  NfcImageInfo info = { 3840, 2160, FALSE, 0, 0 };
  char *src = g_strdup_printf ("/s/a.%s", fmt), *out = g_strdup_printf ("/o/x.%s", key), *s;
  GPtrArray *at;

  g_assert_true (nfc_resize_target (cat, fmt, &t));
  at = nfc_build_attempts_resize (t.cat, fmt, key, nfc_resize_preset_from_key (cat, rs_key), src,
                                  out, "/w", hd ? &info : NULL, NULL);
  g_assert_nonnull (at);
  s = attempts_string (at);
  g_ptr_array_unref (at);
  g_free (src);
  g_free (out);
  return s;
}

static void
test_resize_argv (void)
{
  char *s;
  GPtrArray *at;

  use_fake_tools ();
  set_encoders (0);

  /* PNG stays lossless: Lanczos, long side bounded (never enlarges), alpha kept */
  s = resize_str (NFC_CAT_IMAGE, "png", "png", "1080p", FALSE);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet PNG:/s/a.png[0] -auto-orient -filter Lanczos -resize 1920x1920> PNG:/o/x.png");
  g_free (s);
  s = resize_str (NFC_CAT_IMAGE, "bmp", "bmp", "4k", FALSE);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet BMP:/s/a.bmp[0] -auto-orient -filter Lanczos -resize 3840x3840> BMP:/o/x.bmp");
  g_free (s);
  s = resize_str (NFC_CAT_IMAGE, "tiff", "tiff", "25pct", FALSE);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet TIFF:/s/a.tiff[0] -auto-orient -filter Lanczos -resize 25% TIFF:/o/x.tiff");
  g_free (s);
  /* JPG: the Best settings of the source class (q99 graphics, q95 photos), flattened */
  s = resize_str (NFC_CAT_IMAGE, "jpg", "jpg", "50pct", FALSE);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -filter Lanczos -resize 50% -background white -alpha remove -alpha off -quality 95 -sampling-factor 4:4:4 JPEG:/o/x.jpg");
  g_free (s);
  /* WEBP: photos lossy q90 + sharp YUV, graphics (here: from a PNG... a WEBP source
   * counts as a photo) lossless */
  s = resize_str (NFC_CAT_IMAGE, "webp", "webp", "720p", FALSE);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet WEBP:/s/a.webp[0] -auto-orient -filter Lanczos -resize 1280x1280> -resize 16383x16383> -quality 90 -define webp:method=6 -define webp:use-sharp-yuv=true WEBP:/o/x.webp");
  g_free (s);
  /* AVIF: the flattened PNG gets the resize, then the Best AVIF (crf 12 photo) */
  s = resize_str (NFC_CAT_IMAGE, "avif", "avif", "480p", FALSE);
  g_assert_nonnull (strstr (s, "-auto-orient -filter Lanczos -resize 854x854> -background white"));
  g_assert_nonnull (strstr (s, "-crf 12"));
  g_assert_nonnull (strstr (s, "/bin/nfc-decode /s/a.avif /w/decoded.png"));
  g_free (s);
  /* the dimensions of the source never change the argv of an image */
  {
    char *a = resize_str (NFC_CAT_IMAGE, "png", "png", "720p", FALSE);
    char *b = resize_str (NFC_CAT_IMAGE, "png", "png", "720p", TRUE);

    g_assert_cmpstr (a, ==, b);
    g_free (a);
    g_free (b);
  }

  /* GIF: ffmpeg scale (long side, any size) + the palette chain, NO fps filter */
  s = resize_str (NFC_CAT_IMAGE, "gif", "gif", "480p", FALSE);
  g_assert_cmpstr (s, ==, "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.gif -map 0:v:0 -an -sn -dn -vf scale=w='if(gt(iw,ih),min(iw,854),-1)':h='if(gt(iw,ih),-1,min(ih,854))':flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle -loop 0 /o/x.gif");
  g_assert_null (strstr (s, "fps"));
  g_free (s);
  s = resize_str (NFC_CAT_IMAGE, "gif", "gif", "50pct", FALSE);
  g_assert_nonnull (strstr (s, "scale=w='trunc(iw*50/100+0.5)':h='trunc(ih*50/100+0.5)':flags=lanczos,split"));
  g_free (s);

  /* MP4: libx264 crf 18 slow, SHORT side, even sizes; audio copied first, AAC next */
  s = resize_str (NFC_CAT_VIDEO, "mp4", "mp4", "720p", TRUE);
  g_assert_cmpstr (s, ==, "/bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -map 0:a:0? -sn -dn -vf scale=w='if(gt(iw,ih),-2,min(iw,720))':h='if(gt(iw,ih),min(ih,720),-2)':flags=lanczos,crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a copy -movflags +faststart /o/x.mp4"
                   " ## /bin/ffmpeg -hide_banner -loglevel error -nostdin -y -i /s/a.mp4 -map 0:v:0 -map 0:a:0? -sn -dn -vf scale=w='if(gt(iw,ih),-2,min(iw,720))':h='if(gt(iw,ih),min(ih,720),-2)':flags=lanczos,crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart /o/x.mp4");
  g_free (s);
  /* a .m4v source is written as mp4; mov gets faststart too; mkv none; avi no B-frames */
  s = resize_str (NFC_CAT_VIDEO, "m4v", "mp4", "1080p", FALSE);
  g_assert_nonnull (strstr (s, "-i /s/a.m4v "));
  g_assert_nonnull (strstr (s, "/o/x.mp4"));
  g_free (s);
  s = resize_str (NFC_CAT_VIDEO, "mov", "mov", "480p", FALSE);
  g_assert_nonnull (strstr (s, "-movflags +faststart /o/x.mov"));
  g_free (s);
  s = resize_str (NFC_CAT_VIDEO, "mkv", "mkv", "480p", FALSE);
  g_assert_null (strstr (s, "-movflags"));
  g_assert_nonnull (strstr (s, "-c:a copy /o/x.mkv"));
  g_assert_null (strstr (s, "-c copy"));
  g_free (s);
  s = resize_str (NFC_CAT_VIDEO, "avi", "avi", "480p", FALSE);
  g_assert_nonnull (strstr (s, "-crf 18 -preset slow -bf 0 -pix_fmt yuv420p"));
  g_assert_null (strstr (s, "-movflags"));
  g_free (s);
  /* WebM: VP9 crf 28 (Best), audio copied, else Opus 128k */
  s = resize_str (NFC_CAT_VIDEO, "webm", "webm", "720p", FALSE);
  g_assert_nonnull (strstr (s, "-c:v libvpx-vp9 -crf 28 -b:v 0"));
  g_assert_nonnull (strstr (s, "-c:a copy /o/x.webm ## "));
  g_assert_nonnull (strstr (s, "-c:a libopus -b:a 128k /o/x.webm"));
  g_free (s);

  /* libopenh264 only: bitrate of the OUTPUT size (1280 x 720 x 3 bits) */
  set_encoders (1);
  s = resize_str (NFC_CAT_VIDEO, "mp4", "mp4", "720p", TRUE);
  g_assert_nonnull (strstr (s, "-c:v libopenh264 -rc_mode bitrate -b:v 2764800 "));
  g_assert_nonnull (strstr (s, "-c:a copy"));
  g_assert_nonnull (strstr (s, "-c:a aac -b:a 192k"));
  g_free (s);
  /* both encoders: x264 (copy, aac), then openh264 (copy, aac) */
  set_encoders (2);
  at = nfc_build_attempts_resize (NFC_CAT_VIDEO, "mp4", "mp4",
                                  nfc_resize_preset_from_key (NFC_CAT_VIDEO, "480p"),
                                  "/s/a.mp4", "/o/x.mp4", "/w", NULL, NULL);
  g_assert_cmpuint (at->len, ==, 4);
  g_assert_true (argv_has (last_step (at, 0), "libx264"));
  g_assert_true (argv_has (last_step (at, 1), "libx264"));
  g_assert_true (argv_has (last_step (at, 2), "libopenh264"));
  g_assert_true (argv_has (last_step (at, 3), "libopenh264"));
  g_assert_cmpstr (argv_after (last_step (at, 0), "-c:a"), ==, "copy");
  g_assert_cmpstr (argv_after (last_step (at, 1), "-c:a"), ==, "aac");
  g_ptr_array_unref (at);
  nfc_test_set_encoders ("");

  /* not a resize target / no preset: NULL */
  g_assert_null (nfc_build_attempts_resize (NFC_CAT_VIDEO, "wmv", "wmv",
                                            nfc_resize_preset_from_key (NFC_CAT_VIDEO, "720p"),
                                            "/s/a.wmv", "/o/x.wmv", "/w", NULL, NULL));
  g_assert_null (nfc_build_attempts_resize (NFC_CAT_IMAGE, "png", "png", NULL, "/s/a.png",
                                            "/o/x.png", "/w", NULL, NULL));
  /* the ordinary builders never resize */
  s = build_str (NFC_CAT_IMAGE, "png", "png", BEST, FALSE);
  g_assert_null (strstr (s, "Lanczos"));
  g_free (s);
}

/* The menu labels and the toast sizes of the Small preset (item NAMES are unchanged) */
static void
test_small_labels (void)
{
  guint64 ow, oh;

  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "jpg", SMALL), ==, "Small (max 2560 px)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "webp", SMALL), ==, "Small (max 2560 px)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "avif", SMALL), ==, "Small (max 2560 px)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_VIDEO, "mp4", SMALL), ==, "Small (max 1080p)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_VIDEO, "webm", SMALL), ==, "Small (max 1080p)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_VIDEO, "gif", SMALL), ==, "Small (480 px)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, NULL, SMALL), ==, "Small (max 2560 px / 1080p)");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "jpg", BEST), ==, "Best");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "jpg", HIGH), ==, "High");
  g_assert_cmpstr (nfc_preset_menu_label (NFC_CAT_IMAGE, "jpg", BAL), ==, "Balanced");
  g_assert_cmpstr (nfc_preset_key (SMALL), ==, "small"); /* the item name is unchanged */
  g_assert_cmpstr (nfc_preset_label (SMALL), ==, "Small"); /* and so is the toast's name */

  /* where Small scaled a picture down */
  g_assert_true (nfc_preset_output_size (NFC_CAT_IMAGE, "jpg", SMALL, 3840, 2560, &ow, &oh));
  g_assert_cmpuint (ow, ==, 2560); g_assert_cmpuint (oh, ==, 1707);
  g_assert_true (nfc_preset_output_size (NFC_CAT_IMAGE, "webp", SMALL, 3000, 6000, &ow, &oh));
  g_assert_cmpuint (ow, ==, 1280); g_assert_cmpuint (oh, ==, 2560);
  g_assert_false (nfc_preset_output_size (NFC_CAT_IMAGE, "jpg", SMALL, 2560, 1000, NULL, NULL));
  g_assert_false (nfc_preset_output_size (NFC_CAT_IMAGE, "jpg", HIGH, 6000, 4000, NULL, NULL));
  g_assert_false (nfc_preset_output_size (NFC_CAT_IMAGE, "png", SMALL, 6000, 4000, NULL, NULL));
  g_assert_true (nfc_preset_output_size (NFC_CAT_VIDEO, "mp4", SMALL, 3840, 2160, &ow, &oh));
  g_assert_cmpuint (ow, ==, 1920); g_assert_cmpuint (oh, ==, 1080);
  g_assert_false (nfc_preset_output_size (NFC_CAT_VIDEO, "webm", SMALL, 1920, 1080, NULL, NULL));
  g_assert_true (nfc_preset_output_size (NFC_CAT_VIDEO, "gif", SMALL, 600, 799, &ow, &oh));
  g_assert_cmpuint (ow, ==, 480); g_assert_cmpuint (oh, ==, 639);
  g_assert_false (nfc_preset_output_size (NFC_CAT_VIDEO, "gif", SMALL, 480, 300, NULL, NULL));
  g_assert_false (nfc_preset_output_size (NFC_CAT_VIDEO, "gif", SMALL, 0, 0, NULL, NULL));
}

/* ---------------------------------------------------------------------
 * Upscale
 * --------------------------------------------------------------------- */

/* which presets a w x h image offers, as "a,b,c" of keys */
static char *
upscale_offered (guint64 w, guint64 h)
{
  GString *s = g_string_new (NULL);
  guint n;
  const NfcUpscalePreset *all = nfc_upscale_presets (&n);

  for (guint i = 0; i < n; i++)
    if (nfc_upscale_output_size (&all[i], w, h, NULL, NULL))
      g_string_append_printf (s, "%s%s", s->len ? "," : "", all[i].key);
  return g_string_free (s, FALSE);
}

static void
test_upscale_presets (void)
{
  guint n;
  const NfcUpscalePreset *p = nfc_upscale_presets (&n);
  const char *keys[] = { "2x", "3x", "4x", "5x", "6x", "8x" };
  const char *labels[] = { "2×", "3×", "4×", "5×", "6×", "8×" };
  const guint factors[] = { 2, 3, 4, 5, 6, 8 };
  /* the pass chains: one -s per pass; 5x is made as 6x and shrunk */
  const guint passes[][2] = { { 2, 0 }, { 3, 0 }, { 4, 0 }, { 3, 2 }, { 3, 2 }, { 4, 2 } };
  const guint chains[] = { 2, 3, 4, 6, 6, 8 };
  guint64 ow, oh;
  char *s;

  g_assert_cmpuint (n, ==, 6);
  for (guint i = 0; i < n; i++)
    {
      g_assert_cmpstr (p[i].key, ==, keys[i]);
      g_assert_cmpstr (p[i].label, ==, labels[i]);
      g_assert_cmpuint (p[i].factor, ==, factors[i]);
      g_assert_cmpuint (p[i].pass[0], ==, passes[i][0]);
      g_assert_cmpuint (p[i].pass[1], ==, passes[i][1]);
      g_assert_cmpuint (nfc_upscale_chain_factor (&p[i]), ==, chains[i]);
      g_assert_true (nfc_upscale_preset_from_key (keys[i]) == &p[i]);
    }
  g_assert_null (nfc_upscale_preset_from_key ("7x"));
  g_assert_null (nfc_upscale_preset_from_key (NULL));
  g_assert_cmpuint (nfc_upscale_chain_factor (NULL), ==, 0);

  /* exact size: width x factor */
  g_assert_true (nfc_upscale_output_size (&p[2], 640, 480, &ow, &oh));
  g_assert_cmpuint (ow, ==, 2560);
  g_assert_cmpuint (oh, ==, 1920);
  g_assert_true (nfc_upscale_output_size (&p[3], 87, 98, &ow, &oh));
  g_assert_cmpuint (ow, ==, 435);
  g_assert_cmpuint (oh, ==, 490);
  g_assert_true (nfc_upscale_output_size (&p[5], 640, 480, &ow, &oh));
  g_assert_cmpuint (ow, ==, 5120);
  g_assert_cmpuint (oh, ==, 3840);

  /* which scales a size offers: at most 32768 px per side and 400 megapixels */
  s = upscale_offered (640, 480);
  g_assert_cmpstr (s, ==, "2x,3x,4x,5x,6x,8x");
  g_free (s);
  s = upscale_offered (1000, 1000); /* 8x: 8000 x 8000 = 64 MP */
  g_assert_cmpstr (s, ==, "2x,3x,4x,5x,6x,8x");
  g_free (s);
  s = upscale_offered (5000, 4000); /* 4x: 320 MP; 5x: 500 MP; 6x: 30000 px, 720 MP; 8x: 40000 px */
  g_assert_cmpstr (s, ==, "2x,3x,4x");
  g_free (s);
  s = upscale_offered (4096, 4096); /* 8x is exactly 32768 px per side, but 1074 MP */
  g_assert_cmpstr (s, ==, "2x,3x,4x");
  g_free (s);
  s = upscale_offered (10000, 10000); /* 2x: exactly 400 MP */
  g_assert_cmpstr (s, ==, "2x");
  g_free (s);
  s = upscale_offered (10001, 10000);
  g_assert_cmpstr (s, ==, "");
  g_free (s);
  s = upscale_offered (16384, 1); /* a strip: 2x is exactly 32768 px, 3x is 49152 */
  g_assert_cmpstr (s, ==, "2x");
  g_free (s);
  s = upscale_offered (16385, 1);
  g_assert_cmpstr (s, ==, "");
  g_free (s);
  s = upscale_offered (4000, 1); /* 8x: 32000 px wide, 8 px high */
  g_assert_cmpstr (s, ==, "2x,3x,4x,5x,6x,8x");
  g_free (s);
  s = upscale_offered (4097, 1); /* 8x: 32776 px */
  g_assert_cmpstr (s, ==, "2x,3x,4x,5x,6x");
  g_free (s);
  /* unknown size: not "fits": the selection logic treats it as fitting, the job reads the size */
  g_assert_false (nfc_upscale_output_size (&p[0], 0, 480, NULL, NULL));
  g_assert_false (nfc_upscale_output_size (&p[0], 640, 0, NULL, NULL));
  g_assert_false (nfc_upscale_output_size (NULL, 640, 480, NULL, NULL));
  /* absurd sizes cannot overflow */
  g_assert_false (nfc_upscale_output_size (&p[5], G_MAXUINT64, G_MAXUINT64, &ow, &oh));
  g_assert_false (nfc_upscale_output_size (&p[5], G_MAXUINT64, 1, NULL, NULL));
  /* the exact size is reported also when it is too big (the toast says it) */
  g_assert_false (nfc_upscale_output_size (&p[2], 9000, 9000, &ow, &oh));
  g_assert_cmpuint (ow, ==, 36000);
  g_assert_cmpuint (oh, ==, 36000);

  /* tooltips: the exact size when known and fitting */
  s = nfc_upscale_tooltip (&p[2], 640, 480);
  g_assert_nonnull (strstr (s, "2560 × 1920 px"));
  g_free (s);
  s = nfc_upscale_tooltip (&p[2], 0, 0);
  g_assert_nonnull (strstr (s, "4 times as wide and tall"));
  g_free (s);
  s = nfc_upscale_tooltip (&p[5], 9000, 9000);
  g_assert_null (strstr (s, "×"));
  g_free (s);
  s = nfc_upscale_tooltip (NULL, 1, 1);
  g_assert_cmpstr (s, ==, "");
  g_free (s);
}

static void
test_upscale_targets (void)
{
  NfcUpscaleTarget t;
  const struct { const char *fmt, *key; } yes[] = {
    { "png", "png" }, { "jpg", "jpg" }, { "webp", "webp" },
    { "avif", "png" }, { "heic", "png" }, { "bmp", "png" }, { "tiff", "png" }, { "gif", "png" },
  };
  const char *no[] = { "svg", "ico", "mp4", "pdf", "docx", "xyz", "", NULL };

  for (guint i = 0; i < G_N_ELEMENTS (yes); i++)
    {
      g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, yes[i].fmt, 640, 480, &t));
      g_assert_cmpstr (t.key, ==, yes[i].key);
      g_assert_cmpstr (t.ext, ==, yes[i].key);
      g_assert_false (t.capped);
    }
  for (guint i = 0; no[i] != NULL; i++)
    g_assert_false (nfc_upscale_target (NFC_CAT_IMAGE, no[i], 0, 0, NULL));
  g_assert_false (nfc_upscale_target (NFC_CAT_IMAGE, NULL, 0, 0, NULL));
  /* only images: the same format keys of any other category are no upscale source */
  g_assert_false (nfc_upscale_target (NFC_CAT_VIDEO, "gif", 0, 0, NULL));
  g_assert_false (nfc_upscale_target (NFC_CAT_VIDEO, "png", 0, 0, NULL));
  g_assert_false (nfc_upscale_target (NFC_CAT_AUDIO, "mp3", 0, 0, NULL));

  /* WEBP holds 16383 px per side: a bigger result is a PNG */
  g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", 16383, 16383, &t));
  g_assert_false (t.capped);
  g_assert_cmpstr (t.key, ==, "webp");
  g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", 16384, 100, &t));
  g_assert_true (t.capped);
  g_assert_cmpstr (t.key, ==, "png");
  g_assert_cmpstr (t.ext, ==, "png");
  g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", 100, 16384, &t));
  g_assert_true (t.capped);
  g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", 0, 0, &t)); /* unknown: not capped */
  g_assert_false (t.capped);
  /* a PNG or JPG is never "capped" (the cap is WebP's) */
  g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "jpg", 30000, 30000, &t));
  g_assert_false (t.capped);
  g_assert_cmpstr (t.key, ==, "jpg");
  /* ... and the whole range: the 8x of a 2049 px strip is 16392 px */
  {
    guint64 ow, oh;

    g_assert_true (nfc_upscale_output_size (nfc_upscale_preset_from_key ("8x"), 2049, 100, &ow, &oh));
    g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", ow, oh, &t));
    g_assert_true (t.capped);
    g_assert_true (nfc_upscale_output_size (nfc_upscale_preset_from_key ("8x"), 2047, 100, &ow, &oh));
    g_assert_true (nfc_upscale_target (NFC_CAT_IMAGE, "webp", ow, oh, &t));
    g_assert_false (t.capped);
  }
}

static void
write_text (const char *path, const char *text)
{
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static void
test_upscale_config (void)
{
  char *m;

  /* what the file says */
  m = nfc_upscale_config_model ("upscale-model=remacri-4x\n");
  g_assert_cmpstr (m, ==, "remacri-4x");
  g_free (m);
  m = nfc_upscale_config_model ("# comment\n\n  upscale-model =  ultrasharp-4x  \nother=1\n");
  g_assert_cmpstr (m, ==, "ultrasharp-4x");
  g_free (m);
  m = nfc_upscale_config_model ("upscale-model=a\nupscale-model=b"); /* the last wins, no final newline */
  g_assert_cmpstr (m, ==, "b");
  g_free (m);
  m = nfc_upscale_config_model ("upscale-model=remacri-4x\r\nx=y\r\n"); /* CRLF */
  g_assert_cmpstr (m, ==, "remacri-4x");
  g_free (m);
  g_assert_null (nfc_upscale_config_model (""));
  g_assert_null (nfc_upscale_config_model (NULL));
  g_assert_null (nfc_upscale_config_model ("upscale-model=\n"));
  g_assert_null (nfc_upscale_config_model ("# upscale-model=remacri-4x\n"));
  g_assert_null (nfc_upscale_config_model ("upscale-modelx=remacri-4x\nupscale=remacri-4x\n"));
  g_assert_null (nfc_upscale_config_model ("no equals sign here\n"));
}

/* a fake Upscayl install: <dir>/bin/upscayl-bin and <dir>/models/<model>.{param,bin} */
static char *
make_fake_install (const char *const *models)
{
  char *dir = g_dir_make_tmp ("nfc-upscayl-XXXXXX", NULL);
  char *bin = g_build_filename (dir, "bin", NULL), *md = g_build_filename (dir, "models", NULL), *exe;

  g_assert_nonnull (dir);
  g_assert_cmpint (g_mkdir_with_parents (bin, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (md, 0755), ==, 0);
  exe = g_build_filename (bin, "upscayl-bin", NULL);
  write_text (exe, "#!/bin/sh\nexit 0\n");
  g_assert_cmpint (g_chmod (exe, 0755), ==, 0);
  for (guint i = 0; models != NULL && models[i] != NULL; i++)
    {
      char *p = g_strdup_printf ("%s/%s.param", md, models[i]);
      char *b = g_strdup_printf ("%s/%s.bin", md, models[i]);

      write_text (p, "7767517\n");
      write_text (b, "x");
      g_free (p);
      g_free (b);
    }
  g_free (bin);
  g_free (md);
  g_free (exe);
  return dir;
}

static void
test_upscale_resolve (void)
{
  const char *two[] = { "upscayl-standard-4x", "remacri-4x", NULL };
  const char *only_other[] = { "remacri-4x", NULL };
  char *dir = make_fake_install (two), *exe = g_build_filename (dir, "bin", "upscayl-bin", NULL);
  char *conf = g_build_filename (dir, "config", NULL), *md = g_build_filename (dir, "models", NULL);
  char *models, *model;

  /* no config: the default model, in <bindir>/../models */
  g_assert_true (nfc_upscale_resolve (exe, NULL, &models, &model));
  g_assert_cmpstr (models, ==, md);
  g_assert_cmpstr (model, ==, "upscayl-standard-4x");
  g_free (models);
  g_free (model);
  g_assert_true (nfc_upscale_resolve (exe, conf, &models, &model)); /* a config file that is not there */
  g_assert_cmpstr (model, ==, "upscayl-standard-4x");
  g_free (models);
  g_free (model);
  /* a valid choice */
  write_text (conf, "upscale-model=remacri-4x\n");
  g_assert_true (nfc_upscale_resolve (exe, conf, &models, &model));
  g_assert_cmpstr (model, ==, "remacri-4x");
  g_free (models);
  g_free (model);
  /* invalid choices fall back to the default: not installed, a path, empty, odd characters */
  {
    const char *bad[] = { "upscale-model=nosuch-4x\n", "upscale-model=../models/remacri-4x\n",
                          "upscale-model=/etc/passwd\n", "upscale-model=\n", "upscale-model=remacri 4x\n",
                          "upscale-model=.hidden\n", "upscale-model=remacri-4x.param\n", NULL };

    for (guint i = 0; bad[i] != NULL; i++)
      {
        write_text (conf, bad[i]);
        g_assert_true (nfc_upscale_resolve (exe, conf, &models, &model));
        g_assert_cmpstr (model, ==, "upscayl-standard-4x");
        g_free (models);
        g_free (model);
      }
  }
  /* only the .param exists: not a model */
  {
    char *b = g_build_filename (dir, "models", "remacri-4x.bin", NULL);

    write_text (conf, "upscale-model=remacri-4x\n");
    g_assert_cmpint (g_unlink (b), ==, 0);
    g_assert_true (nfc_upscale_resolve (exe, conf, &models, &model));
    g_assert_cmpstr (model, ==, "upscayl-standard-4x");
    g_free (models);
    g_free (model);
    g_free (b);
  }
  nfc_rm_rf (dir);
  g_free (dir);
  g_free (exe);
  g_free (conf);
  g_free (md);

  /* the default model is missing: the configured one is used, else nothing is offered */
  dir = make_fake_install (only_other);
  exe = g_build_filename (dir, "bin", "upscayl-bin", NULL);
  conf = g_build_filename (dir, "config", NULL);
  g_assert_false (nfc_upscale_resolve (exe, NULL, &models, &model));
  g_assert_null (models);
  g_assert_null (model);
  write_text (conf, "upscale-model=remacri-4x\n");
  g_assert_true (nfc_upscale_resolve (exe, conf, &models, &model));
  g_assert_cmpstr (model, ==, "remacri-4x");
  g_free (models);
  g_free (model);
  nfc_rm_rf (dir);
  g_free (dir);
  g_free (exe);
  g_free (conf);

  /* no models directory at all */
  dir = make_fake_install (NULL);
  exe = g_build_filename (dir, "bin", "upscayl-bin", NULL);
  md = g_build_filename (dir, "models", NULL);
  nfc_rm_rf (md);
  g_assert_false (nfc_upscale_resolve (exe, NULL, &models, &model));
  g_assert_null (models);
  g_assert_false (nfc_upscale_resolve (NULL, NULL, &models, &model));
  nfc_rm_rf (dir);
  g_free (dir);
  g_free (exe);
  g_free (md);
}

/* nfc_tools_init found the fake install of main (NFC_UPSCAYL_BIN, XDG_CONFIG_HOME) */
static const char *discovery_dir; /* g_free'd by main */

static void
test_upscale_discovery (void)
{
  char *exe = g_build_filename (discovery_dir, "bin", "upscayl-bin", NULL);
  char *md = g_build_filename (discovery_dir, "models", NULL);

  nfc_tools_init ();
  g_assert_cmpstr (nfc_tools.upscayl, ==, exe);
  g_assert_cmpstr (nfc_tools.upscale_models, ==, md);
  /* the configuration file of XDG_CONFIG_HOME names an installed model */
  g_assert_cmpstr (nfc_tools.upscale_model, ==, "remacri-4x");
  g_free (exe);
  g_free (md);
}

/* the lookup list of upscayl-bin: /opt, the system Flatpak, then the user Flatpak under the
 * XDG data home; the first executable one wins (the host's own /opt and system Flatpak are
 * replaced by missing paths, so this passes wherever it runs) */
static void
test_upscale_candidates (void)
{
  char *data = g_dir_make_tmp ("nfc-upfp-XXXXXX", NULL), *want, *dir, *got;
  char **c;

  g_assert_nonnull (data);
  c = nfc_upscayl_candidates (data);
  g_assert_cmpuint (g_strv_length (c), ==, 3);
  g_assert_cmpstr (c[0], ==, "/opt/Upscayl/resources/bin/upscayl-bin");
  g_assert_cmpstr (c[1], ==, "/var/lib/flatpak/app/org.upscayl.Upscayl/current/active/files/upscayl/resources/bin/upscayl-bin");
  want = g_build_filename (data, "flatpak", "app", "org.upscayl.Upscayl", "current", "active", "files",
                           "upscayl", "resources", "bin", "upscayl-bin", NULL);
  g_assert_cmpstr (c[2], ==, want);

  /* not installed: no candidate is taken (the PATH may still offer one: not under test) */
  g_free (c[0]);
  g_free (c[1]);
  c[0] = g_build_filename (data, "none-a", NULL);
  c[1] = g_build_filename (data, "none-b", NULL);
  got = nfc_find_upscayl_in (c);
  g_assert_true (got == NULL || (strcmp (got, want) != 0 && strstr (got, data) == NULL));
  g_free (got);

  /* a user Flatpak install is found, and a non-executable one is not */
  dir = g_path_get_dirname (want);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  write_text (want, "#!/bin/sh\nexit 0\n");
  g_assert_cmpint (g_chmod (want, 0644), ==, 0);
  got = nfc_find_upscayl_in (c);
  g_assert_true (got == NULL || strcmp (got, want) != 0);
  g_free (got);
  g_assert_cmpint (g_chmod (want, 0755), ==, 0);
  got = nfc_find_upscayl_in (c);
  g_assert_cmpstr (got, ==, want);
  g_free (got);

  nfc_rm_rf (data);
  g_strfreev (c);
  g_free (dir);
  g_free (want);
  g_free (data);
}

static void
use_fake_upscayl (void)
{
  use_fake_tools ();
  g_free (nfc_tools.upscayl);
  g_free (nfc_tools.upscale_models);
  g_free (nfc_tools.upscale_model);
  nfc_tools.upscayl = g_strdup ("/u/upscayl-bin");
  nfc_tools.upscale_models = g_strdup ("/u/models");
  nfc_tools.upscale_model = g_strdup ("m-4x");
}

/* the argv of an upscale of source fmt to output key with preset up_key, as one string */
static char *
upscale_str (const char *fmt, const char *key, const char *up_key, gboolean *dec)
{
  char *src = g_strdup_printf ("/s/a.%s", fmt), *out = g_strdup_printf ("/o/x.%s", key), *s;
  GPtrArray *at = nfc_build_attempts_upscale (fmt, key, nfc_upscale_preset_from_key (up_key), src, out,
                                              "/w", dec);

  g_assert_nonnull (at);
  g_assert_cmpuint (at->len, ==, 1); /* one attempt: no fallback to anything else */
  s = attempts_string (at);
  g_ptr_array_unref (at);
  g_free (src);
  g_free (out);
  return s;
}

#define UP_PREP "/bin/magick -quiet PNG:/s/a.png[0] -auto-orient -depth 8 PNG:/w/in.png"
#define UP_PASS(i, o, s) " && /u/upscayl-bin -i " i " -o " o " -m /u/models -n m-4x -s " s " -f png"

static void
test_upscale_argv (void)
{
  gboolean dec = TRUE;
  char *s;
  GPtrArray *at;
  const char *bad_in = "/w";

  use_fake_upscayl ();

  /* one pass: -s N, then the PNG out by ImageMagick (alpha kept, nothing else done to it) */
  s = upscale_str ("png", "png", "2x", &dec);
  g_assert_false (dec);
  g_assert_cmpstr (s, ==, UP_PREP UP_PASS ("/w/in.png", "/w/up1.png", "2")
                          " && /bin/magick -quiet PNG:/w/up1.png[0] PNG:/o/x.png");
  g_free (s);
  s = upscale_str ("png", "png", "3x", NULL);
  g_assert_nonnull (strstr (s, UP_PASS ("/w/in.png", "/w/up1.png", "3") " && /bin/magick"));
  g_assert_null (strstr (s, "up2.png"));
  g_free (s);
  s = upscale_str ("png", "png", "4x", NULL);
  g_assert_nonnull (strstr (s, UP_PASS ("/w/in.png", "/w/up1.png", "4") " && /bin/magick"));
  g_free (s);

  /* chains: the second pass reads the first's output; 6x = 3x then 2x, 8x = 4x then 2x */
  s = upscale_str ("png", "png", "6x", NULL);
  g_assert_cmpstr (s, ==, UP_PREP UP_PASS ("/w/in.png", "/w/up1.png", "3")
                          UP_PASS ("/w/up1.png", "/w/up2.png", "2")
                          " && /bin/magick -quiet PNG:/w/up2.png[0] PNG:/o/x.png");
  g_free (s);
  s = upscale_str ("png", "png", "8x", NULL);
  g_assert_cmpstr (s, ==, UP_PREP UP_PASS ("/w/in.png", "/w/up1.png", "4")
                          UP_PASS ("/w/up1.png", "/w/up2.png", "2")
                          " && /bin/magick -quiet PNG:/w/up2.png[0] PNG:/o/x.png");
  g_free (s);
  /* 5x: the 6x chain, shrunk by 5/6 with Lanczos */
  s = upscale_str ("png", "png", "5x", NULL);
  g_assert_cmpstr (s, ==, UP_PREP UP_PASS ("/w/in.png", "/w/up1.png", "3")
                          UP_PASS ("/w/up1.png", "/w/up2.png", "2")
                          " && /bin/magick -quiet PNG:/w/up2.png[0] -filter Lanczos -resize 83.3333333333% PNG:/o/x.png");
  g_free (s);

  /* the format of the result: JPG at the Best settings of a photo, flattened */
  s = upscale_str ("jpg", "jpg", "2x", NULL);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet JPEG:/s/a.jpg[0] -auto-orient -depth 8 PNG:/w/in.png"
                          UP_PASS ("/w/in.png", "/w/up1.png", "2")
                          " && /bin/magick -quiet PNG:/w/up1.png[0] -background white -alpha remove -alpha off -quality 95 -sampling-factor 4:4:4 JPEG:/o/x.jpg");
  g_free (s);
  /* WEBP at the Best settings of a photo; alpha is kept */
  s = upscale_str ("webp", "webp", "4x", NULL);
  g_assert_cmpstr (s, ==, "/bin/magick -quiet WEBP:/s/a.webp[0] -auto-orient -depth 8 PNG:/w/in.png"
                          UP_PASS ("/w/in.png", "/w/up1.png", "4")
                          " && /bin/magick -quiet PNG:/w/up1.png[0] -resize 16383x16383> -quality 90 -define webp:method=6 -define webp:use-sharp-yuv=true WEBP:/o/x.webp");
  g_assert_null (strstr (s, "-alpha"));
  g_free (s);
  /* the WEBP that became a PNG (see nfc_upscale_target): a plain PNG out */
  s = upscale_str ("webp", "png", "4x", NULL);
  g_assert_nonnull (strstr (s, "PNG:/w/up1.png[0] PNG:/o/x.png"));
  g_free (s);

  /* sources the backend cannot read go through ImageMagick, PNG out */
  {
    const struct { const char *fmt, *coder; } im[] = {
      { "bmp", "BMP" }, { "tiff", "TIFF" }, { "gif", "GIF" }, { "webp", "WEBP" }, { "jpg", "JPEG" },
    };

    for (guint i = 0; i < G_N_ELEMENTS (im); i++)
      {
        char *want = g_strdup_printf ("/bin/magick -quiet %s:/s/a.%s[0] -auto-orient -depth 8 PNG:/w/in.png",
                                      im[i].coder, im[i].fmt);

        s = upscale_str (im[i].fmt, "png", "2x", &dec);
        g_assert_true (g_str_has_prefix (s, want));
        g_assert_false (dec);
        g_free (want);
        g_free (s);
      }
  }
  /* HEIC and AVIF: the out-of-process decoder makes the PNG (oriented), no ImageMagick step */
  s = upscale_str ("heic", "png", "3x", &dec);
  g_assert_true (dec);
  g_assert_cmpstr (s, ==, "/bin/nfc-decode /s/a.heic /w/decoded.png"
                          UP_PASS ("/w/decoded.png", "/w/up1.png", "3")
                          " && /bin/magick -quiet PNG:/w/up1.png[0] PNG:/o/x.png");
  g_free (s);
  s = upscale_str ("avif", "png", "2x", &dec);
  g_assert_true (dec);
  g_assert_true (g_str_has_prefix (s, "/bin/nfc-decode /s/a.avif /w/decoded.png"));
  g_free (s);

  /* nothing for SVG, ICO, other output formats, or a missing tool */
  g_assert_null (nfc_build_attempts_upscale ("svg", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.svg",
                                             "/o/x.png", "/w", NULL));
  g_assert_null (nfc_build_attempts_upscale ("ico", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.ico",
                                             "/o/x.png", "/w", NULL));
  g_assert_null (nfc_build_attempts_upscale ("png", "avif", nfc_upscale_preset_from_key ("2x"), "/s/a.png",
                                             "/o/x.avif", "/w", NULL));
  g_assert_null (nfc_build_attempts_upscale ("mp4", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.mp4",
                                             "/o/x.png", "/w", NULL));
  g_assert_null (nfc_build_attempts_upscale ("png", "png", NULL, "/s/a.png", "/o/x.png", "/w", NULL));
  g_assert_null (nfc_build_attempts_upscale ("png", "png", nfc_upscale_preset_from_key ("2x"), NULL, "/o/x.png",
                                             "/w", NULL));
  {
    char *saved = nfc_tools.upscayl;

    nfc_tools.upscayl = NULL;
    g_assert_null (nfc_build_attempts_upscale ("png", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.png",
                                               "/o/x.png", "/w", NULL));
    nfc_tools.upscayl = saved;
    saved = nfc_tools.magick;
    nfc_tools.magick = NULL;
    g_assert_null (nfc_build_attempts_upscale ("png", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.png",
                                               "/o/x.png", "/w", NULL));
    nfc_tools.magick = saved;
    saved = nfc_tools.decode;
    nfc_tools.decode = NULL;
    g_assert_null (nfc_build_attempts_upscale ("heic", "png", nfc_upscale_preset_from_key ("2x"), "/s/a.heic",
                                               "/o/x.png", "/w", NULL));
    nfc_tools.decode = saved;
  }

  /* paths with spaces and ImageMagick's escapes stay single arguments, % doubled */
  at = nfc_build_attempts_upscale ("png", "png", nfc_upscale_preset_from_key ("2x"), "/s/my 100%.png",
                                   "/o/out 100%.png", "/w d%", NULL);
  g_assert_nonnull (at);
  {
    GPtrArray *a0 = at->pdata[0];
    char **prep = a0->pdata[0], **pass = a0->pdata[1], **fin = a0->pdata[2];

    g_assert_true (argv_has (prep, "PNG:/s/my 100%%.png[0]"));
    g_assert_true (argv_has (prep, "PNG:/w d%%/in.png"));
    g_assert_true (argv_has (pass, "/w d%/in.png")); /* not ImageMagick: no escaping */
    g_assert_true (argv_has (pass, "/w d%/up1.png"));
    g_assert_true (argv_has (fin, "PNG:/w d%%/up1.png[0]"));
    g_assert_true (argv_has (fin, "PNG:/o/out 100%%.png"));
  }
  g_ptr_array_unref (at);
  (void) bad_in;
}

static void
test_upscale_estimates (void)
{
  const NfcUpscalePreset *p4 = nfc_upscale_preset_from_key ("4x"), *p5 = nfc_upscale_preset_from_key ("5x");
  const NfcUpscalePreset *p8 = nfc_upscale_preset_from_key ("8x");
  guint64 gib = (guint64) 1024 * 1024 * 1024;

  /* the picture the passes make, at the B/px of ImageMagick's output format, plus 1 GiB */
  g_assert_cmpuint (nfc_upscale_peak_bytes (p4, 640, 480, "png"), ==, 2560 * 1920 * (guint64) 30 + gib);
  g_assert_cmpuint (nfc_upscale_peak_bytes (p4, 640, 480, "jpg"), ==, 2560 * 1920 * (guint64) 30 + gib);
  g_assert_cmpuint (nfc_upscale_peak_bytes (p4, 640, 480, "webp"), ==, 2560 * 1920 * (guint64) 44 + gib);
  /* 5x is made as 6x */
  g_assert_cmpuint (nfc_upscale_peak_bytes (p5, 640, 480, "png"), ==, 3840 * 2880 * (guint64) 30 + gib);
  g_assert_cmpuint (nfc_upscale_peak_bytes (p8, 640, 480, "png"), ==, 5120 * 3840 * (guint64) 30 + gib);
  g_assert_cmpuint (nfc_upscale_peak_bytes (p4, 0, 0, "png"), ==, gib); /* unknown size: the backend alone */
  /* a result at the limits (400 MP) is far beyond the small-image threshold: it is queued */
  g_assert_cmpuint (nfc_upscale_peak_bytes (nfc_upscale_preset_from_key ("2x"), 10000, 10000, "png"), >,
                    NFC_SMALL_IMAGE_PIXELS);

  /* 120 s plus 4 s per output megapixel, at most 2 h */
  g_assert_cmpuint (nfc_upscale_timeout_secs (0, 0), ==, 120);
  g_assert_cmpuint (nfc_upscale_timeout_secs (1000, 1000), ==, 124);
  g_assert_cmpuint (nfc_upscale_timeout_secs (2560, 1920), ==, 120 + 4 * 5);
  g_assert_cmpuint (nfc_upscale_timeout_secs (20000, 20000), ==, 120 + 4 * 400);
  g_assert_cmpuint (nfc_upscale_timeout_secs (G_MAXUINT64, G_MAXUINT64), ==, 7200);
}

static void
test_upscale_failure_messages (void)
{
  const struct { const char *text, *want; } c[] = {
    /* what upscayl-bin prints: no Vulkan at all ... */
    { "🚀 Starting Upscayl - Copyright © 2024\n✨ Detected scale x4\nvkCreateInstance failed -9\n"
      "vkCreateInstance failed -9\nvkCreateInstance failed -9\n🚨 Error: Invalid GPU Device\n",
      "no usable GPU: Upscale needs a Vulkan-capable graphics card and driver" },
    { "vkCreateInstance failed -9\n", "no usable GPU: Upscale needs a Vulkan-capable graphics card and driver" },
    /* ... out of video memory, with the device list it always prints ... */
    { "WARNING: radv is not a conformant Vulkan implementation, testing use only.\n"
      "[0 AMD Radeon RX 9070 XT (RADV GFX1201)]  queueC=1[4]  queueG=0[1]  queueT=0[1]\n"
      "[0 AMD Radeon RX 9070 XT (RADV GFX1201)]  subgroup=64  basic=1  vote=1  ballot=1  shuffle=1\n"
      "vkAllocateMemory failed -2\n",
      "the GPU ran out of memory (try a smaller image or a lower scale)" },
    { "vkCreateBuffer failed -2\n", "the GPU ran out of memory (try a smaller image or a lower scale)" },
    { "vkQueueSubmit failed -4\n", "the GPU stopped responding (try again, or a lower scale)" },
    { "vkWaitForFences failed -4\n", "the GPU stopped responding (try again, or a lower scale)" },
    /* ... a missing model (after the success line it prints anyway) ... */
    { "fopen /opt/Upscayl/resources/bin//opt/Upscayl/resources/models/nosuch.param failed\n"
      "🙌 Upscayled Successfully!\n",
      "the Upscale model files are missing (check upscale-model in the config)" },
    { "🚨 Error: Unknown model dir type. Make sure that the model directory is called 'models' with "
      "*.param and *.bin files inside it.\n",
      "the Upscale model files are missing (check upscale-model in the config)" },
    /* ... and an input it cannot read */
    { "[1 llvmpipe (LLVM 22.1.8, 256 bits)]  subgroup=8  basic=1\n🚨 Error: Couldn't read the image 'x.png'!\n",
      "the Upscale backend could not read the image" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (c); i++)
    {
      char *m = nfc_failure_message (c[i].text, "png");

      g_assert_cmpstr (m, ==, c[i].want);
      g_free (m);
    }
  /* nothing but the device list and chatter: no message (the runner says "exit code N") */
  {
    char *m = nfc_failure_message ("WARNING: radv is not a conformant Vulkan implementation\n"
                                   "[0 AMD Radeon]  queueC=1[4]\n✨ Using the default scale x4\n", "png");

    g_assert_null (m);
  }
  /* ffmpeg's bracketed lines are still read as before */
  {
    char *m = nfc_failure_message ("[dec:libopenh264 @ 0x55d3] Error while opening decoder\n", "mp4");

    g_assert_nonnull (m);
    g_assert_nonnull (strstr (m, "video decoder failed"));
    g_free (m);
  }
}

int
main (int argc, char **argv)
{
  /* a fake Upscayl install and a config file for the discovery test: set before anything
   * calls nfc_tools_init (), which reads them once */
  {
    const char *models[] = { "upscayl-standard-4x", "remacri-4x", NULL };
    char *exe, *cfgdir, *cfg;

    discovery_dir = make_fake_install (models);
    exe = g_build_filename (discovery_dir, "bin", "upscayl-bin", NULL);
    g_setenv ("NFC_UPSCAYL_BIN", exe, TRUE);
    cfgdir = g_build_filename (discovery_dir, "xdg", NULL);
    g_setenv ("XDG_CONFIG_HOME", cfgdir, TRUE);
    g_setenv ("XDG_CACHE_HOME", cfgdir, TRUE); /* keep the log out of the user's ~/.cache */
    g_free (cfgdir);
    cfgdir = g_build_filename (discovery_dir, "xdg", "nautilus-file-converter", NULL);
    g_assert_cmpint (g_mkdir_with_parents (cfgdir, 0755), ==, 0);
    cfg = g_build_filename (cfgdir, "config", NULL);
    write_text (cfg, "# the model of Upscale\nupscale-model=remacri-4x\n");
    g_free (cfg);
    g_free (cfgdir);
    g_free (exe);
  }
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
  g_test_add_func ("/core/header-handmade", test_header_handmade);
  g_test_add_func ("/core/header-isobmff-handmade", test_header_isobmff_handmade);
  g_test_add_func ("/core/header-tiff-handmade", test_header_tiff_handmade);
  g_test_add_func ("/core/header-files-and-fuzz", test_header_files_and_fuzz);
  g_test_add_func ("/core/resize-presets", test_resize_presets);
  g_test_add_func ("/core/resize-targets", test_resize_targets);
  g_test_add_func ("/core/resize-argv", test_resize_argv);
  g_test_add_func ("/core/small-labels", test_small_labels);
  g_test_add_func ("/core/upscale-discovery", test_upscale_discovery); /* first: reads what main set up */
  g_test_add_func ("/core/upscale-candidates", test_upscale_candidates);
  g_test_add_func ("/core/upscale-presets", test_upscale_presets);
  g_test_add_func ("/core/upscale-targets", test_upscale_targets);
  g_test_add_func ("/core/upscale-config", test_upscale_config);
  g_test_add_func ("/core/upscale-resolve", test_upscale_resolve);
  g_test_add_func ("/core/upscale-argv", test_upscale_argv);
  g_test_add_func ("/core/upscale-estimates", test_upscale_estimates);
  g_test_add_func ("/core/upscale-failure-messages", test_upscale_failure_messages);
  {
    int r = g_test_run ();

    nfc_rm_rf (discovery_dir);
    g_free ((char *) discovery_dir);
    return r;
  }
}
