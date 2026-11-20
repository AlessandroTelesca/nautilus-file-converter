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
/* Menu label of a preset, saying what Small does to the size: "Best", "High",
 * "Balanced", and "Small (max 2560 px)" for JPG/WEBP/AVIF, "Small (max 1080p)" for
 * MP4/WebM, "Small (480 px)" for GIF. key NULL: the Compress entry ("Small (max * 2560 px / 1080p)"). The item NAME stays nfc_preset_key (). Static string. */
const char *nfc_preset_menu_label (NfcCategory cat, const char *key, NfcPreset preset);
/* Did this preset scale a w x h source of target (cat, key) down, and to what?
 * TRUE (and *ow, *oh) only for Small when the source is larger than its limit
 * (images: long side 2560 px; MP4/WebM: short side 1080, even; GIF: width 480);
 * FALSE otherwise. For the toast: "Small, 2560×1707". */
gboolean    nfc_preset_output_size (NfcCategory cat, const char *key, NfcPreset preset,
                                    guint64 w, guint64 h, guint64 *ow, guint64 *oh);
/* One-line tooltip of the preset of a target: what it trades, including any
 * downscaling. Static string, never NULL. */
const char *nfc_preset_tooltip (NfcCategory cat, const char *key, NfcPreset preset);

/* ---- keep only what is smaller ----
 *
 * "Reference size + keep only if smaller": an output is worth keeping only if
 * it is strictly smaller than some reference (the source file for Compress, and
 * later for Resize/Upscale; the other candidate when two encodes compete).
 * nfc_is_smaller is the single rule; the runner applies it
 *   - to a whole job: NfcConvertOptions.reference_size (nfc-convert.h): an
 *     output that is not smaller than the reference is deleted and the job ends
 *     with NFC_CONVERT_ERROR_NOT_SMALLER, and
 *   - inside an attempt, between candidate files: the internal step
 *     NFC_STEP_KEEP_SMALLEST (see below). */
gboolean nfc_is_smaller (gint64 size, gint64 reference);

/* An internal "step" of an attempt that is not a program: argv is
 * { NFC_STEP_KEEP_SMALLEST, out, other1, other2, ..., NULL }. The runner keeps
 * the smallest of out and the others (that exist and are not empty) at `out`
 * (a tie keeps out) and deletes all the others. All files must be on the same
 * file system (the winner is renamed onto `out`). It also deletes them when the
 * job ends early, whatever the reason. */
#define NFC_STEP_KEEP_SMALLEST "nfc:keep-smallest"

/* ---- Compress: a smaller copy in the same format ----
 *
 * Offered for the formats that have presets: JPG, WEBP, AVIF (images), MP4/M4V
 * (H.264), WebM (VP9) and GIF (animated, converted with the GIF presets). The
 * copy is the same format with the preset's settings, named "<name> (compressed).<ext>",
 * and is only kept when it is smaller than the original. */
typedef struct {
  NfcCategory cat;     /* category to convert in: a GIF image is compressed as video */
  const char *key;     /* target key ("jpg", "mp4", "gif") */
  const char *ext;     /* output extension (a .jpeg or .m4v source gets .jpg / .mp4) */
} NfcCompressTarget;

/* The compress target of a source of category cat and canonical format fmt;
 * FALSE if that format cannot be compressed (PNG, HEIC, MKV, audio, ...). Static
 * table: says nothing about whether the tools are installed. */
gboolean nfc_compress_target (NfcCategory cat, const char *fmt, NfcCompressTarget *out);
/* One-line tooltip of a Compress preset (HIGH, BALANCED or SMALL); "" otherwise. */
const char *nfc_compress_tooltip (NfcPreset preset);

/* ---- Resize: a smaller copy in the same format ----
 *
 * "Resize" > 1080p (1920 px) / 50% / ...: every selected file becomes a copy in
 * its own format, named "<name> (1080p).<ext>" / "<name> (50%).<ext>", with the
 * pixel size of the preset and the Best quality of that format (lossless stays
 * lossless). Only presets that make the picture STRICTLY smaller are offered
 * (nfc_resize_output_size).
 *
 * Images (and GIF, resized by ffmpeg as an animation: same frames and timing):
 * the named sizes are the LONG side (4K 3840, 1440p 2560, 1080p 1920, 720p 1280,
 * 480p 854), aspect ratio kept, plus 50% and 25% (not below 16 px on the short
 * side). Videos: 1080p, 720p, 480p are the SHORT side (video convention: 1080p
 * is 1080 lines, so a portrait phone video is 1080 px wide), even dimensions. */

#define NFC_RESIZE_MIN_SIDE 16 /* a percent preset must leave at least this on the short side */

typedef struct {
  const char *key;    /* menu item name part and CLI key: "1080p", "50pct" */
  const char *label;  /* menu label: "1080p (1920 px)" */
  const char *name;   /* "1080p", "50%": the name suffix " (1080p)" and the notifications */
  guint       side;   /* > 0: target side in pixels (long side; short side if short_side) */
  guint       percent; /* > 0 (and side == 0): scale factor in percent */
  gboolean    short_side;
} NfcResizePreset;

/* The presets of a source category (NFC_CAT_IMAGE or NFC_CAT_VIDEO; none, with
 * *n = 0, for any other), largest first. Static table. */
const NfcResizePreset *nfc_resize_presets (NfcCategory cat, guint *n);
const NfcResizePreset *nfc_resize_preset_from_key (NfcCategory cat, const char *key);

/* The size a w x h source gets with preset p. TRUE only when that is strictly
 * smaller (long side for named image presets, short side for video, both sides
 * for percent) and usable; FALSE otherwise ("already 1080p or smaller").
 * ow/oh (may be NULL) are what ImageMagick / ffmpeg make of it: nearest integer,
 * video sides even. Unknown size (0): FALSE. */
gboolean nfc_resize_output_size (const NfcResizePreset *p, guint64 w, guint64 h,
                                 guint64 *ow, guint64 *oh);

/* Menu tooltip: "Scale down to 1920 x 1080" when the size (w, h; 0 = unknown) is
 * known, else what the preset means in words. g_free. */
char *nfc_resize_tooltip (const NfcResizePreset *p, guint64 w, guint64 h);

typedef struct {
  NfcCategory cat;     /* category to convert in: a GIF image is resized as video */
  const char *key;     /* target key ("jpg", "mp4", "gif", "mov") */
  const char *ext;     /* output extension (a .jpeg or .m4v source gets .jpg / .mp4) */
} NfcResizeTarget;

/* The resize target of a source of category cat and canonical format fmt; FALSE
 * if it cannot be resized (HEIC: no encoder; SVG, ICO, WMV, FLV, audio, documents).
 * Static table: says nothing about the installed tools. */
gboolean nfc_resize_target (NfcCategory cat, const char *fmt, NfcResizeTarget *out);
/* Can a source of category/fmt be resized right now (target + tools probed). */
gboolean nfc_resize_available (NfcCategory cat, const char *fmt);

/* ---- Upscale: an AI-enlarged copy (Upscayl) ----
 *
 * "Upscale" > 2× / 3× / 4× / 5× / 6× / 8×: every selected image becomes a copy
 * N times as wide and tall, named "<name> (4x).<ext>", made by the native
 * backend of Upscayl (upscayl-bin, Real-ESRGAN on the GPU through Vulkan; see
 * README). Offered only when upscayl-bin and its models are found
 * (nfc_tools.upscayl) and only for images ImageMagick or the decoder can read,
 * never for SVG (vector) or ICO.
 *
 * One pass is `upscayl-bin -s N`: the 4x model enlarges 4x and the backend
 * resizes that to N (2x and 3x measured as good as, and simpler than, a 4x pass
 * Lanczos-shrunk by ImageMagick). Bigger factors chain passes, the second one on
 * the output of the first: 6x = 3x then 2x, 8x = 4x then 2x; 5x has no integer
 * product, so it is made as 6x and Lanczos-shrunk by 5/6 (sharper than one 4x
 * pass stretched by 1.25: see README). The sizes are exact (width x factor).
 *
 * The output keeps the source format when that is JPG, PNG or WEBP, with the Best
 * settings of the format (alpha is kept; JPG is flattened onto white); anything
 * else (AVIF, HEIC, BMP, TIFF, GIF first frame) becomes PNG; a WEBP that would
 * exceed WebP's 16383 px per side becomes PNG. */

typedef struct {
  const char *key;    /* menu item name part and CLI key: "4x" */
  const char *label;  /* menu label: "4×" */
  guint       factor; /* output size / input size */
  guint       pass[2]; /* the `upscayl-bin -s` values, pass[1] == 0: a single pass */
} NfcUpscalePreset;

/* An upscale never makes an image beyond this: per side and in pixels (the
 * entry is hidden for a selection it would exceed, the job is skipped). */
#define NFC_UPSCALE_MAX_SIDE   32768
#define NFC_UPSCALE_MAX_PIXELS ((guint64) 400 * 1000 * 1000)

/* The presets in menu order (2x, 3x, 4x, 5x, 6x, 8x). Static table. */
const NfcUpscalePreset *nfc_upscale_presets (guint *n);
const NfcUpscalePreset *nfc_upscale_preset_from_key (const char *key);
/* The size of the picture the passes make (the product of the -s values; 6x for
 * the 5x preset, which is then shrunk). */
guint                   nfc_upscale_chain_factor (const NfcUpscalePreset *p);

/* The size a w x h source gets (width x factor). TRUE only when it is within the
 * limits above; FALSE when too big or when the size is unknown (0). ow/oh may be
 * NULL; they are set (also when FALSE, if the size is known) to the exact size. */
gboolean nfc_upscale_output_size (const NfcUpscalePreset *p, guint64 w, guint64 h,
                                  guint64 *ow, guint64 *oh);
/* Menu tooltip: "AI-upscale to 2560 × 1920 px" when the size (0: unknown) is known
 * and fits, "too big" when it does not, else what the preset means. g_free. */
char *nfc_upscale_tooltip (const NfcUpscalePreset *p, guint64 w, guint64 h);

typedef struct {
  const char *key;   /* output format key: "png", "jpg", "webp" */
  const char *ext;
  gboolean    capped; /* WEBP source whose result exceeds 16383 px: PNG instead */
} NfcUpscaleTarget;

/* The output format of a source of canonical format fmt (category must be image;
 * FALSE for SVG, ICO, everything that is no image). ow/oh: the output size, 0 if
 * unknown (no WEBP cap). Static table: says nothing about the installed tools. */
gboolean nfc_upscale_target (NfcCategory cat, const char *fmt, guint64 ow, guint64 oh,
                             NfcUpscaleTarget *out);
/* Can a source of category/fmt be upscaled right now: backend and model found,
 * target and decoder available (probes finished). */
gboolean nfc_upscale_available (NfcCategory cat, const char *fmt);

/* The model of upscale-model=<name> in the text of a configuration file (lines
 * "key=value", "#" comments, blanks trimmed; the last upscale-model line wins), or
 * NULL if there is none. g_free. */
char    *nfc_upscale_config_model (const char *text);
/* Where upscayl-bin is looked for, in order: /opt/Upscayl, the system Flatpak, the
 * user Flatpak under data_dir (XDG data home). NULL-terminated; g_strfreev. */
char   **nfc_upscayl_candidates (const char *data_dir);
/* The first executable of candidates, else upscayl-bin on the PATH, else NULL. g_free. */
char    *nfc_find_upscayl_in (char **candidates);
/* Resolve the backend of the upscayl-bin at `bin`: its models directory
 * (<bindir>/../models) and the model to use: upscale-model of the configuration
 * file config_path (may be NULL or missing) if both its .param and .bin files exist
 * there (else logged, and the default), else "upscayl-standard-4x" if present.
 * FALSE if neither exists. *models and *model are g_free'd. */
gboolean nfc_upscale_resolve (const char *bin, const char *config_path,
                              char **models, char **model);
#define NFC_UPSCALE_DEFAULT_MODEL "upscayl-standard-4x"

/* Estimated peak memory (bytes) of upscaling a w x h source to preset p with
 * output format key: the picture the passes make (nfc_estimate_peak_bytes of it)
 * plus 1 GiB for the backend itself. */
guint64  nfc_upscale_peak_bytes (const NfcUpscalePreset *p, guint64 w, guint64 h, const char *key);
/* Timeout of one step of an upscale job with this output size: 120 s plus 4 s per
 * output megapixel, at most 2 h. */
guint    nfc_upscale_timeout_secs (guint64 ow, guint64 oh);

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
  char *upscayl; /* upscayl-bin, the Upscale backend; NULL: no Upscale entry */
  char *upscale_models; /* its models directory (<bindir>/../models) */
  char *upscale_model;  /* the model used: upscale-model of the config file, or the default */
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
/* Can a source of category/fmt be compressed right now: nfc_compress_target has
 * an entry and its tools (encoder, decoder) are installed and probed. */
gboolean    nfc_compress_available (NfcCategory cat, const char *fmt);

/* ---- selection analysis (menu logic) ---- */

typedef struct {
  const char *uri;
  const char *mime;
  gboolean    is_dir;
} NfcSelItem;

/* Pixel size of one selected file as far as the right-click menu knows it;
 * 0 x 0: unknown (not an image, a format without a header parser (SVG, ICO), a header that
 * is not in the first 64 KiB, an unreadable file). */
typedef struct {
  guint64 width, height;
} NfcDims;

typedef struct {
  NfcCategory cat;
  GPtrArray  *paths;   /* char *, owned */
  GPtrArray  *fmts;    /* const char *, static */
  GPtrArray  *targets; /* const NfcTarget *, static */
  gboolean    can_compress; /* "Compress" is offered: see nfc_compress_target */
  /* Image selections of up to NFC_SELECTION_DIMS_MAX files: the size of each
   * file, read from its header (nfc-header.h) by nfc_analyze_selection: an array
   * of NfcDims parallel to paths. NULL otherwise (all sizes unknown). */
  GArray     *dims;
  /* Resize presets (const NfcResizePreset *, static, in menu order) that make at
   * least one selected file smaller (a file of unknown size counts); NULL or
   * empty when "Resize" is not offered. */
  GPtrArray  *resize;
  /* Upscale presets (const NfcUpscalePreset *, static, in menu order) whose
   * result fits the limits for every selected file (a file of unknown size counts);
   * NULL or empty when "Upscale" is not offered. */
  GPtrArray  *upscale;
} NfcSelection;

/* Selections with more files than this are not given sizes (the menu must stay
 * instant: each size is a read of up to 64 KiB on the main thread) */
#define NFC_SELECTION_DIMS_MAX 32

/* TRUE (and a filled sel) if a menu should be shown for this selection.
 *
 * NEVER BLOCKS: it is called from the Nautilus main thread. Targets whose
 * capability probe has not finished yet are simply left out (documents,
 * spreadsheets and presentations need no probe and appear immediately; image,
 * audio and video targets appear as soon as their own probe is done, normally
 * within a fraction of a second of Nautilus starting). If nothing is known to
 * be available yet the result is FALSE, i.e. no menu at all rather than an
 * empty or half-built one. A probe that timed out counts as "unavailable".
 *
 * Compress is separate from the convert targets: sel->can_compress is TRUE when
 * every selected file can be compressed (nfc_compress_available); each in its
 * own format. The result is TRUE when there is a convert target, can_compress
 * or a resize preset (sel->targets may then be empty).
 *
 * The one thing that touches file content: for image selections it reads the
 * first 64 KiB of each (regular, local) file (a TIFF: and 64 KiB at its first IFD)
 * to learn its pixel size
 * (sel->dims, nfc-header.h), so that Resize offers only presets that make the
 * picture smaller. Bounded, no tool is run; a file whose size cannot be found
 * simply has none. */
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

/* The commands of a Resize job: cat/fmt/key/ext as nfc_resize_target gives for
 * the source (key is the output format; an image resize is ImageMagick
 * `-filter Lanczos -resize` followed by the Best settings of that format; a GIF is
 * ffmpeg's scale + palette chain without touching the frame rate; a video is
 * ffmpeg `scale=...:flags=lanczos` (short side) with the Best settings of the
 * container's codec, the audio stream copied when the container takes it, else
 * re-encoded). info (may be NULL) is the SOURCE size; the commands never enlarge
 * (the geometry is "WxH>" and the video scale takes min()), so a missing size is
 * harmless. Other arguments as nfc_build_attempts_preset. NULL if a tool is
 * missing or key is not a resize target. */
GPtrArray *nfc_build_attempts_resize (NfcCategory cat, const char *fmt, const char *key,
                                      const NfcResizePreset *rs, const char *src,
                                      const char *out, const char *workdir,
                                      const NfcImageInfo *info, gboolean *needs_decode);

/* The commands of an Upscale job: one attempt of these steps, run in order:
 *   1. a PNG the backend can read, <workdir>/in.png: `nfc-decode` for HEIC/AVIF
 *      (decoded.png, orientation applied), else ImageMagick `-auto-orient` of the
 *      first frame (the backend reads neither TIFF, BMP nor GIF, applies no EXIF
 *      orientation and stumbles over CMYK and 16-bit files);
 *   2. `upscayl-bin -i in -o up1.png -m <models> -n <model> -s p->pass[0] -f png`,
 *      and, for the chained presets, a second one on up1.png writing up2.png;
 *   3. ImageMagick: the last PNG to `out` as `key` ("png", "jpg", "webp"; the Best
 *      settings of that format as in a conversion; the 5x preset first shrinks the
 *      6x picture by 5/6, Lanczos), alpha kept.
 * The commands never fall back to anything else (a single attempt). *needs_decode
 * is set for HEIC/AVIF. NULL if a tool (upscayl-bin, its models, ImageMagick, the
 * decoder) is missing or fmt/key are not an upscale source/target. */
GPtrArray *nfc_build_attempts_upscale (const char *fmt, const char *key, const NfcUpscalePreset *p,
                                       const char *src, const char *out, const char *workdir,
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
