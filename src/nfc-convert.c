/* nautilus-file-converter: asynchronous conversion runner. See nfc-convert.h. */
#include "nfc-convert.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#include "nfc-header.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* ffmpeg's own words for "this file has no audio stream" are no help in a toast
 * ("Stream map '' matches no streams."): recognise that failure (the audio
 * targets map a required audio stream, the video targets map it optionally, so
 * it can only come from there) and say it plainly. NULL if it is something else. */
static char *
no_audio_message (char **argv, const char *stderr_text)
{
  gboolean maps_audio = FALSE;

  if (stderr_text == NULL || strstr (stderr_text, "matches no streams") == NULL)
    return NULL;
  for (char **a = argv; *a != NULL; a++)
    if (g_str_equal (*a, "0:a:0"))
      maps_audio = TRUE;
  return maps_audio ? g_strdup ("it has no audio track") : NULL;
}

static char *
join_argv (char **argv)
{
  GString *s = g_string_new (NULL);

  for (char **a = argv; *a; a++)
    {
      char *q = g_shell_quote (*a);

      if (s->len)
        g_string_append_c (s, ' ');
      g_string_append (s, q);
      g_free (q);
    }
  return g_string_free (s, FALSE);
}

/* -------------------------------------------------------------------------
 * Timeouts
 *
 * Per step (one tool invocation): images 600 s, audio and documents 1800 s,
 * video 1800 s plus 60 s per 100 MiB of input, capped at 4 h (a long film
 * re-encode legitimately takes hours; a hung tool is killed eventually); an
 * Upscale step 120 s plus 4 s per output megapixel, capped at 2 h.
 * NFC_TEST_TIMEOUT_SECS overrides all of them (tests only, honoured only when
 * set to a positive number).
 * ---------------------------------------------------------------------- */

#define TIMEOUT_IMAGE_SECS     600
#define TIMEOUT_DEFAULT_SECS   1800
#define TIMEOUT_VIDEO_CAP_SECS (4 * 3600)

static guint
timeout_secs (NfcCategory cat, gint64 input_bytes, guint upscale_secs)
{
  const char *e = g_getenv ("NFC_TEST_TIMEOUT_SECS");
  guint64 v = (e != NULL && *e != '\0') ? g_ascii_strtoull (e, NULL, 10) : 0;
  guint64 t;

  if (v > 0 && v < G_MAXUINT)
    return (guint) v;
  if (upscale_secs > 0) /* Upscale: by output size, see nfc_upscale_timeout_secs */
    return upscale_secs;
  if (cat == NFC_CAT_IMAGE)
    return TIMEOUT_IMAGE_SECS;
  if (cat == NFC_CAT_VIDEO)
    {
      t = TIMEOUT_DEFAULT_SECS + 60 * (guint64) (MAX (input_bytes, 0) / (100 * 1024 * 1024));
      return (guint) MIN (t, (guint64) TIMEOUT_VIDEO_CAP_SECS);
    }
  return TIMEOUT_DEFAULT_SECS;
}

/* -------------------------------------------------------------------------
 * Single conversion job
 * ---------------------------------------------------------------------- */

typedef struct {
  NfcCategory cat;
  char *fmt, *key, *ext, *src, *dir, *stem;
  char *pub_stem;         /* stem of the published name when it is not the source's */
  gint64 ref_size;        /* keep the output only if smaller than this (-1: always keep) */
  NfcPreset preset;
  const NfcResizePreset *resize; /* Resize job: static preset, NULL otherwise */
  const NfcUpscalePreset *upscale; /* Upscale job: static preset, NULL otherwise */
  guint64 out_w, out_h;   /* Upscale: the size of the result, once probed */
  char *workdir, *produced, *hidden;
  gint64 src_size;
  GPtrArray *attempts;
  guint ai, si;
  char *err;
  GSubprocess *proc;
  GCancellable *cancel;   /* cancelled by the step timeout and by shutdown */
  pid_t pgid;             /* process group of the running step, 0 if none */
  guint timeout_id;
  gboolean timed_out;
  gboolean aborted;       /* module shutdown: no further steps, no fallbacks */
  gboolean finalizing;    /* output being moved into place by a worker thread */
  NfcImageInfo info;     /* size from the probe, valid when have_info */
  gboolean have_info;
  guint64 pixels, est;    /* pixels (SVG: as rendered) and estimated peak bytes */
  gboolean queued;        /* waiting for memory in the admission queue */
  gboolean reserved;      /* its estimate is counted in reserved_bytes */
  char *spill;            /* disk-backed temp dir for ImageMagick (images only) */
  GTask *task;
} Job;

/* Jobs in flight; touched from the main thread only. */
static GHashTable *active_jobs = NULL;
static gboolean shutting_down = FALSE;

static void run_step (Job *j);
static void job_start (Job *j);
static void admission_release (Job *j);
static void admission_forget (Job *j);

static gboolean
is_office (NfcCategory c)
{
  return c == NFC_CAT_DOC || c == NFC_CAT_SHEET || c == NFC_CAT_SLIDE;
}

/* The files the internal NFC_STEP_KEEP_SMALLEST steps compare besides the
 * output: whatever is left of them when a job ends early. */
static void
unlink_candidates (Job *j)
{
  if (j->attempts == NULL)
    return;
  for (guint a = 0; a < j->attempts->len; a++)
    {
      GPtrArray *at = j->attempts->pdata[a];

      for (guint s = 0; s < at->len; s++)
        {
          char **step = at->pdata[s];

          if (g_str_equal (step[0], NFC_STEP_KEEP_SMALLEST) && step[1] != NULL)
            for (guint i = 2; step[i] != NULL; i++)
              g_unlink (step[i]);
        }
    }
}

static void
job_cleanup_files (Job *j)
{
  unlink_candidates (j);
  if (j->hidden != NULL && g_file_test (j->hidden, G_FILE_TEST_EXISTS))
    g_unlink (j->hidden);
  if (j->workdir != NULL)
    nfc_rm_rf (j->workdir);
}

static void
job_free (Job *j)
{
  admission_forget (j);
  admission_release (j);
  if (active_jobs != NULL)
    g_hash_table_remove (active_jobs, j);
  if (j->timeout_id)
    g_source_remove (j->timeout_id);
  g_clear_object (&j->proc);
  g_clear_object (&j->cancel);
  g_clear_pointer (&j->attempts, g_ptr_array_unref);
  g_free (j->fmt);
  g_free (j->key);
  g_free (j->ext);
  g_free (j->src);
  g_free (j->dir);
  g_free (j->stem);
  g_free (j->pub_stem);
  g_free (j->workdir);
  g_free (j->produced);
  g_free (j->spill);
  g_free (j->hidden);
  g_free (j->err);
  g_free (j);
}

/* Whatever the outcome, the callback can ask for the source's size
 * (nfc_convert_result_dims): kept on the task before it returns */
static void
task_keep_dims (Job *j)
{
  NfcDims *d;

  if (j->task == NULL || !j->have_info)
    return;
  d = g_new (NfcDims, 1);
  d->width = j->info.width;
  d->height = j->info.height;
  g_object_set_data_full (G_OBJECT (j->task), "nfc-src-dims", d, g_free);
}

gboolean
nfc_convert_result_dims (GAsyncResult *result, guint64 *width, guint64 *height)
{
  const NfcDims *d = result != NULL ? g_object_get_data (G_OBJECT (result), "nfc-src-dims") : NULL;

  if (width != NULL)
    *width = d != NULL ? d->width : 0;
  if (height != NULL)
    *height = d != NULL ? d->height : 0;
  return d != NULL;
}

static void
job_fail (Job *j, const char *msg)
{
  GTask *task = j->task;

  job_cleanup_files (j);
  task_keep_dims (j);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                           msg != NULL && *msg ? msg : "conversion failed");
  j->task = NULL;
  job_free (j);
  g_object_unref (task);
}

GQuark
nfc_convert_error_quark (void)
{
  return g_quark_from_static_string ("nfc-convert-error");
}

/* The output is not smaller than the reference: nothing is left behind and the
 * task ends with NFC_CONVERT_ERROR_NOT_SMALLER (not a failure, see NfcResult). */
static void
job_skip (Job *j, gint64 out_size)
{
  GTask *task = j->task;

  nfc_log ("SKIP %s -> %s (%s): output %" G_GINT64_FORMAT " bytes, reference %" G_GINT64_FORMAT
           " bytes: not smaller, discarded", j->src, j->key, nfc_preset_label (j->preset),
           out_size, j->ref_size);
  job_cleanup_files (j);
  task_keep_dims (j);
  g_task_return_new_error (task, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_NOT_SMALLER,
                           "already as small as it gets");
  j->task = NULL;
  job_free (j);
  g_object_unref (task);
}

/* A Resize job whose source the preset would not make smaller: ends before any
 * converter runs, with the same error (and no output, no temporary file) */
static void
job_skip_resize (Job *j)
{
  GTask *task = j->task;
  char *msg;

  nfc_log ("SKIP %s -> %s (%s): source %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT
           " is already smaller than the preset", j->src, j->key, j->resize->name,
           j->info.width, j->info.height);
  job_cleanup_files (j);
  task_keep_dims (j);
  msg = j->resize->percent > 0 ? g_strdup_printf ("too small to resize to %s", j->resize->name)
                               : g_strdup_printf ("already smaller than %s", j->resize->name);
  g_task_return_new_error (task, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_NOT_SMALLER, "%s", msg);
  g_free (msg);
  j->task = NULL;
  job_free (j);
  g_object_unref (task);
}

/* An Upscale job whose result (ow x oh) would exceed the size limits: ends before
 * any tool runs, with NFC_CONVERT_ERROR_TOO_BIG, no output, no temporary file */
static void
job_skip_upscale (Job *j, guint64 ow, guint64 oh)
{
  GTask *task = j->task;

  nfc_log ("SKIP %s -> %s (%s): the result, %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT
           ", would exceed %d px per side or %" G_GUINT64_FORMAT " pixels",
           j->src, j->key, j->upscale->key, ow, oh, NFC_UPSCALE_MAX_SIDE,
           (guint64) NFC_UPSCALE_MAX_PIXELS);
  job_cleanup_files (j);
  task_keep_dims (j);
  g_task_return_new_error (task, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_TOO_BIG,
                           "too big to upscale %s (%" G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT
                           " px; the limit is %d px per side or %d megapixels)",
                           j->upscale->label, ow, oh, NFC_UPSCALE_MAX_SIDE,
                           (int) (NFC_UPSCALE_MAX_PIXELS / 1000000));
  j->task = NULL;
  job_free (j);
  g_object_unref (task);
}

/* The internal NFC_STEP_KEEP_SMALLEST step: the smallest of the files (argv[1],
 * the output, and argv[2...]) ends up at argv[1], the others are deleted. NULL
 * on success, else the error message. A file that does not exist or is empty
 * does not compete. */
static char *
keep_smallest_step (char **argv)
{
  const char *best = NULL;
  gint64 best_size = G_MAXINT64;
  char *err = NULL;

  for (guint i = 1; argv[i] != NULL; i++)
    {
      GStatBuf st;

      if (g_stat (argv[i], &st) != 0 || !S_ISREG (st.st_mode) || st.st_size <= 0)
        continue;
      /* strictly smaller wins: a tie keeps the earlier one (the output) */
      if (best == NULL || nfc_is_smaller (st.st_size, best_size))
        {
          best = argv[i];
          best_size = st.st_size;
        }
    }
  if (best == NULL)
    err = g_strdup ("no output file was produced");
  else if (!g_str_equal (best, argv[1]) && g_rename (best, argv[1]) != 0)
    err = g_strdup_printf ("cannot keep the smaller file: %s", g_strerror (errno));
  for (guint i = 2; argv[i] != NULL; i++)
    g_unlink (argv[i]); /* the winner (if not argv[1]) was renamed away already */
  return err;
}

/* current attempt failed: drop partial output and move to the next one */
static void
attempt_failed (Job *j, const char *msg)
{
  g_free (j->err);
  j->err = g_strdup (msg);
  if (j->produced != NULL && g_file_test (j->produced, G_FILE_TEST_EXISTS))
    g_unlink (j->produced);
  unlink_candidates (j);
  j->ai++;
  j->si = 0;
  run_step (j);
}

static void
finalize_thread (GTask *task, gpointer source G_GNUC_UNUSED, gpointer data, GCancellable *c G_GNUC_UNUSED)
{
  Job *j = data;
  GError *error = NULL;
  char *final;

  if (is_office (j->cat))
    {
      /* copy from the private workdir next to the source, then publish */
      GFile *from = g_file_new_for_path (j->produced);
      GFile *to;

      j->hidden = nfc_hidden_temp_name (j->dir, j->stem, j->ext);
      to = g_file_new_for_path (j->hidden);
      if (!g_file_copy (from, to, G_FILE_COPY_NONE, NULL, NULL, NULL, &error))
        {
          g_object_unref (from);
          g_object_unref (to);
          g_task_return_error (task, error);
          return;
        }
      g_object_unref (from);
      g_object_unref (to);
    }
  final = nfc_publish (j->hidden, j->dir, j->pub_stem != NULL ? j->pub_stem : j->stem,
                       j->ext, &error);
  if (final == NULL)
    {
      g_task_return_error (task, error);
      return;
    }
  g_free (j->hidden);
  j->hidden = NULL;
  g_task_return_pointer (task, final, g_free);
}

static void
finalize_done (GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  GError *error = NULL;
  char *final = g_task_propagate_pointer (G_TASK (res), &error);

  if (final == NULL)
    {
      nfc_log ("ERROR %s -> %s: %s", j->src, j->key, error ? error->message : "?");
      job_fail (j, error ? error->message : "could not write the output file");
      g_clear_error (&error);
      return;
    }
  {
    GTask *task = j->task;

    nfc_rm_rf (j->workdir);
    task_keep_dims (j);
    g_task_return_pointer (task, final, g_free);
    j->task = NULL;
    job_free (j);
    g_object_unref (task);
  }
}

/* -------------------------------------------------------------------------
 * Running one step: bounded output capture
 *
 * A tool may print without limit (a runaway ffmpeg log, a hostile file making a
 * decoder spew). stdout and stderr are therefore NOT collected with
 * g_subprocess_communicate (which keeps everything in memory) but read
 * incrementally into tail buffers that keep only the last STEP_KEEP_BYTES of
 * each stream: memory stays bounded while the tool runs, and so do the log
 * entry and the toast. What is logged is capped further (LOG_*_BYTES), so one
 * failure never writes more than about 64 KiB to the log.
 * ---------------------------------------------------------------------- */

#define STEP_KEEP_BYTES (64 * 1024) /* retained per stream */
#define LOG_ERR_BYTES   (48 * 1024) /* logged from stderr */
#define LOG_OUT_BYTES   (8 * 1024)  /* logged from stdout */
#define READ_CHUNK      (16 * 1024)

typedef struct {
  GByteArray *data; /* the last bytes seen: at most 2 * KEEP while reading */
  guint64 total;    /* all bytes seen */
} Tail;

typedef struct {
  Job *j;
  Tail tail[2]; /* [0] stdout, [1] stderr */
  int pending;  /* reads + the wait still running */
  gboolean cancelled;
  char *io_error;
} StepIO;

typedef struct {
  StepIO *io;
  guint which;
} StepRead;

static void
tail_append (Tail *t, const guint8 *p, gsize n)
{
  t->total += n;
  if (n >= STEP_KEEP_BYTES)
    {
      g_byte_array_set_size (t->data, 0);
      p += n - STEP_KEEP_BYTES;
      n = STEP_KEEP_BYTES;
    }
  g_byte_array_append (t->data, p, n);
  if (t->data->len > 2 * STEP_KEEP_BYTES)
    g_byte_array_remove_range (t->data, 0, t->data->len - STEP_KEEP_BYTES);
}

/* the last `max` bytes (at most) as valid UTF-8, with a note when output was
 * dropped before them */
static char *
tail_text (const Tail *t, gsize max, gboolean with_note)
{
  gsize n = MIN ((gsize) t->data->len, max);
  char *text = n == 0 ? g_strdup ("")
                      : g_utf8_make_valid ((const char *) t->data->data + (t->data->len - n), n);

  if (with_note && t->total > n)
    {
      char *r = g_strdup_printf ("[... %" G_GUINT64_FORMAT " earlier bytes omitted]\n%s",
                                 t->total - n, text);

      g_free (text);
      return r;
    }
  return text;
}

static void step_finished (StepIO *io);

static void
step_io_done_one (StepIO *io)
{
  if (--io->pending == 0)
    step_finished (io);
}

static void
step_read_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  StepRead *r = data;
  StepIO *io = r->io;
  GError *error = NULL;
  GBytes *b = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), res, &error);

  if (b != NULL && g_bytes_get_size (b) > 0)
    {
      gsize n;
      const guint8 *p = g_bytes_get_data (b, &n);

      tail_append (&io->tail[r->which], p, n);
      g_bytes_unref (b);
      g_input_stream_read_bytes_async (G_INPUT_STREAM (source), READ_CHUNK, G_PRIORITY_DEFAULT,
                                       io->j->cancel, step_read_cb, r);
      return;
    }
  if (b != NULL)
    g_bytes_unref (b); /* EOF */
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    io->cancelled = TRUE;
  else if (io->io_error == NULL)
    io->io_error = g_strdup (error->message);
  g_clear_error (&error);
  g_free (r);
  step_io_done_one (io);
}

static void
step_wait_cb (GObject *source, GAsyncResult *res, gpointer data)
{
  StepIO *io = data;
  GError *error = NULL;

  if (!g_subprocess_wait_finish (G_SUBPROCESS (source), res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        io->cancelled = TRUE;
      else if (io->io_error == NULL)
        io->io_error = g_strdup (error->message);
      g_clear_error (&error);
    }
  step_io_done_one (io);
}

static void
step_start_io (Job *j)
{
  StepIO *io = g_new0 (StepIO, 1);
  GInputStream *in[2];

  io->j = j;
  io->tail[0].data = g_byte_array_new ();
  io->tail[1].data = g_byte_array_new ();
  io->pending = 3;
  in[0] = g_subprocess_get_stdout_pipe (j->proc);
  in[1] = g_subprocess_get_stderr_pipe (j->proc);
  for (guint i = 0; i < 2; i++)
    {
      StepRead *r = g_new0 (StepRead, 1);

      r->io = io;
      r->which = i;
      g_input_stream_read_bytes_async (in[i], READ_CHUNK, G_PRIORITY_DEFAULT, j->cancel,
                                       step_read_cb, r);
    }
  g_subprocess_wait_async (j->proc, j->cancel, step_wait_cb, io);
}

/* Is the step's leader still unreaped? g_subprocess_get_identifier() is NULL
 * once GLib has reaped it. Then j->pgid is stale unless a group member survives
 * (a living member pins the number, see step_timeout), so the kills at the end
 * of a step and at shutdown only happen while the leader is alive, and none is
 * repeated after the timeout kill. step_timeout itself does not use this guard:
 * it must also reach grandchildren of a leader that is already gone. */
static gboolean
step_leader_alive (Job *j)
{
  return j->proc != NULL && j->pgid > 1 && g_subprocess_get_identifier (j->proc) != NULL;
}

/* what a killed step is called in the failure message */
static const char *
timed_out_text (const Job *j)
{
  return j->upscale != NULL ? "timed out (the GPU may be busy or too slow)" : "timed out";
}

/* The value after "-o" of an upscayl-bin step (its output file), NULL for any
 * other step. upscayl-bin exits 0 after most of its failures (unreadable input,
 * missing model): the only sign is that it wrote nothing. */
static const char *
upscayl_output (const Job *j, char **argv)
{
  if (j->upscale == NULL || nfc_tools.upscayl == NULL || !g_str_equal (argv[0], nfc_tools.upscayl))
    return NULL;
  for (guint i = 1; argv[i] != NULL && argv[i + 1] != NULL; i++)
    if (g_str_equal (argv[i], "-o"))
      return argv[i + 1];
  return NULL;
}

static void
step_finished (StepIO *io)
{
  Job *j = io->j;
  GPtrArray *attempt = j->attempts->pdata[j->ai];
  char **argv = attempt->pdata[j->si];
  char *msg = NULL;

  if (j->timeout_id)
    {
      g_source_remove (j->timeout_id);
      j->timeout_id = 0;
    }
  if (io->cancelled || io->io_error != NULL)
    {
      /* the step was cancelled (timeout, shutdown) or the pipes broke; a
       * timeout or shutdown is final, no fallback attempt is tried */
      char *cmd = join_argv (argv);
      gboolean final = j->timed_out || j->aborted;
      const char *why = io->io_error != NULL ? io->io_error : "Operation was cancelled";

      msg = g_strdup (j->timed_out ? timed_out_text (j) : j->aborted ? "cancelled" : why);
      nfc_log ("FAIL %s -> %s (%s)\n  cmd: %s\n  %s", j->src, j->key,
               j->timed_out ? "timeout" : j->aborted ? "cancelled" : "I/O error", cmd, why);
      /* a timeout or shutdown already killed the group while the leader was
       * alive; only a broken pipe leaves it to us */
      if (!final && step_leader_alive (j))
        nfc_kill_group (j->pgid);
      j->pgid = 0;
      g_free (cmd);
      g_clear_object (&j->proc);
      g_free (io->io_error);
      g_byte_array_unref (io->tail[0].data);
      g_byte_array_unref (io->tail[1].data);
      g_free (io);
      if (final)
        job_fail (j, msg);
      else
        attempt_failed (j, msg);
      g_free (msg);
      return;
    }
  j->pgid = 0;
  {
    gboolean exited = g_subprocess_get_if_exited (j->proc);
    int status = exited ? g_subprocess_get_exit_status (j->proc) : -1;

    g_clear_object (&j->proc);
    if (status != 0)
      {
        char *cmd = join_argv (argv);
        char *se = tail_text (&io->tail[1], STEP_KEEP_BYTES, FALSE);
        char *so = tail_text (&io->tail[0], STEP_KEEP_BYTES, FALSE);
        char *log_se = tail_text (&io->tail[1], LOG_ERR_BYTES, TRUE);
        char *log_so = tail_text (&io->tail[0], LOG_OUT_BYTES, TRUE);
        gboolean final = j->timed_out; /* killed by the timeout: final */

        msg = j->timed_out ? NULL : no_audio_message (argv, se);
        if (msg != NULL)
          final = TRUE; /* no other attempt can find audio that is not there */
        else if (j->timed_out)
          msg = g_strdup (timed_out_text (j));
        else
          msg = nfc_failure_message (se, j->fmt);
        if (msg == NULL)
          msg = nfc_failure_message (so, j->fmt);
        if (msg == NULL)
          msg = g_strdup_printf ("exit code %d", status);
        nfc_log ("FAIL %s -> %s (%s)\n  cmd: %s\n  stderr:\n%s\n  stdout:\n%s",
                 j->src, j->key, j->timed_out ? "timeout" : "exit status", cmd, log_se, log_so);
        g_free (cmd);
        g_free (so);
        g_free (se);
        g_free (log_so);
        g_free (log_se);
        g_byte_array_unref (io->tail[0].data);
        g_byte_array_unref (io->tail[1].data);
        g_free (io);
        if (final)
          job_fail (j, msg);
        else
          attempt_failed (j, msg);
        g_free (msg);
        return;
      }
    {
      /* upscayl-bin said 0 but wrote nothing: say why, from what it printed */
      const char *up_out = upscayl_output (j, argv);
      GStatBuf st;

      if (up_out != NULL && !(g_stat (up_out, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0))
        {
          char *cmd = join_argv (argv);
          char *se = tail_text (&io->tail[1], STEP_KEEP_BYTES, FALSE);
          char *log_se = tail_text (&io->tail[1], LOG_ERR_BYTES, TRUE);

          msg = nfc_failure_message (se, j->fmt);
          if (msg == NULL)
            msg = g_strdup ("the Upscale backend produced no image");
          nfc_log ("FAIL %s -> %s (no output)\n  cmd: %s\n  stderr:\n%s", j->src, j->key, cmd,
                   log_se);
          g_free (cmd);
          g_free (se);
          g_free (log_se);
          g_byte_array_unref (io->tail[0].data);
          g_byte_array_unref (io->tail[1].data);
          g_free (io);
          attempt_failed (j, msg);
          g_free (msg);
          return;
        }
    }
  }
  g_byte_array_unref (io->tail[0].data);
  g_byte_array_unref (io->tail[1].data);
  g_free (io);
  j->si++;
  run_step (j);
}

/* The step ran too long. /usr/bin/soffice is a shell script that starts
 * soffice.bin, which keeps our stdout/stderr pipes open even when the script
 * is killed, so killing the direct child is not enough: kill the whole process
 * group (the launcher put the child in its own) AND cancel the communicate
 * operation so it finishes now instead of waiting for the pipes. */
static gboolean
step_timeout (gpointer data)
{
  Job *j = data;

  j->timeout_id = 0;
  j->timed_out = TRUE;
  /* Unconditional, unlike the other kills (see step_leader_alive): the leader may
   * already be reaped while a grandchild still holds the pipes (a wrapper script
   * that exited), which is exactly what must die here. That is safe: Linux does
   * not reuse a pid number while it is still the PGID or SID of any living
   * process, so as long as a member of the group survives, j->pgid is pinned and
   * can only mean our group. If nothing survives, the kill is a no-op (the only
   * leftover risk is a pid wrap-around where the new process also makes itself a
   * group leader with that very id, which is negligible next to leaving an orphan
   * running). */
  nfc_kill_group (j->pgid);
  if (j->proc != NULL)
    g_subprocess_force_exit (j->proc);
  if (j->cancel != NULL)
    g_cancellable_cancel (j->cancel);
  return G_SOURCE_REMOVE;
}

static void
run_step (Job *j)
{
  GPtrArray *attempt;
  GSubprocessLauncher *l;
  GError *error = NULL;
  char **argv;

  if (shutting_down || j->aborted)
    {
      j->aborted = TRUE;
      job_fail (j, "cancelled");
      return;
    }
  if (j->ai >= j->attempts->len)
    {
      job_fail (j, j->err);
      return;
    }
  attempt = j->attempts->pdata[j->ai];
  if (j->si >= attempt->len)
    {
      /* all steps of this attempt ran: verify the output */
      GStatBuf st;

      if (g_stat (j->produced, &st) == 0 && S_ISREG (st.st_mode) && st.st_size > 0)
        {
          GTask *ft;

          if (j->ref_size >= 0 && !nfc_is_smaller (st.st_size, j->ref_size))
            {
              job_skip (j, st.st_size);
              return;
            }
          ft = g_task_new (NULL, NULL, finalize_done, j);

          j->finalizing = TRUE;

          g_task_set_task_data (ft, j, NULL);
          g_task_run_in_thread (ft, finalize_thread);
          g_object_unref (ft);
          return;
        }
      nfc_log ("FAIL %s -> %s: no output file was produced", j->src, j->key);
      attempt_failed (j, "no output file was produced");
      return;
    }
  argv = attempt->pdata[j->si];
  if (g_str_equal (argv[0], NFC_STEP_KEEP_SMALLEST))
    {
      char *msg = keep_smallest_step (argv);

      if (msg != NULL)
        {
          nfc_log ("FAIL %s -> %s: %s", j->src, j->key, msg);
          attempt_failed (j, msg);
          g_free (msg);
          return;
        }
      j->si++;
      run_step (j);
      return;
    }
  l = nfc_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  if (j->spill != NULL)
    {
      /* ImageMagick spills pixel data it cannot keep in memory to its temporary
       * directory: not /tmp (RAM on most systems), see nfc_spill_dir() */
      g_subprocess_launcher_setenv (l, "MAGICK_TEMPORARY_PATH", j->spill, TRUE);
      g_subprocess_launcher_setenv (l, "MAGICK_TMPDIR", j->spill, TRUE);
    }
  j->proc = g_subprocess_launcher_spawnv (l, (const char *const *) argv, &error);
  g_object_unref (l);
  if (j->proc == NULL)
    {
      char *cmd = join_argv (argv);
      char *msg = g_strdup_printf ("cannot run %s: %s", argv[0], error->message);

      nfc_log ("FAIL %s -> %s\n  cmd: %s\n  %s", j->src, j->key, cmd, msg);
      g_free (cmd);
      g_clear_error (&error);
      attempt_failed (j, msg);
      g_free (msg);
      return;
    }
  j->timed_out = FALSE;
  j->pgid = nfc_subprocess_pgid (j->proc);
  g_clear_object (&j->cancel);
  j->cancel = g_cancellable_new ();
  j->timeout_id = g_timeout_add_seconds (timeout_secs (j->cat, j->src_size,
                                                       j->upscale != NULL
                                                         ? nfc_upscale_timeout_secs (j->out_w, j->out_h)
                                                         : 0),
                                         step_timeout, j);
  step_start_io (j);
}

/* -------------------------------------------------------------------------
 * Memory-aware admission of image jobs
 *
 * One 200 megapixel image needs 6 to 8 GB while it converts; the runner starts
 * up to ncpu/2 images at once. Before an image job starts, its size is read
 * (`identify -ping`: header only, own process, asynchronous) and the peak
 * memory estimated (nfc_estimate_peak_bytes). The job then starts when
 *   - it is small (< NFC_SMALL_IMAGE_PIXELS): at once, never held back, or
 *   - nothing else runs (a single huge file always runs, alone), or
 *   - the estimates of the running jobs plus its own fit in 60 % of the
 *     MemAvailable read at that moment.
 * Otherwise it waits in a FIFO queue that is re-examined whenever a job ends
 * (and every 2 s, as other programs free memory too). All bookkeeping, no
 * blocking: it runs in the main loop. The accounting is global, so two
 * batches started one after the other share it.
 * ---------------------------------------------------------------------- */

#define PROBE_TIMEOUT_SECS 30
#define PUMP_RETRY_SECS    2

static GQueue admit_queue = G_QUEUE_INIT; /* Job *, FIFO, jobs with queued == TRUE */
static guint64 reserved_bytes = 0;        /* sum of est over reserved jobs */
static guint n_reserved = 0;
static guint n_gpu = 0;                   /* running Upscale jobs: at most one (the GPU slot) */
static guint pump_idle_id = 0, pump_timer_id = 0;

static void admission_pump (void);

static gboolean
pump_idle_cb (gpointer data G_GNUC_UNUSED)
{
  pump_idle_id = 0;
  admission_pump ();
  return G_SOURCE_REMOVE;
}

static gboolean
pump_timer_cb (gpointer data G_GNUC_UNUSED)
{
  pump_timer_id = 0;
  admission_pump ();
  return G_SOURCE_REMOVE;
}

static void
admission_reserve (Job *j)
{
  j->reserved = TRUE;
  reserved_bytes += j->est;
  n_reserved++;
  if (j->upscale != NULL)
    n_gpu++;
}

static void
admission_release (Job *j)
{
  if (!j->reserved)
    return;
  j->reserved = FALSE;
  reserved_bytes -= MIN (reserved_bytes, j->est);
  n_reserved--;
  if (j->upscale != NULL)
    n_gpu--;
  /* a freed reservation may let a waiting job start; not from inside the
   * callback chain of the job that just ended */
  if (!g_queue_is_empty (&admit_queue) && pump_idle_id == 0)
    pump_idle_id = g_idle_add (pump_idle_cb, NULL);
}

static void
admission_forget (Job *j)
{
  if (j->queued)
    {
      g_queue_remove (&admit_queue, j);
      j->queued = FALSE;
    }
}

static void
admission_pump (void)
{
  guint64 budget;

  if (g_queue_is_empty (&admit_queue))
    return;
  budget = nfc_admission_budget_bytes ();
  while (!g_queue_is_empty (&admit_queue) && !shutting_down)
    {
      Job *pick = NULL;

      /* FIFO, except that an Upscale job waiting for the GPU slot does not hold
       * up the jobs behind it; a job that does not fit in memory does (it would
       * starve otherwise). Scanned afresh each time: job_start may end its job at
       * once and, through its callback, queue new ones. */
      for (GList *l = admit_queue.head; l != NULL; l = l->next)
        {
          Job *j = l->data;

          if (j->upscale != NULL && n_gpu > 0)
            continue;
          if (nfc_admission_allows (j->pixels, j->est, reserved_bytes, n_reserved, budget))
            pick = j;
          break;
        }
      if (pick == NULL)
        break;
      g_queue_remove (&admit_queue, pick);
      pick->queued = FALSE;
      admission_reserve (pick);
      job_start (pick); /* may end the job at once: that only schedules a pump */
    }
  if (!g_queue_is_empty (&admit_queue) && pump_timer_id == 0)
    pump_timer_id = g_timeout_add_seconds (PUMP_RETRY_SECS, pump_timer_cb, NULL);
}

/* the size is known (or not): start the job now or queue it */
static void
admission_submit (Job *j)
{
  NfcImageInfo *info = j->have_info ? &j->info : NULL;

  if (j->upscale != NULL)
    {
      /* always queued: the GPU is one resource; the memory is the picture the
       * passes make, and its pixels (not the source's) decide where the work
       * directory goes and whether the job may be held back */
      guint f = nfc_upscale_chain_factor (j->upscale);

      j->est = nfc_upscale_peak_bytes (j->upscale, j->info.width, j->info.height, j->key);
      j->pixels = j->info.width * f * j->info.height * f;
      j->queued = TRUE;
      g_queue_push_tail (&admit_queue, j);
      admission_pump ();
      return;
    }
  j->est = nfc_estimate_peak_bytes (j->fmt, j->key, info);
  j->pixels = info != NULL ? info->width * info->height : 0;
  if (j->fmt != NULL && g_str_equal (j->fmt, "svg"))
    j->pixels = j->pixels * 64 / 9;
  if (j->pixels < NFC_SMALL_IMAGE_PIXELS)
    {
      admission_reserve (j); /* counted for the others, but never held back */
      job_start (j);
      return;
    }
  j->queued = TRUE;
  g_queue_push_tail (&admit_queue, j);
  admission_pump ();
}


/* the size probe is over: images go through the memory admission, a video (probed
 * only for its encoder bitrate) starts at once */
static void
probe_finished (Job *j)
{
  /* Upscale: the result must fit the limits; a WEBP too big for WebP becomes PNG */
  if (j->upscale != NULL)
    {
      guint64 ow, oh;
      NfcUpscaleTarget t;

      if (!j->have_info)
        {
          job_fail (j, "cannot read the size of the image");
          return;
        }
      if (!nfc_upscale_output_size (j->upscale, j->info.width, j->info.height, &ow, &oh))
        {
          job_skip_upscale (j, ow, oh);
          return;
        }
      j->out_w = ow;
      j->out_h = oh;
      if (nfc_upscale_target (j->cat, j->fmt, ow, oh, &t))
        {
          g_free (j->key);
          g_free (j->ext);
          j->key = g_strdup (t.key);
          j->ext = g_strdup (t.ext);
        }
      admission_submit (j);
      return;
    }
  /* Resize: the preset must make the picture smaller, else there is nothing to do */
  if (j->resize != NULL && j->have_info &&
      !nfc_resize_output_size (j->resize, j->info.width, j->info.height, NULL, NULL))
    {
      job_skip_resize (j);
      return;
    }
  if (j->cat == NFC_CAT_VIDEO)
    job_start (j);
  else
    admission_submit (j);
}

static gboolean
probe_timeout (gpointer data)
{
  Job *j = data;

  j->timeout_id = 0;
  nfc_kill_group (j->pgid);
  if (j->proc != NULL)
    g_subprocess_force_exit (j->proc);
  if (j->cancel != NULL)
    g_cancellable_cancel (j->cancel);
  return G_SOURCE_REMOVE;
}

static void
probe_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  char *out = NULL;
  GError *error = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, &error);

  if (j->timeout_id)
    {
      g_source_remove (j->timeout_id);
      j->timeout_id = 0;
    }
  ok = ok && g_subprocess_get_if_exited (j->proc) && g_subprocess_get_exit_status (j->proc) == 0;
  if (step_leader_alive (j))
    nfc_kill_group (j->pgid); /* nothing of the probe may linger */
  j->pgid = 0;
  g_clear_object (&j->proc);
  g_clear_object (&j->cancel);
  if (shutting_down || j->aborted)
    {
      g_free (out);
      g_clear_error (&error);
      job_fail (j, "cancelled");
      return;
    }
  if (ok && nfc_image_info_parse (out, &j->info))
    j->have_info = TRUE;
  else
    nfc_log ("size probe failed for %s (%s): converting without a memory estimate",
             j->src, error != NULL ? error->message : "no usable output");
  g_free (out);
  g_clear_error (&error);
  probe_finished (j);
}

/* first step of an image job: read the size without decoding pixels */
static void
probe_start (Job *j)
{
  char **argv = j->cat == NFC_CAT_VIDEO ? nfc_build_video_probe_argv (j->src)
                                         : nfc_build_probe_argv (j->fmt, j->src);
  GSubprocessLauncher *l;

  if (argv == NULL)
    {
      probe_finished (j);
      return;
    }
  l = nfc_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  j->proc = g_subprocess_launcher_spawnv (l, (const char *const *) argv, NULL);
  g_object_unref (l);
  g_strfreev (argv);
  if (j->proc == NULL)
    {
      probe_finished (j);
      return;
    }
  j->pgid = nfc_subprocess_pgid (j->proc);
  j->cancel = g_cancellable_new ();
  j->timeout_id = g_timeout_add_seconds (PROBE_TIMEOUT_SECS, probe_timeout, j);
  g_subprocess_communicate_utf8_async (j->proc, NULL, j->cancel, probe_done, j);
}

/* A private work directory. Image jobs of a big picture keep their intermediates
 * (decoded.png, flat.png: hundreds of MB) in the disk-backed spill directory, not
 * in /tmp. */
static char *
make_workdir (Job *j, GError **error)
{
  if (j->spill != NULL && j->pixels >= NFC_SMALL_IMAGE_PIXELS)
    {
      char *tmpl = g_build_filename (j->spill, "nfc-XXXXXX", NULL);
      char *dir = g_mkdtemp_full (tmpl, 0700);

      if (dir != NULL)
        return dir;
      g_free (tmpl);
    }
  return g_dir_make_tmp ("nfc-XXXXXX", error);
}

static void
job_start (Job *j)
{
  GError *error = NULL;

  if (shutting_down || j->aborted)
    {
      j->aborted = TRUE;
      job_fail (j, "cancelled");
      return;
    }
  if (j->cat == NFC_CAT_IMAGE)
    j->spill = nfc_spill_dir ();
  j->workdir = make_workdir (j, &error);
  if (j->workdir == NULL)
    {
      char *msg = g_strdup_printf ("cannot create a temporary directory: %s", error->message);

      g_clear_error (&error);
      job_fail (j, msg);
      g_free (msg);
      return;
    }
  if (is_office (j->cat))
    {
      char *outdir = g_build_filename (j->workdir, "out", NULL);
      char *name = g_strdup_printf ("%s.%s", j->stem, j->ext);

      g_mkdir_with_parents (outdir, 0700);
      j->produced = g_build_filename (outdir, name, NULL);
      g_free (outdir);
      g_free (name);
    }
  else
    {
      j->hidden = nfc_hidden_temp_name (j->dir, j->stem, j->ext);
      j->produced = g_strdup (j->hidden);
    }
  /* HEIC/AVIF input: the attempt starts with an nfc-decode step (own process) */
  if (j->upscale != NULL)
    j->attempts = nfc_build_attempts_upscale (j->fmt, j->key, j->upscale, j->src, j->produced,
                                              j->workdir, NULL);
  else if (j->resize != NULL)
    j->attempts = nfc_build_attempts_resize (j->cat, j->fmt, j->key, j->resize, j->src,
                                             j->produced, j->workdir,
                                             j->have_info ? &j->info : NULL, NULL);
  else
    j->attempts = nfc_build_attempts_preset (j->cat, j->fmt, j->key, j->preset, j->src,
                                             j->produced, j->workdir,
                                             j->have_info ? &j->info : NULL, NULL);
  if (j->attempts == NULL)
    {
      job_fail (j, "no converter available for this format");
      return;
    }
  run_step (j);
}

void
nfc_convert_file_async (NfcCategory cat, const char *fmt, const char *key,
                        const char *ext, const char *src,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  nfc_convert_file_preset_async (cat, fmt, key, NFC_PRESET_BEST, ext, src, callback, user_data);
}

void
nfc_convert_file_preset_async (NfcCategory cat, const char *fmt, const char *key,
                               NfcPreset preset, const char *ext, const char *src_in,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  NfcConvertOptions opts = { preset, NULL, NFC_REFERENCE_NONE, NULL, NULL };

  nfc_convert_file_opts_async (cat, fmt, key, ext, src_in, &opts, callback, user_data);
}

void
nfc_convert_file_opts_async (NfcCategory cat, const char *fmt, const char *key,
                             const char *ext, const char *src_in,
                             const NfcConvertOptions *opts,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  Job *j = g_new0 (Job, 1);
  GStatBuf st;
  NfcPreset preset = opts != NULL ? opts->preset : NFC_PRESET_BEST;

  j->task = g_task_new (NULL, NULL, callback, user_data);
  j->cat = cat;
  j->fmt = g_strdup (fmt);
  j->key = g_strdup (key);
  j->preset = preset;
  j->ext = g_strdup (ext);
  j->src = g_canonicalize_filename (src_in, NULL);
  j->dir = g_path_get_dirname (j->src);
  j->stem = nfc_stem_of (j->src);
  j->src_size = g_stat (j->src, &st) == 0 ? (gint64) st.st_size : 0;
  j->ref_size = NFC_REFERENCE_NONE;
  if (opts != NULL)
    {
      j->resize = opts->resize;
      j->upscale = opts->upscale;
      if (opts->name_suffix != NULL && *opts->name_suffix != '\0')
        {
          /* a file name is at most 255 bytes: the suffix, the extension and a
           * " (99)" must fit, so a very long stem is cut (on a character) */
          gsize room = 255 - 7 - 1 - strlen (opts->name_suffix) - strlen (ext);
          gsize len = strlen (j->stem);

          if (len > room)
            {
              len = room;
              while (len > 0 && (j->stem[len] & 0xC0) == 0x80)
                len--;
            }
          j->pub_stem = g_strdup_printf ("%.*s%s", (int) len, j->stem, opts->name_suffix);
        }
      if (opts->reference_size >= 0)
        j->ref_size = opts->reference_size;
      else if (opts->reference_size == NFC_REFERENCE_SOURCE && j->src_size > 0)
        j->ref_size = j->src_size; /* unreadable source: nothing to compare with */
    }

  if (shutting_down)
    {
      job_fail (j, "Nautilus is shutting down");
      return;
    }
  if (active_jobs == NULL)
    active_jobs = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_hash_table_add (active_jobs, j);

  if (cat == NFC_CAT_IMAGE)
    probe_start (j); /* then the admission decides when job_start runs */
  else if (cat == NFC_CAT_VIDEO &&
           (g_str_equal (key, "mp4") || g_str_equal (key, "mkv") || j->resize != NULL ||
            (preset == NFC_PRESET_SMALL && (g_str_equal (key, "webm") || g_str_equal (key, "gif")))))
    probe_start (j); /* the picture size sets the libopenh264 bitrate, tells a resize
                      * whether there is anything to do and the toast what the Small
                      * preset scaled to; then job_start */
  else
    job_start (j);
}

char *
nfc_convert_file_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
nfc_shutdown (void)
{
  GHashTableIter it;
  gpointer key;
  GList *waiting = NULL;

  shutting_down = TRUE;
  /* jobs waiting for memory have started nothing: fail them (after the loop
   * below, which must not see the table change under it) */
  while (!g_queue_is_empty (&admit_queue))
    {
      Job *q = g_queue_pop_head (&admit_queue);

      q->queued = FALSE;
      q->aborted = TRUE;
      waiting = g_list_prepend (waiting, q);
    }
  if (pump_timer_id != 0)
    {
      g_source_remove (pump_timer_id);
      pump_timer_id = 0;
    }
  if (active_jobs != NULL)
    {
      g_hash_table_iter_init (&it, active_jobs);
      while (g_hash_table_iter_next (&it, &key, NULL))
        {
          Job *j = key;

          j->aborted = TRUE;
          if (step_leader_alive (j)) /* see step_leader_alive(): never a stale pgid */
            nfc_kill_group (j->pgid);
          if (j->cancel != NULL)
            g_cancellable_cancel (j->cancel);
          /* a job in its finalize thread is left alone: it ends within moments
           * and cleans up after itself */
          if (!j->finalizing)
            job_cleanup_files (j);
        }
    }
  for (GList *l = waiting; l != NULL; l = l->next)
    job_fail (l->data, "cancelled");
  g_list_free (waiting);
}

/* -------------------------------------------------------------------------
 * Notifications (notify-send --print-id / --replace-id)
 * ---------------------------------------------------------------------- */

typedef struct {
  char *title, *body, *icon, *urgency;
} NotifyMsg;

typedef struct {
  int ref;
  char *sync;
  guint64 id;
  gboolean busy;
  NotifyMsg *pending;
} Notifier;

static void
msg_free (NotifyMsg *m)
{
  if (m == NULL)
    return;
  g_free (m->title);
  g_free (m->body);
  g_free (m->icon);
  g_free (m->urgency);
  g_free (m);
}

static void
notifier_unref (Notifier *n)
{
  if (--n->ref > 0)
    return;
  msg_free (n->pending);
  g_free (n->sync);
  g_free (n);
}

static void notifier_launch (Notifier *n, NotifyMsg *m);

static void
notify_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Notifier *n = data;
  char *out = NULL;

  if (g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, NULL) &&
      out != NULL && n->id == 0)
    n->id = g_ascii_strtoull (g_strstrip (out), NULL, 10);
  g_free (out);
  n->busy = FALSE;
  if (n->pending != NULL)
    {
      NotifyMsg *m = n->pending;

      n->pending = NULL;
      notifier_launch (n, m);
    }
  notifier_unref (n);
}

static void
notifier_launch (Notifier *n, NotifyMsg *m)
{
  GPtrArray *argv = g_ptr_array_new_null_terminated (16, g_free, TRUE);
  GSubprocessLauncher *l;
  GSubprocess *p;
  char *hint = g_strconcat ("string:x-canonical-private-synchronous:", n->sync, NULL);

  g_ptr_array_add (argv, g_strdup (nfc_tools.notify));
  {
    const char *fixed[] = { "-a", "File Converter", "-i", m->icon, "-u", m->urgency,
                            "-p", "-h", hint, NULL };

    for (const char **s = fixed; *s; s++)
      g_ptr_array_add (argv, g_strdup (*s));
  }
  if (n->id != 0)
    {
      g_ptr_array_add (argv, g_strdup ("-r"));
      g_ptr_array_add (argv, g_strdup_printf ("%" G_GUINT64_FORMAT, n->id));
    }
  /* "--": a title or body that begins with '-' (a file named "-foo.png") must not
   * be parsed as an option */
  g_ptr_array_add (argv, g_strdup ("--"));
  g_ptr_array_add (argv, g_strdup (m->title));
  g_ptr_array_add (argv, g_strdup (m->body));

  l = nfc_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  p = g_subprocess_launcher_spawnv (l, (const char *const *) argv->pdata, NULL);
  g_object_unref (l);
  g_ptr_array_unref (argv);
  g_free (hint);
  msg_free (m);
  if (p == NULL)
    return;
  n->busy = TRUE;
  n->ref++;
  g_subprocess_communicate_utf8_async (p, NULL, NULL, notify_done, n);
  g_object_unref (p); /* the async call keeps its own reference */
}

static void
notifier_send (Notifier *n, const char *title, const char *body, const char *icon,
               const char *urgency)
{
  NotifyMsg *m;

  if (nfc_tools.notify == NULL)
    return;
  m = g_new0 (NotifyMsg, 1);
  m->title = g_strdup (title);
  m->body = g_strdup (body);
  m->icon = g_strdup (icon);
  m->urgency = g_strdup (urgency);
  if (n->busy)
    {
      /* the id of the first notification is not known yet: send the next one
       * once it is (latest message wins) */
      msg_free (n->pending);
      n->pending = m;
    }
  else
    notifier_launch (n, m);
}

/* -------------------------------------------------------------------------
 * Batch
 * ---------------------------------------------------------------------- */

typedef struct {
  NfcCategory cat;
  char *key, *label, *ext, *short_label;
  char *suffix;           /* " (Balanced)" when a preset other than Best applies, else "" */
  NfcPreset preset;
  gboolean compress;      /* nfc_batch_run_compress: key/ext/cat are per file */
  const NfcResizePreset *resize; /* nfc_batch_run_resize: key/ext/cat are per file */
  const NfcUpscalePreset *upscale; /* nfc_batch_run_upscale: key/ext are per file */
  guint n, next, running, finished, max_workers;
  char **paths, **fmts;
  GPtrArray *results;
  Notifier *notifier;
  NfcBatchDone done;
  gpointer user_data;
} Batch;

typedef struct {
  Batch *batch;
  guint index;
} BatchItem;

static void
result_free (NfcResult *r)
{
  g_free (r->path);
  g_free (r->output);
  g_free (r->error);
  g_free (r);
}

static guint
workers_for (NfcCategory cat, guint n)
{
  guint cpu = g_get_num_processors ();
  guint w;

  if (is_office (cat))
    w = 1; /* one LibreOffice at a time */
  else if (cat == NFC_CAT_VIDEO)
    w = MIN (2, MAX (1, cpu / 2));
  else
    w = MAX (1, cpu / 2);
  return MIN (w, MAX (n, 1));
}

/* "-62%": how much smaller the outputs are than the sources (at least 1%), ""
 * when there is nothing to compare */
static char *
saved_text (gint64 src, gint64 out)
{
  gint pct;

  if (src <= 0 || out < 0 || out >= src)
    return g_strdup ("");
  pct = (gint) (100.0 * (double) (src - out) / (double) src + 0.5);
  return g_strdup_printf (", −%d%%", MAX (pct, 1));
}

/* " (Small)", or, when the Small preset scaled this file down, " (Small, 2560×1707)":
 * the suffix of the success toast of file i (g_free). The size is the one the preset
 * makes of the source size the job probed. */
static char *
result_suffix (Batch *b, guint i)
{
  const NfcResult *r = b->results->pdata[i];
  NfcCategory cat = b->cat;
  const char *key = b->key;
  NfcCompressTarget ct;
  guint64 ow, oh;

  if (b->compress)
    {
      if (!nfc_compress_target (b->cat, b->fmts[i], &ct))
        return g_strdup (b->suffix);
      cat = ct.cat;
      key = ct.key;
    }
  if (b->preset == NFC_PRESET_SMALL && r->src_w > 0 &&
      nfc_preset_output_size (cat, key, b->preset, r->src_w, r->src_h, &ow, &oh))
    return g_strdup_printf (" (%s, %" G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT ")",
                            nfc_preset_label (b->preset), ow, oh);
  return g_strdup (b->suffix);
}

/* "Failed: name — reason" for the first three failed files of a batch, and a
 * pointer to the log for the rest */
static void
append_failures (Batch *b, GString *body)
{
  guint shown = 0, bad = 0;

  for (guint i = 0; i < b->n; i++)
    {
      NfcResult *r = b->results->pdata[i];
      char *base;

      if (r->ok || r->skipped)
        continue;
      bad++;
      if (shown >= 3)
        continue;
      base = g_path_get_basename (r->path);
      if (body->len)
        g_string_append_c (body, '\n');
      g_string_append_printf (body, "Failed: %s — %s", base, r->error);
      g_free (base);
      shown++;
    }
  if (bad > 3)
    g_string_append_printf (body, "\n…and %u more (see ~/.cache/nautilus-file-converter.log)",
                            bad - 3);
}

/* The notification of a resize batch: what was made (with the sizes for one
 * file), what was already smaller than the preset, what failed. Returns the
 * title; body is filled; *icon is static. */
static const char *
resize_message (Batch *b, GString *body, const char **icon)
{
  const NfcResizePreset *p = b->resize;
  guint good = 0, skipped = 0, bad = 0;
  const char *small_verb = p->percent > 0 ? "too small to resize to" : "already smaller than";

  for (guint i = 0; i < b->n; i++)
    {
      NfcResult *r = b->results->pdata[i];

      if (r->ok)
        good++;
      else if (r->skipped)
        skipped++;
      else
        bad++;
    }
  if (good == 1)
    for (guint i = 0; i < b->n; i++)
      {
        NfcResult *r = b->results->pdata[i];
        guint64 ow, oh;

        if (!r->ok)
          continue;
        {
          char *from = g_path_get_basename (r->path);
          char *to = g_path_get_basename (r->output);

          g_string_append_printf (body, "%s → %s", from, to);
          g_free (from);
          g_free (to);
        }
        if (r->src_w > 0 && nfc_resize_output_size (p, r->src_w, r->src_h, &ow, &oh))
          g_string_append_printf (body, ", %" G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT " → %"
                                  G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT,
                                  r->src_w, r->src_h, ow, oh);
      }
  else if (good > 1 && good == b->n)
    g_string_append_printf (body, "Resized %u files to %s", good, p->name);
  else if (good > 1)
    g_string_append_printf (body, "Resized %u of %u files to %s", good, b->n, p->name);

  if (skipped == 1)
    for (guint i = 0; i < b->n; i++)
      {
        NfcResult *r = b->results->pdata[i];

        if (r->skipped)
          {
            char *base = g_path_get_basename (r->path);

            if (body->len)
              g_string_append_c (body, '\n');
            g_string_append_printf (body, "%s is %s %s", base, small_verb, p->name);
            g_free (base);
          }
      }
  else if (skipped > 1)
    {
      if (body->len)
        g_string_append_c (body, '\n');
      g_string_append_printf (body, "%u files are %s %s", skipped, small_verb, p->name);
    }
  append_failures (b, body);
  if (bad > 0)
    {
      *icon = "dialog-error-symbolic";
      return "Resize failed";
    }
  if (good > 0)
    {
      *icon = "object-select-symbolic";
      return "Resize complete";
    }
  *icon = "dialog-information-symbolic";
  return "Nothing to resize";
}

/* The extension of a path, lower case, as a format key ("png", "jpg", "webp") */
static char *
path_format_key (const char *path)
{
  const char *dot = strrchr (path, '.');
  char *k;

  if (dot == NULL)
    return g_strdup ("");
  k = g_ascii_strdown (dot + 1, -1);
  if (g_str_equal (k, "jpeg"))
    {
      g_free (k);
      return g_strdup ("jpg");
    }
  return k;
}

/* The notification of an upscale batch: what was made (with the sizes for one
 * file), what was too big, what failed. Returns the title; body is filled; *icon
 * is static. The sizes are those of the output file itself (its header), which
 * knows the EXIF rotation the size probe of the source does not. */
static const char *
upscale_message (Batch *b, GString *body, const char **icon)
{
  const NfcUpscalePreset *p = b->upscale;
  guint good = 0, skipped = 0, bad = 0;

  for (guint i = 0; i < b->n; i++)
    {
      NfcResult *r = b->results->pdata[i];

      if (r->ok)
        good++;
      else if (r->skipped)
        skipped++;
      else
        bad++;
    }
  if (good == 1)
    for (guint i = 0; i < b->n; i++)
      {
        NfcResult *r = b->results->pdata[i];
        char *from, *to, *fmt;
        guint32 ow = 0, oh = 0;
        gboolean capped = FALSE;

        if (!r->ok)
          continue;
        from = g_path_get_basename (r->path);
        to = g_path_get_basename (r->output);
        g_string_append_printf (body, "%s → %s", from, to);
        fmt = path_format_key (r->output);
        if (!nfc_header_read_file (r->output, fmt, &ow, &oh))
          ow = oh = 0; /* no header found: the size the probe saw, below */
        if (ow > 0 && ow % p->factor == 0 && oh % p->factor == 0)
          g_string_append_printf (body, ", %u×%u → %u×%u", ow / p->factor, oh / p->factor, ow, oh);
        else if (r->src_w > 0)
          {
            guint64 sw, sh;

            nfc_upscale_output_size (p, r->src_w, r->src_h, &sw, &sh);
            g_string_append_printf (body, ", %" G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT " → %"
                                    G_GUINT64_FORMAT "×%" G_GUINT64_FORMAT,
                                    r->src_w, r->src_h, sw, sh);
          }
        capped = g_str_equal (b->fmts[i], "webp") && g_str_equal (fmt, "png");
        if (capped)
          g_string_append (body, " (saved as PNG: WEBP is limited to 16383 px)");
        g_free (from);
        g_free (to);
        g_free (fmt);
      }
  else if (good > 1 && good == b->n)
    g_string_append_printf (body, "Upscaled %u files %s", good, p->label);
  else if (good > 1)
    g_string_append_printf (body, "Upscaled %u of %u files %s", good, b->n, p->label);

  if (skipped == 1)
    for (guint i = 0; i < b->n; i++)
      {
        NfcResult *r = b->results->pdata[i];

        if (r->skipped)
          {
            char *base = g_path_get_basename (r->path);

            if (body->len)
              g_string_append_c (body, '\n');
            g_string_append_printf (body, "%s is %s", base, r->error);
            g_free (base);
          }
      }
  else if (skipped > 1)
    {
      if (body->len)
        g_string_append_c (body, '\n');
      g_string_append_printf (body, "%u files are too big to upscale %s", skipped, p->label);
    }
  append_failures (b, body);
  if (bad > 0)
    {
      *icon = "dialog-error-symbolic";
      return "Upscale failed";
    }
  if (good > 0)
    {
      *icon = "object-select-symbolic";
      return "Upscale complete";
    }
  *icon = "dialog-information-symbolic";
  return "Nothing to upscale";
}

/* The notification of a compress batch: what was made, what was already as small
 * as it gets, what failed. Returns the title; body is filled; *icon is static. */
static const char *
compress_message (Batch *b, GString *body, const char **icon)
{
  guint good = 0, skipped = 0, bad = 0, shown = 0;
  gint64 src_total = 0, out_total = 0;
  char *saved;

  for (guint i = 0; i < b->n; i++)
    {
      NfcResult *r = b->results->pdata[i];

      if (r->ok)
        {
          good++;
          src_total += r->src_size;
          out_total += r->out_size;
        }
      else if (r->skipped)
        skipped++;
      else
        bad++;
    }
  saved = saved_text (src_total, out_total);
  if (good == 1)
    {
      for (guint i = 0; i < b->n; i++)
        {
          NfcResult *r = b->results->pdata[i];

          if (r->ok)
            {
              char *from = g_path_get_basename (r->path);
              char *to = g_path_get_basename (r->output);
              char *suffix = result_suffix (b, i);

              g_string_append_printf (body, "%s → %s%s%s", from, to, suffix, saved);
              g_free (from);
              g_free (to);
              g_free (suffix);
            }
        }
    }
  else if (good > 1 && good == b->n)
    g_string_append_printf (body, "Compressed %u files%s%s", good, b->suffix, saved);
  else if (good > 1)
    g_string_append_printf (body, "Compressed %u of %u files%s%s", good, b->n, b->suffix, saved);
  g_free (saved);

  if (skipped == 1)
    for (guint i = 0; i < b->n; i++)
      {
        NfcResult *r = b->results->pdata[i];

        if (r->skipped)
          {
            char *base = g_path_get_basename (r->path);

            if (body->len)
              g_string_append_c (body, '\n');
            g_string_append_printf (body, "%s is already as small as it gets", base);
            g_free (base);
          }
      }
  else if (skipped > 1)
    {
      if (body->len)
        g_string_append_c (body, '\n');
      g_string_append_printf (body, "%u files are already as small as they get", skipped);
    }
  for (guint i = 0; i < b->n && shown < 3; i++)
    {
      NfcResult *r = b->results->pdata[i];
      char *base;

      if (r->ok || r->skipped)
        continue;
      base = g_path_get_basename (r->path);
      if (body->len)
        g_string_append_c (body, '\n');
      g_string_append_printf (body, "Failed: %s — %s", base, r->error);
      g_free (base);
      shown++;
    }
  if (bad > 3)
    g_string_append_printf (body, "\n…and %u more (see ~/.cache/nautilus-file-converter.log)",
                            bad - 3);
  if (bad > 0)
    {
      *icon = "dialog-error-symbolic";
      return "Compression failed";
    }
  if (good > 0)
    {
      *icon = "object-select-symbolic";
      return "Compression complete";
    }
  *icon = "dialog-information-symbolic";
  return "Nothing to compress";
}

static void
batch_finish (Batch *b)
{
  guint good = 0;
  GString *body = g_string_new (NULL);

  for (guint i = 0; i < b->n; i++)
    if (((NfcResult *) b->results->pdata[i])->ok)
      good++;

  if (b->upscale != NULL)
    {
      const char *icon;
      const char *title = upscale_message (b, body, &icon);

      notifier_send (b->notifier, title, body->str, icon, "normal");
    }
  else if (b->resize != NULL)
    {
      const char *icon;
      const char *title = resize_message (b, body, &icon);

      notifier_send (b->notifier, title, body->str, icon, "normal");
    }
  else if (b->compress)
    {
      const char *icon;
      const char *title = compress_message (b, body, &icon);

      notifier_send (b->notifier, title, body->str, icon, "normal");
    }
  else if (good == b->n)
    {
      if (b->n == 1)
        {
          NfcResult *r = b->results->pdata[0];
          char *from = g_path_get_basename (r->path);
          char *to = g_path_get_basename (r->output);
          char *suffix = result_suffix (b, 0);

          g_string_printf (body, "%s → %s%s", from, to, suffix);
          g_free (from);
          g_free (to);
          g_free (suffix);
        }
      else
        g_string_printf (body, "Converted %u files to %s", good, b->short_label);
      /* "emblem-ok-symbolic" does not exist in Adwaita 50 (GNOME Shell drew a
       * missing-image placeholder); object-select-symbolic does. Every banner
       * is urgency normal: a critical one stays until dismissed and the later
       * banners of the same sync group then queue behind it and never show. */
      notifier_send (b->notifier, "Conversion complete", body->str,
                     "object-select-symbolic", "normal");
    }
  else
    {
      guint shown = 0, bad = b->n - good;

      if (good)
        g_string_append_printf (body, "Converted %u of %u files to %s", good, b->n,
                                b->short_label);
      else if (b->preset != NFC_PRESET_BEST)
        g_string_append_printf (body, "Could not convert to %s", b->short_label);
      for (guint i = 0; i < b->n && shown < 3; i++)
        {
          NfcResult *r = b->results->pdata[i];
          char *base;

          if (r->ok)
            continue;
          base = g_path_get_basename (r->path);
          if (body->len)
            g_string_append_c (body, '\n');
          g_string_append_printf (body, "Failed: %s — %s", base, r->error);
          g_free (base);
          shown++;
        }
      if (bad > 3)
        g_string_append_printf (body,
                                "\n…and %u more (see ~/.cache/nautilus-file-converter.log)",
                                bad - 3);
      notifier_send (b->notifier, "Conversion failed", body->str,
                     "dialog-error-symbolic", "normal");
    }
  g_string_free (body, TRUE);

  if (b->done != NULL)
    b->done (b->results, b->user_data);

  notifier_unref (b->notifier);
  g_ptr_array_unref (b->results);
  g_strfreev (b->paths);
  g_strfreev (b->fmts);
  g_free (b->key);
  g_free (b->label);
  g_free (b->ext);
  g_free (b->short_label);
  g_free (b->suffix);
  g_free (b);
}

static void batch_pump (Batch *b);

/* an item has ended: output (owned) or error (borrowed, may be NULL) */
static void
batch_item_complete (BatchItem *it, char *output, const GError *error)
{
  Batch *b = it->batch;
  NfcResult *r = b->results->pdata[it->index];

  r->output = output;
  r->ok = r->output != NULL;
  if (!r->ok)
    {
      r->error = g_strdup (error ? error->message : "conversion failed");
      r->skipped = error != NULL &&
                   (g_error_matches (error, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_NOT_SMALLER) ||
                    g_error_matches (error, NFC_CONVERT_ERROR, NFC_CONVERT_ERROR_TOO_BIG));
    }
  if (b->compress)
    {
      GStatBuf st;

      if (g_stat (r->path, &st) == 0)
        r->src_size = st.st_size;
      if (r->ok && g_stat (r->output, &st) == 0)
        r->out_size = st.st_size;
    }
  g_free (it);
  b->running--;
  b->finished++;
  if (b->finished == b->n)
    batch_finish (b);
  else
    batch_pump (b);
}

static void
batch_item_done (GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer data)
{
  GError *error = NULL;
  BatchItem *it = data;
  NfcResult *r = it->batch->results->pdata[it->index];
  char *output = nfc_convert_file_finish (res, &error);

  nfc_convert_result_dims (res, &r->src_w, &r->src_h);
  batch_item_complete (data, output, error);
  g_clear_error (&error);
}

/* a file that cannot be compressed: ends from an idle, never from inside
 * batch_pump (which would find its batch freed) */
static gboolean
batch_item_unsupported (gpointer data)
{
  BatchItem *it = data;
  GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                       it->batch->upscale != NULL
                                         ? "this format cannot be upscaled"
                                         : it->batch->resize != NULL
                                           ? "this format cannot be resized"
                                           : "this format cannot be compressed");

  batch_item_complete (data, NULL, error);
  g_error_free (error);
  return G_SOURCE_REMOVE;
}

static void
batch_pump (Batch *b)
{
  while (b->next < b->n && b->running < b->max_workers)
    {
      BatchItem *it = g_new0 (BatchItem, 1);

      it->batch = b;
      it->index = b->next++;
      b->running++;
      if (b->upscale != NULL)
        {
          NfcUpscaleTarget ut;
          char *suffix = g_strdup_printf (" (%s)", b->upscale->key);
          NfcConvertOptions o = { NFC_PRESET_BEST, suffix, NFC_REFERENCE_NONE, NULL, b->upscale };

          /* the key/ext are the nominal ones: the job switches a WEBP that WebP
           * cannot hold to PNG once it knows the size */
          if (!nfc_upscale_target (b->cat, b->fmts[it->index], 0, 0, &ut))
            g_idle_add (batch_item_unsupported, it);
          else
            nfc_convert_file_opts_async (b->cat, b->fmts[it->index], ut.key, ut.ext,
                                         b->paths[it->index], &o, batch_item_done, it);
          g_free (suffix); /* copied by the job */
          continue;
        }
      if (b->resize != NULL)
        {
          NfcResizeTarget rt;
          char *suffix = g_strdup_printf (" (%s)", b->resize->name);
          NfcConvertOptions o = { NFC_PRESET_BEST, suffix, NFC_REFERENCE_NONE, b->resize, NULL };

          if (!nfc_resize_target (b->cat, b->fmts[it->index], &rt))
            g_idle_add (batch_item_unsupported, it);
          else
            nfc_convert_file_opts_async (rt.cat, b->fmts[it->index], rt.key, rt.ext,
                                         b->paths[it->index], &o, batch_item_done, it);
          g_free (suffix); /* copied by the job */
          continue;
        }
      if (b->compress)
        {
          NfcCompressTarget ct;
          NfcConvertOptions o = { b->preset, " (compressed)", NFC_REFERENCE_SOURCE, NULL, NULL };

          if (!nfc_compress_target (b->cat, b->fmts[it->index], &ct))
            {
              g_idle_add (batch_item_unsupported, it);
              continue;
            }
          nfc_convert_file_opts_async (ct.cat, b->fmts[it->index], ct.key, ct.ext,
                                       b->paths[it->index], &o, batch_item_done, it);
          continue;
        }
      nfc_convert_file_preset_async (b->cat, b->fmts[it->index], b->key, b->preset, b->ext,
                                     b->paths[it->index], batch_item_done, it);
    }
}

void
nfc_batch_run (NfcCategory cat, const char *key, const char *label, const char *ext,
               const char *const *paths, const char *const *fmts, guint n,
               NfcBatchDone done, gpointer user_data)
{
  nfc_batch_run_preset (cat, key, NFC_PRESET_BEST, label, ext, paths, fmts, n, done, user_data);
}

static void
batch_start (gboolean compress, const NfcResizePreset *resize, const NfcUpscalePreset *upscale,
             NfcCategory cat,
             const char *key, NfcPreset preset,
             const char *label, const char *ext, const char *const *paths,
             const char *const *fmts, guint n, NfcBatchDone done, gpointer user_data)
{
  Batch *b;
  char *what, *sp;

  if (n == 0 || paths == NULL || fmts == NULL)
    {
      if (done != NULL)
        {
          GPtrArray *empty = g_ptr_array_new ();

          done (empty, user_data);
          g_ptr_array_unref (empty);
        }
      return;
    }
  b = g_new0 (Batch, 1);
  b->cat = cat;
  b->key = g_strdup (key);
  b->label = g_strdup (label);
  b->ext = g_strdup (ext);
  b->short_label = g_strdup (label);
  if ((sp = strstr (b->short_label, " (")) != NULL)
    *sp = '\0';
  b->compress = compress;
  b->resize = resize;
  b->upscale = upscale;
  b->preset = ((guint) preset < NFC_PRESET_N && (compress || nfc_target_has_presets (cat, key)))
                ? preset : NFC_PRESET_BEST;
  b->suffix = b->preset == NFC_PRESET_BEST ? g_strdup ("")
                                           : g_strdup_printf (" (%s)", nfc_preset_label (b->preset));
  {
    char *with = g_strconcat (b->short_label, b->suffix, NULL);

    g_free (b->short_label);
    b->short_label = with;
  }
  b->n = n;
  b->paths = g_new0 (char *, n + 1);
  b->fmts = g_new0 (char *, n + 1);
  b->results = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);
  for (guint i = 0; i < n; i++)
    {
      NfcResult *r = g_new0 (NfcResult, 1);

      b->paths[i] = g_strdup (paths[i]);
      b->fmts[i] = g_strdup (fmts[i]);
      r->path = g_strdup (paths[i]);
      g_ptr_array_add (b->results, r);
    }
  b->max_workers = upscale != NULL ? 1 : workers_for (cat, n); /* the GPU takes one at a time */
  b->done = done;
  b->user_data = user_data;

  b->notifier = g_new0 (Notifier, 1);
  b->notifier->ref = 1;
  b->notifier->sync = g_strdup_printf ("nfc-%08x", g_random_int ());

  if (n == 1)
    what = g_path_get_basename (paths[0]);
  else
    what = g_strdup_printf ("%u files", n);
  {
    char *body = upscale != NULL ? g_strdup_printf ("%s (%s)", what, upscale->label)
                 : resize != NULL ? (n == 1 ? g_strdup_printf ("%s (%s)", what, resize->name)
                                            : g_strdup_printf ("%s to %s", what, resize->name))
                 : compress ? g_strdup_printf ("%s%s", what, b->suffix)
                            : g_strdup_printf ("%s to %s", what, b->short_label);

    notifier_send (b->notifier, upscale != NULL ? "Upscaling…" : resize != NULL ? "Resizing…"
                                : compress ? "Compressing…" : "Converting…",
                   body, "view-refresh-symbolic", "normal");
    g_free (body);
  }
  g_free (what);
  batch_pump (b);
}

void
nfc_batch_run_preset (NfcCategory cat, const char *key, NfcPreset preset, const char *label,
                      const char *ext, const char *const *paths, const char *const *fmts,
                      guint n, NfcBatchDone done, gpointer user_data)
{
  batch_start (FALSE, NULL, NULL, cat, key, preset, label, ext, paths, fmts, n, done, user_data);
}

void
nfc_batch_run_compress (NfcCategory cat, NfcPreset preset, const char *const *paths,
                        const char *const *fmts, guint n, NfcBatchDone done,
                        gpointer user_data)
{
  batch_start (TRUE, NULL, NULL, cat, "", preset, "Compress", "", paths, fmts, n, done, user_data);
}

void
nfc_batch_run_resize (NfcCategory cat, const char *preset_key, const char *const *paths,
                      const char *const *fmts, guint n, NfcBatchDone done, gpointer user_data)
{
  const NfcResizePreset *p = nfc_resize_preset_from_key (cat, preset_key);

  if (p == NULL)
    {
      /* nothing to run: every file failed */
      GPtrArray *results = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);

      for (guint i = 0; i < n && paths != NULL; i++)
        {
          NfcResult *r = g_new0 (NfcResult, 1);

          r->path = g_strdup (paths[i]);
          r->error = g_strdup ("unknown resize preset");
          g_ptr_array_add (results, r);
        }
      if (done != NULL)
        done (results, user_data);
      g_ptr_array_unref (results);
      return;
    }
  batch_start (FALSE, p, NULL, cat, "", NFC_PRESET_BEST, "Resize", "", paths, fmts, n, done, user_data);
}

void
nfc_batch_run_upscale (const char *preset_key, const char *const *paths, const char *const *fmts,
                       guint n, NfcBatchDone done, gpointer user_data)
{
  const NfcUpscalePreset *p = nfc_upscale_preset_from_key (preset_key);

  if (p == NULL)
    {
      /* nothing to run: every file failed */
      GPtrArray *results = g_ptr_array_new_with_free_func ((GDestroyNotify) result_free);

      for (guint i = 0; i < n && paths != NULL; i++)
        {
          NfcResult *r = g_new0 (NfcResult, 1);

          r->path = g_strdup (paths[i]);
          r->error = g_strdup ("unknown upscale preset");
          g_ptr_array_add (results, r);
        }
      if (done != NULL)
        done (results, user_data);
      g_ptr_array_unref (results);
      return;
    }
  batch_start (FALSE, NULL, p, NFC_CAT_IMAGE, "", NFC_PRESET_BEST, "Upscale", "", paths, fmts, n,
               done, user_data);
}
