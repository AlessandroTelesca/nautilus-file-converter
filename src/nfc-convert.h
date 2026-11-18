/* nautilus-file-converter: asynchronous conversion runner (GLib/GIO only). */
#pragma once

#include <gio/gio.h>
#include "nfc-core.h"
#include "nfc-util.h"

G_BEGIN_DECLS

/* All of this runs on, and must be called from, the main thread (the thread
 * that iterates the default main context). */

/* Convert one file without blocking the main loop. The result is the final
 * output path (g_free) or an error whose message is the user-facing text. */
void  nfc_convert_file_async (NfcCategory cat, const char *fmt, const char *key,
                              const char *ext, const char *src,
                              GAsyncReadyCallback callback, gpointer user_data);
char *nfc_convert_file_finish (GAsyncResult *result, GError **error);

/* The same with a quality preset (BEST = nfc_convert_file_async). A preset on a
 * target without presets (see nfc_target_has_presets) is ignored. */
void  nfc_convert_file_preset_async (NfcCategory cat, const char *fmt, const char *key,
                                     NfcPreset preset, const char *ext, const char *src,
                                     GAsyncReadyCallback callback, gpointer user_data);

/* ---- conversion options, keep-the-smaller ----
 *
 * The general form of a conversion: everything nfc_convert_file_preset_async
 * does plus an output name suffix and a reference size. Resize, Upscale and
 * Compress all go through it. */

#define NFC_CONVERT_ERROR (nfc_convert_error_quark ())
GQuark nfc_convert_error_quark (void);

typedef enum {
  /* the output was not smaller than NfcConvertOptions.reference_size: it has
   * been deleted, nothing was left behind. Not a failure of the conversion. */
  NFC_CONVERT_ERROR_NOT_SMALLER = 1,
  /* Upscale: the result would exceed the size limits (NFC_UPSCALE_MAX_SIDE,
   * NFC_UPSCALE_MAX_PIXELS); nothing ran. Not a failure either (NfcResult.skipped);
   * the message says what it would have been. */
  NFC_CONVERT_ERROR_TOO_BIG = 2
} NfcConvertError;

#define NFC_REFERENCE_NONE   ((gint64) -1) /* keep the output whatever its size */
#define NFC_REFERENCE_SOURCE ((gint64) -2) /* keep it only if smaller than the source file */

typedef struct {
  NfcPreset   preset;
  /* Appended to the stem of the published name: "name" + " (compressed)" gives
   * "name (compressed).jpg" (and "name (compressed) (1).jpg" if that exists).
   * NULL or "": the plain stem. */
  const char *name_suffix;
  /* NFC_REFERENCE_NONE, NFC_REFERENCE_SOURCE, or a size in bytes (>= 0): after
   * the conversion, an output that is NOT strictly smaller (nfc_is_smaller) is
   * deleted, the work directory and temporary files are removed and the task
   * fails with NFC_CONVERT_ERROR_NOT_SMALLER. */
  gint64      reference_size;
  /* Resize: the preset to scale to (NULL: no resize). The job probes the
   * source's pixel size first and, when the preset would not make it smaller,
   * ends with NFC_CONVERT_ERROR_NOT_SMALLER ("already 1080p or smaller")
   * without running the converter. cat/fmt/key/ext are those of
   * nfc_resize_target; preset is ignored (Best). Add new fields below this one:
   * callers initialise the leading ones positionally. */
  const NfcResizePreset *resize;
  /* Upscale: the preset to enlarge by (NULL: no upscale). cat is NFC_CAT_IMAGE,
   * fmt the source format, key/ext those of nfc_upscale_target (a WEBP whose result
   * exceeds 16383 px per side is switched to PNG by the job, once it knows the
   * size). The job probes the size first and ends with NFC_CONVERT_ERROR_TOO_BIG
   * when the result would exceed the limits. The job holds the (single, global)
   * GPU slot while it runs: at most one Upscale job at a time, whichever batch it
   * belongs to. */
  const NfcUpscalePreset *upscale;
} NfcConvertOptions;

/* The pixel size of the source as the job's own size probe found it (the first
 * frame of an image, the first video stream), for the callback of a
 * nfc_convert_file_*_async call, whatever its outcome; FALSE (and 0, 0) when it
 * was not probed (documents, audio, a tool that was missing, a probe that failed).
 * Images and GIFs are always probed, videos for MP4/MKV, resizes and the Small
 * preset. */
gboolean nfc_convert_result_dims (GAsyncResult *result, guint64 *width, guint64 *height);

/* nfc_convert_file_preset_async with options (opts may be NULL: BEST, no
 * suffix, no reference). */
void  nfc_convert_file_opts_async (NfcCategory cat, const char *fmt, const char *key,
                                   const char *ext, const char *src,
                                   const NfcConvertOptions *opts,
                                   GAsyncReadyCallback callback, gpointer user_data);

typedef struct {
  char    *path;
  gboolean ok;
  char    *output; /* final path when ok */
  char    *error;  /* message when !ok */
  gboolean skipped;  /* !ok, but not a failure: the output was not smaller than the
                      * reference and was discarded (NFC_CONVERT_ERROR_NOT_SMALLER);
                      * error then reads "already as small as it gets" */
  gint64   src_size, out_size; /* bytes of the source and, when ok, of the output
                                * (compress batches only, else 0) */
  guint64  src_w, src_h;       /* pixel size of the source when the job probed it, else 0 */
} NfcResult;

/* Called once when the whole batch is finished; results is a GPtrArray of
 * NfcResult * (same order as the input), freed after the callback returns. */
typedef void (*NfcBatchDone) (GPtrArray *results, gpointer user_data);

/* Convert paths[i] (source format fmts[i]) to key/ext with limited
 * parallelism; shows a "Converting..." notification replaced by the result.
 * Inputs are copied. */
void nfc_batch_run (NfcCategory cat, const char *key, const char *label,
                    const char *ext, const char *const *paths,
                    const char *const *fmts, guint n,
                    NfcBatchDone done, gpointer user_data);

/* nfc_batch_run with a quality preset: the notifications name it when it is not
 * Best ("Converted 3 files to JPG (Balanced)"). */
void nfc_batch_run_preset (NfcCategory cat, const char *key, NfcPreset preset,
                           const char *label, const char *ext, const char *const *paths,
                           const char *const *fmts, guint n,
                           NfcBatchDone done, gpointer user_data);

/* Compress paths[i] (source format fmts[i], category cat of the selection):
 * each file becomes a smaller copy in its own format with the preset (HIGH,
 * BALANCED or SMALL; see nfc_compress_target), named "<name> (compressed).<ext>",
 * kept only if smaller than the original (NFC_REFERENCE_SOURCE). The results
 * carry `skipped` for files that were already as small as they get. Shows
 * "Reducing file size...", then "Compressed 3 files (Balanced), -62%" /
 * "a.jpg → a (compressed).jpg (Balanced), -62%", plus a line for each file that
 * was not smaller ("a.jpg is already as small as it gets"). */
void nfc_batch_run_compress (NfcCategory cat, NfcPreset preset,
                             const char *const *paths, const char *const *fmts, guint n,
                             NfcBatchDone done, gpointer user_data);

/* Resize paths[i] (source format fmts[i], category cat of the selection) to the
 * preset (nfc_resize_preset_from_key (cat, preset_key)): each file becomes a copy
 * in its own format (nfc_resize_target), named "<name> (1080p).<ext>" and kept
 * only if the preset makes it smaller; files it would not are `skipped`. Shows
 * "Resizing...", then "photo.jpg → photo (1080p).jpg, 3840×2160 → 1920×1080" /
 * "Resized 3 files to 1080p", plus a line for each file that was already smaller
 * ("photo.jpg is already 1080p or smaller"). An unknown preset key finishes the
 * batch at once with every file failed. */
void nfc_batch_run_resize (NfcCategory cat, const char *preset_key,
                           const char *const *paths, const char *const *fmts, guint n,
                           NfcBatchDone done, gpointer user_data);

/* Upscale paths[i] (source format fmts[i], images; see nfc_upscale_target) with
 * the preset (nfc_upscale_preset_from_key (preset_key)): each file becomes a copy
 * "<name> (4x).<ext>" N times as wide and tall; files whose result would be too
 * big are `skipped`. One file at a time (the GPU). Shows "Upscaling…", then
 * "photo.png → photo (4x).png, 640×480 → 2560×1920" / "Upscaled 3 files 4×", plus
 * a line for each file that was too big. An unknown preset key finishes the batch
 * at once with every file failed. */
void nfc_batch_run_upscale (const char *preset_key, const char *const *paths,
                            const char *const *fmts, guint n,
                            NfcBatchDone done, gpointer user_data);

/* Module shutdown: kill the process groups of all in-flight conversions,
 * remove their work directories and hidden temp files (best effort), and
 * refuse new conversions. Their callbacks still fire (with an error) if the
 * main loop keeps running. */
void nfc_shutdown (void);

G_END_DECLS
