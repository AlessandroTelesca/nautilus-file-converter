/* nautilus-file-converter: pure conversion logic (no Nautilus dependency).
 *
 * Classification, format tables, tool/codec discovery, argv builders and
 * output naming. The Nautilus glue lives in nfc-extension.c; the async
 * runner lives in nfc-convert.c.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NFC_CAT_IMAGE,
  NFC_CAT_AUDIO,
  NFC_CAT_VIDEO,
  NFC_CAT_DOC,
  NFC_CAT_SHEET,
  NFC_CAT_SLIDE,
  NFC_CAT_N
} NfcCategory;

typedef struct {
  const char *key;   /* canonical target key, e.g. "jpg" */
  const char *label; /* menu label */
  const char *ext;   /* output extension */
} NfcTarget;

/* Result of classifying a file. fmt is a static canonical key ("jpg", "heic"). */
typedef struct {
  NfcCategory cat;
  const char *fmt;
} NfcFormat;

typedef struct {
  char *magick;
  char *ffmpeg;
  char *soffice;
  char *notify;
} NfcTools;

/* Global tool paths; resolved by nfc_tools_init(). Tests may overwrite. */
extern NfcTools nfc_tools;

void nfc_tools_init (void);
/* Probe ffmpeg encoders / ImageMagick formats / GdkPixbuf loaders once.
 * Thread-safe; blocks until the (single) probe has finished. */
void nfc_probe_ensure (void);

gboolean nfc_have_encoder (const char *name);
gboolean nfc_magick_can_write (const char *im_format);
gboolean nfc_pixbuf_can_read (const char *name);

/* ---- classification and targets ---- */

gboolean    nfc_classify (const char *path, const char *mime, NfcFormat *out);
gboolean    nfc_category_available (NfcCategory cat);
const char *nfc_category_name (NfcCategory cat);
/* Targets offered for a source of category/fmt (own format removed). Array of
 * const NfcTarget *, free with g_ptr_array_unref(). */
GPtrArray  *nfc_targets_for (NfcCategory cat, const char *fmt);

/* ---- selection analysis (menu logic) ---- */

typedef struct {
  const char *uri;
  const char *mime;
  gboolean    is_dir;
} NfcSelItem;

typedef struct {
  NfcCategory cat;
  GPtrArray  *paths;   /* char *, owned */
  GPtrArray  *fmts;    /* const char *, static */
  GPtrArray  *targets; /* const NfcTarget *, static */
} NfcSelection;

/* TRUE (and a filled sel) if a menu should be shown for this selection. */
gboolean nfc_analyze_selection (const NfcSelItem *items, guint n, NfcSelection *sel);
void     nfc_selection_clear (NfcSelection *sel);

/* ---- command construction ---- */

/* Build the commands for one conversion.
 *
 * Returns a GPtrArray of attempts; each attempt is a GPtrArray of GStrv argv
 * that run sequentially. The first attempt that succeeds wins; later attempts
 * are fallbacks (MKV stream copy -> re-encode). NULL if a tool is missing.
 * *needs_decode is set when the source must first be decoded to
 * <workdir>/decoded.png through GdkPixbuf (HEIC/AVIF input).
 * For documents the basename of out must be "<source stem>.<ext>".
 */
GPtrArray *nfc_build_attempts (NfcCategory cat, const char *fmt, const char *key,
                               const char *src, const char *out,
                               const char *workdir, gboolean *needs_decode);

/* Decode src via GdkPixbuf (applying EXIF orientation) into a PNG. */
gboolean nfc_pixbuf_decode (const char *src, const char *dest, GError **error);

/* ---- output naming ---- */

char *nfc_candidate_name (const char *stem, const char *ext, guint n);
/* Hard-link tmp_path to the first free "stem[ (n)].ext" in directory (never
 * overwrites), then unlink tmp_path. Returns the final path, or NULL+error. */
char *nfc_publish (const char *tmp_path, const char *directory,
                   const char *stem, const char *ext, GError **error);
/* "<dir>/.<stem(<=80 bytes)>.nfc-<random>.<ext>" */
char *nfc_hidden_temp_name (const char *directory, const char *stem, const char *ext);
/* Basename without its last extension. */
char *nfc_stem_of (const char *path);

G_END_DECLS
