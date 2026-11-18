/* Tray icon tests (src/nfc-tray.c): a StatusNotifierItem and its dbusmenu, over a
 * PRIVATE session bus (meson runs this under dbus-run-session and sets
 * NFC_TEST_PRIVATE_BUS; without it the test skips: it must never put an item
 * on the user's real bus).
 *
 * Hermetic: the StatusNotifierWatcher is a tiny fake owned here, on a second
 * connection to the private bus, that records what it is asked to register; the
 * conversions are a fake ffmpeg (a /bin/sh script that sleeps, then writes its
 * output) and a fake notify-send. No Python.
 *
 *  - a job in flight registers the item; its properties, tooltip and menu
 *    describe the jobs; "Cancel all" ends every job and every file of the batches
 *    as "cancelled", leaves no file behind and leaves the module usable
 *  - the item goes (its bus name is released) once the jobs have ended
 *  - a job that ends within the show delay never shows it
 *  - no watcher: nothing happens, the jobs still work; a watcher that appears
 *    later gets the item
 *  - tray=false (text and file): nothing is put on the bus
 *  - file names that are not UTF-8 (two of them running at once, and one alone): the
 *    tooltip, the menu labels and everything sent are valid UTF-8, the layout is
 *    well formed, nothing crashes and GLib raises no critical
 *  - menu calls cannot be amplified: ids named twice are answered once, absurdly long
 *    lists are refused (GetGroupProperties, EventGroup), fast and without memory growth
 *  - the watcher replaced while a registration is pending: the item registers with
 *    the new one (once the old one has answered or died)
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfc-convert.h"
#include "nfc-core.h"
#include "nfc-tray.h"

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define SNI_PATH     "/StatusNotifierItem"
#define SNI_IFACE    "org.kde.StatusNotifierItem"
#define MENU_PATH    "/MenuBar"
#define MENU_IFACE   "com.canonical.dbusmenu"

static char *base, *bindir, *datadir, *config_home;
static GDBusConnection *test_conn; /* the fake watcher and the test's own calls */
static guint n_new_tooltip, n_layout_updated; /* signals seen */

static char *
B (const char *dir, const char *name)
{
  return g_build_filename (dir, name, NULL);
}

static void
write_script (const char *path, const char *body)
{
  char *content = g_strconcat ("#!/bin/sh\n", body, NULL);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  g_assert_cmpint (g_chmod (path, 0755), ==, 0);
  g_free (content);
}

static guint
count_entries (const char *dir, const char *prefix)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *n;
  guint c = 0;

  while (d && (n = g_dir_read_name (d)))
    if (g_str_has_prefix (n, prefix))
      c++;
  if (d)
    g_dir_close (d);
  return c;
}

/* ---- spinning the main loop ---- */

static gboolean
timed_out_cb (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

#define SPIN_UNTIL(cond, ms) \
  G_STMT_START { \
    gboolean to_ = FALSE; \
    guint id_ = g_timeout_add ((ms), timed_out_cb, &to_); \
    while (!(cond) && !to_) \
      g_main_context_iteration (NULL, TRUE); \
    if (!to_) \
      g_source_remove (id_); \
    g_assert_true ((cond)); \
  } G_STMT_END

static void
spin_for (guint ms)
{
  gboolean to = FALSE;

  g_timeout_add (ms, timed_out_cb, &to);
  while (!to)
    g_main_context_iteration (NULL, TRUE);
}

/* ---- calls on the private bus, without blocking the main loop (the tray lives
 * in this process) ---- */

typedef struct {
  gboolean done;
  GVariant *res;
  GError *err;
} CallRes;

static void
call_cb (GObject *src, GAsyncResult *r, gpointer data)
{
  CallRes *c = data;

  c->res = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), r, &c->err);
  c->done = TRUE;
}

/* the reply of a method call (floating params), or NULL with *err set when err is given */
static GVariant *
bus_call_err (const char *dest, const char *path, const char *iface, const char *method,
              GVariant *params, GError **err)
{
  CallRes c = { FALSE, NULL, NULL };

  g_dbus_connection_call (test_conn, dest, path, iface, method, params, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, call_cb, &c);
  SPIN_UNTIL (c.done, 10000);
  if (err != NULL)
    {
      if (c.err != NULL)
        g_propagate_error (err, c.err);
    }
  else
    {
      g_assert_no_error (c.err);
      g_clear_error (&c.err);
    }
  return c.res;
}

static GVariant *
bus_call (const char *dest, const char *path, const char *iface, const char *method,
          GVariant *params)
{
  return bus_call_err (dest, path, iface, method, params, NULL);
}

static gboolean
name_has_owner (const char *name)
{
  GVariant *r = bus_call ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "NameHasOwner", g_variant_new ("(s)", name));
  gboolean has;

  g_variant_get (r, "(b)", &has);
  g_variant_unref (r);
  return has;
}

/* ---- the fake StatusNotifierWatcher ---- */

static const char watcher_xml[] =
  "<node><interface name='org.kde.StatusNotifierWatcher'>"
  " <method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
  " <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
  " <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
  " <property name='ProtocolVersion' type='i' access='read'/>"
  "</interface></node>";

static GPtrArray *registered;   /* char *: the services the watcher was asked to register */
static guint watcher_reg, watcher_own;
static gboolean watcher_up, watcher_name_got;
static GDBusNodeInfo *watcher_info;

static void
watcher_method (GDBusConnection *c G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                const char *p G_GNUC_UNUSED, const char *i G_GNUC_UNUSED, const char *m,
                GVariant *params, GDBusMethodInvocation *inv, gpointer d G_GNUC_UNUSED)
{
  const char *svc;

  g_assert_cmpstr (m, ==, "RegisterStatusNotifierItem");
  g_variant_get (params, "(&s)", &svc);
  g_ptr_array_add (registered, g_strdup (svc));
  g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *
watcher_prop (GDBusConnection *c G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
              const char *p G_GNUC_UNUSED, const char *i G_GNUC_UNUSED, const char *prop,
              GError **e G_GNUC_UNUSED, gpointer d G_GNUC_UNUSED)
{
  if (g_str_equal (prop, "RegisteredStatusNotifierItems"))
    return g_variant_new_strv ((const char *const *) registered->pdata, registered->len);
  if (g_str_equal (prop, "IsStatusNotifierHostRegistered"))
    return g_variant_new_boolean (TRUE);
  return g_variant_new_int32 (0);
}

static const GDBusInterfaceVTable watcher_vtable = { watcher_method, watcher_prop, NULL, { 0 } };

static void
watcher_got_name (GDBusConnection *c G_GNUC_UNUSED, const char *n G_GNUC_UNUSED,
                  gpointer d G_GNUC_UNUSED)
{
  watcher_name_got = TRUE;
}

static void
watcher_start_flags (GBusNameOwnerFlags flags)
{
  g_assert_false (watcher_up);
  if (watcher_info == NULL)
    watcher_info = g_dbus_node_info_new_for_xml (watcher_xml, NULL);
  g_ptr_array_set_size (registered, 0);
  watcher_reg = g_dbus_connection_register_object (test_conn, "/StatusNotifierWatcher",
                                                   watcher_info->interfaces[0], &watcher_vtable,
                                                   NULL, NULL, NULL);
  g_assert_cmpuint (watcher_reg, !=, 0);
  watcher_name_got = FALSE;
  watcher_own = g_bus_own_name_on_connection (test_conn, WATCHER_NAME,
                                              flags, watcher_got_name, NULL,
                                              NULL, NULL);
  SPIN_UNTIL (watcher_name_got, 5000);
  watcher_up = TRUE;
}

static void
watcher_start (void)
{
  watcher_start_flags (G_BUS_NAME_OWNER_FLAGS_NONE);
}

static void
watcher_stop (void)
{
  if (!watcher_up)
    return;
  g_bus_unown_name (watcher_own);
  g_dbus_connection_unregister_object (test_conn, watcher_reg);
  SPIN_UNTIL (!name_has_owner (WATCHER_NAME), 5000);
  watcher_up = FALSE;
  g_ptr_array_set_size (registered, 0);
}

/* ---- reading the item ---- */

static GVariant *
item_props (const char *name)
{
  GVariant *r = bus_call (name, SNI_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                          g_variant_new ("(s)", SNI_IFACE));
  GVariant *dict = g_variant_get_child_value (r, 0);

  g_variant_unref (r);
  return dict;
}

static char *
item_tooltip_text (const char *name)
{
  GVariant *dict = item_props (name);
  GVariant *tt = g_variant_lookup_value (dict, "ToolTip", G_VARIANT_TYPE ("(sa(iiay)ss)"));
  const char *icon, *title, *text;
  char *out;

  g_assert_nonnull (tt);
  g_variant_get (tt, "(&s@a(iiay)&s&s)", &icon, NULL, &title, &text);
  g_assert_cmpstr (icon, ==, "view-refresh-symbolic");
  g_assert_cmpstr (title, ==, "File Converter");
  out = g_strdup (text);
  g_variant_unref (tt);
  g_variant_unref (dict);
  return out;
}

typedef struct {
  gint32 id;
  char *label;      /* NULL: a separator */
  gboolean enabled;
} MenuItem;

static void
menu_item_free (MenuItem *m)
{
  g_free (m->label);
  g_free (m);
}

/* the children of the root of the menu */
static GPtrArray *
menu_items (const char *name)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) menu_item_free);
  GVariant *r = bus_call (name, MENU_PATH, MENU_IFACE, "GetLayout",
                          g_variant_new ("(ii@as)", 0, -1,
                                         g_variant_new_strv (NULL, 0)));
  GVariant *kids;
  GVariantIter iter;
  GVariant *kid;

  g_variant_get (r, "(u(i@a{sv}@av))", NULL, NULL, NULL, &kids);
  g_variant_iter_init (&iter, kids);
  while (g_variant_iter_next (&iter, "v", &kid))
    {
      MenuItem *m = g_new0 (MenuItem, 1);
      GVariant *props;
      const char *s;

      g_variant_get (kid, "(i@a{sv}@av)", &m->id, &props, NULL);
      m->enabled = TRUE;
      if (g_variant_lookup (props, "label", "&s", &s))
        m->label = g_strdup (s);
      g_variant_lookup (props, "enabled", "b", &m->enabled);
      if (g_variant_lookup (props, "type", "&s", &s))
        g_assert_cmpstr (s, ==, "separator");
      else
        g_assert_nonnull (m->label);
      g_ptr_array_add (items, m);
      g_variant_unref (props);
      g_variant_unref (kid);
    }
  g_variant_unref (kids);
  g_variant_unref (r);
  return items;
}

/* ---- running jobs ---- */

typedef struct {
  gboolean done;
  guint n, ok;
  GPtrArray *errors;  /* char *: one per failed file */
  char **outputs;
} BatchResult;

static void
batch_done (GPtrArray *results, gpointer data)
{
  BatchResult *b = data;

  b->n = results->len;
  b->errors = g_ptr_array_new_with_free_func (g_free);
  b->outputs = g_new0 (char *, results->len + 1);
  for (guint i = 0; i < results->len; i++)
    {
      NfcResult *r = results->pdata[i];

      if (r->ok)
        {
          b->ok++;
          b->outputs[i] = g_strdup (r->output);
        }
      else
        g_ptr_array_add (b->errors, g_strdup (r->error));
    }
  b->done = TRUE;
}

static void
batch_result_clear (BatchResult *b)
{
  g_clear_pointer (&b->errors, g_ptr_array_unref);
  g_strfreev (b->outputs);
}

/* a fake ffmpeg: sleeps secs, then writes its output (the last argument) */
static void
set_ffmpeg (const char *secs)
{
  char *script = B (bindir, "ffmpeg");
  char *body = g_strdup_printf ("for a; do last=\"$a\"; done\nsleep %s\nprintf done > \"$last\"\n",
                                secs);

  write_script (script, body);
  g_free (script);
  g_free (body);
}

/* mp4 -> mkv of the files datadir/<names[i]> (any bytes) */
static void
start_batch_names (const char *const *names, guint n, BatchResult *res)
{
  const char **paths = g_new0 (const char *, n + 1), **fmts = g_new0 (const char *, n + 1);
  char **owned = g_new0 (char *, n + 1);

  for (guint i = 0; i < n; i++)
    {
      owned[i] = B (datadir, names[i]);
      g_assert_true (g_file_set_contents (owned[i], "x", -1, NULL));
      paths[i] = owned[i];
      fmts[i] = "mp4";
    }
  memset (res, 0, sizeof *res);
  nfc_batch_run (NFC_CAT_VIDEO, "mkv", "MKV", "mkv", paths, fmts, n, batch_done, res);
  g_strfreev (owned);
  g_free (paths);
  g_free (fmts);
}

/* mp4 -> mkv of the files datadir/<prefix><i>.mp4 */
static void
start_batch (const char *prefix, guint n, BatchResult *res)
{
  char **names = g_new0 (char *, n + 1);

  for (guint i = 0; i < n; i++)
    names[i] = g_strdup_printf ("%s%u.mp4", prefix, i + 1);
  start_batch_names ((const char *const *) names, n, res);
  g_strfreev (names);
}

#define WAIT_BATCH(r, ms) SPIN_UNTIL ((r)->done, (ms))

#define N_REGS() (registered->len)

static void
test_begin (gboolean with_watcher)
{
  g_assert_false (nfc_tray_is_shown ());
  if (with_watcher)
    watcher_start ();
  nfc_tray_start (TRUE);
}

static void
test_end (void)
{
  nfc_tray_shutdown ();
  watcher_stop ();
}

/* ---- tests ---- */

/* the item's registration, properties, tooltip and menu; Cancel all */
static void
test_register_and_cancel (void)
{
  BatchResult a, b, c;
  guint before = count_entries (base, "nfc-");
  char *name, *text;
  GPtrArray *items;
  GVariant *dict, *v;
  MenuItem *cancel;

  set_ffmpeg ("30");
  test_begin (TRUE);
  g_assert_cmpuint (N_REGS (), ==, 0);
  start_batch ("alpha_", 1, &a);
  /* not at once (a job that ends within a moment never shows it), but soon */
  spin_for (100);
  g_assert_cmpuint (N_REGS (), ==, 0);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  {
    char *prefix = g_strdup_printf ("org.kde.StatusNotifierItem-%d-", (int) getpid ());

    g_assert_true (g_str_has_prefix (name, prefix));
    g_free (prefix);
  }
  g_assert_true (name_has_owner (name));
  g_assert_true (nfc_tray_is_shown ());

  /* the properties */
  dict = item_props (name);
  v = g_variant_lookup_value (dict, "Status", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, "Active");
  g_variant_unref (v);
  v = g_variant_lookup_value (dict, "IconName", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, "view-refresh-symbolic");
  g_variant_unref (v);
  v = g_variant_lookup_value (dict, "Category", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, "ApplicationStatus");
  g_variant_unref (v);
  v = g_variant_lookup_value (dict, "Id", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, "nautilus-file-converter");
  g_variant_unref (v);
  v = g_variant_lookup_value (dict, "Title", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, "File Converter");
  g_variant_unref (v);
  v = g_variant_lookup_value (dict, "Menu", NULL);
  g_assert_cmpstr (g_variant_get_string (v, NULL), ==, MENU_PATH);
  g_variant_unref (v);
  g_variant_unref (dict);

  /* the tooltip names the job; the menu has its line (the file name's "_" is
   * not a mnemonic: it is sent as it is, escaped by doubling) */
  text = item_tooltip_text (name);
  g_assert_cmpstr (text, ==, "Converting alpha_1.mp4 to MKV…");
  g_free (text);
  items = menu_items (name);
  g_assert_cmpuint (items->len, ==, 3);
  g_assert_cmpstr (((MenuItem *) items->pdata[0])->label, ==, "alpha__1.mp4 → MKV");
  g_assert_false (((MenuItem *) items->pdata[0])->enabled);
  g_assert_null (((MenuItem *) items->pdata[1])->label); /* separator */
  g_assert_cmpstr (((MenuItem *) items->pdata[2])->label, ==, "Cancel all");
  g_assert_true (((MenuItem *) items->pdata[2])->enabled);
  g_ptr_array_unref (items);

  /* a second operation: the tooltip and the menu follow (within the throttle) */
  start_batch ("beta", 4, &b);
  start_batch ("gamma", 1, &c);
  for (int i = 0; i < 100; i++)
    {
      text = item_tooltip_text (name);
      if (g_str_has_prefix (text, "3 operations"))
        break;
      g_free (text);
      text = NULL;
      spin_for (50);
    }
  g_assert_nonnull (text);
  g_assert_cmpstr (text, ==, "3 operations, 0 of 6 files done…");
  g_free (text);
  g_assert_cmpuint (n_new_tooltip, >, 0);   /* the signals told the host */
  g_assert_cmpuint (n_layout_updated, >, 0);
  items = menu_items (name);
  /* alpha_1, gamma1 and the (at most 2) running files of beta; the rest of beta
   * is not started: "… and N more" */
  g_assert_cmpuint (items->len, >=, 5);
  cancel = items->pdata[items->len - 1];
  g_assert_cmpstr (cancel->label, ==, "Cancel all");
  g_assert_null (((MenuItem *) items->pdata[items->len - 2])->label);
  g_assert_true (g_str_has_prefix (((MenuItem *) items->pdata[items->len - 3])->label, "…")
                 || g_str_has_suffix (((MenuItem *) items->pdata[items->len - 3])->label, "→ MKV"));
  {
    gboolean more = FALSE;

    for (guint i = 0; i < items->len; i++)
      {
        const char *l = ((MenuItem *) items->pdata[i])->label;

        if (l != NULL && g_str_has_prefix (l, "… and "))
          more = TRUE;
      }
    /* beta has 4 files and a video batch runs at most 2 at a time */
    g_assert_true (more);
  }

  /* Cancel all: the "clicked" event of the item */
  v = bus_call (name, MENU_PATH, MENU_IFACE, "Event",
                g_variant_new ("(isvu)", cancel->id, "clicked", g_variant_new_int32 (0), 0));
  g_variant_unref (v);
  g_ptr_array_unref (items);
  WAIT_BATCH (&a, 20000);
  WAIT_BATCH (&b, 20000);
  WAIT_BATCH (&c, 20000);
  g_assert_cmpuint (a.ok + b.ok + c.ok, ==, 0);
  g_assert_cmpuint (a.errors->len, ==, 1);
  g_assert_cmpuint (b.errors->len, ==, 4);   /* the files that never started too */
  g_assert_cmpuint (c.errors->len, ==, 1);
  for (guint i = 0; i < b.errors->len; i++)
    g_assert_cmpstr (b.errors->pdata[i], ==, "cancelled");
  g_assert_cmpstr (a.errors->pdata[0], ==, "cancelled");
  g_assert_cmpstr (c.errors->pdata[0], ==, "cancelled");
  g_assert_cmpuint (count_entries (base, "nfc-"), ==, before);  /* no work directory left */
  g_assert_cmpuint (count_entries (datadir, "."), ==, 0);       /* no hidden temp file left */
  batch_result_clear (&a);
  batch_result_clear (&b);
  batch_result_clear (&c);

  /* nothing in flight: the item goes after the debounce */
  SPIN_UNTIL (!name_has_owner (name), 5000);
  g_assert_false (nfc_tray_is_shown ());
  g_free (name);

  /* the module is usable after a cancel: a new job runs and ends ok */
  set_ffmpeg ("0");
  start_batch ("after", 1, &a);
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.ok, ==, 1);
  g_assert_true (g_str_has_suffix (a.outputs[0], "after1.mkv"));
  g_assert_true (g_file_test (a.outputs[0], G_FILE_TEST_IS_REGULAR));
  batch_result_clear (&a);
  test_end ();
}

/* the item is on the bus while the job runs and its name is released after it */
static void
test_unregister_when_done (void)
{
  BatchResult a;
  char *name;
  gint64 t_end;

  set_ffmpeg ("2");
  test_begin (TRUE);
  start_batch ("done", 1, &a);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  g_assert_true (name_has_owner (name));
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.ok, ==, 1);
  batch_result_clear (&a);
  t_end = g_get_monotonic_time ();
  /* the debounce: still there right after the job */
  g_assert_true (name_has_owner (name));
  SPIN_UNTIL (!name_has_owner (name), 5000);
  g_assert_cmpint (g_get_monotonic_time () - t_end, >=, 300 * 1000);
  g_assert_false (nfc_tray_is_shown ());
  g_assert_cmpuint (N_REGS (), ==, 1);

  /* a later job gets a new item with a new name */
  start_batch ("done2_", 1, &a);
  SPIN_UNTIL (N_REGS () == 2, 5000);
  g_assert_cmpstr (g_ptr_array_index (registered, 1), !=, name);
  g_free (name);
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  test_end ();
}

/* a job that ends within the show delay never shows the item */
static void
test_short_job_never_shows (void)
{
  BatchResult a;

  set_ffmpeg ("0");
  test_begin (TRUE);
  start_batch ("short", 1, &a);
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.ok, ==, 1);
  batch_result_clear (&a);
  spin_for (1800);
  g_assert_cmpuint (N_REGS (), ==, 0);
  g_assert_false (nfc_tray_is_shown ());
  test_end ();
}

/* no watcher: nothing is registered, the job works; a watcher that comes later
 * gets the item */
static void
test_no_watcher_then_watcher (void)
{
  BatchResult a;
  char *name;

  set_ffmpeg ("5");
  test_begin (FALSE);
  start_batch ("nowatch", 1, &a);
  spin_for (1500);
  g_assert_true (nfc_tray_is_shown ()); /* on the bus, nobody to tell */
  watcher_start ();
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  g_assert_true (name_has_owner (name));
  /* the watcher restarts: the item registers with the new one */
  watcher_stop ();
  watcher_start ();
  SPIN_UNTIL (N_REGS () == 1, 5000);
  g_assert_cmpstr (g_ptr_array_index (registered, 0), ==, name);
  g_free (name);
  WAIT_BATCH (&a, 30000);
  g_assert_cmpuint (a.ok, ==, 1);
  batch_result_clear (&a);
  test_end ();
}

static void
test_no_watcher_jobs_work (void)
{
  BatchResult a;

  set_ffmpeg ("1");
  test_begin (FALSE);
  start_batch ("nw", 2, &a);
  WAIT_BATCH (&a, 30000);
  g_assert_cmpuint (a.ok, ==, 2);
  batch_result_clear (&a);
  SPIN_UNTIL (!nfc_tray_is_shown (), 5000); /* released without a watcher too */
  test_end ();
}

/* tray=false: nothing is exported */
static void
test_disabled (void)
{
  BatchResult a;
  char *cfgdir = B (config_home, "nautilus-file-converter"), *cfg = B (cfgdir, "config");

  g_assert_false (nfc_tray_config_enabled ("tray=false\n"));
  g_assert_false (nfc_tray_config_enabled ("upscale-model=x\n # c\n tray = off \n"));
  g_assert_false (nfc_tray_config_enabled ("tray=no"));
  g_assert_false (nfc_tray_config_enabled ("tray=0"));
  g_assert_false (nfc_tray_config_enabled ("tray=true\ntray=False\n")); /* the last wins */
  g_assert_true (nfc_tray_config_enabled ("tray=false\ntray=true\n"));
  g_assert_true (nfc_tray_config_enabled ("tray=true\n"));
  g_assert_true (nfc_tray_config_enabled ("#tray=false\n"));
  g_assert_true (nfc_tray_config_enabled ("upscale-model=x"));
  g_assert_true (nfc_tray_config_enabled (""));
  g_assert_true (nfc_tray_config_enabled (NULL));

  set_ffmpeg ("2");
  watcher_start ();
  nfc_tray_start (nfc_tray_config_enabled ("tray=false\n"));
  start_batch ("off", 1, &a);
  spin_for (1500);
  g_assert_cmpuint (N_REGS (), ==, 0);
  g_assert_false (nfc_tray_is_shown ());
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.ok, ==, 1);
  batch_result_clear (&a);
  nfc_tray_shutdown ();

  /* the real configuration file, read by nfc_tray_init */
  g_assert_cmpint (g_mkdir_with_parents (cfgdir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (cfg, "# File Converter\ntray=false\n", -1, NULL));
  nfc_tray_init ();
  start_batch ("off2_", 1, &a);
  spin_for (1500);
  g_assert_cmpuint (N_REGS (), ==, 0);
  g_assert_false (nfc_tray_is_shown ());
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  nfc_tray_shutdown ();
  /* ... and without the line, it is on */
  g_assert_true (g_file_set_contents (cfg, "upscale-model=whatever\n", -1, NULL));
  set_ffmpeg ("3");
  nfc_tray_init ();
  start_batch ("on_", 1, &a);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  g_unlink (cfg);
  g_free (cfg);
  g_free (cfgdir);
  test_end ();
}

/* menu protocol details: ids, properties, AboutToShow, bad arguments */
static void
test_menu_protocol (void)
{
  BatchResult a;
  char *name;
  GVariant *r, *arr;
  GError *err = NULL;
  GPtrArray *items;
  gint32 id_cancel;

  set_ffmpeg ("30");
  test_begin (TRUE);
  start_batch ("menu", 1, &a);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  items = menu_items (name);
  id_cancel = ((MenuItem *) items->pdata[items->len - 1])->id;
  g_ptr_array_unref (items);

  r = bus_call (name, MENU_PATH, MENU_IFACE, "AboutToShow", g_variant_new ("(i)", 0));
  g_variant_unref (r);
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", g_variant_new_fixed_array (G_VARIANT_TYPE_INT32, &id_cancel, 1, 4),
                               g_variant_new_strv (NULL, 0)));
  arr = g_variant_get_child_value (r, 0);
  g_assert_cmpuint (g_variant_n_children (arr), ==, 1);
  g_variant_unref (arr);
  g_variant_unref (r);
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetProperty", g_variant_new ("(is)", id_cancel, "label"));
  g_variant_get (r, "(v)", &arr);
  g_assert_cmpstr (g_variant_get_string (arr, NULL), ==, "Cancel all");
  g_variant_unref (arr);
  g_variant_unref (r);
  r = bus_call_err (name, MENU_PATH, MENU_IFACE, "GetLayout",
                    g_variant_new ("(ii@as)", 4242, -1, g_variant_new_strv (NULL, 0)), &err);
  g_assert_null (r);
  g_assert_nonnull (err);
  g_clear_error (&err);
  /* Activate and the like do nothing, and do not cancel */
  r = bus_call (name, SNI_PATH, SNI_IFACE, "Activate", g_variant_new ("(ii)", 1, 1));
  g_variant_unref (r);
  r = bus_call (name, MENU_PATH, MENU_IFACE, "Event",
                g_variant_new ("(isvu)", 1, "clicked", g_variant_new_int32 (0), 0)); /* a job line */
  g_variant_unref (r);
  spin_for (300);
  g_assert_false (a.done);
  nfc_cancel_all ();
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.errors->len, ==, 1);
  g_assert_cmpstr (a.errors->pdata[0], ==, "cancelled");
  batch_result_clear (&a);
  g_free (name);
  test_end ();
}

/* ---- file names that are not UTF-8 ---- */

static void
assert_item_valid (const char *name, const char *expect_tooltip)
{
  GPtrArray *items = menu_items (name);   /* asserts every item has a label (or is a separator) */
  char *text = item_tooltip_text (name);
  GVariant *r, *arr;
  GVariantIter iter;
  GVariant *entry;

  g_assert_true (g_utf8_validate (text, -1, NULL));
  if (expect_tooltip != NULL)
    g_assert_cmpstr (text, ==, expect_tooltip);
  g_free (text);
  for (guint i = 0; i < items->len; i++)
    {
      const MenuItem *m = items->pdata[i];

      if (m->label != NULL)
        g_assert_true (g_utf8_validate (m->label, -1, NULL));
    }
  g_ptr_array_unref (items);

  /* the same through the other calls */
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", g_variant_new_fixed_array (G_VARIANT_TYPE_INT32, NULL, 0, 4),
                               g_variant_new_strv (NULL, 0)));
  arr = g_variant_get_child_value (r, 0);
  g_variant_iter_init (&iter, arr);
  while ((entry = g_variant_iter_next_value (&iter)) != NULL)
    {
      GVariant *props;
      const char *label;

      g_variant_get (entry, "(i@a{sv})", NULL, &props);
      if (g_variant_lookup (props, "label", "&s", &label))
        g_assert_true (g_utf8_validate (label, -1, NULL));
      g_variant_unref (props);
      g_variant_unref (entry);
    }
  g_variant_unref (arr);
  g_variant_unref (r);
}

/* two bad names at once (the menu lines are sorted: g_utf8_collate crashed on them),
 * then one alone (a malformed item without its label, and GLib criticals) */
static void
test_non_utf8_names (void)
{
  const char *two[] = { "bad\xff\xfename.mp4", "caf\xe9.mp4" };
  const char *one[] = { "solo\xe9\xff.mp4" };
  BatchResult a;
  char *name;
  GPtrArray *items;
  gboolean seen_bad = FALSE, seen_cafe = FALSE;

  set_ffmpeg ("30");
  test_begin (TRUE);
  start_batch_names (two, 2, &a);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  for (int i = 0; i < 100; i++)   /* both jobs are running */
    {
      items = menu_items (name);
      if (items->len >= 4)
        break;
      g_ptr_array_unref (items);
      items = NULL;
      spin_for (50);
    }
  g_assert_nonnull (items);
  g_assert_cmpuint (items->len, ==, 4);  /* two jobs, the separator, Cancel all */
  for (guint i = 0; i < 2; i++)
    {
      const char *l = ((MenuItem *) items->pdata[i])->label;

      g_assert_nonnull (l);
      g_assert_true (g_utf8_validate (l, -1, NULL));
      /* the display name: invalid bytes shown as U+FFFD, the rest as it is */
      seen_bad |= g_str_equal (l, "bad\xef\xbf\xbd\xef\xbf\xbd" "name.mp4 \xe2\x86\x92 MKV");
      seen_cafe |= g_str_equal (l, "caf\xef\xbf\xbd.mp4 \xe2\x86\x92 MKV");
    }
  g_assert_true (seen_bad);
  g_assert_true (seen_cafe);
  g_ptr_array_unref (items);
  assert_item_valid (name, "Converting 2 files to MKV\xe2\x80\xa6");
  /* new jobs while it is shown: the tooltip and the layout are updated */
  spin_for (700);
  assert_item_valid (name, NULL);
  nfc_cancel_all ();
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  SPIN_UNTIL (!name_has_owner (name), 5000);
  g_free (name);

  /* one file alone */
  start_batch_names (one, 1, &a);
  SPIN_UNTIL (N_REGS () == 2, 5000);
  name = g_strdup (g_ptr_array_index (registered, 1));
  assert_item_valid (name, "Converting solo\xef\xbf\xbd\xef\xbf\xbd.mp4 to MKV\xe2\x80\xa6");
  items = menu_items (name);
  g_assert_cmpuint (items->len, ==, 3);
  g_assert_cmpstr (((MenuItem *) items->pdata[0])->label, ==,
                   "solo\xef\xbf\xbd\xef\xbf\xbd.mp4 \xe2\x86\x92 MKV");
  g_ptr_array_unref (items);
  nfc_cancel_all ();
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  g_free (name);
  test_end ();
}

/* ---- menu calls that could be amplified ---- */

/* the peak resident memory since it was last reset (Linux: clear_refs 5), in kB */
static long
peak_rss_kb (gboolean reset)
{
  char *t = NULL;
  long kb = 0;

  if (reset)
    {
      int fd = open ("/proc/self/clear_refs", O_WRONLY);

      if (fd >= 0)
        {
          if (write (fd, "5", 1) != 1)
            kb = -1;
          close (fd);
        }
    }
  if (g_file_get_contents ("/proc/self/status", &t, NULL, NULL))
    {
      const char *p = strstr (t, "VmHWM:");

      if (p != NULL)
        kb = atol (p + 6);
      g_free (t);
    }
  return kb;
}

static GVariant *
id_array (gint32 id, gsize n, gint32 mod)
{
  gint32 *ids = g_new (gint32, n);
  GVariant *v;

  for (gsize i = 0; i < n; i++)
    ids[i] = mod > 0 ? (gint32) (i % mod) : id;
  v = g_variant_new_fixed_array (G_VARIANT_TYPE_INT32, ids, n, 4);
  g_free (ids);
  return v;
}

static GVariant *
name_array (gsize n)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("as"));
  for (gsize i = 0; i < n; i++)
    g_variant_builder_add (&b, "s", "label");
  return g_variant_builder_end (&b);
}

static GVariant *
events_array (gsize n, gint32 mod, const char *event)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("a(isvu)"));
  for (gsize i = 0; i < n; i++)
    g_variant_builder_add (&b, "(isvu)", (gint32) (i % mod), event, g_variant_new_int32 (0), 0u);
  return g_variant_builder_end (&b);
}

static void
assert_invalid_args (GVariant *r, GError **err)
{
  g_assert_null (r);
  g_assert_error (*err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_clear_error (err);
}

static void
test_menu_flood (void)
{
  BatchResult a;
  char *name;
  GPtrArray *items;
  GVariant *r, *arr;
  GError *err = NULL;
  guint real_items;
  gint64 t0;
  long before, peak;
  gboolean sanitized = g_getenv ("NFC_TEST_SANITIZED") != NULL;
  gboolean valid[20] = { FALSE };
  guint bad_events = 0;

  set_ffmpeg ("30");
  test_begin (TRUE);
  start_batch ("flood", 2, &a);
  SPIN_UNTIL (N_REGS () == 1, 5000);
  name = g_strdup (g_ptr_array_index (registered, 0));
  items = menu_items (name);
  real_items = items->len + 1;   /* and the root */
  valid[0] = TRUE;
  for (guint i = 0; i < items->len; i++)
    valid[((MenuItem *) items->pdata[i])->id] = TRUE;
  g_ptr_array_unref (items);

  /* a million times the same id: refused at once, no memory, no stall */
  before = peak_rss_kb (TRUE);
  t0 = g_get_monotonic_time ();
  r = bus_call_err (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                    g_variant_new ("(@ai@as)", id_array (11, 1000000, 0), g_variant_new_strv (NULL, 0)),
                    &err);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, (sanitized ? 2000 : 200) * 1000);
  if (r != NULL)  /* answered instead of refused: then each item once at most */
    {
      arr = g_variant_get_child_value (r, 0);
      g_assert_cmpuint (g_variant_n_children (arr), <=, real_items);
      g_variant_unref (arr);
      g_variant_unref (r);
      g_clear_error (&err);
    }
  else
    assert_invalid_args (NULL, &err);
  peak = peak_rss_kb (FALSE);
  if (!sanitized && before > 0)
    g_assert_cmpint (peak - before, <, 50 * 1024);   /* the baseline was 2.2 GB */

  /* the longest accepted list, all the same id: that item once */
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", id_array (11, 4096, 0), g_variant_new_strv (NULL, 0)));
  arr = g_variant_get_child_value (r, 0);
  g_assert_cmpuint (g_variant_n_children (arr), ==, 1);
  g_variant_unref (arr);
  g_variant_unref (r);
  /* ids that are mostly unknown, every real one named many times: each once, in the
   * order first named, the unknown ones left out */
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", id_array (0, 4096, 40), g_variant_new_strv (NULL, 0)));
  arr = g_variant_get_child_value (r, 0);
  g_assert_cmpuint (g_variant_n_children (arr), ==, real_items);
  {
    GHashTable *seen = g_hash_table_new (g_direct_hash, g_direct_equal);
    GVariantIter iter;
    GVariant *entry;

    g_variant_iter_init (&iter, arr);
    while ((entry = g_variant_iter_next_value (&iter)) != NULL)
      {
        gint32 id;

        g_variant_get (entry, "(i@a{sv})", &id, NULL);
        g_assert_true (g_hash_table_add (seen, GINT_TO_POINTER (id + 1)));
        g_variant_unref (entry);
      }
    g_hash_table_unref (seen);
  }
  g_variant_unref (arr);
  g_variant_unref (r);
  /* no ids named: all of them, as before */
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", id_array (0, 0, 0), g_variant_new_strv (NULL, 0)));
  arr = g_variant_get_child_value (r, 0);
  g_assert_cmpuint (g_variant_n_children (arr), ==, real_items);
  g_variant_unref (arr);
  g_variant_unref (r);

  /* one more than the limit: refused; so is a property filter that long */
  r = bus_call_err (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                    g_variant_new ("(@ai@as)", id_array (11, 4097, 0), g_variant_new_strv (NULL, 0)),
                    &err);
  assert_invalid_args (r, &err);
  r = bus_call_err (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                    g_variant_new ("(@ai@as)", id_array (11, 1, 0), name_array (5000)), &err);
  assert_invalid_args (r, &err);
  r = bus_call (name, MENU_PATH, MENU_IFACE, "GetGroupProperties",
                g_variant_new ("(@ai@as)", id_array (11, 1, 0), name_array (4096)));
  g_variant_unref (r);

  /* EventGroup: 20000 events are refused; 4096 harmless ones are answered, with the
   * unknown ids as errors, and cancel nothing */
  t0 = g_get_monotonic_time ();
  r = bus_call_err (name, MENU_PATH, MENU_IFACE, "EventGroup",
                    g_variant_new ("(@a(isvu))", events_array (20000, 12, "hovered")), &err);
  assert_invalid_args (r, &err);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, (sanitized ? 2000 : 500) * 1000);
  r = bus_call (name, MENU_PATH, MENU_IFACE, "EventGroup",
                g_variant_new ("(@a(isvu))", events_array (4096, 20, "hovered")));
  arr = g_variant_get_child_value (r, 0);
  /* ids 0..19: those that are no item are errors */
  for (guint i = 0; i < 4096; i++)
    if (!valid[i % 20])
      bad_events++;
  g_assert_cmpuint (bad_events, >, 0);
  g_assert_cmpuint (g_variant_n_children (arr), ==, bad_events);
  g_variant_unref (arr);
  g_variant_unref (r);
  g_assert_false (a.done);

  /* a flood of "Cancel all" clicks cancels once (and ends everything) */
  r = bus_call (name, MENU_PATH, MENU_IFACE, "EventGroup",
                g_variant_new ("(@a(isvu))", events_array (4000, 12, "clicked")));
  g_variant_unref (r);
  WAIT_BATCH (&a, 20000);
  g_assert_cmpuint (a.ok, ==, 0);
  batch_result_clear (&a);
  g_free (name);
  test_end ();
}

/* ---- the watcher replaced while a registration is pending ---- */

/* a StatusNotifierWatcher on its own connection that holds the registration calls
 * unanswered until told */
typedef struct {
  GDBusConnection *conn;
  guint reg, own;
  gboolean got_name;
  GPtrArray *held;     /* GDBusMethodInvocation *, unanswered */
} SilentWatcher;

static void
silent_method (GDBusConnection *c G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
               const char *p G_GNUC_UNUSED, const char *i G_GNUC_UNUSED,
               const char *m G_GNUC_UNUSED, GVariant *params G_GNUC_UNUSED,
               GDBusMethodInvocation *inv, gpointer data)
{
  SilentWatcher *w = data;

  g_ptr_array_add (w->held, inv);
}

static const GDBusInterfaceVTable silent_vtable = { silent_method, watcher_prop, NULL, { 0 } };

static void
silent_got_name (GDBusConnection *c G_GNUC_UNUSED, const char *n G_GNUC_UNUSED, gpointer data)
{
  ((SilentWatcher *) data)->got_name = TRUE;
}

static void
silent_start (SilentWatcher *w)
{
  GError *err = NULL;

  memset (w, 0, sizeof *w);
  w->held = g_ptr_array_new ();
  w->conn = g_dbus_connection_new_for_address_sync (g_getenv ("DBUS_SESSION_BUS_ADDRESS"),
                                                    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                    NULL, NULL, &err);
  g_assert_no_error (err);
  if (watcher_info == NULL)
    watcher_info = g_dbus_node_info_new_for_xml (watcher_xml, NULL);
  w->reg = g_dbus_connection_register_object (w->conn, "/StatusNotifierWatcher",
                                              watcher_info->interfaces[0], &silent_vtable, w,
                                              NULL, NULL);
  g_assert_cmpuint (w->reg, !=, 0);
  w->own = g_bus_own_name_on_connection (w->conn, WATCHER_NAME,
                                         G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                                         silent_got_name, NULL, w, NULL);
  SPIN_UNTIL (w->got_name, 5000);
}

/* A goes: its connection closes (what its held calls get is an error from the bus) */
static void
silent_die (SilentWatcher *w)
{
  g_dbus_connection_close_sync (w->conn, NULL, NULL);
  for (guint i = 0; i < w->held->len; i++)
    g_object_unref (w->held->pdata[i]);
  g_ptr_array_unref (w->held);
  g_object_unref (w->conn);
}

/* A is replaced by B while the call to A is unanswered. Then A either dies
 * (die = TRUE) or answers OK late: the item must register with B either way, and
 * A's answer must not count as B's. */
static void
swap_watcher_while_registering (gboolean die)
{
  SilentWatcher A;
  BatchResult a;
  guint before_b;

  set_ffmpeg ("30");
  silent_start (&A);
  nfc_tray_start (TRUE);
  start_batch ("swap", 1, &a);
  SPIN_UNTIL (A.held->len == 1, 5000);     /* the call to A is pending */
  spin_for (200);

  /* B takes the name over */
  watcher_start_flags (G_BUS_NAME_OWNER_FLAGS_REPLACE);
  spin_for (700);
  /* the item cannot tell B while its call to A is out (one at a time) */
  g_assert_cmpuint (N_REGS (), ==, 0);
  g_assert_true (nfc_tray_is_shown ());
  before_b = N_REGS ();

  if (die)
    {
      silent_die (&A);
    }
  else
    {
      g_dbus_method_invocation_return_value (g_ptr_array_index (A.held, 0), NULL);
      g_ptr_array_remove_index (A.held, 0);
    }
  SPIN_UNTIL (N_REGS () == before_b + 1, 8000);   /* told B, once */
  spin_for (700);
  g_assert_cmpuint (N_REGS (), ==, before_b + 1);
  if (!die)
    silent_die (&A);
  nfc_cancel_all ();
  WAIT_BATCH (&a, 20000);
  batch_result_clear (&a);
  test_end ();
}

static void
test_swap_watcher_a_dies (void)
{
  swap_watcher_while_registering (TRUE);
}

static void
test_swap_watcher_a_answers (void)
{
  swap_watcher_while_registering (FALSE);
}

static void
count_signal (GDBusConnection *c G_GNUC_UNUSED, const char *s G_GNUC_UNUSED,
              const char *p G_GNUC_UNUSED, const char *i G_GNUC_UNUSED, const char *n G_GNUC_UNUSED,
              GVariant *params G_GNUC_UNUSED, gpointer data)
{
  (*(guint *) data)++;
}

int
main (int argc, char **argv)
{
  const char *orig = g_getenv ("TMPDIR");
  char *tmpl, *path, *notify;
  GError *err = NULL;
  int rc;

  g_test_init (&argc, &argv, NULL);

  if (g_getenv ("NFC_TEST_PRIVATE_BUS") == NULL || g_getenv ("DBUS_SESSION_BUS_ADDRESS") == NULL)
    {
      g_print ("skipped: run under dbus-run-session with NFC_TEST_PRIVATE_BUS=1 "
               "(never on the real session bus)\n");
      return 77;
    }

  tmpl = g_strdup_printf ("%s/nfc-tray-XXXXXX", orig && *orig ? orig : "/tmp");
  base = g_mkdtemp (tmpl);
  g_assert_nonnull (base);
  bindir = B (base, "bin");
  datadir = B (base, "data");
  config_home = B (base, "config");
  g_assert_cmpint (g_mkdir_with_parents (bindir, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (datadir, 0755), ==, 0);
  g_setenv ("TMPDIR", base, TRUE);
  g_setenv ("XDG_CONFIG_HOME", config_home, TRUE);
  path = B (base, "cache");
  g_setenv ("XDG_CACHE_HOME", path, TRUE);
  g_free (path);
  g_setenv ("NFC_TEST_TIMEOUT_SECS", "600", TRUE);

  /* the fakes first in PATH, found by nfc_tools_init: ffmpeg is rewritten by every
   * test (set_ffmpeg); no ffprobe, so no size probe of the video */
  notify = B (bindir, "notify-send");
  write_script (notify, "echo 1\n");
  g_free (notify);
  set_ffmpeg ("0");
  path = g_strconcat (bindir, ":/usr/bin:/bin", NULL);
  g_setenv ("PATH", path, TRUE);
  g_free (path);
  nfc_tools_init ();
  g_clear_pointer (&nfc_tools.ffprobe, g_free);
  g_assert_true (g_str_has_prefix (nfc_tools.ffmpeg, bindir));
  g_assert_true (g_str_has_prefix (nfc_tools.notify, bindir));

  registered = g_ptr_array_new_with_free_func (g_free);
  test_conn = g_dbus_connection_new_for_address_sync (g_getenv ("DBUS_SESSION_BUS_ADDRESS"),
                                                      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                      NULL, NULL, &err);
  g_assert_no_error (err);
  g_assert_nonnull (test_conn);
  g_dbus_connection_signal_subscribe (test_conn, NULL, SNI_IFACE, "NewToolTip", SNI_PATH, NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE, count_signal, &n_new_tooltip, NULL);
  g_dbus_connection_signal_subscribe (test_conn, NULL, MENU_IFACE, "LayoutUpdated", MENU_PATH, NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE, count_signal, &n_layout_updated,
                                      NULL);

  g_test_add_func ("/tray/register-and-cancel", test_register_and_cancel);
  g_test_add_func ("/tray/unregister-when-done", test_unregister_when_done);
  g_test_add_func ("/tray/short-job-never-shows", test_short_job_never_shows);
  g_test_add_func ("/tray/no-watcher-jobs-work", test_no_watcher_jobs_work);
  g_test_add_func ("/tray/no-watcher-then-watcher", test_no_watcher_then_watcher);
  g_test_add_func ("/tray/disabled", test_disabled);
  g_test_add_func ("/tray/menu-protocol", test_menu_protocol);
  g_test_add_func ("/tray/non-utf8-names", test_non_utf8_names);
  g_test_add_func ("/tray/menu-flood", test_menu_flood);
  g_test_add_func ("/tray/watcher-swap-a-dies", test_swap_watcher_a_dies);
  g_test_add_func ("/tray/watcher-swap-a-answers", test_swap_watcher_a_answers);
  rc = g_test_run ();

  g_dbus_connection_close_sync (test_conn, NULL, NULL);
  g_object_unref (test_conn);
  g_dbus_node_info_unref (watcher_info);
  g_ptr_array_unref (registered);
  {
    char *rm[] = { "rm", "-rf", base, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (bindir); g_free (datadir); g_free (config_home); g_free (base);
  return rc;
}
