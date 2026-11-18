/* nautilus-file-converter: pure conversion logic. See nfc-core.h. */
#include "nfc-core.h"

#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

NfcTools nfc_tools = { NULL, NULL, NULL, NULL };

/* -------------------------------------------------------------------------
 * Tool discovery and capability probes
 * ---------------------------------------------------------------------- */

static GMutex   probe_lock;
static gboolean probed = FALSE;
static GHashTable *ff_enc = NULL;    /* ffmpeg encoder names */
static GHashTable *im_write = NULL;  /* ImageMagick writable format names */
static GHashTable *pb_read = NULL;   /* GdkPixbuf format names */

static char *
find_first (const char *a, const char *b)
{
  char *p = g_find_program_in_path (a);
  if (p == NULL && b != NULL)
    p = g_find_program_in_path (b);
  return p;
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
      nfc_tools.magick = find_first ("magick", "convert");
      nfc_tools.ffmpeg = find_first ("ffmpeg", NULL);
      nfc_tools.soffice = find_first ("soffice", "libreoffice");
      nfc_tools.notify = find_first ("notify-send", NULL);
      g_once_init_leave (&once, 1);
    }
}

/* Run argv quietly and return stdout (NULL on failure). */
static char *
run_quiet (const char *const *argv)
{
  GSubprocessLauncher *l;
  GSubprocess *p;
  char *out = NULL;

  l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_stdin_file_path (l, "/dev/null");
  p = g_subprocess_launcher_spawnv (l, argv, NULL);
  g_object_unref (l);
  if (p == NULL)
    return NULL;
  if (!g_subprocess_communicate_utf8 (p, NULL, NULL, &out, NULL, NULL))
    out = NULL;
  g_object_unref (p);
  return out;
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
    out = run_quiet (argv);
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
  if (nfc_tools.magick == NULL)
    return;
  {
    const char *argv[] = { nfc_tools.magick, "-list", "format", NULL };
    out = run_quiet (argv);
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

static void
probe_pixbuf (void)
{
  GSList *formats = gdk_pixbuf_get_formats ();

  pb_read = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (GSList *l = formats; l; l = l->next)
    g_hash_table_add (pb_read, gdk_pixbuf_format_get_name (l->data));
  g_slist_free (formats);
}

void
nfc_probe_ensure (void)
{
  nfc_tools_init ();
  g_mutex_lock (&probe_lock);
  if (!probed)
    {
      probe_ffmpeg ();
      probe_magick ();
      probe_pixbuf ();
      probed = TRUE;
    }
  g_mutex_unlock (&probe_lock);
}

gboolean
nfc_have_encoder (const char *name)
{
  nfc_probe_ensure ();
  return name != NULL && g_hash_table_contains (ff_enc, name);
}

gboolean
nfc_magick_can_write (const char *im_format)
{
  nfc_probe_ensure ();
  return im_format != NULL && g_hash_table_contains (im_write, im_format);
}

gboolean
nfc_pixbuf_can_read (const char *name)
{
  nfc_probe_ensure ();
  return name != NULL && g_hash_table_contains (pb_read, name);
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

/* Is the tool/codec needed for this target available? */
static gboolean
target_ok (NfcCategory cat, const char *key)
{
  nfc_probe_ensure ();
  if (cat == NFC_CAT_IMAGE)
    {
      if (g_str_equal (key, "avif"))
        return nfc_tools.ffmpeg != NULL && nfc_have_encoder ("libaom-av1") &&
               g_hash_table_size (im_write) > 0;
      return nfc_magick_can_write (im_format_name (key));
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

      if (nfc_tools.ffmpeg == NULL)
        return FALSE;
      for (guint i = 0; i < G_N_ELEMENTS (need); i++)
        if (g_str_equal (need[i][0], key))
          enc = need[i][1];
      if (enc == NULL)
        return FALSE;
      if (cat == NFC_CAT_VIDEO && g_str_equal (key, "webm"))
        return nfc_have_encoder (enc) && nfc_have_encoder ("libopus");
      return nfc_have_encoder (enc);
    }
  return nfc_tools.soffice != NULL;
}

GPtrArray *
nfc_targets_for (NfcCategory cat, const char *fmt)
{
  GPtrArray *out = g_ptr_array_new ();
  const NfcTarget *table = target_table (cat);

  if (table == NULL || fmt == NULL)
    return out;
  /* ImageMagick here cannot read HEIC/AVIF; without the GdkPixbuf loader
   * there is no way to decode them */
  if (cat == NFC_CAT_IMAGE && is_pixbuf_src (fmt) && !nfc_pixbuf_can_read (fmt))
    return out;
  for (const NfcTarget *t = table; t->key; t++)
    {
      if (g_str_equal (t->key, fmt))
        continue;
      if (!target_ok (cat, t->key))
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

/* one attempt consisting of the given steps (takes ownership of the argv) */
static void
add_attempt (GPtrArray *attempts, char **step1, char **step2)
{
  GPtrArray *at = attempt_new ();

  g_ptr_array_add (at, step1);
  if (step2 != NULL)
    g_ptr_array_add (at, step2);
  g_ptr_array_add (attempts, at);
}

#define EVEN "scale=trunc(iw/2)*2:trunc(ih/2)*2"

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
      char *inp, *im_in;
      const char *imname;

      if (nfc_tools.magick == NULL ||
          (im_format_name (key) == NULL && !g_str_equal (key, "avif")))
        goto fail;
      if (is_pixbuf_src (fmt))
        {
          inp = g_build_filename (workdir, "decoded.png", NULL);
          if (needs_decode != NULL)
            *needs_decode = TRUE;
        }
      else
        inp = g_strdup (src);
      im_in = g_strconcat (inp, "[0]", NULL);
      g_free (inp);

      if (g_str_equal (key, "avif"))
        {
          char *flat, *flatp;
          char **s1, **s2;

          if (nfc_tools.ffmpeg == NULL)
            {
              g_free (im_in);
              goto fail;
            }
          flat = g_build_filename (workdir, "flat.png", NULL);
          flatp = g_strconcat ("PNG:", flat, NULL);
          a = av_new ();
          av_add (a, nfc_tools.magick, "-quiet", NULL);
          if (g_str_equal (fmt, "svg"))
            av_add (a, "-background", "none", "-density", "192", NULL);
          av_add (a, im_in, "-auto-orient", NULL);
          im_flatten (a);
          av_add (a, flatp, NULL);
          s1 = av_done (a);

          a = ff_base (flat);
          av_add (a, "-frames:v", "1", "-vf", EVEN ",format=yuv420p",
                  "-c:v", "libaom-av1", "-crf", "28", "-b:v", "0",
                  "-cpu-used", "6", "-row-mt", "1", "-still-picture", "1",
                  "-f", "avif", out, NULL);
          s2 = av_done (a);
          add_attempt (attempts, s1, s2);
          g_free (flat);
          g_free (flatp);
          g_free (im_in);
          g_free (src);
          return attempts;
        }

      imname = im_format_name (key);
      a = av_new ();
      av_add (a, nfc_tools.magick, "-quiet", NULL);
      if (g_str_equal (fmt, "svg"))
        av_add (a, "-background", "none", "-density", "192", NULL);
      av_add (a, im_in, NULL);
      if (g_str_equal (key, "jpg"))
        {
          av_add (a, "-auto-orient", NULL);
          im_flatten (a);
          av_add (a, "-quality", "92", NULL);
        }
      else if (g_str_equal (key, "webp"))
        av_add (a, "-auto-orient", "-quality", "90", NULL);
      else if (g_str_equal (key, "ico"))
        av_add (a, "-auto-orient", "-resize", "256x256>", NULL);
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
        char *o = g_strconcat (imname, ":", out, NULL);

        av_add (a, o, NULL);
        g_free (o);
      }
      add_attempt (attempts, av_done (a), NULL);
      g_free (im_in);
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
                  "-vf", EVEN, "-c:v", "libx264", "-crf", "23", "-preset", "medium",
                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "160k",
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
                  "-vf", EVEN, "-c:v", "libx264", "-crf", "23", "-preset", "medium",
                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "160k", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "webm"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn",
                  "-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-row-mt", "1",
                  "-cpu-used", "2", "-pix_fmt", "yuv420p", "-vf", EVEN,
                  "-c:a", "libopus", "-b:a", "128k", out, NULL);
          add_attempt (attempts, av_done (a), NULL);
        }
      else if (g_str_equal (key, "gif"))
        {
          a = ff_base (src);
          av_add (a, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
                  "fps=12,scale='min(480,iw)':-1:flags=lanczos,"
                  "split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse",
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
    char *outdir, *profile, *envarg;

    if (flt == NULL || nfc_tools.soffice == NULL)
      goto fail;
    outdir = g_path_get_dirname (out);
    profile = g_build_filename (workdir, "lo_profile", NULL);
    envarg = g_strconcat ("-env:UserInstallation=file://", profile, NULL);
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

gboolean
nfc_pixbuf_decode (const char *src, const char *dest, GError **error)
{
  GdkPixbuf *p, *o;
  gboolean ok;

  p = gdk_pixbuf_new_from_file (src, error);
  if (p == NULL)
    return FALSE;
  o = gdk_pixbuf_apply_embedded_orientation (p);
  if (o == NULL)
    o = g_object_ref (p);
  ok = gdk_pixbuf_save (o, dest, "png", error, NULL);
  g_object_unref (o);
  g_object_unref (p);
  return ok;
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

char *
nfc_publish (const char *tmp_path, const char *directory, const char *stem,
             const char *ext, GError **error)
{
  char *result = NULL;

  g_mutex_lock (&name_lock);
  for (guint n = 0;; n++)
    {
      char *name = nfc_candidate_name (stem, ext, n);
      char *cand = g_build_filename (directory, name, NULL);

      g_free (name);
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
      {
        /* filesystem without hard links (FAT, ...): rename, but never over
         * an existing entry */
        int saved = errno;
        struct stat st;

        if (lstat (cand, &st) == 0)
          {
            g_free (cand);
            continue;
          }
        if (rename (tmp_path, cand) == 0)
          {
            result = cand;
            break;
          }
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "cannot create %s: %s (link: %s)", cand, g_strerror (errno),
                     g_strerror (saved));
        g_free (cand);
        break;
      }
    }
  g_mutex_unlock (&name_lock);
  return result;
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
