/* nautilus-file-converter: pure conversion logic. See nfc-core.h. */
#define _GNU_SOURCE
#include "nfc-core.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "config.h"
#include "nfc-util.h"

NfcTools nfc_tools = { NULL, NULL, NULL, NULL, NULL };

/* -------------------------------------------------------------------------
 * Tool discovery
 * ---------------------------------------------------------------------- */

static char *
find_first (const char *a, const char *b)
{
  char *p = g_find_program_in_path (a);
  if (p == NULL && b != NULL)
    p = g_find_program_in_path (b);
  return p;
}

/* The out-of-process decoder: NFC_DECODE_HELPER (tests) or the configured
 * install path. NULL if it is not there: HEIC/AVIF input is then not offered. */
static char *
find_decode_helper (void)
{
  const char *env = g_getenv ("NFC_DECODE_HELPER");

  if (env != NULL && *env != '\0')
    return g_file_test (env, G_FILE_TEST_IS_EXECUTABLE) ? g_strdup (env) : NULL;
  if (g_file_test (NFC_DECODE_HELPER_PATH, G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup (NFC_DECODE_HELPER_PATH);
  return NULL;
}

void
nfc_tools_init (void)
{
  static gsize once = 0;

  if (g_once_init_enter (&once))
    {
      g_free (nfc_tools.magick);
      g_free (nfc_tools.ffmpeg);
      g_free (nfc_tools.soffice);
      g_free (nfc_tools.notify);
      g_free (nfc_tools.decode);
      nfc_tools.magick = find_first ("magick", "convert");
      nfc_tools.ffmpeg = find_first ("ffmpeg", NULL);
      nfc_tools.soffice = find_first ("soffice", "libreoffice");
      nfc_tools.notify = find_first ("notify-send", NULL);
      nfc_tools.decode = find_decode_helper ();
      g_once_init_leave (&once, 1);
    }
}

/* -------------------------------------------------------------------------
 * Capability probes (background threads; see nfc-core.h)
 * ---------------------------------------------------------------------- */

#define PROBE_TIMEOUT_SECS 10

/* 0 = pending, 1 = done. Stored AFTER the table of that probe is complete;
 * readers check it first, so the tables need no lock (they are never changed
 * once the flag is set). */
static gint probe_state[NFC_PROBE_N];
static GMutex probe_lock;
static GCond probe_cond;
static GHashTable *ff_enc = NULL;    /* ffmpeg encoder names */
static GHashTable *im_write = NULL;  /* ImageMagick writable format names */
static GHashTable *im_read = NULL;   /* ImageMagick readable format (coder) names */
static GHashTable *pb_read = NULL;   /* GdkPixbuf format names */

static guint
probe_timeout_secs (void)
{
  const char *e = g_getenv ("NFC_TEST_PROBE_TIMEOUT_SECS"); /* tests only */
  guint64 v = (e != NULL && *e != '\0') ? g_ascii_strtoull (e, NULL, 10) : 0;

  return v > 0 && v < 3600 ? (guint) v : PROBE_TIMEOUT_SECS;
}

typedef struct {
  GMainLoop *loop;
  GSubprocess *proc;
  GCancellable *cancel;
  gboolean timed_out;
  gboolean ok;
  char *out;
} QuietCtx;

static void
quiet_done (GObject *source, GAsyncResult *res, gpointer data)
{
  QuietCtx *q = data;

  q->ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &q->out,
                                                NULL, NULL);
  g_main_loop_quit (q->loop);
}

static gboolean
quiet_timeout (gpointer data)
{
  QuietCtx *q = data;

  q->timed_out = TRUE;
  nfc_kill_group (nfc_subprocess_pgid (q->proc));
  g_cancellable_cancel (q->cancel);
  return G_SOURCE_REMOVE;
}

/* Run argv quietly and return stdout (NULL on failure or timeout). Blocks only
 * the calling (probe) thread, in a private main context; after the timeout the
 * whole process group is killed. */
static char *
run_quiet (const char *const *argv, const char *what)
{
  GMainContext *ctx = g_main_context_new ();
  GSubprocessLauncher *l;
  GSource *timer;
  QuietCtx q = { NULL, NULL, NULL, FALSE, FALSE, NULL };
  GError *error = NULL;
  guint secs = probe_timeout_secs ();

  g_main_context_push_thread_default (ctx);
  l = nfc_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  q.proc = g_subprocess_launcher_spawnv (l, argv, &error);
  g_object_unref (l);
  if (q.proc == NULL)
    {
      nfc_log ("probe %s: cannot run %s: %s", what, argv[0], error->message);
      g_clear_error (&error);
      goto out;
    }
  q.loop = g_main_loop_new (ctx, FALSE);
  q.cancel = g_cancellable_new ();
  timer = g_timeout_source_new_seconds (secs);
  g_source_set_callback (timer, quiet_timeout, &q, NULL);
  g_source_attach (timer, ctx);
  g_subprocess_communicate_utf8_async (q.proc, NULL, q.cancel, quiet_done, &q);
  g_main_loop_run (q.loop);
  g_source_destroy (timer);
  g_source_unref (timer);
  if (q.timed_out)
    {
      nfc_log ("probe %s: %s did not answer within %u s, killed; treated as unavailable "
               "for this session", what, argv[0], secs);
      g_clear_pointer (&q.out, g_free);
    }
  else if (!q.ok)
    g_clear_pointer (&q.out, g_free);
  g_main_loop_unref (q.loop);
  g_object_unref (q.cancel);
  g_object_unref (q.proc);
out:
  g_main_context_pop_thread_default (ctx);
  g_main_context_unref (ctx);
  return q.out;
}

static gboolean
is_word_char (char c, const char *extra)
{
  return g_ascii_isalnum (c) || (c != 0 && strchr (extra, c) != NULL);
}

static void
probe_ffmpeg (void)
{
  char *out;
  char **lines;

  ff_enc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (nfc_tools.ffmpeg == NULL)
    return;
  {
    const char *argv[] = { nfc_tools.ffmpeg, "-hide_banner", "-encoders", NULL };
    out = run_quiet (argv, "ffmpeg");
  }
  if (out == NULL)
    return;
  lines = g_strsplit (out, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      /* " V....D libx264   description": flags are 6 chars [VAS][\w.]{5} */
      char *s = g_strstrip (*l);
      char *sp = strchr (s, ' ');
      gboolean ok;

      if (sp == NULL || sp - s != 6 || (s[0] != 'V' && s[0] != 'A' && s[0] != 'S'))
        continue;
      ok = TRUE;
      for (int i = 1; i < 6; i++)
        if (!is_word_char (s[i], "._"))
          ok = FALSE;
      if (!ok)
        continue;
      while (*sp == ' ')
        sp++;
      {
        char *e = sp;
        while (*e && *e != ' ')
          e++;
        if (e > sp)
          g_hash_table_add (ff_enc, g_strndup (sp, e - sp));
      }
    }
  g_strfreev (lines);
  g_free (out);
}

static void
probe_magick (void)
{
  char *out;
  char **lines;

  im_write = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  im_read = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (nfc_tools.magick == NULL)
    return;
  {
    const char *argv[] = { nfc_tools.magick, "-list", "format", NULL };
    out = run_quiet (argv, "magick");
  }
  if (out == NULL)
    return;
  lines = g_strsplit (out, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      /* "   PNG* PNG       rw-   description": name, module, mode */
      char **raw = g_strsplit_set (g_strstrip (*l), " \t", -1);
      char *tok[4] = { NULL, NULL, NULL, NULL };
      guint n = 0;

      for (char **r = raw; *r && n < 3; r++)
        if (**r != '\0')
          tok[n++] = *r;

      if (n >= 3)
        {
          char *name = g_strdup (tok[0]);
          size_t len = strlen (name);
          const char *mode;
          gboolean ok = len > 0;

          if (len > 0 && name[len - 1] == '*')
            name[--len] = '\0';
          for (size_t i = 0; i < len; i++)
            if (!(g_ascii_isupper (name[i]) || g_ascii_isdigit (name[i]) ||
                  name[i] == '_' || name[i] == '-'))
              ok = FALSE;
          /* the mode column is the first token after the module that looks
           * like [r-][w-][+-]; tokens may be separated by several spaces */
          mode = NULL;
          for (guint i = 1; i < n && mode == NULL; i++)
            if (strlen (tok[i]) == 3 && (tok[i][0] == 'r' || tok[i][0] == '-') &&
                (tok[i][1] == 'w' || tok[i][1] == '-') &&
                (tok[i][2] == '+' || tok[i][2] == '-'))
              mode = tok[i];
          if (ok && mode != NULL && mode[0] == 'r')
            g_hash_table_add (im_read, g_strdup (name));
          if (ok && mode != NULL && mode[1] == 'w')
            g_hash_table_add (im_write, name);
          else
            g_free (name);
        }
      g_strfreev (raw);
    }
  g_strfreev (lines);
  g_free (out);
}

/* Which image formats GdkPixbuf can read: asked of the helper process, never
 * loaded into Nautilus. */
static void
probe_pixbuf (void)
{
  char *out;

  pb_read = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (nfc_tools.decode == NULL)
    return;
  {
    const char *argv[] = { nfc_tools.decode, "--formats", NULL };
    out = run_quiet (argv, "nfc-decode");
  }
  if (out == NULL)
    return;
  {
    char **lines = g_strsplit (out, "\n", -1);

    for (char **l = lines; *l; l++)
      {
        char *s = g_strstrip (*l);

        if (*s != '\0')
          g_hash_table_add (pb_read, g_strdup (s));
      }
    g_strfreev (lines);
  }
  g_free (out);
}

static gpointer
probe_thread (gpointer data)
{
  NfcProbe which = GPOINTER_TO_INT (data);

  switch (which)
    {
    case NFC_PROBE_FFMPEG: probe_ffmpeg (); break;
    case NFC_PROBE_MAGICK: probe_magick (); break;
    case NFC_PROBE_PIXBUF: probe_pixbuf (); break;
    default: break;
    }
  g_atomic_int_set (&probe_state[which], 1);
  g_mutex_lock (&probe_lock);
  g_cond_broadcast (&probe_cond);
  g_mutex_unlock (&probe_lock);
  return NULL;
}

void
nfc_probe_start (void)
{
  static gsize once = 0;

  nfc_tools_init ();
  if (g_once_init_enter (&once))
    {
      for (int i = 0; i < NFC_PROBE_N; i++)
        {
          GThread *t = g_thread_try_new ("nfc-probe", probe_thread, GINT_TO_POINTER (i), NULL);

          if (t != NULL)
            g_thread_unref (t);
          else
            {
              /* cannot spawn a thread: leave this probe empty ("unavailable")
               * rather than ever running a tool on the calling thread */
              GHashTable **tbl = i == NFC_PROBE_FFMPEG ? &ff_enc
                                 : i == NFC_PROBE_MAGICK ? &im_write : &pb_read;

              *tbl = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
              g_atomic_int_set (&probe_state[i], 1);
            }
        }
      g_once_init_leave (&once, 1);
    }
}

gboolean
nfc_probe_done (NfcProbe probe)
{
  return (guint) probe < NFC_PROBE_N && g_atomic_int_get (&probe_state[probe]) != 0;
}

void
nfc_probe_ensure (void)
{
  nfc_probe_start ();
  g_mutex_lock (&probe_lock);
  for (;;)
    {
      gboolean all = TRUE;

      for (int i = 0; i < NFC_PROBE_N; i++)
        all = all && nfc_probe_done (i);
      if (all)
        break;
      g_cond_wait (&probe_cond, &probe_lock);
    }
  g_mutex_unlock (&probe_lock);
}

gboolean
nfc_have_encoder (const char *name)
{
  return name != NULL && nfc_probe_done (NFC_PROBE_FFMPEG) && g_hash_table_contains (ff_enc, name);
}

gboolean
nfc_magick_can_write (const char *im_format)
{
  return im_format != NULL && nfc_probe_done (NFC_PROBE_MAGICK) &&
         g_hash_table_contains (im_write, im_format);
}

/* Readable coder names (the "r" column of `magick -list format`), e.g. "RSVG". */
gboolean
nfc_magick_can_read (const char *im_format)
{
  return im_format != NULL && nfc_probe_done (NFC_PROBE_MAGICK) && im_read != NULL &&
         g_hash_table_contains (im_read, im_format);
}

gboolean
nfc_pixbuf_can_read (const char *name)
{
  return name != NULL && nfc_probe_done (NFC_PROBE_PIXBUF) &&
         g_hash_table_contains (pb_read, name);
}

/* -------------------------------------------------------------------------
 * Format tables
 * ---------------------------------------------------------------------- */

typedef struct {
  const char *ext;
  NfcCategory cat;
  const char *key;
} ExtEntry;

static const ExtEntry ext_table[] = {
  { "png", NFC_CAT_IMAGE, "png" },
  { "jpg", NFC_CAT_IMAGE, "jpg" }, { "jpeg", NFC_CAT_IMAGE, "jpg" },
  { "jpe", NFC_CAT_IMAGE, "jpg" }, { "jfif", NFC_CAT_IMAGE, "jpg" },
  { "webp", NFC_CAT_IMAGE, "webp" }, { "avif", NFC_CAT_IMAGE, "avif" },
  { "bmp", NFC_CAT_IMAGE, "bmp" }, { "gif", NFC_CAT_IMAGE, "gif" },
  { "ico", NFC_CAT_IMAGE, "ico" }, { "svg", NFC_CAT_IMAGE, "svg" },
  { "tif", NFC_CAT_IMAGE, "tiff" }, { "tiff", NFC_CAT_IMAGE, "tiff" },
  { "heic", NFC_CAT_IMAGE, "heic" }, { "heif", NFC_CAT_IMAGE, "heic" },
  { "mp3", NFC_CAT_AUDIO, "mp3" }, { "wav", NFC_CAT_AUDIO, "wav" },
  { "flac", NFC_CAT_AUDIO, "flac" }, { "ogg", NFC_CAT_AUDIO, "ogg" },
  { "oga", NFC_CAT_AUDIO, "oga" }, { "opus", NFC_CAT_AUDIO, "opus" },
  { "m4a", NFC_CAT_AUDIO, "m4a" }, { "aac", NFC_CAT_AUDIO, "aac" },
  { "wma", NFC_CAT_AUDIO, "wma" },
  { "mp4", NFC_CAT_VIDEO, "mp4" }, { "mkv", NFC_CAT_VIDEO, "mkv" },
  { "webm", NFC_CAT_VIDEO, "webm" }, { "mov", NFC_CAT_VIDEO, "mov" },
  { "avi", NFC_CAT_VIDEO, "avi" }, { "wmv", NFC_CAT_VIDEO, "wmv" },
  { "flv", NFC_CAT_VIDEO, "flv" }, { "m4v", NFC_CAT_VIDEO, "m4v" },
  { "doc", NFC_CAT_DOC, "doc" }, { "docx", NFC_CAT_DOC, "docx" },
  { "odt", NFC_CAT_DOC, "odt" }, { "rtf", NFC_CAT_DOC, "rtf" },
  { "txt", NFC_CAT_DOC, "txt" },
  { "xls", NFC_CAT_SHEET, "xls" }, { "xlsx", NFC_CAT_SHEET, "xlsx" },
  { "ods", NFC_CAT_SHEET, "ods" }, { "csv", NFC_CAT_SHEET, "csv" },
  { "ppt", NFC_CAT_SLIDE, "ppt" }, { "pptx", NFC_CAT_SLIDE, "pptx" },
  { "odp", NFC_CAT_SLIDE, "odp" },
};

static const ExtEntry mime_table[] = {
  { "image/png", NFC_CAT_IMAGE, "png" }, { "image/jpeg", NFC_CAT_IMAGE, "jpg" },
  { "image/webp", NFC_CAT_IMAGE, "webp" }, { "image/avif", NFC_CAT_IMAGE, "avif" },
  { "image/heic", NFC_CAT_IMAGE, "heic" }, { "image/heif", NFC_CAT_IMAGE, "heic" },
  { "image/bmp", NFC_CAT_IMAGE, "bmp" }, { "image/x-ms-bmp", NFC_CAT_IMAGE, "bmp" },
  { "image/tiff", NFC_CAT_IMAGE, "tiff" }, { "image/gif", NFC_CAT_IMAGE, "gif" },
  { "image/vnd.microsoft.icon", NFC_CAT_IMAGE, "ico" },
  { "image/x-icon", NFC_CAT_IMAGE, "ico" },
  { "image/svg+xml", NFC_CAT_IMAGE, "svg" },
  { "audio/mpeg", NFC_CAT_AUDIO, "mp3" }, { "audio/x-wav", NFC_CAT_AUDIO, "wav" },
  { "audio/wav", NFC_CAT_AUDIO, "wav" }, { "audio/flac", NFC_CAT_AUDIO, "flac" },
  { "audio/x-flac", NFC_CAT_AUDIO, "flac" }, { "audio/ogg", NFC_CAT_AUDIO, "ogg" },
  { "audio/mp4", NFC_CAT_AUDIO, "m4a" }, { "audio/x-m4a", NFC_CAT_AUDIO, "m4a" },
  { "audio/aac", NFC_CAT_AUDIO, "aac" }, { "audio/x-ms-wma", NFC_CAT_AUDIO, "wma" },
  { "video/mp4", NFC_CAT_VIDEO, "mp4" }, { "video/x-matroska", NFC_CAT_VIDEO, "mkv" },
  { "video/webm", NFC_CAT_VIDEO, "webm" }, { "video/quicktime", NFC_CAT_VIDEO, "mov" },
  { "video/x-msvideo", NFC_CAT_VIDEO, "avi" }, { "video/x-ms-wmv", NFC_CAT_VIDEO, "wmv" },
  { "video/x-flv", NFC_CAT_VIDEO, "flv" }, { "video/x-m4v", NFC_CAT_VIDEO, "m4v" },
  { "application/msword", NFC_CAT_DOC, "doc" },
  { "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    NFC_CAT_DOC, "docx" },
  { "application/vnd.oasis.opendocument.text", NFC_CAT_DOC, "odt" },
  { "application/rtf", NFC_CAT_DOC, "rtf" }, { "text/rtf", NFC_CAT_DOC, "rtf" },
  { "text/plain", NFC_CAT_DOC, "txt" },
  { "application/vnd.ms-excel", NFC_CAT_SHEET, "xls" },
  { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    NFC_CAT_SHEET, "xlsx" },
  { "application/vnd.oasis.opendocument.spreadsheet", NFC_CAT_SHEET, "ods" },
  { "text/csv", NFC_CAT_SHEET, "csv" },
  { "application/vnd.ms-powerpoint", NFC_CAT_SLIDE, "ppt" },
  { "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    NFC_CAT_SLIDE, "pptx" },
  { "application/vnd.oasis.opendocument.presentation", NFC_CAT_SLIDE, "odp" },
};

char *
nfc_stem_of (const char *path)
{
  char *base = g_path_get_basename (path);
  char *dot = strrchr (base, '.');

  if (dot != NULL && dot != base)
    *dot = '\0';
  return base;
}

gboolean
nfc_classify (const char *path, const char *mime, NfcFormat *out)
{
  char *base;
  const char *dot;

  if (path == NULL || out == NULL)
    return FALSE;
  base = g_path_get_basename (path);
  dot = strrchr (base, '.');
  if (dot != NULL && dot != base && dot[1] != '\0')
    {
      char *ext = g_ascii_strdown (dot + 1, -1);

      for (guint i = 0; i < G_N_ELEMENTS (ext_table); i++)
        if (g_str_equal (ext_table[i].ext, ext))
          {
            out->cat = ext_table[i].cat;
            out->fmt = ext_table[i].key;
            g_free (ext);
            g_free (base);
            return TRUE;
          }
      g_free (ext);
    }
  g_free (base);
  if (mime != NULL)
    for (guint i = 0; i < G_N_ELEMENTS (mime_table); i++)
      if (g_str_equal (mime_table[i].ext, mime))
        {
          out->cat = mime_table[i].cat;
          out->fmt = mime_table[i].key;
          return TRUE;
        }
  return FALSE;
}

const char *
nfc_category_name (NfcCategory cat)
{
  static const char *names[] = { "image", "audio", "video", "doc", "sheet", "slide" };

  return (guint) cat < NFC_CAT_N ? names[cat] : "?";
}

#define T(k, l, e) { k, l, e }
static const NfcTarget image_targets[] = {
  T ("png", "PNG", "png"), T ("jpg", "JPG", "jpg"), T ("webp", "WEBP", "webp"),
  T ("avif", "AVIF", "avif"), T ("ico", "ICO", "ico"), T ("bmp", "BMP", "bmp"),
  T ("tiff", "TIFF", "tiff"), T ("pdf", "PDF", "pdf"), { NULL, NULL, NULL }
};
static const NfcTarget audio_targets[] = {
  T ("mp3", "MP3", "mp3"), T ("flac", "FLAC", "flac"), T ("wav", "WAV", "wav"),
  T ("ogg", "OGG (Vorbis)", "ogg"), T ("opus", "OPUS", "opus"),
  T ("m4a", "M4A (AAC)", "m4a"), { NULL, NULL, NULL }
};
static const NfcTarget video_targets[] = {
  T ("mp4", "MP4 (H.264)", "mp4"), T ("mkv", "MKV", "mkv"),
  T ("webm", "WebM (VP9)", "webm"), T ("gif", "GIF (animated)", "gif"),
  T ("mp3", "MP3 (audio only)", "mp3"), T ("m4a", "M4A (audio only)", "m4a"),
  { NULL, NULL, NULL }
};
static const NfcTarget doc_targets[] = {
  T ("pdf", "PDF", "pdf"), T ("docx", "DOCX", "docx"), T ("odt", "ODT", "odt"),
  { NULL, NULL, NULL }
};
static const NfcTarget sheet_targets[] = {
  T ("pdf", "PDF", "pdf"), T ("xlsx", "XLSX", "xlsx"), T ("ods", "ODS", "ods"),
  T ("csv", "CSV", "csv"), { NULL, NULL, NULL }
};
static const NfcTarget slide_targets[] = {
  T ("pdf", "PDF", "pdf"), T ("pptx", "PPTX", "pptx"), T ("odp", "ODP", "odp"),
  { NULL, NULL, NULL }
};
#undef T

static const NfcTarget *
target_table (NfcCategory cat)
{
  switch (cat)
    {
    case NFC_CAT_IMAGE: return image_targets;
    case NFC_CAT_AUDIO: return audio_targets;
    case NFC_CAT_VIDEO: return video_targets;
    case NFC_CAT_DOC: return doc_targets;
    case NFC_CAT_SHEET: return sheet_targets;
    case NFC_CAT_SLIDE: return slide_targets;
    default: return NULL;
    }
}

/* LibreOffice --convert-to argument per target key */
static const char *
lo_filter (const char *key)
{
  static const char *map[][2] = {
    { "pdf", "pdf" },
    { "docx", "docx:MS Word 2007 XML" },
    { "odt", "odt" },
    { "xlsx", "xlsx:Calc MS Excel 2007 XML" },
    { "ods", "ods" },
    { "csv", "csv" },
    { "pptx", "pptx:Impress MS PowerPoint 2007 XML" },
    { "odp", "odp" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i][0], key))
      return map[i][1];
  return NULL;
}

static const char *
im_format_name (const char *key)
{
  static const char *map[][2] = {
    { "png", "PNG" }, { "jpg", "JPEG" }, { "webp", "WEBP" }, { "ico", "ICO" },
    { "bmp", "BMP" }, { "tiff", "TIFF" }, { "pdf", "PDF" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i][0], key))
      return map[i][1];
  return NULL;
}

static gboolean
is_pixbuf_src (const char *fmt)
{
  return g_str_equal (fmt, "heic") || g_str_equal (fmt, "avif");
}

gboolean
nfc_category_available (NfcCategory cat)
{
  nfc_tools_init ();
  switch (cat)
    {
    case NFC_CAT_IMAGE: return nfc_tools.magick != NULL;
    case NFC_CAT_AUDIO:
    case NFC_CAT_VIDEO: return nfc_tools.ffmpeg != NULL;
    case NFC_CAT_DOC:
    case NFC_CAT_SHEET:
    case NFC_CAT_SLIDE: return nfc_tools.soffice != NULL;
    default: return FALSE;
    }
}

/* Is the tool/codec needed for something available? Three-valued: the
 * capability probes run in the background, and until one has finished the
 * answer is PENDING (never "wait for it"). */
typedef enum { AV_NO, AV_YES, AV_PENDING } Avail;

static Avail
avail_and (Avail a, Avail b)
{
  if (a == AV_NO || b == AV_NO)
    return AV_NO;
  if (a == AV_PENDING || b == AV_PENDING)
    return AV_PENDING;
  return AV_YES;
}

static Avail
ff_has (const char *encoder)
{
  if (nfc_tools.ffmpeg == NULL)
    return AV_NO;
  if (!nfc_probe_done (NFC_PROBE_FFMPEG))
    return AV_PENDING;
  return nfc_have_encoder (encoder) ? AV_YES : AV_NO;
}

static Avail
im_can_write (const char *im_format)
{
  if (nfc_tools.magick == NULL)
    return AV_NO;
  if (!nfc_probe_done (NFC_PROBE_MAGICK))
    return AV_PENDING;
  return nfc_magick_can_write (im_format) ? AV_YES : AV_NO;
}

static Avail
im_has_any_writer (void)
{
  if (nfc_tools.magick == NULL)
    return AV_NO;
  if (!nfc_probe_done (NFC_PROBE_MAGICK))
    return AV_PENDING;
  return g_hash_table_size (im_write) > 0 ? AV_YES : AV_NO;
}

/* HEIC/AVIF input is decoded by the helper process via GdkPixbuf */
static Avail
pixbuf_can_read (const char *fmt)
{
  if (nfc_tools.decode == NULL)
    return AV_NO;
  if (!nfc_probe_done (NFC_PROBE_PIXBUF))
    return AV_PENDING;
  return nfc_pixbuf_can_read (fmt) ? AV_YES : AV_NO;
}

static Avail
target_avail (NfcCategory cat, const char *key)
{
  if (cat == NFC_CAT_IMAGE)
    {
      if (g_str_equal (key, "avif"))
        return avail_and (avail_and (nfc_tools.ffmpeg != NULL ? AV_YES : AV_NO,
                                     ff_has ("libaom-av1")),
                          im_has_any_writer ());
      return im_can_write (im_format_name (key));
    }
  if (cat == NFC_CAT_AUDIO || cat == NFC_CAT_VIDEO)
    {
      static const char *need[][2] = {
        { "mp3", "libmp3lame" }, { "flac", "flac" }, { "wav", "pcm_s16le" },
        { "ogg", "libvorbis" }, { "opus", "libopus" }, { "m4a", "aac" },
        { "mp4", "libx264" }, { "mkv", "libx264" }, { "webm", "libvpx-vp9" },
        { "gif", "gif" },
      };
      const char *enc = NULL;

      for (guint i = 0; i < G_N_ELEMENTS (need); i++)
        if (g_str_equal (need[i][0], key))
          enc = need[i][1];
      if (enc == NULL)
        return AV_NO;
      if (cat == NFC_CAT_VIDEO && g_str_equal (key, "webm"))
        return avail_and (ff_has (enc), ff_has ("libopus"));
      return ff_has (enc);
    }
  return nfc_tools.soffice != NULL ? AV_YES : AV_NO;
}

GPtrArray *
nfc_targets_for (NfcCategory cat, const char *fmt)
{
  GPtrArray *out = g_ptr_array_new ();
  const NfcTarget *table = target_table (cat);
  Avail src_ok = AV_YES;

  if (table == NULL || fmt == NULL)
    return out;
  /* ImageMagick here cannot read HEIC/AVIF; without the GdkPixbuf loader (in
   * the helper process) there is no way to decode them */
  if (cat == NFC_CAT_IMAGE && is_pixbuf_src (fmt))
    src_ok = pixbuf_can_read (fmt);
  if (src_ok != AV_YES)
    return out;
  for (const NfcTarget *t = table; t->key; t++)
    {
      if (g_str_equal (t->key, fmt))
        continue;
      if (target_avail (cat, t->key) != AV_YES) /* NO, or probe still running */
        continue;
      g_ptr_array_add (out, (gpointer) t);
    }
  return out;
}

/* -------------------------------------------------------------------------
 * Selection analysis
 * ---------------------------------------------------------------------- */

void
nfc_selection_clear (NfcSelection *sel)
{
  if (sel == NULL)
    return;
  g_clear_pointer (&sel->paths, g_ptr_array_unref);
  g_clear_pointer (&sel->fmts, g_ptr_array_unref);
  g_clear_pointer (&sel->targets, g_ptr_array_unref);
}

static gboolean
target_in (GPtrArray *arr, const NfcTarget *t)
{
  for (guint i = 0; i < arr->len; i++)
    if (g_str_equal (((const NfcTarget *) arr->pdata[i])->key, t->key))
      return TRUE;
  return FALSE;
}

gboolean
nfc_analyze_selection (const NfcSelItem *items, guint n, NfcSelection *sel)
{
  gboolean ok = FALSE;
  GPtrArray *first = NULL;
  NfcCategory cat = NFC_CAT_N;

  if (sel == NULL)
    return FALSE;
  memset (sel, 0, sizeof *sel);
  if (items == NULL || n == 0)
    return FALSE;

  sel->paths = g_ptr_array_new_with_free_func (g_free);
  sel->fmts = g_ptr_array_new ();
  sel->targets = g_ptr_array_new ();

  for (guint i = 0; i < n; i++)
    {
      const NfcSelItem *it = &items[i];
      char *scheme, *path;
      NfcFormat f;

      if (it->uri == NULL || it->is_dir)
        goto out;
      scheme = g_uri_parse_scheme (it->uri);
      if (scheme == NULL || g_ascii_strcasecmp (scheme, "file") != 0)
        {
          g_free (scheme);
          goto out;
        }
      g_free (scheme);
      path = g_filename_from_uri (it->uri, NULL, NULL);
      if (path == NULL)
        goto out;
      if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        {
          g_free (path);
          goto out;
        }
      if (!nfc_classify (path, it->mime, &f))
        {
          g_free (path);
          goto out;
        }
      if (i == 0)
        cat = f.cat;
      else if (f.cat != cat)
        {
          g_free (path);
          goto out;
        }
      g_ptr_array_add (sel->paths, path);
      g_ptr_array_add (sel->fmts, (gpointer) f.fmt);
    }

  if (!nfc_category_available (cat))
    goto out;
  nfc_probe_start (); /* no-op once started; never blocks */
  sel->cat = cat;

  /* targets valid for every selected file; a target equal to the source
   * format of any selected file is dropped (nfc_targets_for does that) */
  first = nfc_targets_for (cat, sel->fmts->pdata[0]);
  for (guint k = 0; k < first->len; k++)
    {
      const NfcTarget *t = first->pdata[k];
      gboolean all = TRUE;

      for (guint i = 1; i < sel->fmts->len && all; i++)
        {
          GPtrArray *o = nfc_targets_for (cat, sel->fmts->pdata[i]);

          all = target_in (o, t);
          g_ptr_array_unref (o);
        }
      if (all)
        g_ptr_array_add (sel->targets, (gpointer) t);
    }
  ok = sel->targets->len > 0;

out:
  g_clear_pointer (&first, g_ptr_array_unref);
  if (!ok)
    nfc_selection_clear (sel);
  return ok;
}

/* -------------------------------------------------------------------------
 * Command construction
 * ---------------------------------------------------------------------- */

static GPtrArray *
av_new (void)
{
  return g_ptr_array_new_null_terminated (24, g_free, TRUE);
}

static void av_add (GPtrArray *a, ...) G_GNUC_NULL_TERMINATED;

static void
av_add (GPtrArray *a, ...)
{
  va_list ap;
  const char *s;

  va_start (ap, a);
  while ((s = va_arg (ap, const char *)) != NULL)
    g_ptr_array_add (a, g_strdup (s));
  va_end (ap);
}

static char **
av_done (GPtrArray *a)
{
  return (char **) g_ptr_array_free (a, FALSE);
}

static GPtrArray *
attempt_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
}

static GPtrArray *
attempts_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
}

/* one attempt consisting of the given steps (NULL-terminated list of argv; a
 * NULL step is skipped; takes ownership of the argv) */
static void add_attempt (GPtrArray *attempts, char **first, ...) G_GNUC_NULL_TERMINATED;

static void
add_attempt (GPtrArray *attempts, char **first, ...)
{
  GPtrArray *at = attempt_new ();
  va_list ap;
  char **step = first;

  va_start (ap, first);
  for (;;)
    {
      if (step != NULL)
        g_ptr_array_add (at, step);
      step = va_arg (ap, char **);
      if (step == NULL)
        break;
    }
  va_end (ap);
  g_ptr_array_add (attempts, at);
}

/* H.264/VP9 4:2:0 need even sizes: drop the odd last row/column (a scale would
 * resample, i.e. blur, the whole picture by a pixel) */
#define EVEN "crop=trunc(iw/2)*2:trunc(ih/2)*2"

static GPtrArray *
ff_base (const char *src)
{
  GPtrArray *a = av_new ();

  av_add (a, nfc_tools.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
          "-y", "-i", src, NULL);
  return a;
}

static void
im_flatten (GPtrArray *a)
{
  av_add (a, "-background", "white", "-alpha", "remove", "-alpha", "off", NULL);
}

/* ImageMagick expands %-escapes ("%d", "%[filename]", ...) in every file name
 * it is given, input and output; "%%" is a literal percent sign. */
static char *
im_escape (const char *s)
{
  GString *g = g_string_new (NULL);

  for (; *s; s++)
    {
      if (*s == '%')
        g_string_append_c (g, '%');
      g_string_append_c (g, *s);
    }
  return g_string_free (g, FALSE);
}

/* Explicit input coder, from the classified type: ImageMagick must not pick a
 * coder by sniffing the content (an ".png" that is really PostScript, MVG or
 * MSL would otherwise reach Ghostscript or the script coders).
 *
 * SVG: never the plain "SVG:" coder. Depending on the build, ImageMagick
 * delegates it to an external renderer (Inkscape here: a child process per
 * conversion that follows some references). "RSVG:" is librsvg in-process (no
 * helper, much faster, and it does not embed text:/file:// or relative hrefs).
 * Without librsvg fall back to ImageMagick's own "MSVG:" renderer (IM 7.1.2
 * refuses explicit coders inside it). No policy.xml is installed. */
static const char *
im_input_coder (const char *fmt)
{
  static const char *map[][2] = {
    { "png", "PNG" }, { "jpg", "JPEG" }, { "webp", "WEBP" }, { "bmp", "BMP" },
    { "gif", "GIF" }, { "ico", "ICO" }, { "tiff", "TIFF" },
  };

  if (g_str_equal (fmt, "svg"))
    return nfc_magick_can_read ("RSVG") ? "RSVG" : "MSVG";

  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i][0], fmt))
      return map[i][1];
  return NULL;
}

/* "CODER:<escaped path>" plus an optional suffix such as "[0]" */
static char *
im_path_arg (const char *coder, const char *path, const char *suffix)
{
  char *esc = im_escape (path);
  char *r = g_strconcat (coder, ":", esc, suffix != NULL ? suffix : "", NULL);

  g_free (esc);
  return r;
}

/* Resource limits: a hostile or huge image must not eat the machine, but a
 * legitimate big one (a 19200 x 10800 upscaled screenshot is 207 megapixels,
 * ~3.3 GB of Q16-HDRI pixel cache) must still convert. ImageMagick's own
 * defaults scale with the machine, so only the memory budget is tightened: a
 * quarter of the physical RAM, between 1 and 8 GiB. Pixel data beyond that
 * spills to a memory-mapped file (map: twice the budget) and then to a disk
 * cache (generous 16 GiB), so a huge image becomes slow rather than failing,
 * and two parallel conversions cannot take the whole machine. Memory cannot
 * be read (no /proc)? 2 GiB. */
#define GIB ((guint64) 1024 * 1024 * 1024)

static guint64
im_memory_budget (void)
{
  char *text = NULL;
  guint64 total = 0, budget;

  if (g_file_get_contents ("/proc/meminfo", &text, NULL, NULL))
    {
      const char *p = strstr (text, "MemTotal:");

      if (p != NULL)
        total = g_ascii_strtoull (p + strlen ("MemTotal:"), NULL, 10) * 1024; /* kB */
    }
  g_free (text);
  budget = total != 0 ? total / 4 : 2 * GIB;
  return CLAMP (budget, 1 * GIB, 8 * GIB);
}

static void
im_limits (GPtrArray *a)
{
  guint64 mem = im_memory_budget () / (1024 * 1024);
  char *m = g_strdup_printf ("%" G_GUINT64_FORMAT "MiB", mem);
  char *map = g_strdup_printf ("%" G_GUINT64_FORMAT "MiB", 2 * mem);

  av_add (a, "-limit", "memory", m, "-limit", "map", map, "-limit", "disk", "16GiB",
          "-limit", "time", "600", NULL);
  g_free (m);
  g_free (map);
}


/* Photographic (lossy, camera or phone) source versus graphics (screenshots,
 * drawings, icons, scans: lossless formats). The two want different
 * settings: sharp text and colour edges need full chroma resolution and a
 * much higher quality; a photo does not, and the lossy encoders already
 * start from lossy data. WEBP sources count as photos: the format is mostly
 * used lossy. */
static gboolean
photo_source (const char *fmt)
{
  return g_str_equal (fmt, "jpg") || g_str_equal (fmt, "heic") ||
         g_str_equal (fmt, "avif") || g_str_equal (fmt, "webp");
}

/* The ffmpeg step that makes the AVIF from the flattened PNG.
 *
 * Graphics: 4:4:4 (no chroma smearing of text), crf 9. Photos: 4:2:0, crf 12.
 * 8-bit: the 10-bit variants gave no better result on screenshots once the
 * viewer converts back to 8 bits, and 8-bit 4:4:4 decodes everywhere.
 * Colour: RGB -> YCbCr with the BT.709 matrix, FULL range, and the file is
 * tagged exactly that (colr box), so viewers do not guess BT.601/limited and
 * shift the colours. No even-size scale: AVIF handles odd sizes, and the old
 * even-size scale resampled (blurred) every odd-sized image by a pixel.
 * No -tiles: AV1 allows tiles up to 4096 px wide only, libaom picks as many as
 * the width needs by itself (an explicit count made wide images fail). */
static char **
ff_avif_step (const char *flat, const char *out, gboolean photo)
{
  GPtrArray *a = ff_base (flat);

  av_add (a, "-frames:v", "1", "-vf",
          photo ? "scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv420p"
                : "scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv444p",
          "-c:v", "libaom-av1", "-crf", photo ? "12" : "9", "-b:v", "0",
          "-cpu-used", "6", "-row-mt", "1", "-still-picture", "1",
          "-colorspace", "bt709", "-color_primaries", "bt709", "-color_range", "pc",
          "-f", "avif", out, NULL);
  return av_done (a);
}

GPtrArray *
nfc_build_attempts (NfcCategory cat, const char *fmt, const char *key,
                    const char *src_in, const char *out, const char *workdir,
                    gboolean *needs_decode)
{
  GPtrArray *attempts, *a;
  char *src;

  if (needs_decode != NULL)
    *needs_decode = FALSE;
  if (src_in == NULL || fmt == NULL || key == NULL || out == NULL || workdir == NULL)
    return NULL;
  src = g_canonicalize_filename (src_in, NULL);
  attempts = attempts_new ();

  if (cat == NFC_CAT_IMAGE)
    {
      char *im_in, *decoded = NULL;
      char **dec_step = NULL;
      const char *imname, *coder;

      if (nfc_tools.magick == NULL ||
          (im_format_name (key) == NULL && !g_str_equal (key, "avif")))
        goto fail;
      if (is_pixbuf_src (fmt))
        {
          /* step 1: nfc-decode <src> <workdir>/decoded.png (own process) */
          if (nfc_tools.decode == NULL)
            goto fail;
          decoded = g_build_filename (workdir, "decoded.png", NULL);
          a = av_new ();
          av_add (a, nfc_tools.decode, src, decoded, NULL);
          dec_step = av_done (a);
          im_in = im_path_arg ("PNG", decoded, "[0]");
          if (needs_decode != NULL)
            *needs_decode = TRUE;
        }
      else
        {
          coder = im_input_coder (fmt);
          if (coder == NULL)
            goto fail;
          im_in = im_path_arg (coder, src, "[0]");
        }

      if (g_str_equal (key, "avif"))
        {
          char *flat, *flatp;
          char **s1, **s2;

          if (nfc_tools.ffmpeg == NULL)
            {
              g_free (im_in);
              g_free (decoded);
              g_strfreev (dec_step);
              goto fail;
            }
          flat = g_build_filename (workdir, "flat.png", NULL);
          flatp = im_path_arg ("PNG", flat, NULL);
          a = av_new ();
          av_add (a, nfc_tools.magick, "-quiet", NULL);
          im_limits (a);
          if (g_str_equal (fmt, "svg"))
            av_add (a, "-background", "none", "-density", "192", NULL);
          av_add (a, im_in, "-auto-orient", NULL);
          im_flatten (a);
          av_add (a, flatp, NULL);
          s1 = av_done (a);

          /* attempt 1: the quality settings of the source class. Attempt 2
           * (8-bit 4:2:0) only runs when libaom refuses attempt 1, which it
           * does for some huge images (4:4:4 at ~200 megapixels). Each attempt
           * runs the whole chain: the steps are owned by the attempt. */
          s2 = ff_avif_step (flat, out, photo_source (fmt));
          add_attempt (attempts, dec_step, s1, s2, NULL);
          if (!photo_source (fmt))
            add_attempt (attempts, g_strdupv (dec_step), g_strdupv (s1),
                         ff_avif_step (flat, out, TRUE), NULL);
          g_free (flat);
          g_free (flatp);
          g_free (im_in);
          g_free (decoded);
          g_free (src);
          return attempts;
        }

      imname = im_format_name (key);
      a = av_new ();
      av_add (a, nfc_tools.magick, "-quiet", NULL);
      im_limits (a);
      if (g_str_equal (fmt, "svg"))
        av_add (a, "-background", "none", "-density", "192", NULL);
      av_add (a, im_in, NULL);
      if (g_str_equal (key, "jpg"))
        {
          /* 4:4:4 stated explicitly (ImageMagick only does it by itself from
           * quality 90 up, and a screenshot's text and colour edges are
           * smeared by 4:2:0). Graphics need q99 to stay clean around text
           * (SSIM on text-heavy screenshots: q92 0.90-0.98, q98 0.96-0.998, q99
           * 0.989-0.999);
           * photos are fine at q95. */
          av_add (a, "-auto-orient", NULL);
          im_flatten (a);
          av_add (a, "-quality", photo_source (fmt) ? "95" : "99",
                  "-sampling-factor", "4:4:4", NULL);
        }
      else if (g_str_equal (key, "webp"))
        {
          /* WEBP stores at most 16383 pixels per side: shrink only beyond that.
           * Graphics (lossless sources): lossless WEBP, which was smaller than
           * the source PNG for every screenshot tried and has no chroma
           * subsampling or ringing. Photos: lossy q94, method 6, sharp YUV
           * (the chroma conversion that keeps colour edges). */
          av_add (a, "-auto-orient", "-resize", "16383x16383>", NULL);
          if (photo_source (fmt))
            av_add (a, "-quality", "94", "-define", "webp:method=6",
                    "-define", "webp:sharp-yuv=true", NULL);
          else
            av_add (a, "-define", "webp:lossless=true", "-define", "webp:method=4", NULL);
        }
      else if (g_str_equal (key, "ico"))
        {
          /* up to 256 px, never enlarged, aspect ratio kept (padded
           * transparently to a square: icon:auto-resize would stretch a
           * non-square image), then the usual 256/48/32/16 sizes */
          av_add (a, "-auto-orient", "-resize", "256x256>", "-background", "none",
                  "-gravity", "center", "-set", "option:nfc:side", "%[fx:max(w,h)]",
                  "-extent", "%[option:nfc:side]x%[option:nfc:side]",
                  "-define", "icon:auto-resize=256,48,32,16", NULL);
        }
      else if (g_str_equal (key, "pdf"))
        {
          av_add (a, "-auto-orient", NULL);
          im_flatten (a);
        }
      else /* png, bmp, tiff */
        av_add (a, "-auto-orient", NULL);
      {
        /* explicit format prefix: ImageMagick silently writes PNG data under
         * an .avif/.heic name, and never trusts the extension we want */
        char *o = im_path_arg (imname, out, NULL);

        av_add (a, o, NULL);
        g_free (o);
      }
      add_attempt (attempts, dec_step, av_done (a), NULL);
      g_free (im_in);
      g_free (decoded);
      g_free (src);
      return attempts;
    }

  if (cat == NFC_CAT_AUDIO || cat == NFC_CAT_VIDEO)
    {
      if (nfc_tools.ffmpeg == NULL)
        goto fail;

      if (g_str_equal (key, "mp3") || g_str_equal (key, "flac") ||
          g_str_equal (key, "wav") || g_str_equal (key, "ogg") ||
          g_str_equal (key, "opus") || g_str_equal (key, "m4a"))
        {
          a = ff_base (src);
          /* -vn drops cover art/video; map first audio stream only */
          av_add (a, "-vn", "-sn", "-dn", "-map", "0:a:0", "-map_metadata", "0", NULL);
          if (g_str_equal (key, "mp3"))
            av_add (a, "-c:a", "libmp3lame", "-q:a", "2", NULL);
          else if (g_str_equal (key, "flac"))
            av_add (a, "-c:a", "flac", NULL);
          else if (g_str_equal (key, "wav"))
            av_add (a, "-c:a", "pcm_s16le", NULL);
          else if (g_str_equal (key, "ogg"))
            av_add (a, "-c:a", "libvorbis", "-q:a", "5", NULL);
          else if (g_str_equal (key, "opus"))
            av_add (a, "-c:a", "libopus", "-b:a", "128k", NULL);
          else
            av_add (a, "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", NULL);
          av_add (a, out, NULL);
          add_attempt (attempts, av_done (a), NULL);
          g_free (src);
          return attempts;
        }

      if (g_str_equal (key, "mp4"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-vf", EVEN, "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
                  "-movflags", "+faststart", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "mkv"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c", "copy", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-vf", EVEN, "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "webm"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c:v", "libvpx-vp9", "-crf", "28", "-b:v", "0", "-row-mt", "1",
                  "-deadline", "good", "-cpu-used", "2", "-pix_fmt", "yuv420p", "-vf", EVEN,
                  "-c:a", "libopus", "-b:a", "128k", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "gif"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
                  "fps=15,scale='min(960,iw)':-1:flags=lanczos,"
                  "split[s0][s1];[s0]palettegen=stats_mode=diff[p];"
                  "[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
                  "-loop", "0", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else
        goto fail;
      g_free (src);
      return attempts;
    }

  /* documents */
  {
    const char *flt = lo_filter (key);
    char *outdir, *profile, *envarg, *puri;

    if (flt == NULL || nfc_tools.soffice == NULL)
      goto fail;
    /* --outdir is percent-decoded by LibreOffice when given as a plain path
     * ("%20" in a directory name would become a space), so give it a URI */
    char *outdir_path = g_path_get_dirname (out);

    outdir = g_filename_to_uri (outdir_path, NULL, NULL);
    g_free (outdir_path);
    if (outdir == NULL)
      outdir = g_strdup ("");
    profile = g_build_filename (workdir, "lo_profile", NULL);
    /* a proper URI: a space or '%' in the temp dir would otherwise crash soffice */
    puri = g_filename_to_uri (profile, NULL, NULL);
    envarg = g_strconcat ("-env:UserInstallation=", puri != NULL ? puri : "", NULL);
    g_free (puri);
    a = av_new ();
    av_add (a, nfc_tools.soffice, envarg, "--headless", "--norestore",
            "--nolockcheck", "--nodefault", "--nologo", "--convert-to", flt,
            "--outdir", outdir, src, NULL);
    add_attempt (attempts, av_done (a), NULL);
    g_free (outdir);
    g_free (profile);
    g_free (envarg);
    g_free (src);
    return attempts;
  }

fail:
  g_free (src);
  g_ptr_array_unref (attempts);
  return NULL;
}

/* -------------------------------------------------------------------------
 * Output naming
 * ---------------------------------------------------------------------- */

char *
nfc_candidate_name (const char *stem, const char *ext, guint n)
{
  if (n == 0)
    return g_strdup_printf ("%s.%s", stem, ext);
  return g_strdup_printf ("%s (%u).%s", stem, n, ext);
}

static GMutex name_lock;

/* rename without ever replacing an existing entry. Returns 0, or -1 with errno
 * (EEXIST: the name is taken; EINVAL/ENOSYS/...: not supported here). */
static int
rename_noreplace (const char *from, const char *to)
{
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
  return (int) syscall (SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
#else
  (void) from;
  (void) to;
  errno = ENOSYS;
  return -1;
#endif
}

/* Last resort for filesystems that neither link nor know RENAME_NOREPLACE:
 * reserve the name with O_CREAT|O_EXCL (atomic), then rename over our own
 * placeholder.
 *
 * Only the reservation's own EEXIST (the open below) means "name taken, try
 * the next one": the caller loops on it. rename() over our own placeholder
 * cannot legitimately fail with EEXIST, but a buggy filesystem may say so; it
 * is mapped to EIO so the caller fails instead of retrying forever. */
static int
reserve_and_rename (const char *from, const char *to)
{
  int fd = open (to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

  if (fd < 0)
    return -1;
  close (fd);
  if (rename (from, to) != 0)
    {
      int saved = errno == EEXIST ? EIO : errno;

      unlink (to);
      errno = saved;
      return -1;
    }
  return 0;
}

/* upper bound for the "stem (n).ext" counter in nfc_publish_with_mode */
#define NFC_MAX_NAME_TRIES 10000

char *
nfc_publish_with_mode (const char *tmp_path, const char *directory, const char *stem,
                       const char *ext, NfcPublishMode mode, GError **error)
{
  char *result = NULL;

  g_mutex_lock (&name_lock);
  for (guint n = 0;; n++)
    {
      char *name, *cand;
      int link_errno = 0;

      /* "name (1)" .. "name (9999)" taken: give up rather than loop on and on */
      if (n >= NFC_MAX_NAME_TRIES)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                       "cannot create %s.%s in %s: the first %u names are all taken",
                       stem, ext, directory, (guint) NFC_MAX_NAME_TRIES);
          break;
        }
      name = nfc_candidate_name (stem, ext, n);
      cand = g_build_filename (directory, name, NULL);
      g_free (name);
      if (mode == NFC_PUBLISH_AUTO)
        {
          if (link (tmp_path, cand) == 0) /* fails with EEXIST if it appeared */
            {
              unlink (tmp_path);
              result = cand;
              break;
            }
          if (errno == EEXIST)
            {
              g_free (cand);
              continue;
            }
          link_errno = errno;
        }
      /* filesystem without hard links (FAT, ...) */
      if (mode != NFC_PUBLISH_NO_LINK_NO_RENAME2)
        {
          if (rename_noreplace (tmp_path, cand) == 0)
            {
              result = cand;
              break;
            }
          if (errno == EEXIST)
            {
              g_free (cand);
              continue;
            }
          if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP)
            goto failed;
        }
      if (reserve_and_rename (tmp_path, cand) == 0)
        {
          result = cand;
          break;
        }
      if (errno == EEXIST)
        {
          g_free (cand);
          continue;
        }
    failed:
      {
        int e = errno;

        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e),
                     "cannot create %s: %s (link: %s)", cand, g_strerror (e),
                     link_errno ? g_strerror (link_errno) : "not tried");
        g_free (cand);
        break;
      }
    }
  g_mutex_unlock (&name_lock);
  return result;
}

char *
nfc_publish (const char *tmp_path, const char *directory, const char *stem,
             const char *ext, GError **error)
{
  return nfc_publish_with_mode (tmp_path, directory, stem, ext, NFC_PUBLISH_AUTO, error);
}

char *
nfc_hidden_temp_name (const char *directory, const char *stem, const char *ext)
{
  size_t len = strlen (stem);
  char *short_stem, *name, *path;

  if (len > 80)
    {
      /* cut on a UTF-8 character boundary */
      const char *p = stem + 80;

      while (p > stem && (*p & 0xC0) == 0x80)
        p--;
      len = p - stem;
    }
  short_stem = g_strndup (stem, len);
  name = g_strdup_printf (".%s.nfc-%08x.%s", short_stem, g_random_int (), ext);
  path = g_build_filename (directory, name, NULL);
  g_free (short_stem);
  g_free (name);
  return path;
}
