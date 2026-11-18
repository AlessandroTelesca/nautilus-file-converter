/* nautilus-file-converter: asynchronous conversion runner (GLib/GIO only). */
#pragma once

#include <gio/gio.h>
#include "nfc-core.h"

G_BEGIN_DECLS

/* Append a timestamped line to ~/.cache/nautilus-file-converter.log */
void nfc_log (const char *format, ...) G_GNUC_PRINTF (1, 2);

/* Convert one file without blocking the main loop. The result is the final
 * output path (g_free) or an error whose message is the user-facing text. */
void  nfc_convert_file_async (NfcCategory cat, const char *fmt, const char *key,
                              const char *ext, const char *src,
                              GAsyncReadyCallback callback, gpointer user_data);
char *nfc_convert_file_finish (GAsyncResult *result, GError **error);

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

G_END_DECLS
