/* nfc-decode must refuse images that claim more than 100 megapixels, or more
 * than 30000 pixels on a side, quickly and without decoding them.
 *
 * The hostile files are real, valid but tiny (all-white, 1 bit) PNGs of huge
 * dimensions, written here with zlib. Env:
 * NFC_DECODE_HELPER = the nfc-decode binary; NFC_HEIC_SAMPLE (optional) = a
 * real 9 MP HEIC that must still decode.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <zlib.h>

static char *dir;

static void
put32 (guint8 *p, guint32 v)
{
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void
add_chunk (GByteArray *a, const char *type, const guint8 *data, guint32 len)
{
  guint8 hdr[8], crc[4];
  uLong c;

  put32 (hdr, len);
  memcpy (hdr + 4, type, 4);
  g_byte_array_append (a, hdr, 8);
  if (len > 0)
    g_byte_array_append (a, data, len);
  c = crc32 (0, (const Bytef *) type, 4);
  c = crc32 (c, data, len);
  put32 (crc, (guint32) c);
  g_byte_array_append (a, crc, 4);
}

/* A real, valid PNG of w x h pixels, 1 bit per pixel, all white. It deflates to
 * a few KiB however large the picture claims to be, and is written in well
 * under a second even for 20000 x 20000 (50 MB of raw rows, never kept). */
static char *
write_png (const char *name, guint32 w, guint32 h)
{
  static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
  GByteArray *png = g_byte_array_new (), *idat = g_byte_array_new ();
  guint8 ihdr[13], *row, out[16384];
  gsize rowlen = 1 + (w + 7) / 8;
  z_stream z;
  char *path = g_build_filename (dir, name, NULL);

  g_byte_array_append (png, sig, 8);
  put32 (ihdr, w);
  put32 (ihdr + 4, h);
  ihdr[8] = 1; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; /* 1 bit gray */
  add_chunk (png, "IHDR", ihdr, 13);

  row = g_malloc (rowlen);
  memset (row, 0xff, rowlen);
  row[0] = 0; /* filter: none */
  memset (&z, 0, sizeof z);
  g_assert_cmpint (deflateInit (&z, 1), ==, Z_OK);
  for (guint32 y = 0; y <= h; y++)
    {
      z.next_in = row;
      z.avail_in = y < h ? (uInt) rowlen : 0;
      do
        {
          z.next_out = out;
          z.avail_out = sizeof out;
          deflate (&z, y < h ? Z_NO_FLUSH : Z_FINISH);
          g_byte_array_append (idat, out, sizeof out - z.avail_out);
        }
      while (z.avail_out == 0);
    }
  deflateEnd (&z);
  add_chunk (png, "IDAT", idat->data, idat->len);
  add_chunk (png, "IEND", NULL, 0);
  g_assert_true (g_file_set_contents (path, (const char *) png->data, png->len, NULL));
  g_free (row);
  g_byte_array_unref (idat);
  g_byte_array_unref (png);
  return path;
}

static int
run_decode (const char *in, const char *out, char **err, gint64 *us)
{
  const char *helper = g_getenv ("NFC_DECODE_HELPER");
  char *argv[] = { (char *) helper, (char *) in, (char *) out, NULL };
  gint status = -1;
  gint64 t0 = g_get_monotonic_time ();

  g_assert_nonnull (helper);
  g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, err, &status, NULL));
  *us = g_get_monotonic_time () - t0;
  return g_spawn_check_wait_status (status, NULL) ? 0 : (WIFEXITED (status) ? WEXITSTATUS (status) : -1);
}

static void
expect_refused (const char *name, guint32 w, guint32 h)
{
  char *in = write_png (name, w, h), *out = g_build_filename (dir, "out.png", NULL), *err = NULL;
  gint64 us;
  int rc = run_decode (in, out, &err, &us);

  g_print ("%ux%u: exit %d in %.3f s: %s", w, h, rc, us / 1e6, err);
  g_assert_cmpint (rc, ==, 1);
  g_assert_nonnull (strstr (err, "refusing"));
  g_assert_nonnull (strstr (err, "100 megapixels"));
  g_assert_cmpint (us, <, 2 * G_USEC_PER_SEC);
  g_assert_false (g_file_test (out, G_FILE_TEST_EXISTS));
  g_free (in); g_free (out); g_free (err);
}

static void
test_refused (void)
{
  expect_refused ("20000x20000.png", 20000, 20000); /* 400 MP */
  expect_refused ("10001x10000.png", 10001, 10000); /* just over 100 MP */
  expect_refused ("30001x10.png", 30001, 10);       /* 0.3 MP, but too wide */
  expect_refused ("10x30001.png", 10, 30001);
}

static void
test_accepted (void)
{
  char *in = write_png ("10000x10000.png", 10000, 10000), *out = g_build_filename (dir, "o.png", NULL);
  char *err = NULL;
  gint64 us;
  int rc;

  /* exactly 100 MP (and 10000 on a side) is within the limit and decodes */
  rc = run_decode (in, out, &err, &us);
  g_print ("10000x10000: exit %d in %.3f s\n", rc, us / 1e6);
  g_assert_cmpint (rc, ==, 0);
  g_assert_true (g_file_test (out, G_FILE_TEST_IS_REGULAR));
  g_free (err); err = NULL;
  g_free (in);
  g_free (out);

  /* a real small PNG */
  {
    char *png = g_build_filename (dir, "real.png", NULL);
    char *mk[] = { "magick", "-size", "64x48", "xc:red", png, NULL };
    gint st = -1;

    out = g_build_filename (dir, "real-out.png", NULL);
    if (g_spawn_sync (NULL, mk, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      NULL, NULL, &st, NULL) && g_spawn_check_wait_status (st, NULL))
      {
        g_assert_cmpint (run_decode (png, out, &err, &us), ==, 0);
        g_assert_true (g_file_test (out, G_FILE_TEST_IS_REGULAR));
        g_free (err); err = NULL;
      }
    g_free (png); g_free (out);
  }

  /* the 9.1 MP HEIC sample still decodes */
  {
    const char *heic = g_getenv ("NFC_HEIC_SAMPLE");

    if (heic != NULL && g_file_test (heic, G_FILE_TEST_IS_REGULAR))
      {
        out = g_build_filename (dir, "heic-out.png", NULL);
        g_assert_cmpint (run_decode (heic, out, &err, &us), ==, 0);
        g_print ("HEIC sample decoded in %.2f s\n", us / 1e6);
        g_assert_true (g_file_test (out, G_FILE_TEST_IS_REGULAR));
        g_free (err); g_free (out);
      }
    else
      g_print ("HEIC sample: NOT FOUND (set NFC_HEIC_SAMPLE) - HEIC decode check skipped\n");
  }
}

/* "heic" is listed by `nfc-decode --formats` exactly when a HEIC really decodes
 * (an HEVC decoder is installed): the synthetic 16 x 16 sample in tests/data
 * is the same picture the helper embeds, so the two answers must agree. glycin
 * advertises heic even without HEVC, which is what this probe exists for. */
static void
test_heic_probe (void)
{
  const char *helper = g_getenv ("NFC_DECODE_HELPER");
  const char *data = g_getenv ("NFC_TEST_DATA");
  char *argv[] = { (char *) helper, (char *) "--formats", NULL };
  char *sample, *out, *err = NULL, *so = NULL;
  gint status = -1;
  gint64 us;
  gboolean decodes, listed;
  char **lines;

  g_assert_nonnull (helper);
  g_assert_nonnull (data);
  sample = g_build_filename (data, "data", "tiny-hevc.heic", NULL);
  out = g_build_filename (dir, "tiny-out.png", NULL);
  decodes = run_decode (sample, out, &err, &us) == 0;
  g_free (err); err = NULL;
  g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &so, NULL, &status, NULL));
  g_assert_true (g_spawn_check_wait_status (status, NULL));
  lines = g_strsplit (so, "\n", -1);
  listed = g_strv_contains ((const char *const *) lines, "heic");
  g_assert_true (g_strv_contains ((const char *const *) lines, "png"));
  g_print ("HEIC probe: sample decodes=%d, --formats lists heic=%d\n", decodes, listed);
  g_assert_cmpint (listed, ==, decodes);
  g_strfreev (lines);
  g_free (so); g_free (sample); g_free (out);
}

int
main (int argc, char **argv)
{
  char *tmpl;
  int rc;

  g_test_init (&argc, &argv, NULL);
  tmpl = g_build_filename (g_get_tmp_dir (), "nfc-decode-XXXXXX", NULL);
  dir = g_mkdtemp (tmpl);
  g_assert_nonnull (dir);
  g_test_add_func ("/decode/refuses-huge", test_refused);
  g_test_add_func ("/decode/accepts-limit-and-real-files", test_accepted);
  g_test_add_func ("/decode/heic-probe", test_heic_probe);
  rc = g_test_run ();
  {
    char *rm[] = { "rm", "-rf", dir, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (dir);
  return rc;
}
