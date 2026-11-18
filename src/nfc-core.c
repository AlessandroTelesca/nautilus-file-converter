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
#include "nfc-header.h"
#include "nfc-util.h"

NfcTools nfc_tools = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

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


/* The Upscale backend: NFC_UPSCAYL_BIN (tests; set but not executable means none),
 * else Upscayl's own install, else upscayl-bin on the PATH. NULL if absent. */
static char *
find_upscayl (void)
{
  const char *env = g_getenv ("NFC_UPSCAYL_BIN");

  if (env != NULL && *env != '\0')
    return g_file_test (env, G_FILE_TEST_IS_EXECUTABLE) ? g_strdup (env) : NULL;
  if (g_file_test ("/opt/Upscayl/resources/bin/upscayl-bin", G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup ("/opt/Upscayl/resources/bin/upscayl-bin");
  return g_find_program_in_path ("upscayl-bin");
}

static char *
upscale_config_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "nautilus-file-converter", "config", NULL);
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
      g_free (nfc_tools.ffprobe);
      g_free (nfc_tools.upscayl);
      g_free (nfc_tools.upscale_models);
      g_free (nfc_tools.upscale_model);
      nfc_tools.magick = find_first ("magick", "convert");
      nfc_tools.ffmpeg = find_first ("ffmpeg", NULL);
      nfc_tools.soffice = find_first ("soffice", "libreoffice");
      nfc_tools.notify = find_first ("notify-send", NULL);
      nfc_tools.decode = find_decode_helper ();
      nfc_tools.ffprobe = find_first ("ffprobe", NULL);
      /* Upscayl: the binary, its models and (read once, here) the configured
       * model; without usable models there is no Upscale entry */
      nfc_tools.upscayl = find_upscayl ();
      nfc_tools.upscale_models = NULL;
      nfc_tools.upscale_model = NULL;
      if (nfc_tools.upscayl != NULL)
        {
          char *config = upscale_config_path ();

          if (!nfc_upscale_resolve (nfc_tools.upscayl, config, &nfc_tools.upscale_models,
                                    &nfc_tools.upscale_model))
            g_clear_pointer (&nfc_tools.upscayl, g_free);
          g_free (config);
        }
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

/* Fill a fresh ff_enc from the text of `ffmpeg -encoders`. */
static GHashTable *
parse_encoders (const char *out)
{
  GHashTable *enc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  char **lines = g_strsplit (out, "\n", -1);

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
          g_hash_table_add (enc, g_strndup (sp, e - sp));
      }
    }
  g_strfreev (lines);
  return enc;
}

static void
probe_ffmpeg (void)
{
  char *out = NULL;

  if (nfc_tools.ffmpeg == NULL)
    {
      ff_enc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      return;
    }
  {
    const char *argv[] = { nfc_tools.ffmpeg, "-hide_banner", "-encoders", NULL };
    out = run_quiet (argv, "ffmpeg");
  }
  ff_enc = parse_encoders (out != NULL ? out : "");
  g_free (out);
}

/* Tests only: take `text` as the output of `ffmpeg -encoders` (instead of
 * running ffmpeg) and mark the ffmpeg probe done. Call it before anything
 * starts the probes, or while no probe thread is running. */
void
nfc_test_set_encoders (const char *text)
{
  GHashTable *old = ff_enc;

  ff_enc = parse_encoders (text);
  g_atomic_int_set (&probe_state[NFC_PROBE_FFMPEG], 1);
  if (old != NULL)
    g_hash_table_unref (old);
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

/* -------------------------------------------------------------------------
 * Quality presets
 * ---------------------------------------------------------------------- */

static const struct { const char *key, *label; } preset_names[NFC_PRESET_N] = {
  { "best", "Best" }, { "high", "High" }, { "balanced", "Balanced" }, { "small", "Small" },
};

const char *
nfc_preset_key (NfcPreset preset)
{
  return (guint) preset < NFC_PRESET_N ? preset_names[preset].key : "best";
}

const char *
nfc_preset_label (NfcPreset preset)
{
  return (guint) preset < NFC_PRESET_N ? preset_names[preset].label : "Best";
}

gboolean
nfc_preset_from_key (const char *key, NfcPreset *out)
{
  if (key == NULL)
    return FALSE;
  for (guint i = 0; i < NFC_PRESET_N; i++)
    if (g_str_equal (preset_names[i].key, key))
      {
        if (out != NULL)
          *out = (NfcPreset) i;
        return TRUE;
      }
  return FALSE;
}

gboolean
nfc_target_has_presets (NfcCategory cat, const char *key)
{
  if (key == NULL)
    return FALSE;
  if (cat == NFC_CAT_IMAGE)
    return g_str_equal (key, "jpg") || g_str_equal (key, "webp") || g_str_equal (key, "avif");
  if (cat == NFC_CAT_VIDEO)
    return g_str_equal (key, "mp4") || g_str_equal (key, "webm") || g_str_equal (key, "gif");
  return FALSE;
}

const char *
nfc_preset_menu_label (NfcCategory cat, const char *key, NfcPreset preset)
{
  if (preset != NFC_PRESET_SMALL)
    return nfc_preset_label (preset);
  if (key == NULL)
    return "Small (max 2560 px / 1080p)";
  if (!nfc_target_has_presets (cat, key))
    return "Small";
  if (cat == NFC_CAT_IMAGE)
    return "Small (max 2560 px)";
  return g_str_equal (key, "gif") ? "Small (480 px)" : "Small (max 1080p)";
}

gboolean
nfc_preset_output_size (NfcCategory cat, const char *key, NfcPreset preset,
                        guint64 w, guint64 h, guint64 *ow, guint64 *oh)
{
  static const NfcResizePreset small_image = { "small", "", "", 2560, 0, FALSE };
  static const NfcResizePreset small_video = { "small", "", "", 1080, 0, TRUE };

  if (preset != NFC_PRESET_SMALL || key == NULL || !nfc_target_has_presets (cat, key))
    return FALSE;
  if (cat == NFC_CAT_IMAGE)
    return nfc_resize_output_size (&small_image, w, h, ow, oh);
  if (g_str_equal (key, "gif"))
    {
      /* scale='min(480,iw)':-1 */
      if (w <= 480 || h == 0)
        return FALSE;
      if (ow != NULL)
        *ow = 480;
      if (oh != NULL)
        *oh = MAX ((MIN (h, (guint64) 1 << 31) * 480 + w / 2) / w, (guint64) 1);
      return TRUE;
    }
  return nfc_resize_output_size (&small_video, w, h, ow, oh);
}

/* The tooltips say what a preset does, with the sizes measured against Best
 * (see README: they depend on the content). */
const char *
nfc_preset_tooltip (NfcCategory cat, const char *key, NfcPreset preset)
{
  static const char *const image_tips[NFC_PRESET_N] = {
    "Best — the highest quality; the file can be large",
    "High — visually identical, typically 55-70% of the size of Best",
    "Balanced — good for sharing, typically 25-45% of the size of Best",
    "Small — smallest acceptable file, typically 10-30% of Best; images over 2560 px are scaled down to 2560 px",
  };
  /* WEBP of graphics is the smaller of a lossy and the lossless file: flat
   * screenshots stay lossless, so no typical size is promised */
  static const char *const webp_tips[NFC_PRESET_N] = {
    "Best — the highest quality; the file can be large",
    "High — very close to Best, a much smaller file (flat screenshots stay lossless when that is smaller)",
    "Balanced — good for sharing, smaller still (flat screenshots stay lossless when that is smaller)",
    "Small — the smallest acceptable file; images over 2560 px are scaled down to 2560 px",
  };
  static const char *const video_tips[NFC_PRESET_N] = {
    "Best — the highest quality; the file can be large",
    "High — visually identical, typically 55-70% of the size of Best",
    "Balanced — good for sharing, typically 25-45% of the size of Best",
    "Small — smallest acceptable file, typically 15-20% of Best; video above 1080p is scaled down to 1080p",
  };
  static const char *const gif_tips[NFC_PRESET_N] = {
    "Best — 960 px wide, 15 frames/s, 256 colours",
    "High — 720 px wide, 15 frames/s, about 60% of the size of Best",
    "Balanced — 640 px wide, 12 frames/s, about 40% of the size of Best",
    "Small — 480 px wide, 10 frames/s, 128 colours, about 17% of the size of Best",
  };

  if ((guint) preset >= NFC_PRESET_N || !nfc_target_has_presets (cat, key))
    return "";
  if (cat == NFC_CAT_IMAGE)
    return g_str_equal (key, "webp") ? webp_tips[preset] : image_tips[preset];
  return g_str_equal (key, "gif") ? gif_tips[preset] : video_tips[preset];
}

/* ---- keep only what is smaller / Compress ---- */

gboolean
nfc_is_smaller (gint64 size, gint64 reference)
{
  return size >= 0 && reference >= 0 && size < reference;
}

gboolean
nfc_compress_target (NfcCategory cat, const char *fmt, NfcCompressTarget *out)
{
  static const struct {
    NfcCategory src_cat;
    const char *fmt;
    NfcCategory cat;
    const char *key, *ext;
  } map[] = {
    { NFC_CAT_IMAGE, "jpg",  NFC_CAT_IMAGE, "jpg",  "jpg" },
    { NFC_CAT_IMAGE, "webp", NFC_CAT_IMAGE, "webp", "webp" },
    { NFC_CAT_IMAGE, "avif", NFC_CAT_IMAGE, "avif", "avif" },
    /* the GIF presets belong to the video target "gif" (ffmpeg, palette) */
    { NFC_CAT_IMAGE, "gif",  NFC_CAT_VIDEO, "gif",  "gif" },
    { NFC_CAT_VIDEO, "mp4",  NFC_CAT_VIDEO, "mp4",  "mp4" },
    { NFC_CAT_VIDEO, "m4v",  NFC_CAT_VIDEO, "mp4",  "mp4" },
    { NFC_CAT_VIDEO, "webm", NFC_CAT_VIDEO, "webm", "webm" },
  };

  if (fmt == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (map[i].src_cat == cat && g_str_equal (map[i].fmt, fmt))
      {
        if (out != NULL)
          {
            out->cat = map[i].cat;
            out->key = map[i].key;
            out->ext = map[i].ext;
          }
        return TRUE;
      }
  return FALSE;
}

const char *
nfc_compress_tooltip (NfcPreset preset)
{
  switch (preset)
    {
    case NFC_PRESET_HIGH:
      return "High — visually identical, a moderately smaller copy";
    case NFC_PRESET_BALANCED:
      return "Balanced — good for sharing, a clearly smaller copy";
    case NFC_PRESET_SMALL:
      return "Small — the smallest acceptable copy; images over 2560 px and video above 1080p are scaled down";
    default:
      return "";
    }
}

/* ---- Resize ---- */

static const NfcResizePreset image_resize[] = {
  { "4k",    "4K (3840 px)",    "4K",    3840,  0, FALSE },
  { "1440p", "1440p (2560 px)", "1440p", 2560,  0, FALSE },
  { "1080p", "1080p (1920 px)", "1080p", 1920,  0, FALSE },
  { "720p",  "720p (1280 px)",  "720p",  1280,  0, FALSE },
  { "480p",  "480p (854 px)",   "480p",  854,   0, FALSE },
  { "50pct", "50%",             "50%",   0,    50, FALSE },
  { "25pct", "25%",             "25%",   0,    25, FALSE },
};

static const NfcResizePreset video_resize[] = {
  { "1080p", "1080p", "1080p", 1080, 0, TRUE },
  { "720p",  "720p",  "720p",  720,  0, TRUE },
  { "480p",  "480p",  "480p",  480,  0, TRUE },
};

const NfcResizePreset *
nfc_resize_presets (NfcCategory cat, guint *n)
{
  if (cat == NFC_CAT_IMAGE)
    {
      if (n != NULL)
        *n = G_N_ELEMENTS (image_resize);
      return image_resize;
    }
  if (cat == NFC_CAT_VIDEO)
    {
      if (n != NULL)
        *n = G_N_ELEMENTS (video_resize);
      return video_resize;
    }
  if (n != NULL)
    *n = 0;
  return NULL;
}

const NfcResizePreset *
nfc_resize_preset_from_key (NfcCategory cat, const char *key)
{
  guint n;
  const NfcResizePreset *p = nfc_resize_presets (cat, &n);

  if (key == NULL)
    return NULL;
  for (guint i = 0; i < n; i++)
    if (g_str_equal (p[i].key, key))
      return &p[i];
  return NULL;
}

/* nearest integer of v * num / den (what ImageMagick's geometry does: floor(x + 0.5)) */
static guint64
scaled (guint64 v, guint64 num, guint64 den)
{
  return (v * num + den / 2) / den;
}

gboolean
nfc_resize_output_size (const NfcResizePreset *p, guint64 w, guint64 h, guint64 *ow, guint64 *oh)
{
  guint64 nw, nh;

  if (p == NULL || w == 0 || h == 0)
    return FALSE;
  /* bounded, so that the products below cannot overflow */
  w = MIN (w, (guint64) 1 << 31);
  h = MIN (h, (guint64) 1 << 31);
  if (p->percent > 0)
    {
      nw = scaled (w, p->percent, 100);
      nh = scaled (h, p->percent, 100);
      if (p->percent >= 100 || MIN (nw, nh) < NFC_RESIZE_MIN_SIDE)
        return FALSE;
    }
  else if (p->side == 0)
    return FALSE;
  else if (!p->short_side)
    {
      guint64 longer = MAX (w, h);

      if (longer <= p->side)
        return FALSE;
      nw = w >= h ? p->side : MAX (scaled (w, p->side, longer), (guint64) 1);
      nh = w >= h ? MAX (scaled (h, p->side, longer), (guint64) 1) : p->side;
    }
  else
    {
      guint64 shorter = MIN (w, h);

      if (shorter <= p->side)
        return FALSE;
      /* the long side is made even (ffmpeg's -2), the short one is the preset */
      nw = w <= h ? p->side : MAX (2 * scaled (scaled (w, p->side, shorter), 1, 2), (guint64) 2);
      nh = w <= h ? MAX (2 * scaled (scaled (h, p->side, shorter), 1, 2), (guint64) 2) : p->side;
    }
  if (ow != NULL)
    *ow = nw;
  if (oh != NULL)
    *oh = nh;
  return TRUE;
}

char *
nfc_resize_tooltip (const NfcResizePreset *p, guint64 w, guint64 h)
{
  guint64 ow, oh;

  if (p == NULL)
    return g_strdup ("");
  if (nfc_resize_output_size (p, w, h, &ow, &oh))
    return g_strdup_printf ("Make a copy scaled down to %" G_GUINT64_FORMAT " × %" G_GUINT64_FORMAT
                            " px (aspect ratio kept)", ow, oh);
  if (p->percent > 0)
    return g_strdup_printf ("Make a copy scaled down to %u%% of the width and height", p->percent);
  if (p->short_side)
    return g_strdup_printf ("Make a copy scaled down so that its shorter side is %u px "
                            "(aspect ratio kept); the video is re-encoded", p->side);
  return g_strdup_printf ("Make a copy scaled down so that its longer side is %u px "
                          "(aspect ratio kept)", p->side);
}

gboolean
nfc_resize_target (NfcCategory cat, const char *fmt, NfcResizeTarget *out)
{
  static const struct {
    NfcCategory src_cat;
    const char *fmt;
    NfcCategory cat;
    const char *key, *ext;
  } map[] = {
    { NFC_CAT_IMAGE, "png",  NFC_CAT_IMAGE, "png",  "png" },
    { NFC_CAT_IMAGE, "jpg",  NFC_CAT_IMAGE, "jpg",  "jpg" },
    { NFC_CAT_IMAGE, "webp", NFC_CAT_IMAGE, "webp", "webp" },
    { NFC_CAT_IMAGE, "avif", NFC_CAT_IMAGE, "avif", "avif" },
    { NFC_CAT_IMAGE, "bmp",  NFC_CAT_IMAGE, "bmp",  "bmp" },
    { NFC_CAT_IMAGE, "tiff", NFC_CAT_IMAGE, "tiff", "tiff" },
    /* an animation: ffmpeg's scale + palette chain, like the GIF presets */
    { NFC_CAT_IMAGE, "gif",  NFC_CAT_VIDEO, "gif",  "gif" },
    { NFC_CAT_VIDEO, "mp4",  NFC_CAT_VIDEO, "mp4",  "mp4" },
    { NFC_CAT_VIDEO, "m4v",  NFC_CAT_VIDEO, "mp4",  "mp4" }, /* ffmpeg's .m4v muxer is raw H.264 */
    { NFC_CAT_VIDEO, "mkv",  NFC_CAT_VIDEO, "mkv",  "mkv" },
    { NFC_CAT_VIDEO, "webm", NFC_CAT_VIDEO, "webm", "webm" },
    { NFC_CAT_VIDEO, "mov",  NFC_CAT_VIDEO, "mov",  "mov" },
    { NFC_CAT_VIDEO, "avi",  NFC_CAT_VIDEO, "avi",  "avi" },
  };

  if (fmt == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (map[i].src_cat == cat && g_str_equal (map[i].fmt, fmt))
      {
        if (out != NULL)
          {
            out->cat = map[i].cat;
            out->key = map[i].key;
            out->ext = map[i].ext;
          }
        return TRUE;
      }
  return FALSE;
}

/* ---- Upscale ---- */

static const NfcUpscalePreset upscale_presets[] = {
  { "2x", "2×", 2, { 2, 0 } },
  { "3x", "3×", 3, { 3, 0 } },
  { "4x", "4×", 4, { 4, 0 } },
  /* no integer product: 3x then 2x is 6x, shrunk to 5x by the final step */
  { "5x", "5×", 5, { 3, 2 } },
  { "6x", "6×", 6, { 3, 2 } },
  { "8x", "8×", 8, { 4, 2 } },
};

const NfcUpscalePreset *
nfc_upscale_presets (guint *n)
{
  if (n != NULL)
    *n = G_N_ELEMENTS (upscale_presets);
  return upscale_presets;
}

const NfcUpscalePreset *
nfc_upscale_preset_from_key (const char *key)
{
  if (key == NULL)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (upscale_presets); i++)
    if (g_str_equal (upscale_presets[i].key, key))
      return &upscale_presets[i];
  return NULL;
}

guint
nfc_upscale_chain_factor (const NfcUpscalePreset *p)
{
  if (p == NULL)
    return 0;
  return p->pass[0] * (p->pass[1] != 0 ? p->pass[1] : 1);
}

gboolean
nfc_upscale_output_size (const NfcUpscalePreset *p, guint64 w, guint64 h, guint64 *ow, guint64 *oh)
{
  guint64 nw, nh;

  if (p == NULL || w == 0 || h == 0)
    return FALSE;
  /* bounded, so that the products below cannot overflow */
  nw = MIN (w, (guint64) 1 << 31) * p->factor;
  nh = MIN (h, (guint64) 1 << 31) * p->factor;
  if (ow != NULL)
    *ow = nw;
  if (oh != NULL)
    *oh = nh;
  if (MAX (nw, nh) > NFC_UPSCALE_MAX_SIDE)
    return FALSE;
  return nw * nh <= NFC_UPSCALE_MAX_PIXELS;
}

char *
nfc_upscale_tooltip (const NfcUpscalePreset *p, guint64 w, guint64 h)
{
  guint64 ow, oh;

  if (p == NULL)
    return g_strdup ("");
  if (nfc_upscale_output_size (p, w, h, &ow, &oh))
    return g_strdup_printf ("Make an AI-upscaled copy at %" G_GUINT64_FORMAT " × %" G_GUINT64_FORMAT
                            " px (Upscayl, runs on the GPU)", ow, oh);
  return g_strdup_printf ("Make an AI-upscaled copy %u times as wide and tall "
                          "(Upscayl, runs on the GPU)", p->factor);
}

/* WebP stores at most 16383 pixels per side */
#define WEBP_MAX_SIDE 16383

gboolean
nfc_upscale_target (NfcCategory cat, const char *fmt, guint64 ow, guint64 oh, NfcUpscaleTarget *out)
{
  static const struct {
    const char *fmt, *key;
  } map[] = {
    { "png", "png" }, { "jpg", "jpg" }, { "webp", "webp" },
    /* no encoder of their own here (or not worth one): PNG */
    { "avif", "png" }, { "heic", "png" }, { "bmp", "png" }, { "tiff", "png" }, { "gif", "png" },
  };

  if (cat != NFC_CAT_IMAGE || fmt == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i].fmt, fmt))
      {
        if (out != NULL)
          {
            out->key = map[i].key;
            out->capped = g_str_equal (map[i].key, "webp") &&
                          (ow > WEBP_MAX_SIDE || oh > WEBP_MAX_SIDE);
            if (out->capped)
              out->key = "png";
            out->ext = out->key;
          }
        return TRUE;
      }
  return FALSE;
}

char *
nfc_upscale_config_model (const char *text)
{
  char **lines;
  char *model = NULL;

  if (text == NULL)
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l != NULL; l++)
    {
      char *s = g_strstrip (*l);
      char *eq;

      if (*s == '\0' || *s == '#' || (eq = strchr (s, '=')) == NULL)
        continue;
      *eq = '\0';
      if (g_str_equal (g_strstrip (s), "upscale-model"))
        {
          char *v = g_strstrip (eq + 1);

          g_free (model);
          model = *v != '\0' ? g_strdup (v) : NULL;
        }
    }
  g_strfreev (lines);
  return model;
}

/* A model name is a plain file name stem: never a path */
static gboolean
upscale_model_present (const char *dir, const char *name)
{
  gboolean ok = *name != '\0' && *name != '.' && strlen (name) <= 100;

  for (const char *c = name; ok && *c != '\0'; c++)
    ok = g_ascii_isalnum (*c) || *c == '-' || *c == '_' || *c == '.';
  if (ok)
    {
      char *param = g_strconcat (name, ".param", NULL);
      char *bin = g_strconcat (name, ".bin", NULL);
      char *pp = g_build_filename (dir, param, NULL);
      char *bp = g_build_filename (dir, bin, NULL);

      ok = g_file_test (pp, G_FILE_TEST_IS_REGULAR) && g_file_test (bp, G_FILE_TEST_IS_REGULAR);
      g_free (pp);
      g_free (bp);
      g_free (param);
      g_free (bin);
    }
  return ok;
}

gboolean
nfc_upscale_resolve (const char *bin, const char *config_path, char **models, char **model)
{
  char *real, *bindir, *dir, *text = NULL, *want = NULL, *chosen = NULL;

  *models = NULL;
  *model = NULL;
  if (bin == NULL)
    return FALSE;
  real = realpath (bin, NULL);
  bindir = g_path_get_dirname (real != NULL ? real : bin);
  free (real);
  dir = g_build_filename (bindir, "..", "models", NULL);
  g_free (bindir);
  *models = g_canonicalize_filename (dir, NULL);
  g_free (dir);
  if (!g_file_test (*models, G_FILE_TEST_IS_DIR))
    {
      nfc_log ("Upscale: no models directory at %s: Upscale is not offered", *models);
      g_clear_pointer (models, g_free);
      return FALSE;
    }
  if (config_path != NULL && g_file_get_contents (config_path, &text, NULL, NULL))
    want = nfc_upscale_config_model (text);
  g_free (text);
  if (want != NULL)
    {
      if (upscale_model_present (*models, want))
        chosen = g_strdup (want);
      else
        nfc_log ("Upscale: upscale-model=%s in %s is not an installed model (%s/%s.param and "
                 ".bin): using %s", want, config_path, *models, want, NFC_UPSCALE_DEFAULT_MODEL);
    }
  g_free (want);
  if (chosen == NULL && upscale_model_present (*models, NFC_UPSCALE_DEFAULT_MODEL))
    chosen = g_strdup (NFC_UPSCALE_DEFAULT_MODEL);
  if (chosen == NULL)
    {
      nfc_log ("Upscale: model %s is not in %s: Upscale is not offered",
               NFC_UPSCALE_DEFAULT_MODEL, *models);
      g_clear_pointer (models, g_free);
      return FALSE;
    }
  *model = chosen;
  return TRUE;
}

#define UPSCALE_BASE_BYTES ((guint64) 1024 * 1024 * 1024) /* upscayl-bin alone: ~1 GiB RSS */

guint64
nfc_upscale_peak_bytes (const NfcUpscalePreset *p, guint64 w, guint64 h, const char *key)
{
  NfcImageInfo info = { 0 };
  guint f = nfc_upscale_chain_factor (p);

  info.width = MIN (w, (guint64) 1 << 20) * f;
  info.height = MIN (h, (guint64) 1 << 20) * f;
  return nfc_estimate_peak_bytes ("png", key, &info) + UPSCALE_BASE_BYTES;
}

guint
nfc_upscale_timeout_secs (guint64 ow, guint64 oh)
{
  guint64 mp = (MIN (ow, (guint64) 1 << 20) * MIN (oh, (guint64) 1 << 20) + 999999) / 1000000;

  return (guint) MIN (120 + 4 * mp, (guint64) 2 * 3600);
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
avail_or (Avail a, Avail b)
{
  if (a == AV_YES || b == AV_YES)
    return AV_YES;
  if (a == AV_PENDING || b == AV_PENDING)
    return AV_PENDING;
  return AV_NO;
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
        { "webm", "libvpx-vp9" }, { "gif", "gif" },
      };
      const char *enc = NULL;

      /* MP4 and the MKV re-encode need an H.264 encoder: libx264, or libopenh264
       * (what stock Fedora's ffmpeg-free has) as the fallback */
      if (cat == NFC_CAT_VIDEO && (g_str_equal (key, "mp4") || g_str_equal (key, "mkv")))
        return avail_or (ff_has ("libx264"), ff_has ("libopenh264"));

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

gboolean
nfc_compress_available (NfcCategory cat, const char *fmt)
{
  NfcCompressTarget t;

  if (!nfc_compress_target (cat, fmt, &t))
    return FALSE;
  /* an AVIF source is decoded by the helper (GdkPixbuf) */
  if (cat == NFC_CAT_IMAGE && is_pixbuf_src (fmt) && pixbuf_can_read (fmt) != AV_YES)
    return FALSE;
  /* a GIF source is read by ffmpeg, the others by the tool of their category */
  if (!nfc_category_available (cat) || !nfc_category_available (t.cat))
    return FALSE;
  return target_avail (t.cat, t.key) == AV_YES;
}

gboolean
nfc_resize_available (NfcCategory cat, const char *fmt)
{
  NfcResizeTarget t;
  const char *need;

  if (!nfc_resize_target (cat, fmt, &t))
    return FALSE;
  /* an AVIF source is decoded by the helper (GdkPixbuf) */
  if (cat == NFC_CAT_IMAGE && is_pixbuf_src (fmt) && pixbuf_can_read (fmt) != AV_YES)
    return FALSE;
  /* a GIF source is read by ffmpeg, the others by the tool of their category */
  if (!nfc_category_available (cat) || !nfc_category_available (t.cat))
    return FALSE;
  /* mov and avi are written like mp4: an H.264 encoder */
  need = (g_str_equal (t.key, "mov") || g_str_equal (t.key, "avi")) ? "mp4" : t.key;
  return target_avail (t.cat, need) == AV_YES;
}

gboolean
nfc_upscale_available (NfcCategory cat, const char *fmt)
{
  NfcUpscaleTarget t;

  nfc_tools_init ();
  if (nfc_tools.upscayl == NULL || nfc_tools.upscale_models == NULL ||
      nfc_tools.upscale_model == NULL || nfc_tools.magick == NULL)
    return FALSE;
  if (!nfc_upscale_target (cat, fmt, 0, 0, &t))
    return FALSE;
  /* a HEIC/AVIF source is decoded by the helper (GdkPixbuf) */
  if (is_pixbuf_src (fmt) && pixbuf_can_read (fmt) != AV_YES)
    return FALSE;
  /* the result is written by ImageMagick: PNG always (the WEBP cap, the other
   * sources), and the source's own format for JPG and WEBP */
  if (im_can_write ("PNG") != AV_YES)
    return FALSE;
  return g_str_equal (t.key, "png") || im_can_write (im_format_name (t.key)) == AV_YES;
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
  g_clear_pointer (&sel->dims, g_array_unref);
  g_clear_pointer (&sel->resize, g_ptr_array_unref);
  g_clear_pointer (&sel->upscale, g_ptr_array_unref);
  sel->can_compress = FALSE;
}

static gboolean
target_in (GPtrArray *arr, const NfcTarget *t)
{
  for (guint i = 0; i < arr->len; i++)
    if (g_str_equal (((const NfcTarget *) arr->pdata[i])->key, t->key))
      return TRUE;
  return FALSE;
}

/* The Resize presets to offer for a selection: those that make at least one file
 * smaller. A file of unknown size counts as "smaller" for every preset (the fixed
 * list is shown, files that are not smaller are skipped when the job runs). */
static GPtrArray *
resize_presets_for (NfcCategory cat, const GArray *dims)
{
  GPtrArray *out = g_ptr_array_new ();
  guint n;
  const NfcResizePreset *all = nfc_resize_presets (cat, &n);

  for (guint k = 0; k < n; k++)
    {
      gboolean any = dims == NULL || dims->len == 0;

      for (guint i = 0; dims != NULL && i < dims->len && !any; i++)
        {
          const NfcDims *d = &g_array_index (dims, NfcDims, i);

          any = d->width == 0 || d->height == 0 ||
                nfc_resize_output_size (&all[k], d->width, d->height, NULL, NULL);
        }
      if (any)
        g_ptr_array_add (out, (gpointer) &all[k]);
    }
  return out;
}

/* The Upscale presets to offer: those whose result fits the limits for at least
 * one file; a file of unknown size counts as fitting everything (the job reads
 * the real size and skips what is too big). */
static GPtrArray *
upscale_presets_for (const GArray *dims)
{
  GPtrArray *out = g_ptr_array_new ();
  guint n;
  const NfcUpscalePreset *all = nfc_upscale_presets (&n);

  for (guint k = 0; k < n; k++)
    {
      gboolean any = dims == NULL || dims->len == 0;

      for (guint i = 0; dims != NULL && i < dims->len && !any; i++)
        {
          const NfcDims *d = &g_array_index (dims, NfcDims, i);

          any = d->width == 0 || d->height == 0 ||
                nfc_upscale_output_size (&all[k], d->width, d->height, NULL, NULL);
        }
      if (any)
        g_ptr_array_add (out, (gpointer) &all[k]);
    }
  return out;
}

/* The size of each selected image from its header; (0, 0) where it cannot be told */
static GArray *
read_selection_dims (NfcCategory cat, GPtrArray *paths, GPtrArray *fmts)
{
  GArray *dims;

  if (cat != NFC_CAT_IMAGE || paths->len > NFC_SELECTION_DIMS_MAX)
    return NULL;
  dims = g_array_sized_new (FALSE, TRUE, sizeof (NfcDims), paths->len);
  for (guint i = 0; i < paths->len; i++)
    {
      NfcDims d = { 0, 0 };
      guint32 w, h;

      if (nfc_header_read_file (paths->pdata[i], fmts->pdata[i], &w, &h))
        {
          d.width = w;
          d.height = h;
        }
      g_array_append_val (dims, d);
    }
  return dims;
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
  /* Compress: every selected file in a format that has presets (each is
   * compressed in its own format) */
  sel->can_compress = TRUE;
  for (guint i = 0; i < sel->fmts->len && sel->can_compress; i++)
    sel->can_compress = nfc_compress_available (cat, sel->fmts->pdata[i]);
  /* Resize: every selected file in a resizable format, and a preset that makes
   * one of them smaller (sizes from the headers, see nfc-header.h) */
  {
    gboolean all = TRUE;

    for (guint i = 0; i < sel->fmts->len && all; i++)
      all = nfc_resize_available (cat, sel->fmts->pdata[i]);
    sel->dims = read_selection_dims (cat, sel->paths, sel->fmts);
    if (all)
      sel->resize = resize_presets_for (cat, sel->dims);
  }
  /* Upscale: every selected file an image the backend can take, and a preset
   * whose result fits the limits for one of them */
  {
    gboolean all = TRUE;

    for (guint i = 0; i < sel->fmts->len && all; i++)
      all = nfc_upscale_available (cat, sel->fmts->pdata[i]);
    if (all)
      sel->upscale = upscale_presets_for (sel->dims);
  }
  ok = sel->targets->len > 0 || sel->can_compress ||
       (sel->resize != NULL && sel->resize->len > 0) ||
       (sel->upscale != NULL && sel->upscale->len > 0);

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

/* libopenh264 has no constant-quality mode, only a target bitrate. Pick one
 * from the picture size: 0.1 bit per pixel per frame at an assumed 30 fps
 * (3 bits per pixel per second: 6.2 Mbit/s at 1080p, 2.8 at 720p, 24.9 at 4K),
 * which measured SSIM ~0.98 on screen recordings, within 40 Mbit/s and not
 * below 0.5. The frame rate is not probed: a 60 fps source gets half the bits
 * per frame. Unknown size: 1080p. */
#define H264_BITS_PER_PIXEL_SECOND 3
#define H264_MIN_BPS               ((guint64) 500 * 1000)
#define H264_MAX_BPS               ((guint64) 40 * 1000 * 1000)

guint64
nfc_h264_bitrate (const NfcImageInfo *info)
{
  guint64 w = 1920, h = 1080, bps;

  if (info != NULL && info->width > 0 && info->height > 0)
    {
      w = MIN (info->width, (guint64) 65536);
      h = MIN (info->height, (guint64) 65536);
    }
  bps = w * h * H264_BITS_PER_PIXEL_SECOND;
  return CLAMP (bps, H264_MIN_BPS, H264_MAX_BPS);
}

/* Per preset (best, high, balanced, small): the libx264 crf, and libopenh264's
 * share (percent) of the bitrate nfc_h264_bitrate gives. Small also uses the
 * faster x264 preset and shrinks video larger than 1080p (short side) to 1080p. */
static const char *const x264_crf[NFC_PRESET_N] = { "18", "23", "28", "34" };
static const guint oh264_percent[NFC_PRESET_N] = { 100, 60, 40, 25 };

/* ffmpeg scale filter: short side above 1080 -> 1080, aspect ratio kept, even sizes */
#define SCALE_1080 "scale=w='if(gt(iw,ih),-2,min(iw,1080))':h='if(gt(iw,ih),min(ih,1080),-2)'"

/* The video filter chain of the H.264 and VP9 encoders for a preset */
static const char *
video_filter (NfcPreset preset)
{
  return preset == NFC_PRESET_SMALL ? EVEN "," SCALE_1080 : EVEN;
}

/* What differs between the H.264 re-encodes of a plain conversion and of a
 * resize (see add_h264_attempts_ex). */
typedef struct {
  const char *vf;      /* video filter chain; NULL: video_filter (preset) */
  gboolean    audio_copy; /* try "-c:a copy" first; AAC 192k is the next attempt */
  gboolean    faststart;  /* -movflags +faststart (mp4, mov) */
  gboolean    no_bframes; /* libx264 -bf 0 (AVI cannot carry B-frames reliably) */
} H264Opts;

/* The re-encode attempts for the H.264 encoders ffmpeg has: libx264 (crf by
 * preset: 18 for Best; slow) first, then libopenh264 (bitrate, see above) as the
 * next attempt. With neither known (the probe has not run) the libx264 attempt
 * alone, as before. With audio_copy each encoder runs twice: with the audio
 * stream copied (the muxer refuses a codec the container cannot hold while
 * writing the header, i.e. at once), then with AAC. */
static void
add_h264_attempts_ex (GPtrArray *attempts, const char *src, const char *out,
                      const NfcImageInfo *info, NfcPreset preset, const H264Opts *o)
{
  gboolean x264 = nfc_have_encoder ("libx264");
  gboolean oh264 = nfc_have_encoder ("libopenh264");
  const char *vf = o->vf != NULL ? o->vf : video_filter (preset);
  GPtrArray *a;

  for (int enc = 0; enc < 2; enc++)
    {
      char *bps = NULL;

      if (enc == 0 && !(x264 || !oh264))
        continue;
      if (enc == 1 && !oh264)
        continue;
      if (enc == 1)
        {
          guint64 rate = nfc_h264_bitrate (info) * oh264_percent[preset] / 100;

          bps = g_strdup_printf ("%" G_GUINT64_FORMAT, MAX (rate, (guint64) H264_MIN_BPS));
        }
      for (int copy = o->audio_copy ? 1 : 0; copy >= 0; copy--)
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn", "-vf", vf, NULL);
          if (enc == 0)
            {
              av_add (a, "-c:v", "libx264", "-crf", x264_crf[preset],
                      "-preset", preset == NFC_PRESET_SMALL ? "medium" : "slow", NULL);
              if (o->no_bframes)
                av_add (a, "-bf", "0", NULL);
            }
          else
            av_add (a, "-c:v", "libopenh264", "-rc_mode", "bitrate", "-b:v", bps, NULL);
          av_add (a, "-pix_fmt", "yuv420p", NULL);
          if (copy)
            av_add (a, "-c:a", "copy", NULL);
          else
            av_add (a, "-c:a", "aac", "-b:a", "192k", NULL);
          if (o->faststart)
            av_add (a, "-movflags", "+faststart", NULL);
          av_add (a, out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      g_free (bps);
    }
}

/* The MP4 / MKV re-encode attempts of a conversion (see add_h264_attempts_ex) */
static void
add_h264_attempts (GPtrArray *attempts, const char *src, const char *out, gboolean mp4,
                   const NfcImageInfo *info, NfcPreset preset)
{
  H264Opts o = { NULL, FALSE, mp4, FALSE };

  add_h264_attempts_ex (attempts, src, out, info, preset, &o);
}

/* "-auto-orient", then for a resize the geometry that makes the picture smaller:
 * Lanczos (the sharpest of the stock filters for shrinking), the long side
 * bounded to the preset ("1920x1920>": never enlarges, so a missing size is
 * harmless) or a percentage. Takes the picture's alpha channel along. */
static void
im_orient (GPtrArray *a, const NfcResizePreset *rs)
{
  av_add (a, "-auto-orient", NULL);
  if (rs == NULL)
    return;
  if (rs->percent > 0)
    {
      char *g = g_strdup_printf ("%u%%", rs->percent);

      av_add (a, "-filter", "Lanczos", "-resize", g, NULL);
      g_free (g);
    }
  else
    {
      char *g = g_strdup_printf ("%ux%u>", rs->side, rs->side);

      av_add (a, "-filter", "Lanczos", "-resize", g, NULL);
      g_free (g);
    }
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


/* The icon:auto-resize list for a source whose larger side is known: the
 * usual 256/48/32/16 sizes, but only those that do not enlarge the (padded
 * square) source, plus the source's own size when it is none of them (a 98 px
 * source gives 98,48,32,16); a source below 16 px gives its own single size.
 * Without a known size, or for SVG (vector: scales losslessly), the full list.
 * g_free. */
static char *
ico_sizes (const char *fmt, const NfcImageInfo *info)
{
  static const guint std[] = { 256, 48, 32, 16 };
  GString *s;
  guint side;

  if (info == NULL || info->width == 0 || info->height == 0 || g_str_equal (fmt, "svg"))
    return g_strdup ("256,48,32,16");
  /* the -resize 256x256> shrinks anything bigger */
  side = (guint) MIN (MAX (info->width, info->height), 256);
  s = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (std); i++)
    {
      if (std[i] > side)
        continue;
      if (s->len == 0 && std[i] < side)
        g_string_append_printf (s, "%u", side);
      g_string_append_printf (s, "%s%u", s->len ? "," : "", std[i]);
    }
  if (s->len == 0)
    g_string_append_printf (s, "%u", side);
  return g_string_free (s, FALSE);
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

/* Max side of an image converted with the Small preset (larger ones shrink) */
#define SMALL_MAX_SIDE "2560x2560>"

/* AVIF crf per preset (best, high, balanced, small). */
static const char *const avif_crf_graphics[NFC_PRESET_N] = { "9", "18", "26", "34" };
static const char *const avif_crf_photo[NFC_PRESET_N] = { "12", "20", "28", "36" };

/* JPG, per preset (best, high, balanced, small): quality and chroma sampling
 * for graphics (lossless sources) and photos (lossy sources). Best's values are
 * written out where they are used. */
static const char *const jpg_quality_graphics[NFC_PRESET_N] = { "99", "95", "85", "75" };
static const char *const jpg_sampling_graphics[NFC_PRESET_N] = { "4:4:4", "4:4:4", "4:4:4", "4:2:0" };
static const char *const jpg_quality_photo[NFC_PRESET_N] = { "95", "92", "80", "70" };
static const char *const jpg_sampling_photo[NFC_PRESET_N] = { "4:4:4", "4:2:0", "4:2:0", "4:2:0" };

/* WEBP lossy quality of photos, per preset */
static const char *const webp_quality_photo[NFC_PRESET_N] = { "90", "85", "78", "65" };
/* ... and of graphics (Best is lossless: the entry is not used) */
static const char *const webp_quality_graphics[NFC_PRESET_N] = { "100", "95", "88", "75" };

/* The ffmpeg step that makes the AVIF from the flattened PNG.
 *
 * Graphics: 4:4:4 (no chroma smearing of text), crf 9 (Best). Photos: 4:2:0,
 * crf 12 (Best). The other presets raise the crf (tables above).
 * 8-bit: the 10-bit variants gave no better result on screenshots once the
 * viewer converts back to 8 bits, and 8-bit 4:4:4 decodes everywhere.
 * Colour: RGB -> YCbCr with the BT.709 matrix, FULL range, and the file is
 * tagged exactly that (colr box), so viewers do not guess BT.601/limited and
 * shift the colours. No even-size scale: AVIF handles odd sizes, and the old
 * even-size scale resampled (blurred) every odd-sized image by a pixel.
 * No -tiles: AV1 allows tiles up to 4096 px wide only, libaom picks as many as
 * the width needs by itself (an explicit count made wide images fail). */
static char **
ff_avif_step (const char *flat, const char *out, gboolean photo, NfcPreset preset)
{
  GPtrArray *a = ff_base (flat);

  av_add (a, "-frames:v", "1", "-vf",
          photo ? "scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv420p"
                : "scale=out_color_matrix=bt709:out_range=pc:flags=accurate_rnd+full_chroma_int,format=yuv444p",
          "-c:v", "libaom-av1", "-crf", (photo ? avif_crf_photo : avif_crf_graphics)[preset],
          "-b:v", "0",
          "-cpu-used", "6", "-row-mt", "1", "-still-picture", "1",
          "-colorspace", "bt709", "-color_primaries", "bt709", "-color_range", "pc",
          "-f", "avif", out, NULL);
  return av_done (a);
}

/* -------------------------------------------------------------------------
 * Image size, memory estimate and admission (see nfc-core.h)
 * ---------------------------------------------------------------------- */

/* The size comes from the header only: `identify -ping` decodes no pixels. The
 * format is "<w> <h>|<x> <y> <units>" (%w %h|%x %y %U); nfc-decode --info
 * prints just "<w> <h>". */
gboolean
nfc_image_info_parse (const char *text, NfcImageInfo *out)
{
  NfcImageInfo r = { 0, 0, FALSE, 0, 0 };
  const char *p = text;
  char *end;

  if (out != NULL)
    *out = r;
  if (p == NULL)
    return FALSE;
  while (g_ascii_isspace (*p))
    p++;
  r.width = g_ascii_strtoull (p, &end, 10);
  if (end == p || r.width == 0)
    return FALSE;
  p = end;
  r.height = g_ascii_strtoull (p, &end, 10);
  if (end == p || r.height == 0)
    return FALSE;
  p = end;
  if (*p == '|')
    {
      double x, y;
      gboolean cm;

      p++;
      x = g_ascii_strtod (p, &end);
      if (end != p)
        {
          p = end;
          y = g_ascii_strtod (p, &end);
          if (end != p)
            {
              p = end;
              while (g_ascii_isspace (*p))
                p++;
              /* "Undefined" units: the numbers only give a pixel aspect ratio */
              cm = g_str_has_prefix (p, "PixelsPerCentimeter");
              if (cm || g_str_has_prefix (p, "PixelsPerInch"))
                {
                  r.xdpi = cm ? x * 2.54 : x;
                  r.ydpi = cm ? y * 2.54 : y;
                  /* 72 dpi is what everything writes when it knows nothing */
                  r.has_density = r.xdpi > 0 && r.ydpi > 0 &&
                                  !(r.xdpi > 71.5 && r.xdpi < 72.5 &&
                                    r.ydpi > 71.5 && r.ydpi < 72.5);
                }
            }
        }
    }
  if (out != NULL)
    *out = r;
  return TRUE;
}

char **
nfc_build_probe_argv (const char *fmt, const char *src_in)
{
  GPtrArray *a;
  char *src, *arg;
  const char *coder;
  char *base;

  nfc_tools_init ();
  if (fmt == NULL || src_in == NULL)
    return NULL;
  src = g_canonicalize_filename (src_in, NULL);
  a = av_new ();
  if (is_pixbuf_src (fmt))
    {
      /* no ImageMagick delegate for HEIC/AVIF here: the helper that decodes
       * them reads the header */
      if (nfc_tools.decode == NULL)
        {
          g_ptr_array_unref (a);
          g_free (src);
          return NULL;
        }
      av_add (a, nfc_tools.decode, "--info", src, NULL);
      g_free (src);
      return av_done (a);
    }
  coder = im_input_coder (fmt);
  if (coder == NULL || nfc_tools.magick == NULL)
    {
      g_ptr_array_unref (a);
      g_free (src);
      return NULL;
    }
  base = g_path_get_basename (nfc_tools.magick);
  if (g_str_equal (base, "magick"))
    av_add (a, nfc_tools.magick, "identify", NULL);
  else
    {
      /* ImageMagick 6: `convert` has no identify mode, `identify` is a tool */
      char *ident = g_find_program_in_path ("identify");

      if (ident == NULL)
        {
          g_free (base);
          g_ptr_array_unref (a);
          g_free (src);
          return NULL;
        }
      av_add (a, ident, NULL);
      g_free (ident);
    }
  g_free (base);
  arg = im_path_arg (coder, src, "[0]");
  av_add (a, "-quiet", "-ping", "-format", "%w %h|%x %y %U", arg, NULL);
  g_free (arg);
  g_free (src);
  return av_done (a);
}

guint
nfc_pdf_fit_dpi (guint64 w, guint64 h)
{
  /* A4 is 11.69 x 8.27 in; the long side goes along the long side */
  double longer = (double) MAX (w, h), shorter = (double) MIN (w, h);
  double dpi = MAX (longer / 11.69, shorter / 8.27);

  if (dpi < 96.0)
    dpi = 96.0;
  if (dpi > 100000.0)
    dpi = 100000.0;
  return (guint) dpi + ((double) (guint) dpi < dpi ? 1 : 0);
}

/* Bytes of peak memory per pixel, measured on a 19200 x 10800 RGBA PNG (207
 * megapixels, ImageMagick 7 Q16-HDRI, limits as in im_limits):
 *   jpg, ico, bmp, tiff, pdf, png  5.7 GB  = 27.5 B/px  -> 30
 *   avif (flat PNG, then libaom)   7.6 GB  = 36.7 B/px  -> 40
 *   webp (lossless or sharp YUV)   8.4 GB  = 40.5 B/px  -> 44
 * i.e. the measured value plus 8 to 10 % margin. */
guint64
nfc_estimate_peak_bytes (const char *fmt, const char *key, const NfcImageInfo *info)
{
  guint64 px, bpp;

  if (info == NULL || info->width == 0 || info->height == 0)
    return 0;
  px = MIN (info->width, (guint64) 1000000) * MIN (info->height, (guint64) 1000000);
  /* an SVG is rendered at 192 dpi instead of its nominal 72 */
  if (fmt != NULL && g_str_equal (fmt, "svg"))
    px = px * 64 / 9;
  if (key != NULL && g_str_equal (key, "webp"))
    bpp = 44;
  else if (key != NULL && g_str_equal (key, "avif"))
    bpp = 40;
  else
    bpp = 30;
  return px * bpp;
}

static guint64
meminfo_kb (const char *text, const char *field)
{
  const char *p = strstr (text, field);

  return p != NULL ? g_ascii_strtoull (p + strlen (field), NULL, 10) : 0;
}

guint64
nfc_mem_available_bytes (void)
{
  const char *e = g_getenv ("NFC_TEST_MEMAVAILABLE_KB");
  char *text = NULL;
  guint64 kb = 0;

  if (e != NULL && *e != '\0')
    return g_ascii_strtoull (e, NULL, 10) * 1024;
  if (g_file_get_contents ("/proc/meminfo", &text, NULL, NULL))
    kb = meminfo_kb (text, "MemAvailable:");
  g_free (text);
  return kb * 1024;
}

guint64
nfc_admission_budget_bytes (void)
{
  return nfc_mem_available_bytes () / 10 * 6;
}

gboolean
nfc_admission_allows (guint64 pixels, guint64 est, guint64 reserved, guint n_running,
                      guint64 budget)
{
  if (pixels < NFC_SMALL_IMAGE_PIXELS || n_running == 0)
    return TRUE;
  return est <= budget && reserved <= budget - est;
}

/* Remove what a crashed or killed session left behind: entries not modified
 * for a day. Own thread: they can be gigabytes of pixel cache. */
static gpointer
spill_cleanup_thread (gpointer data)
{
  char *dir = data;
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  if (d != NULL)
    {
      while ((name = g_dir_read_name (d)) != NULL)
        {
          char *p = g_build_filename (dir, name, NULL);
          GStatBuf st;

          if (g_lstat (p, &st) == 0 && now - (gint64) st.st_mtime > 24 * 3600)
            nfc_rm_rf (p);
          g_free (p);
        }
      g_dir_close (d);
    }
  g_free (dir);
  return NULL;
}

char *
nfc_spill_dir (void)
{
  static gsize cleaned = 0;
  char *dir = g_build_filename (g_get_user_cache_dir (), "nautilus-file-converter", "tmp", NULL);
  GStatBuf st;

  if (g_mkdir_with_parents (dir, 0700) != 0 || g_stat (dir, &st) != 0 || !S_ISDIR (st.st_mode) ||
      st.st_uid != geteuid ())
    {
      g_free (dir);
      return g_strdup (g_get_tmp_dir ());
    }
  if ((st.st_mode & 07777) != 0700)
    g_chmod (dir, 0700);
  if (g_once_init_enter (&cleaned))
    {
      char *copy = g_strdup (dir);
      GThread *t = g_thread_try_new ("nfc-spill-clean", spill_cleanup_thread, copy, NULL);

      if (t != NULL)
        g_thread_unref (t);
      else
        g_free (copy);
      g_once_init_leave (&cleaned, 1);
    }
  return dir;
}

GPtrArray *
nfc_build_attempts (NfcCategory cat, const char *fmt, const char *key,
                    const char *src, const char *out, const char *workdir,
                    gboolean *needs_decode)
{
  return nfc_build_attempts_info (cat, fmt, key, src, out, workdir, NULL, needs_decode);
}

GPtrArray *
nfc_build_attempts_info (NfcCategory cat, const char *fmt, const char *key,
                         const char *src, const char *out, const char *workdir,
                         const NfcImageInfo *info, gboolean *needs_decode)
{
  return nfc_build_attempts_preset (cat, fmt, key, NFC_PRESET_BEST, src, out, workdir,
                                    info, needs_decode);
}

/* ffmpeg scale filter of a resize preset. Video: the SHORT side becomes `side`
 * (portrait or landscape), the other follows with an even value (-2); GIF and
 * the like: the long side (or a percentage), any value (-1). Never enlarges
 * (min), so a source whose size is unknown cannot get bigger. Lanczos. */
static char *
resize_scale_filter (const NfcResizePreset *rs, gboolean video)
{
  if (rs->percent > 0)
    return g_strdup_printf ("scale=w='trunc(iw*%u/100+0.5)':h='trunc(ih*%u/100+0.5)':flags=lanczos",
                            rs->percent, rs->percent);
  if (rs->short_side)
    return g_strdup_printf ("scale=w='if(gt(iw,ih),-2,min(iw,%u))':h='if(gt(iw,ih),min(ih,%u),-2)'"
                            ":flags=lanczos", rs->side, rs->side);
  return g_strdup_printf ("scale=w='if(gt(iw,ih),min(iw,%u),%s)':h='if(gt(iw,ih),%s,min(ih,%u))'"
                          ":flags=lanczos", rs->side, video ? "-2" : "-1", video ? "-2" : "-1",
                          rs->side);
}

/* The attempts that resize a video (or animated GIF) to the preset. MP4, MOV, MKV
 * and AVI: libx264 crf 18 slow (the Best preset), libopenh264 when that is all
 * ffmpeg has; the audio stream is copied when the container takes it, else AAC
 * 192k. AVI gets no B-frames (it cannot carry them reliably). WebM: VP9 crf 28
 * (Best), audio copied, else Opus 128k. GIF: the palette chain of the GIF presets
 * at the new size, frame rate untouched. FALSE for any other key. */
static gboolean
add_resize_video_attempts (GPtrArray *attempts, const char *key, const char *src,
                           const char *out, const NfcResizePreset *rs, const NfcImageInfo *info)
{
  GPtrArray *a;
  guint64 ow, oh;
  NfcImageInfo oinfo = { 0, 0, FALSE, 0, 0 };

  if (g_str_equal (key, "gif"))
    {
      char *scale = resize_scale_filter (rs, FALSE);
      char *vf = g_strdup_printf ("%s,split[s0][s1];[s0]palettegen=stats_mode=diff[p];"
                                  "[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
                                  scale);

      a = ff_base (src);
      av_add (a, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf", vf, "-loop", "0", out, NULL);
      add_attempt (attempts, av_done (a), NULL);
      g_free (vf);
      g_free (scale);
      return TRUE;
    }
  if (g_str_equal (key, "webm"))
    {
      char *scale = resize_scale_filter (rs, TRUE);
      char *vf = g_strconcat (scale, "," EVEN, NULL);

      for (int copy = 1; copy >= 0; copy--)
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c:v", "libvpx-vp9", "-crf", "28", "-b:v", "0", "-row-mt", "1",
                  "-deadline", "good", "-cpu-used", "2", "-pix_fmt", "yuv420p", "-vf", vf, NULL);
          if (copy)
            av_add (a, "-c:a", "copy", NULL);
          else
            av_add (a, "-c:a", "libopus", "-b:a", "128k", NULL);
          av_add (a, out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      g_free (vf);
      g_free (scale);
      return TRUE;
    }
  if (g_str_equal (key, "mp4") || g_str_equal (key, "mov") || g_str_equal (key, "mkv") ||
      g_str_equal (key, "avi"))
    {
      char *scale = resize_scale_filter (rs, TRUE);
      char *vf = g_strconcat (scale, "," EVEN, NULL);
      H264Opts o = { vf, TRUE, g_str_equal (key, "mp4") || g_str_equal (key, "mov"),
                     g_str_equal (key, "avi") };

      /* the libopenh264 bitrate follows the size of the OUTPUT */
      if (info != NULL && nfc_resize_output_size (rs, info->width, info->height, &ow, &oh))
        {
          oinfo.width = ow;
          oinfo.height = oh;
        }
      add_h264_attempts_ex (attempts, src, out, oinfo.width > 0 ? &oinfo : NULL,
                            NFC_PRESET_BEST, &o);
      g_free (vf);
      g_free (scale);
      return TRUE;
    }
  return FALSE;
}

/* One `upscayl-bin` pass: in -> out (PNG), the model's output scaled to `scale`
 * times the input (the backend resizes the 4x result itself). */
static char **
upscayl_step (const char *in, const char *out, guint scale)
{
  GPtrArray *a = av_new ();
  char *s = g_strdup_printf ("%u", scale);

  av_add (a, nfc_tools.upscayl, "-i", in, "-o", out, "-m", nfc_tools.upscale_models,
          "-n", nfc_tools.upscale_model, "-s", s, "-f", "png", NULL);
  g_free (s);
  return av_done (a);
}

GPtrArray *
nfc_build_attempts_upscale (const char *fmt, const char *key, const NfcUpscalePreset *p,
                            const char *src_in, const char *out, const char *workdir,
                            gboolean *needs_decode)
{
  GPtrArray *attempts, *a;
  GPtrArray *at;
  char *src, *in_png, *last, *arg, *o;
  const char *imname;
  guint chain;

  if (needs_decode != NULL)
    *needs_decode = FALSE;
  if (fmt == NULL || key == NULL || p == NULL || src_in == NULL || out == NULL || workdir == NULL)
    return NULL;
  if (nfc_tools.upscayl == NULL || nfc_tools.upscale_models == NULL ||
      nfc_tools.upscale_model == NULL || nfc_tools.magick == NULL)
    return NULL;
  imname = im_format_name (key);
  if (imname == NULL || !(g_str_equal (key, "png") || g_str_equal (key, "jpg") ||
                          g_str_equal (key, "webp")))
    return NULL;
  if (!nfc_upscale_target (NFC_CAT_IMAGE, fmt, 0, 0, NULL))
    return NULL;
  chain = nfc_upscale_chain_factor (p);
  src = g_canonicalize_filename (src_in, NULL);
  attempts = attempts_new ();
  at = attempt_new ();

  /* 1. a PNG the backend reads: what it gets is always 8-bit, upright, with a
   * colour type it knows */
  if (is_pixbuf_src (fmt))
    {
      /* nfc-decode <src> <workdir>/decoded.png: orientation applied, 8-bit RGB(A) */
      if (nfc_tools.decode == NULL)
        goto fail;
      in_png = g_build_filename (workdir, "decoded.png", NULL);
      a = av_new ();
      av_add (a, nfc_tools.decode, src, in_png, NULL);
      g_ptr_array_add (at, av_done (a));
      if (needs_decode != NULL)
        *needs_decode = TRUE;
    }
  else
    {
      const char *coder = im_input_coder (fmt);

      if (coder == NULL)
        goto fail;
      in_png = g_build_filename (workdir, "in.png", NULL);
      a = av_new ();
      av_add (a, nfc_tools.magick, "-quiet", NULL);
      im_limits (a);
      arg = im_path_arg (coder, src, "[0]");
      o = im_path_arg ("PNG", in_png, NULL);
      av_add (a, arg, "-auto-orient", "-depth", "8", o, NULL);
      g_free (arg);
      g_free (o);
      g_ptr_array_add (at, av_done (a));
    }

  /* 2. the passes */
  last = g_build_filename (workdir, "up1.png", NULL);
  g_ptr_array_add (at, upscayl_step (in_png, last, p->pass[0]));
  if (p->pass[1] != 0)
    {
      char *next = g_build_filename (workdir, "up2.png", NULL);

      g_ptr_array_add (at, upscayl_step (last, next, p->pass[1]));
      g_free (last);
      last = next;
    }

  /* 3. the result: exact size, the Best settings of the output format */
  a = av_new ();
  av_add (a, nfc_tools.magick, "-quiet", NULL);
  im_limits (a);
  arg = im_path_arg ("PNG", last, "[0]"); /* the [0] also makes ImageMagick read "%%" as "%" */
  av_add (a, arg, NULL);
  g_free (arg);
  if (chain != p->factor)
    {
      /* 6x -> 5x: Lanczos, by an exact ratio (the size rounds to the integer) */
      char *pct = g_strdup_printf ("%.10f%%", 100.0 * p->factor / chain);

      av_add (a, "-filter", "Lanczos", "-resize", pct, NULL);
      g_free (pct);
    }
  if (g_str_equal (key, "jpg"))
    {
      im_flatten (a);
      av_add (a, "-quality", photo_source (fmt) ? "95" : "99", "-sampling-factor", "4:4:4", NULL);
    }
  else if (g_str_equal (key, "webp"))
    {
      av_add (a, "-resize", "16383x16383>", NULL);
      if (photo_source (fmt))
        av_add (a, "-quality", "90", "-define", "webp:method=6",
                "-define", "webp:use-sharp-yuv=true", NULL);
      else
        av_add (a, "-define", "webp:lossless=true", "-define", "webp:method=4", NULL);
    }
  o = im_path_arg (imname, out, NULL);
  av_add (a, o, NULL);
  g_free (o);
  g_ptr_array_add (at, av_done (a));
  g_ptr_array_add (attempts, at);
  g_free (in_png);
  g_free (last);
  g_free (src);
  return attempts;

fail:
  g_ptr_array_unref (at);
  g_ptr_array_unref (attempts);
  g_free (src);
  return NULL;
}

static GPtrArray *build_attempts (NfcCategory cat, const char *fmt, const char *key,
                                  NfcPreset preset, const NfcResizePreset *rs,
                                  const char *src_in, const char *out, const char *workdir,
                                  const NfcImageInfo *info, gboolean *needs_decode);

GPtrArray *
nfc_build_attempts_preset (NfcCategory cat, const char *fmt, const char *key,
                           NfcPreset preset, const char *src_in, const char *out,
                           const char *workdir, const NfcImageInfo *info,
                           gboolean *needs_decode)
{
  return build_attempts (cat, fmt, key, preset, NULL, src_in, out, workdir, info, needs_decode);
}

GPtrArray *
nfc_build_attempts_resize (NfcCategory cat, const char *fmt, const char *key,
                           const NfcResizePreset *rs, const char *src_in, const char *out,
                           const char *workdir, const NfcImageInfo *info,
                           gboolean *needs_decode)
{
  if (rs == NULL)
    return NULL;
  return build_attempts (cat, fmt, key, NFC_PRESET_BEST, rs, src_in, out, workdir, info,
                         needs_decode);
}

static GPtrArray *
build_attempts (NfcCategory cat, const char *fmt, const char *key, NfcPreset preset,
                const NfcResizePreset *rs, const char *src_in, const char *out,
                const char *workdir, const NfcImageInfo *info, gboolean *needs_decode)
{
  GPtrArray *attempts, *a;
  char *src;

  if (needs_decode != NULL)
    *needs_decode = FALSE;
  if (src_in == NULL || fmt == NULL || key == NULL || out == NULL || workdir == NULL)
    return NULL;
  /* targets without presets always convert as Best; a resize is always Best */
  if (rs != NULL || (guint) preset >= NFC_PRESET_N || !nfc_target_has_presets (cat, key))
    preset = NFC_PRESET_BEST;
  src = g_canonicalize_filename (src_in, NULL);
  attempts = attempts_new ();

  if (cat == NFC_CAT_IMAGE)
    {
      char *im_in, *decoded = NULL;
      char **dec_step = NULL;
      const char *imname, *coder;
      char **lossless_step = NULL;
      char *alt_webp = NULL;

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
          av_add (a, im_in, NULL);
          im_orient (a, rs);
          if (preset == NFC_PRESET_SMALL)
            av_add (a, "-resize", SMALL_MAX_SIDE, NULL);
          im_flatten (a);
          av_add (a, flatp, NULL);
          s1 = av_done (a);

          /* attempt 1: the quality settings of the source class. Attempt 2
           * (8-bit 4:2:0) only runs when libaom refuses attempt 1, which it
           * does for some huge images (4:4:4 at ~200 megapixels). Each attempt
           * runs the whole chain: the steps are owned by the attempt. */
          s2 = ff_avif_step (flat, out, photo_source (fmt), preset);
          add_attempt (attempts, dec_step, s1, s2, NULL);
          if (!photo_source (fmt))
            add_attempt (attempts, g_strdupv (dec_step), g_strdupv (s1),
                         ff_avif_step (flat, out, TRUE, preset), NULL);
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
           * photos are fine at q95. The other presets lower the quality (tables
           * above), write progressive files and subsample the chroma where that
           * costs little; Small also shrinks images beyond 2560 px. */
          im_orient (a, rs);
          if (preset == NFC_PRESET_SMALL)
            av_add (a, "-resize", SMALL_MAX_SIDE, NULL);
          im_flatten (a);
          if (preset == NFC_PRESET_BEST)
            av_add (a, "-quality", photo_source (fmt) ? "95" : "99",
                    "-sampling-factor", "4:4:4", NULL);
          else if (photo_source (fmt))
            av_add (a, "-quality", jpg_quality_photo[preset], "-sampling-factor",
                    jpg_sampling_photo[preset], "-interlace", "JPEG", NULL);
          else
            av_add (a, "-quality", jpg_quality_graphics[preset], "-sampling-factor",
                    jpg_sampling_graphics[preset], "-interlace", "JPEG", NULL);
        }
      else if (g_str_equal (key, "webp"))
        {
          /* WEBP stores at most 16383 pixels per side: shrink only beyond that.
           * Best, graphics (lossless sources): lossless WEBP, which was smaller
           * than the source PNG for every screenshot tried and has no chroma
           * subsampling or ringing. Photos: lossy q90, method 6 (sharp YUV). 94 made a phone JPEG
           * larger than its source (a lossy source re-encoded at a higher quality
           * keeps its artefacts at a higher bitrate): at q90 the output is not
           * larger, SSIM >= 0.98 against the source on most photos.
           *
           * Other presets, photos: a lower lossy quality. Graphics: a lossy
           * encode (q95/88/75, method 6, sharp YUV, alpha at q100) AND, for
           * images below NFC_SMALL_IMAGE_PIXELS, the lossless encode of Best,
           * and the smaller file wins (NFC_STEP_KEEP_SMALLEST): lossy WEBP is
           * larger than lossless for flat text/UI screenshots (a terminal: 32 KB
           * lossless, 59 KB lossy q60), smaller for screenshots with gradients
           * and photos. The lossless encode comes first and is not essential:
           * if it fails (memory), the second attempt is the lossy encode alone.
           * Small also shrinks images beyond 2560 px (both candidates). */
          im_orient (a, rs);
          av_add (a, "-resize", preset == NFC_PRESET_SMALL ? SMALL_MAX_SIDE : "16383x16383>", NULL);
          if (photo_source (fmt))
            av_add (a, "-quality", webp_quality_photo[preset], "-define", "webp:method=6",
                    "-define", "webp:use-sharp-yuv=true", NULL);
          else if (preset == NFC_PRESET_BEST)
            av_add (a, "-define", "webp:lossless=true", "-define", "webp:method=4", NULL);
          else
            {
              guint64 px = info != NULL ? info->width * info->height : G_MAXUINT64;

              if (g_str_equal (fmt, "svg") && px != G_MAXUINT64)
                px = px * 64 / 9;
              if (px < NFC_SMALL_IMAGE_PIXELS)
                {
                  /* the lossless candidate: same input and resize, own output */
                  GPtrArray *l = av_new ();
                  char *o;

                  alt_webp = g_strconcat (out, ".alt", NULL);
                  o = im_path_arg ("WEBP", alt_webp, NULL);

                  for (char **p = (char **) a->pdata; *p != NULL; p++)
                    g_ptr_array_add (l, g_strdup (*p));
                  av_add (l, "-define", "webp:lossless=true", "-define", "webp:method=4", o, NULL);
                  lossless_step = av_done (l);
                  g_free (o);
                }
              av_add (a, "-quality", webp_quality_graphics[preset], "-define", "webp:method=6",
                      "-define", "webp:use-sharp-yuv=true", "-define", "webp:alpha-quality=100",
                      NULL);
            }
        }
      else if (g_str_equal (key, "ico"))
        {
          /* up to 256 px, never enlarged, aspect ratio kept (padded
           * transparently to a square: icon:auto-resize would stretch a
           * non-square image), then the usual 256/48/32/16 sizes that do not
           * enlarge it (see ico_sizes) */
          char *sizes = ico_sizes (fmt, info);
          char *def = g_strconcat ("icon:auto-resize=", sizes, NULL);

          av_add (a, "-auto-orient", "-resize", "256x256>", "-background", "none",
                  "-gravity", "center", "-set", "option:nfc:side", "%[fx:max(w,h)]",
                  "-extent", "%[option:nfc:side]x%[option:nfc:side]",
                  "-define", def, NULL);
          g_free (def);
          g_free (sizes);
        }
      else if (g_str_equal (key, "pdf"))
        {
          av_add (a, "-auto-orient", NULL);
          im_flatten (a);
          /* Screenshots and renders carry no resolution, so ImageMagick writes
           * them at 72 dpi: a 1920 px wide one becomes a 26 inch page. Give
           * those (only those: a photo or scan with a real dpi keeps it) a
           * density that fits an A4 sheet. Only the density is set, the pixels
           * are embedded untouched. SVG is rendered at 192 dpi above. */
          if (info != NULL && !info->has_density && !g_str_equal (fmt, "svg"))
            {
              char *dpi = g_strdup_printf ("%u", nfc_pdf_fit_dpi (info->width, info->height));

              av_add (a, "-units", "PixelsPerInch", "-density", dpi, NULL);
              g_free (dpi);
            }
        }
      else /* png, bmp, tiff */
        im_orient (a, rs);
      {
        /* explicit format prefix: ImageMagick silently writes PNG data under
         * an .avif/.heic name, and never trusts the extension we want */
        char *o = im_path_arg (imname, out, NULL);

        av_add (a, o, NULL);
        g_free (o);
      }
      if (lossless_step != NULL)
        {
          char **lossy = av_done (a);
          char **keep = g_new0 (char *, 4);

          /* attempt 1: both encodes, the smaller one stays at `out`; attempt 2
           * (only if attempt 1 failed, e.g. the lossless one ran out of memory):
           * the lossy encode alone */
          keep[0] = g_strdup (NFC_STEP_KEEP_SMALLEST);
          keep[1] = g_strdup (out);
          keep[2] = g_strdup (alt_webp);
          add_attempt (attempts, dec_step, lossless_step, lossy, keep, NULL);
          add_attempt (attempts, g_strdupv (dec_step), g_strdupv (lossy), NULL);
        }
      else
        add_attempt (attempts, dec_step, av_done (a), NULL);
      g_free (alt_webp);
      g_free (im_in);
      g_free (decoded);
      g_free (src);
      return attempts;

    }

  if (cat == NFC_CAT_AUDIO || cat == NFC_CAT_VIDEO)
    {
      if (nfc_tools.ffmpeg == NULL)
        goto fail;

      if (rs != NULL)
        {
          if (cat != NFC_CAT_VIDEO || !add_resize_video_attempts (attempts, key, src, out, rs, info))
            goto fail;
          g_free (src);
          return attempts;
        }

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
        add_h264_attempts (attempts, src, out, TRUE, info, preset);
      else if (g_str_equal (key, "mkv"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c", "copy", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
          add_h264_attempts (attempts, src, out, FALSE, info, NFC_PRESET_BEST);
        }
      else if (g_str_equal (key, "webm"))
        {
          static const char *const vp9_crf[NFC_PRESET_N] = { "28", "36", "44", "52" };

          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c:v", "libvpx-vp9", "-crf", vp9_crf[preset], "-b:v", "0", "-row-mt", "1",
                  "-deadline", "good", "-cpu-used", "2", "-pix_fmt", "yuv420p",
                  "-vf", video_filter (preset),
                  "-c:a", "libopus", "-b:a", "128k", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "gif"))
        {
          /* width cap, frame rate and palette size per preset */
          static const struct { guint width, fps; const char *colors; } gif_p[NFC_PRESET_N] = {
            { 960, 15, "" }, { 720, 15, "" }, { 640, 12, "" }, { 480, 10, ":max_colors=128" },
          };
          /* min(source_fps, preset fps): a 10 fps source keeps its 10 fps instead
           * of getting every frame doubled (a bigger file with nothing gained) */
          char *vf = g_strdup_printf ("fps=fps='min(source_fps,%u)',scale='min(%u,iw)':-1:flags=lanczos,"
                                      "split[s0][s1];[s0]palettegen=stats_mode=diff%s[p];"
                                      "[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
                                      gif_p[preset].fps, gif_p[preset].width, gif_p[preset].colors);

          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf", vf,
                  "-loop", "0", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
          g_free (vf);
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

/* -------------------------------------------------------------------------
 * Failure messages
 * ---------------------------------------------------------------------- */

#define MSG_MAX_BYTES 300 /* longest failure text shown in the toast */

static gboolean
line_has_any (const char *lower, const char *const *needles)
{
  for (; *needles != NULL; needles++)
    if (strstr (lower, *needles) != NULL)
      return TRUE;
  return FALSE;
}

/* Lines that name the real cause, whatever comes after them on stderr. */
static const char *const known_cause[] = {
  "error while opening decoder", "decoderpluginerror", "loadingerror", "unknown decoder",
  "no such file", "permission denied", "invalid data found", "not a jpeg",
  "improper image header", "cache resources exhausted", "no space left", "memory allocation",
  "cannot allocate memory", "out of memory", "unable to open", "corrupt", "truncated",
  /* upscayl-bin (Vulkan / ncnn) */
  "vkcreateinstance failed", "invalid gpu device", "vkallocatememory failed",
  "vkcreatebuffer failed", "vkqueuesubmit failed", "vkwaitforfences failed",
  "device_lost", "couldn't read the image", "unknown model dir", ".param failed", ".bin failed", NULL
};
static const char *const generic_cause[] = { "error", "failed", "unable", NULL };

/* Remove what only clutters a toast: leading "[dec:x @ 0x..] " groups, a
 * "program: " prefix (magick: , /usr/bin/convert: ) and ImageMagick's
 * " @ error/file.c/Function/123." tail. In place. */
static void
clean_line (char *s)
{
  char *p, *at;

  for (;;)
    {
      p = s;
      if (*p != '[' || (at = strchr (p, ']')) == NULL)
        break;
      p = at + 1;
      while (*p == ' ')
        p++;
      if (*p == '\0')
        break;
      memmove (s, p, strlen (p) + 1);
    }
  /* "magick: ", "/usr/bin/convert-im6.q16: ", "ffmpeg: ": our own tools' names */
  p = strstr (s, ": ");
  if (p != NULL && p > s && p[2] != '\0')
    {
      char *prog = g_strndup (s, p - s);
      char *base = g_path_get_basename (prog);

      if (strchr (prog, ' ') == NULL &&
          (g_str_has_prefix (base, "magick") || g_str_has_prefix (base, "convert") ||
           g_str_has_prefix (base, "identify") || g_str_has_prefix (base, "ffmpeg") ||
           g_str_has_prefix (base, "soffice") || g_str_has_prefix (base, "nfc-decode")))
        memmove (s, p + 2, strlen (p + 2) + 1);
      g_free (base);
      g_free (prog);
    }
  if ((at = strstr (s, " @ error/")) != NULL)
    {
      *at = '\0';
      g_strchomp (s);
      if (*s != '\0' && s[strlen (s) - 1] == '.')
        s[strlen (s) - 1] = '\0';
    }
}

/* The codec named in "[dec:libopenh264 @ 0x55d3] Error while opening decoder",
 * or NULL. g_free. */
static char *
decoder_name (const char *text)
{
  const char *p = strstr (text, "[dec:");
  const char *e;

  if (p == NULL)
    return NULL;
  p += 5;
  e = p;
  while (*e != '\0' && *e != ' ' && *e != ']' && *e != '\n')
    e++;
  return e > p ? g_strndup (p, e - p) : NULL;
}

static char *
truncated_copy (const char *s)
{
  if (strlen (s) > MSG_MAX_BYTES)
    {
      const char *cut = s + MSG_MAX_BYTES;
      char *head;
      char *res;

      /* keep the start, on a UTF-8 character boundary */
      while (cut > s && (*cut & 0xC0) == 0x80)
        cut--;
      head = g_strndup (s, cut - s);
      res = g_strconcat (head, "…", NULL);
      g_free (head);
      return res;
    }
  return g_strdup (s);
}

/* Lines that are not a message about what happened: upscayl-bin lists its Vulkan
 * devices ("[0 AMD Radeon ...]  queueC=1[4] ...") and chats about its progress. */
static gboolean
ignorable_line (const char *s)
{
  static const char *const prefixes[] = {
    "WARNING: radv", "🚀", "✨", "🏞", "🙌", NULL
  };

  if (s[0] == '[' && g_ascii_isdigit (s[1]))
    return TRUE;
  for (const char *const *p = prefixes; *p != NULL; p++)
    if (g_str_has_prefix (s, *p))
      return TRUE;
  return FALSE;
}

char *
nfc_failure_message (const char *text, const char *src_fmt)
{
  char **lines;
  char *best = NULL, *lb, *res = NULL;
  int best_rank = 0, n;
  char *lower_best = NULL;

  if (text == NULL)
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  n = (int) g_strv_length (lines);
  /* rank 3: a known cause, 2: error/failed/unable, 1: any non-empty line; the
   * last line of the best rank wins */
  for (int i = n - 1; i >= 0; i--)
    {
      char *s = g_strstrip (lines[i]);
      char *lower;
      int rank;

      if (*s == '\0' || ignorable_line (s))
        continue;
      lower = g_ascii_strdown (s, -1);
      rank = line_has_any (lower, known_cause) ? 3 : line_has_any (lower, generic_cause) ? 2 : 1;
      if (rank > best_rank)
        {
          best_rank = rank;
          best = s;
          g_free (lower_best);
          lower_best = lower;
        }
      else
        g_free (lower);
      if (best_rank == 3)
        break;
    }
  if (best == NULL)
    {
      g_strfreev (lines);
      return NULL;
    }
  lb = g_ascii_strdown (best, -1);

  /* causes we can say in words */
  if ((strstr (lb, "decoderpluginerror") != NULL ||
       strstr (lb, "loadingerror") != NULL) &&
      src_fmt != NULL && (g_str_equal (src_fmt, "heic") || g_str_equal (src_fmt, "heif")))
    res = g_strdup ("can't decode HEIC: the HEVC decoder is missing "
                    "(install libheif-freeworld from RPM Fusion)");
  else if (strstr (lb, "error while opening decoder") != NULL)
    {
      char *codec = decoder_name (best);

      res = codec != NULL ? g_strdup_printf ("the video decoder failed (%s decoder unavailable)", codec)
                          : g_strdup ("the video decoder failed (decoder unavailable)");
      g_free (codec);
    }
  else if (strstr (lb, "cache resources exhausted") != NULL)
    res = g_strdup ("image too large for available memory");
  /* the Upscale backend (upscayl-bin: ncnn on Vulkan) */
  else if (strstr (lb, "vkcreateinstance failed") != NULL || strstr (lb, "invalid gpu device") != NULL)
    res = g_strdup ("no usable GPU: Upscale needs a Vulkan-capable graphics card and driver");
  else if (strstr (lb, "vkallocatememory failed") != NULL || strstr (lb, "vkcreatebuffer failed") != NULL)
    res = g_strdup ("the GPU ran out of memory (try a smaller image or a lower scale)");
  else if (strstr (lb, "vkqueuesubmit failed") != NULL || strstr (lb, "vkwaitforfences failed") != NULL ||
           strstr (lb, "device_lost") != NULL)
    res = g_strdup ("the GPU stopped responding (try again, or a lower scale)");
  else if (strstr (lb, "unknown model dir") != NULL || strstr (lb, ".param failed") != NULL ||
           strstr (lb, ".bin failed") != NULL)
    res = g_strdup ("the Upscale model files are missing (check upscale-model in the config)");
  else if (strstr (lb, "couldn't read the image") != NULL)
    res = g_strdup ("the Upscale backend could not read the image");
  else if (strstr (lb, "no space left on device") != NULL)
    res = g_strdup ("disk full");
  else
    {
      clean_line (best);
      res = truncated_copy (*best != '\0' ? best : lines[0]);
    }
  g_free (lb);
  g_free (lower_best);
  g_strfreev (lines);
  return res;
}

char **
nfc_build_video_probe_argv (const char *src_in)
{
  GPtrArray *a;
  char *src;

  nfc_tools_init ();
  if (src_in == NULL || nfc_tools.ffprobe == NULL)
    return NULL;
  src = g_canonicalize_filename (src_in, NULL); /* absolute: never an option */
  a = av_new ();
  av_add (a, nfc_tools.ffprobe, "-v", "error", "-select_streams", "v:0",
          "-show_entries", "stream=width,height", "-of", "default=noprint_wrappers=1:nokey=1",
          "-i", src, NULL);
  g_free (src);
  return av_done (a);
}
