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

/* ---- quality presets ----
 *
 * The lossy image and video targets (JPG, WEBP, AVIF, MP4, WebM, GIF) offer
 * four named presets in a submenu. BEST is what the extension has always done
 * (its argv is unchanged); the others trade quality for size and may reduce
 * the resolution (see README for the measured numbers). Every other target is
 * a plain one-click item and always converts as BEST. */
typedef enum {
  NFC_PRESET_BEST,
  NFC_PRESET_HIGH,
  NFC_PRESET_BALANCED,
  NFC_PRESET_SMALL,
  NFC_PRESET_N
} NfcPreset;

/* "best", "high", "balanced", "small" (menu item names) */
const char *nfc_preset_key (NfcPreset preset);
/* "Best", "High", "Balanced", "Small" (menu labels, notifications) */
const char *nfc_preset_label (NfcPreset preset);
/* TRUE (and *out set) if key names a preset */
gboolean    nfc_preset_from_key (const char *key, NfcPreset *out);
/* Does this target (category, target key) have a preset submenu? */
gboolean    nfc_target_has_presets (NfcCategory cat, const char *key);
/* One-line tooltip of the preset of a target: what it trades, including any
 * downscaling. Static string, never NULL. */
const char *nfc_preset_tooltip (NfcCategory cat, const char *key, NfcPreset preset);

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
  char *ffprobe; /* video size probe for the H.264 bitrate; optional */
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

/* Tests only: use `text` as the output of `ffmpeg -encoders` and mark the
 * ffmpeg probe done, without running ffmpeg. */
void     nfc_test_set_encoders (const char *text);
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

/* ---- image size, memory estimate and admission ----
 *
 * Several huge images converted at the same time would need far more memory
 * than the machine has. Before an image job starts, its size is read WITHOUT
 * decoding pixels (nfc_build_probe_argv: `magick identify -ping`, or
 * `nfc-decode --info` for HEIC/AVIF), the peak memory of the conversion is
 * estimated from it, and the runner only starts the job when the estimates of
 * the running jobs plus this one fit in the memory that is available. */

typedef struct {
  guint64  width, height;
  gboolean has_density;   /* the file carries a real resolution (see below) */
  double   xdpi, ydpi;    /* pixels per inch, valid when has_density */
} NfcImageInfo;

/* Parse the output of the probe command: "<w> <h>" or "<w> <h>|<x> <y> <units>"
 * (identify's %x %y %U). has_density is TRUE only for a resolution that
 * somebody wrote: units PixelsPerInch or PixelsPerCentimeter and not the 72
 * dpi ImageMagick and most tools stamp on everything. FALSE if unparsable. */
gboolean nfc_image_info_parse (const char *text, NfcImageInfo *out);

/* argv (g_strfreev) that prints the size of src's first frame; NULL if no tool
 * is available for it. */
char   **nfc_build_probe_argv (const char *fmt, const char *src);

/* argv (g_strfreev) that prints "<width>\n<height>\n" of src's first video
 * stream (nfc_image_info_parse reads it), for the libopenh264 bitrate; NULL if
 * ffprobe is missing. */
char   **nfc_build_video_probe_argv (const char *src);

/* Target bitrate (bits/s) for the libopenh264 fallback encoder of a video of
 * this size (info may be NULL: 1080p assumed). */
guint64  nfc_h264_bitrate (const NfcImageInfo *info);

/* Same as nfc_build_attempts, but info (may be NULL) is what the probe found:
 * a PDF made from an image without a resolution gets one that makes the page
 * fit an A4 sheet (see README). */
GPtrArray *nfc_build_attempts_info (NfcCategory cat, const char *fmt, const char *key,
                                    const char *src, const char *out,
                                    const char *workdir, const NfcImageInfo *info,
                                    gboolean *needs_decode);

/* Same as nfc_build_attempts_info with a quality preset (ignored, i.e. BEST, for
 * targets without presets: see nfc_target_has_presets). BEST gives exactly the
 * argv of nfc_build_attempts_info. */
GPtrArray *nfc_build_attempts_preset (NfcCategory cat, const char *fmt, const char *key,
                                      NfcPreset preset, const char *src, const char *out,
                                      const char *workdir, const NfcImageInfo *info,
                                      gboolean *needs_decode);

/* dpi that makes a w x h pixel image fit an A4 page (landscape for a wide
 * image), never below 96 and never resampling; whole number. */
guint    nfc_pdf_fit_dpi (guint64 w, guint64 h);

/* Estimated peak memory (bytes) of converting an image of this size from
 * source format fmt to target key. */
guint64  nfc_estimate_peak_bytes (const char *fmt, const char *key,
                                  const NfcImageInfo *info);

/* Images below this many pixels skip the accounting (they are never held back). */
#define NFC_SMALL_IMAGE_PIXELS ((guint64) 25 * 1000 * 1000)

/* MemAvailable from /proc/meminfo in bytes (0 if unreadable). The environment
 * variable NFC_TEST_MEMAVAILABLE_KB, when set, replaces it (tests only). */
guint64  nfc_mem_available_bytes (void);
/* The part of it the converters may use: 60 %. */
guint64  nfc_admission_budget_bytes (void);

/* May a job of `pixels` pixels and estimate `est` start while n_running jobs
 * hold `reserved` bytes? Yes when it is small, when nothing runs (a single huge
 * file always runs, alone), or when everything fits in `budget`. */
gboolean nfc_admission_allows (guint64 pixels, guint64 est, guint64 reserved,
                               guint n_running, guint64 budget);

/* Where ImageMagick spills pixels and where big per-job work directories go:
 * $XDG_CACHE_HOME/nautilus-file-converter/tmp (mode 0700, created if needed;
 * leftovers older than a day are removed once per process, in the background).
 * It is on the disk, unlike /tmp, which is usually RAM (tmpfs). Falls back to
 * the system temp directory (never NULL). g_free. */
char    *nfc_spill_dir (void);


/* ---- failure messages ---- */

/* The one line of a failed tool's stderr (or stdout) that tells the user what
 * went wrong, in words where the cause is known: picks the line that names the
 * cause (not the last one: the glycin loader ends with "Setting process memory
 * limit"), strips noise (program names, "[dec:x @ 0x..]", ImageMagick's
 * "@ error/file.c/Fn/123."), and maps known causes to plain text (missing HEVC
 * decoder for src_fmt "heic", broken ffmpeg decoder, out of memory, disk full).
 * src_fmt (may be NULL) is the canonical source format key. Never longer than
 * ~300 bytes. NULL if text has no non-blank line. g_free. */
char *nfc_failure_message (const char *text, const char *src_fmt);

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
