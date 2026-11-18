/* Image headers made by hand for the tests of nfc-header.h and of the menu
 * filtering that uses it: a minimal HEIF/AVIF (ISO base media boxes) and a minimal
 * TIFF, with any size, so that a 8000 x 6000 picture does not have to be encoded.
 * Static functions: include from one test source each. */
#pragma once

#include <glib.h>
#include <string.h>

static inline void
hf_u8 (GByteArray *a, guint v)
{
  guint8 b = (guint8) v;

  g_byte_array_append (a, &b, 1);
}

static inline void
hf_u16 (GByteArray *a, guint v)
{
  hf_u8 (a, v >> 8);
  hf_u8 (a, v & 0xff);
}

static inline void
hf_u32 (GByteArray *a, guint32 v)
{
  hf_u16 (a, v >> 16);
  hf_u16 (a, v & 0xffff);
}

static inline void
hf_cc (GByteArray *a, const char *cc)
{
  g_byte_array_append (a, (const guint8 *) cc, 4);
}

/* a box "type" around payload (which is freed) */
static inline GByteArray *
hf_box (const char *type, GByteArray *payload)
{
  GByteArray *a = g_byte_array_new ();

  hf_u32 (a, (guint32) payload->len + 8);
  hf_cc (a, type);
  g_byte_array_append (a, payload->data, payload->len);
  g_byte_array_unref (payload);
  return a;
}

/* append box (freed) to a */
static inline void
hf_add (GByteArray *a, GByteArray *box)
{
  g_byte_array_append (a, box->data, box->len);
  g_byte_array_unref (box);
}

static inline GByteArray *
hf_ispe (guint32 w, guint32 h)
{
  GByteArray *p = g_byte_array_new ();

  hf_u32 (p, 0); /* version and flags */
  hf_u32 (p, w);
  hf_u32 (p, h);
  return hf_box ("ispe", p);
}

/* irot: -1 none; 0..3 quarter turns */
typedef struct {
  const char *brand;  /* major brand of the ftyp: "avif", "heic", ... */
  guint32 w, h;       /* the primary item's ispe */
  int irot;           /* -1: none; 0..3 */
  gboolean clap;      /* a clean aperture of clap_w x clap_h, before irot */
  guint32 clap_w, clap_h;
  gboolean decoy;     /* a second item with a LARGER ispe (2w x 2h) and a smaller thumbnail item */
  gboolean pitm;      /* a pitm box naming the primary item (else: the largest ispe is taken) */
  gboolean wide_ipma; /* ipma version 1, flags 1: 32-bit item ids and 15-bit indices */
} HfHeif;

#define HF_HEIF_DEFAULT(b, w_, h_) \
  { (b), (w_), (h_), -1, FALSE, 0, 0, FALSE, TRUE, FALSE }

/* ftyp, meta (hdlr, pitm, iprp: ipco + ipma), then an mdat */
static inline GByteArray *
hf_make_heif (const HfHeif *o)
{
  GByteArray *file = g_byte_array_new (), *p, *ipco, *ipma, *meta;
  guint n_props = 0;
  guint prop_main, prop_decoy = 0, prop_thumb = 0, prop_irot = 0, prop_clap = 0;

  p = g_byte_array_new ();
  hf_cc (p, o->brand);
  hf_u32 (p, 0);
  hf_cc (p, "mif1");
  hf_cc (p, o->brand);
  hf_add (file, hf_box ("ftyp", p));

  ipco = g_byte_array_new ();
  if (o->decoy)
    {
      hf_add (ipco, hf_ispe (o->w / 4 + 1, o->h / 4 + 1)); /* a thumbnail */
      prop_thumb = ++n_props;
    }
  hf_add (ipco, hf_ispe (o->w, o->h));
  prop_main = ++n_props;
  if (o->decoy)
    {
      hf_add (ipco, hf_ispe (o->w * 2, o->h * 2));
      prop_decoy = ++n_props;
    }
  p = g_byte_array_new ();
  hf_u32 (p, 0); hf_u32 (p, 0x00000008); hf_u32 (p, 0x0001); /* junk property in between */
  hf_add (ipco, hf_box ("pixi", p));
  n_props++;
  if (o->clap)
    {
      p = g_byte_array_new ();
      hf_u32 (p, o->clap_w); hf_u32 (p, 1); hf_u32 (p, o->clap_h); hf_u32 (p, 1);
      hf_u32 (p, 0); hf_u32 (p, 1); hf_u32 (p, 0); hf_u32 (p, 1);
      hf_add (ipco, hf_box ("clap", p));
      prop_clap = ++n_props;
    }
  if (o->irot >= 0)
    {
      p = g_byte_array_new ();
      hf_u8 (p, (guint) o->irot);
      hf_add (ipco, hf_box ("irot", p));
      prop_irot = ++n_props;
    }

  ipma = g_byte_array_new ();
  hf_u8 (ipma, o->wide_ipma ? 1 : 0);
  hf_u8 (ipma, 0); hf_u8 (ipma, 0); hf_u8 (ipma, o->wide_ipma ? 1 : 0);
  hf_u32 (ipma, o->decoy ? 3 : 1);
  {
    guint ids[3] = { 1, 2, 3 }; /* 1: primary; 2: the larger decoy; 3: thumbnail */
    guint plist[3][4] = { { prop_main, prop_clap, prop_irot, 0 }, { prop_decoy, 0, 0, 0 },
                          { prop_thumb, 0, 0, 0 } };

    for (guint i = 0; i < (o->decoy ? 3u : 1u); i++)
      {
        guint cnt = 0;

        for (guint k = 0; k < 4; k++)
          cnt += plist[i][k] != 0;
        if (o->wide_ipma)
          hf_u32 (ipma, ids[i]);
        else
          hf_u16 (ipma, ids[i]);
        hf_u8 (ipma, cnt);
        for (guint k = 0; k < 4; k++)
          if (plist[i][k] != 0)
            {
              if (o->wide_ipma)
                hf_u16 (ipma, plist[i][k] | 0x8000); /* essential bit */
              else
                hf_u8 (ipma, plist[i][k] | 0x80);
            }
      }
  }

  meta = g_byte_array_new ();
  hf_u32 (meta, 0); /* version and flags */
  p = g_byte_array_new ();
  hf_u32 (p, 0); hf_u32 (p, 0); hf_cc (p, "pict"); hf_u32 (p, 0); hf_u32 (p, 0); hf_u32 (p, 0); hf_u8 (p, 0);
  hf_add (meta, hf_box ("hdlr", p));
  if (o->pitm)
    {
      p = g_byte_array_new ();
      hf_u32 (p, 0); /* version 0 */
      hf_u16 (p, 1);
      hf_add (meta, hf_box ("pitm", p));
    }
  p = g_byte_array_new ();
  hf_add (p, hf_box ("ipco", ipco));
  hf_add (p, hf_box ("ipma", ipma));
  hf_add (meta, hf_box ("iprp", p));
  hf_add (file, hf_box ("meta", meta));
  p = g_byte_array_new ();
  for (guint i = 0; i < 64; i++)
    hf_u8 (p, i * 7);
  hf_add (file, hf_box ("mdat", p));
  return file;
}

static inline void
hf_t16 (GByteArray *a, gboolean le, guint v)
{
  if (le)
    {
      hf_u8 (a, v & 0xff);
      hf_u8 (a, v >> 8);
    }
  else
    hf_u16 (a, v);
}

static inline void
hf_t32 (GByteArray *a, gboolean le, guint32 v)
{
  if (le)
    {
      hf_t16 (a, TRUE, v & 0xffff);
      hf_t16 (a, TRUE, v >> 16);
    }
  else
    hf_u32 (a, v);
}

static inline void
hf_t64 (GByteArray *a, gboolean le, guint64 v)
{
  hf_t32 (a, le, le ? (guint32) v : (guint32) (v >> 32));
  hf_t32 (a, le, le ? (guint32) (v >> 32) : (guint32) v);
}

/* A TIFF whose first IFD follows the header: a filler tag (254), ImageWidth,
 * ImageLength (SHORT when the value fits and short_ok, else LONG), and an
 * Orientation if orient > 0. le: II (else MM); big: BigTIFF (LONG8 sizes). */
static inline GByteArray *
hf_make_tiff (gboolean le, gboolean big, guint32 w, guint32 h, guint orient, gboolean short_ok)
{
  GByteArray *a = g_byte_array_new ();
  const guint tags[4] = { 254, 256, 257, 274 };
  const guint64 vals[4] = { 0, w, h, orient };
  guint n = orient > 0 ? 4 : 3;

  hf_u8 (a, le ? 'I' : 'M');
  hf_u8 (a, le ? 'I' : 'M');
  if (big)
    {
      hf_t16 (a, le, 43); hf_t16 (a, le, 8); hf_t16 (a, le, 0);
      hf_t64 (a, le, 16);
      hf_t64 (a, le, n);
    }
  else
    {
      hf_t16 (a, le, 42);
      hf_t32 (a, le, 8);
      hf_t16 (a, le, n);
    }
  for (guint i = 0; i < n; i++)
    {
      guint type;

      if (tags[i] == 274)
        type = 3;
      else if (tags[i] == 254)
        type = 4;
      else
        type = big && !short_ok ? 16 : short_ok && vals[i] <= 0xffff ? 3 : 4;
      hf_t16 (a, le, tags[i]);
      hf_t16 (a, le, type);
      if (big)
        hf_t64 (a, le, 1);
      else
        hf_t32 (a, le, 1);
      if (type == 3)
        {
          hf_t16 (a, le, (guint) vals[i]);
          hf_t16 (a, le, 0);
          if (big)
            hf_t32 (a, le, 0);
        }
      else if (type == 4)
        {
          hf_t32 (a, le, (guint32) vals[i]);
          if (big)
            hf_t32 (a, le, 0);
        }
      else
        hf_t64 (a, le, vals[i]);
    }
  if (big)
    hf_t64 (a, le, 0); /* no next IFD */
  else
    hf_t32 (a, le, 0);
  return a;
}
