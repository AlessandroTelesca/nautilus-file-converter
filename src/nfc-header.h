/* nautilus-file-converter: pixel size of an image from its first bytes.
 *
 * The one place where the extension reads file content on the Nautilus main
 * thread: a bounded read (NFC_HEADER_MAX_BYTES) of a regular local file, parsed
 * here in C, so the right-click menu can hide presets that would not make the
 * picture smaller (Resize) or would make it too big (Upscale). Nothing here
 * decodes pixels or calls a tool, and the parsers never read beyond the buffer
 * they are given (fuzzed under ASan in tests/test-core.c). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The header of a file is looked for in its first 64 KiB: PNG, GIF, WebP and BMP
 * keep the size in the first 30 bytes, JPEG after its APPn segments (an EXIF/ICC
 * block can be up to 64 KiB each; a file whose size marker is further in is just
 * not recognised, and the caller falls back to "size unknown"). */
#define NFC_HEADER_MAX_BYTES (64 * 1024)

/* Size of the image in buf[0..len), format fmt (canonical key: "png", "jpg",
 * "gif", "webp", "bmp"; anything else is not parsed). TRUE and *w, *h (both
 * > 0) on success; FALSE for truncated, malformed or mismatching data (a ".png"
 * that holds JPEG data, say). Pure: no allocation, no I/O. The stored size is
 * returned; EXIF orientation is not applied (it only swaps width and height). */
gboolean nfc_header_parse (const char *fmt, const guint8 *buf, gsize len,
                           guint32 *w, guint32 *h);

/* Read at most NFC_HEADER_MAX_BYTES of path and nfc_header_parse them. Only a
 * regular file is read (never blocks on a FIFO or a device: opened O_NONBLOCK
 * and checked with fstat). FALSE on any error
 * or when the size is not in the bytes read. */
gboolean nfc_header_read_file (const char *path, const char *fmt, guint32 *w, guint32 *h);

/* Does nfc_header_parse know this format key? */
gboolean nfc_header_format_known (const char *fmt);

G_END_DECLS
