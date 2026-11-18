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
  char *decode; /* nfc-decode helper (HEIC/AVIF decode, GdkPixbuf format list) */
} NfcTools;

/* Global tool paths; resolved by nfc_tools_init(). Tests may overwrite. */
extern NfcTools nfc_tools;

void nfc_tools_init (void);

/* ---- capability probes ----
 *
 * Which ffmpeg encoders / ImageMagick formats / GdkPixbuf loaders exist is
 * found by running the tools (`ffmpeg -encoders`, `magick -list format`,
 * `nfc-decode --formats`). The three probes are independent: each runs in its
 * own background thread, with a timeout (10 s) and a kill of the whole process
 * group, so a hung tool can neither block Nautilus nor hide targets that need
 * a different tool. A probe that fails or times out counts as "tool
 * unavailable" for the rest of the session (and is logged).
 *
 * Nothing the extension calls from the Nautilus main thread ever waits:
 * the query functions below return FALSE while their probe is unfinished, and
 * nfc_probe_done() says whether FALSE means "no" or "not yet known". */
typedef enum {
  NFC_PROBE_FFMPEG,
  NFC_PROBE_MAGICK,
  NFC_PROBE_PIXBUF,
  NFC_PROBE_N
} NfcProbe;

/* Start the probes if not started yet. Never blocks. */
void     nfc_probe_start (void);
/* Has this probe finished (successfully or not)? Never blocks. */
gboolean nfc_probe_done (NfcProbe probe);
/* Start the probes and block until all have finished (bounded by the probe
 * timeout). For tests and command line tools only, never from Nautilus. */
void     nfc_probe_ensure (void);

gboolean nfc_have_encoder (const char *name);
gboolean nfc_magick_can_write (const char *im_format);
gboolean nfc_magick_can_read (const char *im_format);
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

/* TRUE (and a filled sel) if a menu should be shown for this selection.
 *
 * NEVER BLOCKS: it is called from the Nautilus main thread. Targets whose
 * capability probe has not finished yet are simply left out (documents,
 * spreadsheets and presentations need no probe and appear immediately; image,
 * audio and video targets appear as soon as their own probe is done, normally
 * within a fraction of a second of Nautilus starting). If nothing is known to
 * be available yet the result is FALSE, i.e. no menu at all rather than an
 * empty or half-built one. A probe that timed out counts as "unavailable". */
gboolean nfc_analyze_selection (const NfcSelItem *items, guint n, NfcSelection *sel);
void     nfc_selection_clear (NfcSelection *sel);

/* ---- command construction ---- */

/* Build the commands for one conversion.
 *
 * Returns a GPtrArray of attempts; each attempt is a GPtrArray of GStrv argv
 * that run sequentially. The first attempt that succeeds wins; later attempts
 * are fallbacks (MKV stream copy -> re-encode). NULL if a tool is missing.
 * *needs_decode is set when the source is HEIC/AVIF: the first step of the
 * attempt is then `nfc-decode <src> <workdir>/decoded.png` (out-of-process
 * GdkPixbuf, see src/nfc-decode.c) and ImageMagick reads the decoded PNG.
 * For documents the basename of out must be "<source stem>.<ext>".
 */
GPtrArray *nfc_build_attempts (NfcCategory cat, const char *fmt, const char *key,
                               const char *src, const char *out,
                               const char *workdir, gboolean *needs_decode);

/* ---- output naming ---- */

char *nfc_candidate_name (const char *stem, const char *ext, guint n);
/* Move tmp_path to the first free "stem[ (n)].ext" in directory, never
 * overwriting anything (also not a dangling symlink). Returns the final path,
 * or NULL+error. Strategy: link() + unlink() (atomic, fails with EEXIST); on
 * filesystems without hard links renameat2(RENAME_NOREPLACE); where that is
 * unsupported too, an O_CREAT|O_EXCL placeholder followed by rename(). */
typedef enum {
  NFC_PUBLISH_AUTO,
  NFC_PUBLISH_NO_LINK,             /* tests: behave as if link() is unsupported */
  NFC_PUBLISH_NO_LINK_NO_RENAME2   /* tests: ... and renameat2 flags too */
} NfcPublishMode;

char *nfc_publish (const char *tmp_path, const char *directory,
                   const char *stem, const char *ext, GError **error);
char *nfc_publish_with_mode (const char *tmp_path, const char *directory,
                             const char *stem, const char *ext,
                             NfcPublishMode mode, GError **error);
/* "<dir>/.<stem(<=80 bytes)>.nfc-<random>.<ext>" */
char *nfc_hidden_temp_name (const char *directory, const char *stem, const char *ext);
/* Basename without its last extension. */
char *nfc_stem_of (const char *path);

G_END_DECLS
