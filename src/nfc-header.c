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

/* ISO base media file format (AVIF, HEIC, HEIF): a file is a sequence of boxes
 * "size (4) type (4) [largesize (8) if size == 1] payload"; size 0 means "up to
 * the end of the file". The image size is an `ispe` property (FullBox: version and
 * flags, then width and height, 32 bits each) in meta > iprp > ipco. A file has
 * several (the tiles of a grid, a thumbnail), so the primary item's is looked
 * for: meta > pitm names the item, meta > iprp > ipma lists, for each item, the
 * 1-based indices of its properties in ipco. Of those, `clap` (clean aperture:
 * crops to the given size) and `irot` (angle in quarter turns: 1 and 3 swap
 * width and height) are applied in the order they are listed, the way a decoder
 * does, so the size is the one nfc-decode reports. If that chain does not lead to
 * an `ispe`, the largest `ispe` of the file is taken.
 *
 * The boxes are walked by fixed nested loops (no recursion), each loop moves at
 * least 8 bytes per step inside a range that is checked against the buffer, and a
 * box that is not entirely inside the buffer ends the search (meta is normally in
 * the first KiB; one that reaches past the buffer is not read). */
#define ISO_MAX_PROPS 512
#define ISO_MAX_IPMA 4

#define FOURCC(a, b, c, d) (((guint32) (a) << 24) | ((guint32) (b) << 16) | ((guint32) (c) << 8) | (guint32) (d))

enum { PROP_OTHER = 0, PROP_ISPE, PROP_IROT, PROP_CLAP };

typedef struct {
  guint8 kind;
  guint32 a, b; /* ispe, clap: width, height; irot: a = quarter turns (0..3) */
} IsoProp;

/* The next box of b[*pos..end): its type, its payload [*payload, *box_end), and
 * *pos moved behind it. FALSE for a header that does not fit, a size below the
 * header's own (< 8, or < 16 with a largesize) or one that reaches past end. */
static gboolean
iso_box (const guint8 *b, gsize end, gsize *pos, guint32 *type, gsize *payload, gsize *box_end)
{
  gsize p = *pos, hdr = 8;
  guint64 size;

  if (p > end || end - p < 8)
    return FALSE;
  size = be32 (b + p);
  *type = be32 (b + p + 4);
  if (size == 1)
    {
      if (end - p < 16)
        return FALSE;
      size = ((guint64) be32 (b + p + 8) << 32) | be32 (b + p + 12);
      hdr = 16;
    }
  else if (size == 0)
    size = end - p;
  if (size < hdr || size > end - p)
    return FALSE;
  *payload = p + hdr;
  *box_end = p + size;
  *pos = p + size;
  return TRUE;
}

static gboolean
iso_brand_known (guint32 v)
{
  return v == FOURCC ('a', 'v', 'i', 'f') || v == FOURCC ('a', 'v', 'i', 's') ||
         v == FOURCC ('h', 'e', 'i', 'c') || v == FOURCC ('h', 'e', 'i', 'x') ||
         v == FOURCC ('h', 'e', 'v', 'c') || v == FOURCC ('h', 'e', 'i', 'm') ||
         v == FOURCC ('h', 'e', 'i', 's') || v == FOURCC ('m', 'i', 'f', '1') ||
         v == FOURCC ('m', 's', 'f', '1');
}

/* A property box of ipco (payload b[p..e)) as an IsoProp. */
static void
iso_prop (const guint8 *b, guint32 type, gsize p, gsize e, IsoProp *pr)
{
  pr->kind = PROP_OTHER;
  pr->a = pr->b = 0;
  if (type == FOURCC ('i', 's', 'p', 'e') && e - p >= 12)
    {
      pr->a = be32 (b + p + 4);
      pr->b = be32 (b + p + 8);
      if (pr->a > 0 && pr->b > 0 && pr->a <= 0x7fffffff && pr->b <= 0x7fffffff)
        pr->kind = PROP_ISPE;
    }
  else if (type == FOURCC ('i', 'r', 'o', 't') && e - p >= 1)
    {
      pr->kind = PROP_IROT;
      pr->a = b[p] & 3;
    }
  else if (type == FOURCC ('c', 'l', 'a', 'p') && e - p >= 32)
    {
      guint32 wn = be32 (b + p), wd = be32 (b + p + 4), hn = be32 (b + p + 8), hd = be32 (b + p + 12);

      if (wd > 0 && hd > 0)
        {
          guint64 cw = ((guint64) wn + wd / 2) / wd, ch = ((guint64) hn + hd / 2) / hd;

          if (cw > 0 && ch > 0 && cw <= 0x7fffffff && ch <= 0x7fffffff)
            {
              pr->kind = PROP_CLAP;
              pr->a = (guint32) cw;
              pr->b = (guint32) ch;
            }
        }
    }
}

/* The size of the item `id` from the ipma box b[p..e): its properties applied in
 * order. FALSE if the item is not listed or has no ispe. */
static gboolean
iso_item_size (const guint8 *b, gsize p, gsize e, guint32 id, const IsoProp *props, guint n_props,
               guint32 *w, guint32 *h)
{
  guint8 version;
  guint32 flags, entries;
  gboolean wide;

  if (e - p < 8)
    return FALSE;
  version = b[p];
  flags = ((guint32) b[p + 1] << 16) | ((guint32) b[p + 2] << 8) | b[p + 3];
  wide = (flags & 1) != 0; /* 15-bit property indices */
  entries = be32 (b + p + 4);
  p += 8;
  for (guint32 i = 0; i < entries; i++)
    {
      guint32 item;
      guint count;
      gboolean have = FALSE;
      guint32 cw = 0, ch = 0;

      if (e - p < (version == 0 ? 3u : 5u))
        return FALSE;
      item = version == 0 ? be16 (b + p) : be32 (b + p);
      p += version == 0 ? 2 : 4;
      count = b[p++];
      if ((gsize) count * (wide ? 2 : 1) > e - p)
        return FALSE;
      for (guint k = 0; k < count; k++)
        {
          guint idx;

          if (wide)
            {
              idx = be16 (b + p) & 0x7fff;
              p += 2;
            }
          else
            idx = b[p++] & 0x7f;
          if (item != id || idx == 0 || idx > n_props)
            continue;
          switch (props[idx - 1].kind)
            {
            case PROP_ISPE:
              if (!have)
                {
                  cw = props[idx - 1].a;
                  ch = props[idx - 1].b;
                  have = TRUE;
                }
              break;
            case PROP_CLAP:
              if (have)
                {
                  cw = props[idx - 1].a;
                  ch = props[idx - 1].b;
                }
              break;
            case PROP_IROT:
              if (have && (props[idx - 1].a & 1))
                {
                  guint32 t = cw;

                  cw = ch;
                  ch = t;
                }
              break;
            default:
              break;
            }
        }
      if (item == id)
        {
          *w = cw;
          *h = ch;
          return have;
        }
    }
  return FALSE;
}

/* meta (a FullBox) of b[p..e): the primary item's size, else the largest ispe */
static gboolean
iso_meta (const guint8 *b, gsize p, gsize e, guint32 *w, guint32 *h)
{
  IsoProp props[ISO_MAX_PROPS];
  guint n_props = 0, n_ipma = 0;
  gsize ipma_p[ISO_MAX_IPMA], ipma_e[ISO_MAX_IPMA];
  gboolean have_ipco = FALSE, have_pitm = FALSE;
  guint32 pitm = 0, type;
  gsize pos, q, qe;

  if (e - p < 4)
    return FALSE;
  pos = p + 4; /* version and flags */
  while (pos < e)
    {
      if (!iso_box (b, e, &pos, &type, &q, &qe))
        return FALSE;
      if (type == FOURCC ('p', 'i', 't', 'm') && !have_pitm && qe - q >= 6)
        {
          if (b[q] == 0)
            {
              pitm = be16 (b + q + 4);
              have_pitm = TRUE;
            }
          else if (qe - q >= 8)
            {
              pitm = be32 (b + q + 4);
              have_pitm = TRUE;
            }
        }
      else if (type == FOURCC ('i', 'p', 'r', 'p'))
        {
          gsize ipos = q, iq, iqe;
          guint32 itype;

          while (ipos < qe)
            {
              if (!iso_box (b, qe, &ipos, &itype, &iq, &iqe))
                return FALSE;
              if (itype == FOURCC ('i', 'p', 'c', 'o') && !have_ipco)
                {
                  gsize cpos = iq, cq, cqe;
                  guint32 ctype;

                  have_ipco = TRUE;
                  while (cpos < iqe)
                    {
                      if (!iso_box (b, iqe, &cpos, &ctype, &cq, &cqe))
                        return FALSE;
                      if (n_props < ISO_MAX_PROPS)
                        iso_prop (b, ctype, cq, cqe, &props[n_props++]);
                    }
                }
              else if (itype == FOURCC ('i', 'p', 'm', 'a') && n_ipma < ISO_MAX_IPMA)
                {
                  ipma_p[n_ipma] = iq;
                  ipma_e[n_ipma++] = iqe;
                }
            }
        }
    }
  if (have_pitm)
    for (guint i = 0; i < n_ipma; i++)
      if (iso_item_size (b, ipma_p[i], ipma_e[i], pitm, props, n_props, w, h))
        return TRUE;
  {
    guint64 best = 0; /* fallback: the largest ispe */

    for (guint i = 0; i < n_props; i++)
      if (props[i].kind == PROP_ISPE && (guint64) props[i].a * props[i].b > best)
        {
          best = (guint64) props[i].a * props[i].b;
          *w = props[i].a;
          *h = props[i].b;
        }
    return best > 0;
  }
}

/* ftyp first, with a major or a compatible brand of AVIF/HEIF; then boxes up to meta */
static gboolean
parse_isobmff (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  gsize pos = 0, q, qe;
  guint32 type;
  gboolean brand = FALSE;

  if (!iso_box (b, len, &pos, &type, &q, &qe) || type != FOURCC ('f', 't', 'y', 'p') || qe - q < 8)
    return FALSE;
  brand = iso_brand_known (be32 (b + q));
  for (gsize k = q + 8; !brand && qe - k >= 4; k += 4)
    brand = iso_brand_known (be32 (b + k));
  if (!brand)
    return FALSE;
  while (pos < len)
    {
      if (!iso_box (b, len, &pos, &type, &q, &qe))
        return FALSE;
      if (type == FOURCC ('m', 'e', 't', 'a'))
        return iso_meta (b, q, qe, w, h) && *w > 0 && *h > 0;
    }
  return FALSE;
}

/* TIFF: "II" or "MM", 42, then the offset of the first IFD; BigTIFF has 43, an
 * offset size of 8, a zero, and a 64-bit offset. An IFD is a count (16 bits;
 * 64 for BigTIFF) of entries of 12 bytes (BigTIFF: 20): tag (2), type (2), count
 * (4; 8), value (4; 8) inline. ImageWidth (256) and ImageLength (257) are one
 * SHORT, LONG (or BigTIFF LONG8) each; the Orientation (274) 5..8 swaps them,
 * as nfc-decode does. */
typedef struct {
  gboolean le, big;
  guint64 ifd;
} TiffHead;

static guint32
tf16 (const guint8 *p, gboolean le)
{
  return le ? le16 (p) : be16 (p);
}

static guint32
tf32 (const guint8 *p, gboolean le)
{
  return le ? le32 (p) : be32 (p);
}

static guint64
tf64 (const guint8 *p, gboolean le)
{
  return le ? ((guint64) le32 (p + 4) << 32) | le32 (p) : ((guint64) be32 (p) << 32) | be32 (p + 4);
}

static gboolean
tiff_head (const guint8 *b, gsize len, TiffHead *t)
{
  if (len < 8)
    return FALSE;
  if (b[0] == 'I' && b[1] == 'I')
    t->le = TRUE;
  else if (b[0] == 'M' && b[1] == 'M')
    t->le = FALSE;
  else
    return FALSE;
  t->big = FALSE;
  if (tf16 (b + 2, t->le) == 42)
    t->ifd = tf32 (b + 4, t->le);
  else if (tf16 (b + 2, t->le) == 43)
    {
      if (len < 16 || tf16 (b + 4, t->le) != 8 || tf16 (b + 6, t->le) != 0)
        return FALSE;
      t->big = TRUE;
      t->ifd = tf64 (b + 8, t->le);
    }
  else
    return FALSE;
  return t->ifd >= (t->big ? 16 : 8);
}

/* the IFD whose first byte is b[0] (len bytes up to the end of what was read) */
static gboolean
tiff_ifd (const guint8 *b, gsize len, const TiffHead *t, guint32 *w, guint32 *h)
{
  gsize csz = t->big ? 8 : 2, esz = t->big ? 20 : 12, voff = t->big ? 12 : 8;
  guint64 count;
  guint64 width = 0, height = 0;
  guint orient = 1;

  if (len < csz)
    return FALSE;
  count = t->big ? tf64 (b, t->le) : tf16 (b, t->le);
  if (count > (len - csz) / esz)
    count = (len - csz) / esz;
  for (guint64 i = 0; i < count; i++)
    {
      const guint8 *e = b + csz + i * esz;
      guint tag = tf16 (e, t->le), type = tf16 (e + 2, t->le);
      guint64 n = t->big ? tf64 (e + 4, t->le) : tf32 (e + 4, t->le), v;

      if ((tag != 256 && tag != 257 && tag != 274) || n != 1)
        continue;
      if (type == 3)
        v = tf16 (e + voff, t->le);
      else if (type == 4 && tag != 274)
        v = tf32 (e + voff, t->le);
      else if (type == 16 && t->big && tag != 274)
        v = tf64 (e + voff, t->le);
      else
        continue;
      if (tag == 256)
        width = v;
      else if (tag == 257)
        height = v;
      else
        orient = (guint) v;
    }
  if (width == 0 || height == 0 || width > 0x7fffffff || height > 0x7fffffff)
    return FALSE;
  if (orient >= 5 && orient <= 8)
    {
      guint64 s = width;

      width = height;
      height = s;
    }
  *w = (guint32) width;
  *h = (guint32) height;
  return TRUE;
}

static gboolean
parse_tiff (const guint8 *b, gsize len, guint32 *w, guint32 *h)
{
  TiffHead t;

  if (!tiff_head (b, len, &t) || t.ifd >= len)
    return FALSE;
  return tiff_ifd (b + t.ifd, len - (gsize) t.ifd, &t, w, h);
}

gboolean
nfc_header_format_known (const char *fmt)
{
  return fmt != NULL && (g_str_equal (fmt, "png") || g_str_equal (fmt, "jpg") ||
                         g_str_equal (fmt, "gif") || g_str_equal (fmt, "webp") ||
                         g_str_equal (fmt, "bmp") || g_str_equal (fmt, "avif") ||
                         g_str_equal (fmt, "heic") || g_str_equal (fmt, "tiff"));
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
  else if (g_str_equal (fmt, "avif") || g_str_equal (fmt, "heic"))
    ok = parse_isobmff (buf, len, &ww, &hh);
  else if (g_str_equal (fmt, "tiff"))
    ok = parse_tiff (buf, len, &ww, &hh);
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

/* up to NFC_HEADER_MAX_BYTES of fd from offset off into buf; how many came */
static gsize
read_at (int fd, guint8 *buf, off_t off)
{
  gsize got = 0;

  while (got < NFC_HEADER_MAX_BYTES)
    {
      ssize_t n = pread (fd, buf + got, NFC_HEADER_MAX_BYTES - got, off + (off_t) got);

      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      got += (gsize) n;
    }
  return got;
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
  got = read_at (fd, buf, 0);
  if (g_str_equal (fmt, "tiff"))
    {
      TiffHead t;

      /* libtiff (so ImageMagick, scanners' software, ...) writes the pixel data
       * first and the IFD after it: that IFD is read from where it is, one more
       * bounded read of a regular file, instead of the file being "unknown" */
      if (tiff_head (buf, got, &t) && t.ifd >= got && t.ifd < (guint64) st.st_size)
        {
          guint32 ww = 0, hh = 0;
          gsize n = read_at (fd, buf, (off_t) t.ifd);

          ok = tiff_ifd (buf, n, &t, &ww, &hh);
          if (ok)
            {
              if (w != NULL)
                *w = ww;
              if (h != NULL)
                *h = hh;
            }
          close (fd);
          g_free (buf);
          return ok;
        }
    }
  close (fd);
  ok = nfc_header_parse (fmt, buf, got, w, h);
  g_free (buf);
  return ok;
}
