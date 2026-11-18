/* nautilus-file-converter: asynchronous conversion runner. See nfc-convert.h. */
#include "nfc-convert.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */

static GMutex log_lock;

void
nfc_log (const char *format, ...)
{
  va_list ap;
  char *msg, *dir, *path, *ts;
  GDateTime *now;
  FILE *fh;

  va_start (ap, format);
  msg = g_strdup_vprintf (format, ap);
  va_end (ap);

  g_mutex_lock (&log_lock);
  dir = g_strdup (g_get_user_cache_dir ());
  g_mkdir_with_parents (dir, 0755);
  path = g_build_filename (dir, "nautilus-file-converter.log", NULL);
  now = g_date_time_new_now_local ();
  ts = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
  fh = g_fopen (path, "a");
  if (fh != NULL)
    {
      fprintf (fh, "[%s] %s\n", ts, msg);
      fclose (fh);
    }
  g_mutex_unlock (&log_lock);
  g_date_time_unref (now);
  g_free (ts);
  g_free (path);
  g_free (dir);
  g_free (msg);
}

static char *
last_line (const char *text)
{
  char **lines;
  char *res = NULL;

  if (text == NULL)
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  for (int i = (int) g_strv_length (lines) - 1; i >= 0; i--)
    {
      char *s = g_strstrip (lines[i]);

      if (*s != '\0')
        {
          res = g_strdup (s);
          break;
        }
    }
  g_strfreev (lines);
  return res;
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

static void
rm_rf (const char *path)
{
  GDir *d;
  const char *name;

  if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    {
      d = g_dir_open (path, 0, NULL);
      if (d != NULL)
        {
          while ((name = g_dir_read_name (d)) != NULL)
            {
              char *child = g_build_filename (path, name, NULL);

              rm_rf (child);
              g_free (child);
            }
          g_dir_close (d);
        }
      g_rmdir (path);
    }
  else
    g_unlink (path);
}

/* -------------------------------------------------------------------------
 * Single conversion job
 * ---------------------------------------------------------------------- */

typedef struct {
  NfcCategory cat;
  char *fmt, *key, *ext, *src, *dir, *stem;
  char *workdir, *produced, *hidden;
  gboolean needs_decode;
  GPtrArray *attempts;
  guint ai, si;
  char *err;
  GSubprocess *proc;
  guint timeout_id;
  gboolean timed_out;
  GTask *task;
} Job;

static void run_step (Job *j);

static gboolean
is_office (NfcCategory c)
{
  return c == NFC_CAT_DOC || c == NFC_CAT_SHEET || c == NFC_CAT_SLIDE;
}

static void
job_cleanup_files (Job *j)
{
  if (j->hidden != NULL && g_file_test (j->hidden, G_FILE_TEST_EXISTS))
    g_unlink (j->hidden);
  if (j->workdir != NULL)
    rm_rf (j->workdir);
}

static void
job_free (Job *j)
{
  if (j->timeout_id)
    g_source_remove (j->timeout_id);
  g_clear_object (&j->proc);
  g_clear_pointer (&j->attempts, g_ptr_array_unref);
  g_free (j->fmt);
  g_free (j->key);
  g_free (j->ext);
  g_free (j->src);
  g_free (j->dir);
  g_free (j->stem);
  g_free (j->workdir);
  g_free (j->produced);
  g_free (j->hidden);
  g_free (j->err);
  g_free (j);
}

static void
job_fail (Job *j, const char *msg)
{
  GTask *task = j->task;

  job_cleanup_files (j);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                           msg != NULL && *msg ? msg : "conversion failed");
  j->task = NULL;
  job_free (j);
  g_object_unref (task);
}

/* current attempt failed: drop partial output and move to the next one */
static void
attempt_failed (Job *j, const char *msg)
{
  g_free (j->err);
  j->err = g_strdup (msg);
  if (j->produced != NULL && g_file_test (j->produced, G_FILE_TEST_EXISTS))
    g_unlink (j->produced);
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
  final = nfc_publish (j->hidden, j->dir, j->stem, j->ext, &error);
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

    rm_rf (j->workdir);
    g_task_return_pointer (task, final, g_free);
    j->task = NULL;
    job_free (j);
    g_object_unref (task);
  }
}

static void
step_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  GBytes *out = NULL, *errb = NULL;
  GError *error = NULL;
  char **argv;
  GPtrArray *attempt = j->attempts->pdata[j->ai];

  argv = attempt->pdata[j->si];
  if (j->timeout_id)
    {
      g_source_remove (j->timeout_id);
      j->timeout_id = 0;
    }
  if (!g_subprocess_communicate_finish (G_SUBPROCESS (source), res, &out, &errb, &error))
    {
      char *cmd = join_argv (argv);

      nfc_log ("FAIL %s -> %s\n  cmd: %s\n  %s", j->src, j->key, cmd, error->message);
      g_free (cmd);
      g_clear_object (&j->proc);
      attempt_failed (j, error->message);
      g_clear_error (&error);
      return;
    }
  {
    gboolean exited = g_subprocess_get_if_exited (j->proc);
    int status = exited ? g_subprocess_get_exit_status (j->proc) : -1;
    gsize n1, n2;
    const char *p1 = out ? g_bytes_get_data (out, &n1) : NULL;
    const char *p2 = errb ? g_bytes_get_data (errb, &n2) : NULL;
    char *so = p1 ? g_utf8_make_valid (p1, n1) : g_strdup ("");
    char *se = p2 ? g_utf8_make_valid (p2, n2) : g_strdup ("");

    g_clear_object (&j->proc);
    if (j->timed_out || status != 0)
      {
        char *cmd = join_argv (argv);
        char *msg;

        if (j->timed_out)
          msg = g_strdup ("timed out");
        else
          {
            msg = last_line (se);
            if (msg == NULL)
              msg = last_line (so);
            if (msg == NULL)
              msg = g_strdup_printf ("exit code %d", status);
          }
        nfc_log ("FAIL %s -> %s (%s)\n  cmd: %s\n  stderr:\n%s\n  stdout:\n%s", j->src,
                 j->key, j->timed_out ? "timeout" : "exit status", cmd, se, so);
        j->timed_out = FALSE;
        g_free (cmd);
        g_free (so);
        g_free (se);
        g_bytes_unref (out);
        g_bytes_unref (errb);
        attempt_failed (j, msg);
        g_free (msg);
        return;
      }
    g_free (so);
    g_free (se);
  }
  g_bytes_unref (out);
  g_bytes_unref (errb);
  j->si++;
  run_step (j);
}

static gboolean
step_timeout (gpointer data)
{
  Job *j = data;

  j->timeout_id = 0;
  j->timed_out = TRUE;
  if (j->proc != NULL)
    g_subprocess_force_exit (j->proc);
  return G_SOURCE_REMOVE;
}

static void
run_step (Job *j)
{
  GPtrArray *attempt;
  GSubprocessLauncher *l;
  GError *error = NULL;
  char **argv;

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
          GTask *ft = g_task_new (NULL, NULL, finalize_done, j);

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
  l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_stdin_file_path (l, "/dev/null");
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
  j->timeout_id = g_timeout_add_seconds (j->cat == NFC_CAT_IMAGE ? 600 : 1800,
                                         step_timeout, j);
  g_subprocess_communicate_async (j->proc, NULL, NULL, step_done, j);
}

static void
decode_thread (GTask *task, gpointer source G_GNUC_UNUSED, gpointer data, GCancellable *c G_GNUC_UNUSED)
{
  Job *j = data;
  GError *error = NULL;
  char *dest = g_build_filename (j->workdir, "decoded.png", NULL);

  if (!nfc_pixbuf_decode (j->src, dest, &error))
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
  g_free (dest);
}

static void
decode_done (GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer data)
{
  Job *j = data;
  GError *error = NULL;

  if (!g_task_propagate_boolean (G_TASK (res), &error))
    {
      char *msg = g_strdup_printf ("cannot decode image: %s", error->message);

      nfc_log ("FAIL %s -> %s\n  GdkPixbuf: %s", j->src, j->key, error->message);
      g_clear_error (&error);
      job_fail (j, msg);
      g_free (msg);
      return;
    }
  run_step (j);
}

void
nfc_convert_file_async (NfcCategory cat, const char *fmt, const char *key,
                        const char *ext, const char *src_in,
                        GAsyncReadyCallback callback, gpointer user_data)
{
  Job *j = g_new0 (Job, 1);
  GError *error = NULL;
  GTask *t;

  j->task = g_task_new (NULL, NULL, callback, user_data);
  j->cat = cat;
  j->fmt = g_strdup (fmt);
  j->key = g_strdup (key);
  j->ext = g_strdup (ext);
  j->src = g_canonicalize_filename (src_in, NULL);
  j->dir = g_path_get_dirname (j->src);
  j->stem = nfc_stem_of (j->src);

  j->workdir = g_dir_make_tmp ("nfc-XXXXXX", &error);
  if (j->workdir == NULL)
    {
      char *msg = g_strdup_printf ("cannot create a temporary directory: %s", error->message);

      g_clear_error (&error);
      job_fail (j, msg);
      g_free (msg);
      return;
    }
  if (is_office (cat))
    {
      char *outdir = g_build_filename (j->workdir, "out", NULL);
      char *name = g_strdup_printf ("%s.%s", j->stem, ext);

      g_mkdir_with_parents (outdir, 0700);
      j->produced = g_build_filename (outdir, name, NULL);
      g_free (outdir);
      g_free (name);
    }
  else
    {
      j->hidden = nfc_hidden_temp_name (j->dir, j->stem, ext);
      j->produced = g_strdup (j->hidden);
    }
  j->attempts = nfc_build_attempts (cat, j->fmt, key, j->src, j->produced, j->workdir,
                                    &j->needs_decode);
  if (j->attempts == NULL)
    {
      job_fail (j, "no converter available for this format");
      return;
    }
  if (j->needs_decode)
    {
      /* HEIC/AVIF: decode in-process via GdkPixbuf off the main thread */
      t = g_task_new (NULL, NULL, decode_done, j);
      g_task_set_task_data (t, j, NULL);
      g_task_run_in_thread (t, decode_thread);
      g_object_unref (t);
    }
  else
    run_step (j);
}

char *
nfc_convert_file_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
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
  g_ptr_array_add (argv, g_strdup (m->title));
  g_ptr_array_add (argv, g_strdup (m->body));

  l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_stdin_file_path (l, "/dev/null");
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

static void
batch_finish (Batch *b)
{
  guint good = 0;
  GString *body = g_string_new (NULL);

  for (guint i = 0; i < b->n; i++)
    if (((NfcResult *) b->results->pdata[i])->ok)
      good++;

  if (good == b->n)
    {
      if (b->n == 1)
        {
          NfcResult *r = b->results->pdata[0];
          char *from = g_path_get_basename (r->path);
          char *to = g_path_get_basename (r->output);

          g_string_printf (body, "%s → %s", from, to);
          g_free (from);
          g_free (to);
        }
      else
        g_string_printf (body, "Converted %u files to %s", good, b->short_label);
      notifier_send (b->notifier, "Conversion complete", body->str,
                     "emblem-ok-symbolic", "normal");
    }
  else
    {
      guint shown = 0, bad = b->n - good;

      if (good)
        g_string_append_printf (body, "Converted %u of %u files to %s", good, b->n,
                                b->short_label);
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
                     "dialog-error-symbolic", "critical");
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
  g_free (b);
}

static void batch_pump (Batch *b);

static void
batch_item_done (GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer data)
{
  BatchItem *it = data;
  Batch *b = it->batch;
  NfcResult *r = b->results->pdata[it->index];
  GError *error = NULL;

  r->output = nfc_convert_file_finish (res, &error);
  r->ok = r->output != NULL;
  if (!r->ok)
    r->error = g_strdup (error ? error->message : "conversion failed");
  g_clear_error (&error);
  g_free (it);
  b->running--;
  b->finished++;
  if (b->finished == b->n)
    batch_finish (b);
  else
    batch_pump (b);
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
      nfc_convert_file_async (b->cat, b->fmts[it->index], b->key, b->ext,
                              b->paths[it->index], batch_item_done, it);
    }
}

void
nfc_batch_run (NfcCategory cat, const char *key, const char *label, const char *ext,
               const char *const *paths, const char *const *fmts, guint n,
               NfcBatchDone done, gpointer user_data)
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
  b->max_workers = workers_for (cat, n);
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
    char *body = g_strdup_printf ("%s to %s", what, b->short_label);

    notifier_send (b->notifier, "Converting…", body, "view-refresh-symbolic", "normal");
    g_free (body);
  }
  g_free (what);
  batch_pump (b);
}
