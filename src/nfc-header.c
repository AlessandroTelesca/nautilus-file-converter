/* nautilus-file-converter: image size from the first bytes of a file. See nfc-header.h.
 *
 * Every accessor checks the offset against len before it reads; sizes taken from
 * the file (a JPEG segment length) are compared as `seglen > len - i`, never as
 * `i + seglen > len`, so nothing overflows. */
#define _GNU_SOURCE
#include "nfc-header.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static guint32
be16 (const guint8 *p)
{
  return ((guint32) p[0] << 8) | p[1];
}

static guint32
be32 (const guint8 *p)
{
  return ((guint32) p[0] << 24) | ((guint32) p[1] << 16) | ((guint32) p[2] << 8) | p[3];
}

static guint32
le16 (const guint8 *p)
{
  return ((guint32) p[1] << 8) | p[0];
}

static guint32
le24 (const guint8 *p)
{
  return ((guint32) p[2] << 16) | ((guint32) p[1] << 8) | p[0];
}

static guint32
le32 (const guint8 *p)
{
  return ((guint32) p[3] << 24) | ((guint32) p[2] << 16) | ((guint32) p[1] << 8) | p[0];
}

/* PNG: 8-byte signature, then the IHDR chunk: length 13, "IHDR", width, height
 * (big endian). The size must fit the 31 bits the specification allows. */
static gboolean
parse_png (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  static const guint8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

  if (len < 24 || memcmp (b, sig, 8) != 0 || be32 (b + 8) != 13 ||
      memcmp (b + 12, "IHDR", 4) != 0)
    return FALSE;
  *w = be32 (b + 16);
  *h = be32 (b + 20);
  return *w > 0 && *h > 0 && *w <= 0x7fffffff && *h <= 0x7fffffff;
}

/* GIF: "GIF87a" / "GIF89a", then the logical screen size (little endian). */
static gboolean
parse_gif (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  if (len < 10 || (memcmp (b, "GIF87a", 6) != 0 && memcmp (b, "GIF89a", 6) != 0))
    return FALSE;
  *w = le16 (b + 6);
  *h = le16 (b + 8);
  return *w > 0 && *h > 0;
}

/* BMP: "BM", file header (14 bytes), then the DIB header, whose first field is
 * its own size: 12 (OS/2: 16-bit width and height) or 16 and up (32-bit signed;
 * a negative height means top-down). */
static gboolean
parse_bmp (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  guint32 hs;

  if (len < 22 || b[0] != 'B' || b[1] != 'M')
    return FALSE;
  hs = le32 (b + 14);
  if (hs == 12)
    {
      *w = le16 (b + 18);
      *h = le16 (b + 20);
    }
  else if (hs >= 16 && len >= 26)
    {
      gint32 sw = (gint32) le32 (b + 18), sh = (gint32) le32 (b + 22);
      gint64 ah = sh < 0 ? -(gint64) sh : sh;

      if (sw <= 0 || ah == 0)
        return FALSE;
      *w = (guint32) sw;
      *h = (guint32) ah;
    }
  else
    return FALSE;
  return *w > 0 && *h > 0;
}

/* WebP: RIFF container, "WEBP", then the first chunk: lossy "VP8 " (frame
 * header after the 3-byte tag and the 9d 01 2a start code: 14-bit width and
 * height), lossless "VP8L" (signature 0x2f, then two 14-bit values minus one)
 * or extended "VP8X" (24-bit canvas width and height minus one). */
static gboolean
parse_webp (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  if (len < 25 || memcmp (b, "RIFF", 4) != 0 || memcmp (b + 8, "WEBP", 4) != 0)
    return FALSE;
  if (memcmp (b + 12, "VP8 ", 4) == 0)
    {
      if (len < 30 || b[23] != 0x9d || b[24] != 0x01 || b[25] != 0x2a)
        return FALSE;
      *w = le16 (b + 26) & 0x3fff;
      *h = le16 (b + 28) & 0x3fff;
    }
  else if (memcmp (b + 12, "VP8L", 4) == 0)
    {
      guint32 bits;

      if (b[20] != 0x2f)
        return FALSE;
      bits = le32 (b + 21);
      *w = (bits & 0x3fff) + 1;
      *h = ((bits >> 14) & 0x3fff) + 1;
    }
  else if (memcmp (b + 12, "VP8X", 4) == 0)
    {
      if (len < 30)
        return FALSE;
      *w = le24 (b + 24) + 1;
      *h = le24 (b + 27) + 1;
    }
  else
    return FALSE;
  return *w > 0 && *h > 0;
}

/* JPEG: SOI, then segments "FF <marker> <length (2, includes itself)> <data>".
 * The size is in the first start-of-frame (SOF0..SOF15 but DHT, JPG and DAC):
 * precision (1), height (2), width (2). APPn, COM, DQT, ... are skipped by their
 * length. The scan (SOS) or the end of the data before a frame header: not
 * found. */
static gboolean
parse_jpeg (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  gsize i = 2;

  if (len < 4 || b[0] != 0xff || b[1] != 0xd8)
    return FALSE;
  for (;;)
    {
      guint8 m;
      guint32 seglen;

      if (i >= len || b[i] != 0xff)
        return FALSE;
      while (i < len && b[i] == 0xff) /* any number of fill bytes */
        i++;
      if (i >= len)
        return FALSE;
      m = b[i++];
      if (m == 0x00)
        return FALSE;
      if (m == 0x01 || (m >= 0xd0 && m <= 0xd8)) /* TEM, RSTn, SOI: no length */
        continue;
      if (m == 0xd9 || m == 0xda) /* EOI, SOS: the frame header did not come */
        return FALSE;
      if (len - i < 2)
        return FALSE;
      seglen = be16 (b + i);
      if (seglen < 2)
        return FALSE;
      if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc)
        {
          if (seglen < 8 || len - i < 7)
            return FALSE;
          *h = be16 (b + i + 3);
          *w = be16 (b + i + 5);
          return *w > 0 && *h > 0; /* a height of 0 is defined by a later DNL: unknown */
        }
      if (seglen > len - i)
        return FALSE;
      i += seglen;
    }
}

gboolean
nfc_header_format_known (const char *fmt)
{
  return fmt != NULL && (g_str_equal (fmt, "png") || g_str_equal (fmt, "jpg") ||
                         g_str_equal (fmt, "gif") || g_str_equal (fmt, "webp") ||
                         g_str_equal (fmt, "bmp"));
}

gboolean
nfc_header_parse (const char *fmt, const guint8 *buf, gsize len, guint32 *w, guint32 *h)
{
  guint32 ww = 0, hh = 0;
  gboolean ok;

  if (fmt == NULL || buf == NULL)
    return FALSE;
  if (g_str_equal (fmt, "png"))
    ok = parse_png (buf, len, &ww, &hh);
  else if (g_str_equal (fmt, "jpg"))
    ok = parse_jpeg (buf, len, &ww, &hh);
  else if (g_str_equal (fmt, "gif"))
    ok = parse_gif (buf, len, &ww, &hh);
  else if (g_str_equal (fmt, "webp"))
    ok = parse_webp (buf, len, &ww, &hh);
  else if (g_str_equal (fmt, "bmp"))
    ok = parse_bmp (buf, len, &ww, &hh);
  else
    ok = FALSE;
  if (ok)
    {
      if (w != NULL)
        *w = ww;
      if (h != NULL)
        *h = hh;
    }
  return ok;
}

gboolean
nfc_header_read_file (const char *path, const char *fmt, guint32 *w, guint32 *h)
{
  guint8 *buf;
  struct stat st;
  gsize got = 0;
  gboolean ok = FALSE;
  int fd;

  if (path == NULL || !nfc_header_format_known (fmt))
    return FALSE;
  /* O_NONBLOCK: a FIFO swapped in for the file after Nautilus statted it must
   * not hang the open (it has no effect on a regular file) */
  fd = open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0)
    return FALSE;
  if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode) || st.st_size <= 0)
    {
      close (fd);
      return FALSE;
    }
  buf = g_malloc (NFC_HEADER_MAX_BYTES);
  while (got < NFC_HEADER_MAX_BYTES)
    {
      ssize_t n = read (fd, buf + got, NFC_HEADER_MAX_BYTES - got);

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      got += (gsize) n;
    }
  close (fd);
  ok = nfc_header_parse (fmt, buf, got, w, h);
  g_free (buf);
  return ok;
}
