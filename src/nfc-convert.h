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

typedef struct {
  char    *path;
  gboolean ok;
  char    *output; /* final path when ok */
  char    *error;  /* message when !ok */
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

/* Module shutdown: kill the process groups of all in-flight conversions,
 * remove their work directories and hidden temp files (best effort), and
 * refuse new conversions. Their callbacks still fire (with an error) if the
 * main loop keeps running. */
void nfc_shutdown (void);

G_END_DECLS
