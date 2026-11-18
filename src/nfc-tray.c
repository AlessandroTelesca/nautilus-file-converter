/* nautilus-file-converter: the tray icon, see nfc-tray.h.
 *
 * Two D-Bus objects on one well-known name, org.kde.StatusNotifierItem-<pid>-<n>:
 *   /StatusNotifierItem  org.kde.StatusNotifierItem  (the icon, its tooltip)
 *   /MenuBar             com.canonical.dbusmenu      (its menu)
 * which the StatusNotifierWatcher is told about with RegisterStatusNotifierItem.
 *
 * Whether the icon is on the bus is decided by tray_tick(), a single timer that
 * is rescheduled, never stacked: it is asked for whenever a job changes
 * (throttled to one run per THROTTLE_MS), shows the item once something has been
 * in flight for SHOW_DELAY_MS and removes it when nothing has been for
 * HIDE_DELAY_MS. Each time the item is on the bus there is a fresh Item: its own
 * bus name, objects and watcher subscription, all released together.
 */
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "nfc-convert.h"
#include "nfc-tray.h"
#include "nfc-util.h"

#define SNI_PATH        "/StatusNotifierItem"
#define SNI_IFACE       "org.kde.StatusNotifierItem"
#define MENU_PATH       "/MenuBar"
#define MENU_IFACE      "com.canonical.dbusmenu"
#define WATCHER_NAME    "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH    "/StatusNotifierWatcher"
#define WATCHER_IFACE   "org.kde.StatusNotifierWatcher"

#define TRAY_ICON       "view-refresh-symbolic"
#define TRAY_ID         "nautilus-file-converter"
#define TRAY_TITLE      "File Converter"

#define SHOW_DELAY_MS   300   /* a job that ends sooner never shows the icon */
#define HIDE_DELAY_MS   1000  /* the icon stays this long after the last job */
#define THROTTLE_MS     500   /* at most one update this often */
#define REGISTER_TIMEOUT_MS 10000
#define MAX_JOB_LINES   8
#define MAX_CALL_ITEMS  4096  /* the longest id, name or event list a menu call may bring */

/* dbusmenu item ids */
#define ID_ROOT         0
#define ID_FIRST_JOB    1  /* ..MAX_JOB_LINES */
#define ID_MORE         (ID_FIRST_JOB + MAX_JOB_LINES)
#define ID_SEPARATOR    (ID_MORE + 1)
#define ID_CANCEL_ALL   (ID_SEPARATOR + 1)

static const char sni_xml[] =
  "<node>"
  " <interface name='" SNI_IFACE "'>"
  "  <property name='Category' type='s' access='read'/>"
  "  <property name='Id' type='s' access='read'/>"
  "  <property name='Title' type='s' access='read'/>"
  "  <property name='Status' type='s' access='read'/>"
  "  <property name='WindowId' type='i' access='read'/>"
  "  <property name='IconThemePath' type='s' access='read'/>"
  "  <property name='Menu' type='o' access='read'/>"
  "  <property name='ItemIsMenu' type='b' access='read'/>"
  "  <property name='IconName' type='s' access='read'/>"
  "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
  "  <property name='OverlayIconName' type='s' access='read'/>"
  "  <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
  "  <property name='AttentionIconName' type='s' access='read'/>"
  "  <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
  "  <property name='AttentionMovieName' type='s' access='read'/>"
  "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "  <property name='IconAccessibleDesc' type='s' access='read'/>"
  "  <property name='AttentionAccessibleDesc' type='s' access='read'/>"
  "  <method name='ContextMenu'><arg type='i' name='x' direction='in'/>"
  "   <arg type='i' name='y' direction='in'/></method>"
  "  <method name='Activate'><arg type='i' name='x' direction='in'/>"
  "   <arg type='i' name='y' direction='in'/></method>"
  "  <method name='SecondaryActivate'><arg type='i' name='x' direction='in'/>"
  "   <arg type='i' name='y' direction='in'/></method>"
  "  <method name='Scroll'><arg type='i' name='delta' direction='in'/>"
  "   <arg type='s' name='orientation' direction='in'/></method>"
  "  <signal name='NewTitle'/>"
  "  <signal name='NewIcon'/>"
  "  <signal name='NewAttentionIcon'/>"
  "  <signal name='NewOverlayIcon'/>"
  "  <signal name='NewToolTip'/>"
  "  <signal name='NewStatus'><arg type='s' name='status'/></signal>"
  " </interface>"
  "</node>";

static const char menu_xml[] =
  "<node>"
  " <interface name='" MENU_IFACE "'>"
  "  <property name='Version' type='u' access='read'/>"
  "  <property name='TextDirection' type='s' access='read'/>"
  "  <property name='Status' type='s' access='read'/>"
  "  <property name='IconThemePath' type='as' access='read'/>"
  "  <method name='GetLayout'>"
  "   <arg type='i' name='parentId' direction='in'/>"
  "   <arg type='i' name='recursionDepth' direction='in'/>"
  "   <arg type='as' name='propertyNames' direction='in'/>"
  "   <arg type='u' name='revision' direction='out'/>"
  "   <arg type='(ia{sv}av)' name='layout' direction='out'/></method>"
  "  <method name='GetGroupProperties'>"
  "   <arg type='ai' name='ids' direction='in'/>"
  "   <arg type='as' name='propertyNames' direction='in'/>"
  "   <arg type='a(ia{sv})' name='properties' direction='out'/></method>"
  "  <method name='GetProperty'>"
  "   <arg type='i' name='id' direction='in'/>"
  "   <arg type='s' name='name' direction='in'/>"
  "   <arg type='v' name='value' direction='out'/></method>"
  "  <method name='Event'>"
  "   <arg type='i' name='id' direction='in'/>"
  "   <arg type='s' name='eventId' direction='in'/>"
  "   <arg type='v' name='data' direction='in'/>"
  "   <arg type='u' name='timestamp' direction='in'/></method>"
  "  <method name='EventGroup'>"
  "   <arg type='a(isvu)' name='events' direction='in'/>"
  "   <arg type='ai' name='idErrors' direction='out'/></method>"
  "  <method name='AboutToShow'>"
  "   <arg type='i' name='id' direction='in'/>"
  "   <arg type='b' name='needUpdate' direction='out'/></method>"
  "  <method name='AboutToShowGroup'>"
  "   <arg type='ai' name='ids' direction='in'/>"
  "   <arg type='ai' name='updatesNeeded' direction='out'/>"
  "   <arg type='ai' name='idErrors' direction='out'/></method>"
  "  <signal name='ItemsPropertiesUpdated'>"
  "   <arg type='a(ia{sv})' name='updatedProps'/>"
  "   <arg type='a(ias)' name='removedProps'/></signal>"
  "  <signal name='LayoutUpdated'>"
  "   <arg type='u' name='revision'/><arg type='i' name='parent'/></signal>"
  "  <signal name='ItemActivationRequested'>"
  "   <arg type='i' name='id'/><arg type='u' name='timestamp'/></signal>"
  " </interface>"
  "</node>";

/* ---- one appearance of the icon on the bus ---- */

typedef struct {
  int ref;
  gboolean dead;            /* unexported: its callbacks do nothing */
  char *bus_name;
  guint owner_id, watch_id; /* g_bus_own_name, g_bus_watch_name */
  GDBusConnection *conn;    /* from the bus_acquired callback */
  guint sni_reg, menu_reg;
  gboolean name_owned, watcher_present, registered, registering;
  guint watcher_gen;        /* bumped when the watcher or our name changes: a registration
                             * made under an older one counts for nothing */
  GCancellable *cancel;     /* the RegisterStatusNotifierItem call */
  char *tooltip;            /* the tooltip text */
  GPtrArray *lines;         /* char *: the job lines of the menu (at most MAX_JOB_LINES) */
  guint more;               /* jobs and files beyond them */
  guint revision;           /* of the menu layout */
} Item;

static struct {
  gboolean enabled;
  Item *item;               /* NULL: not on the bus */
  guint timer_id;
  gint64 timer_due;         /* monotonic ms */
  gint64 last_tick;
  gint64 active_since;      /* 0: nothing was in flight at the last tick */
  gint64 hide_at;           /* 0: no hide pending */
  guint seq;
} tray;

static GDBusNodeInfo *sni_info, *menu_info;

static gint64
now_ms (void)
{
  return g_get_monotonic_time () / 1000;
}

static Item *
item_ref (Item *it)
{
  it->ref++;
  return it;
}

static void
item_unref (gpointer data)
{
  Item *it = data;

  if (--it->ref > 0)
    return;
  g_clear_object (&it->conn);
  g_clear_object (&it->cancel);
  g_clear_pointer (&it->lines, g_ptr_array_unref);
  g_free (it->tooltip);
  g_free (it->bus_name);
  g_free (it);
}

/* ---- the menu ---- */

/* a string variant of text that may hold any bytes (a file name from an old zip): a
 * variant string must be valid UTF-8, and a bad one makes GLib complain and the item
 * malformed */
static GVariant *
string_variant (const char *text)
{
  return g_variant_new_take_string (g_utf8_make_valid (text != NULL ? text : "", -1));
}

/* a dbusmenu label: "_" introduces a mnemonic there, a file name keeps its own */
static char *
menu_label (const char *text)
{
  GString *s = g_string_new (NULL);

  for (const char *c = text; *c != '\0'; c++)
    {
      if (*c == '_')
        g_string_append_c (s, '_');
      g_string_append_c (s, *c);
    }
  return g_string_free (s, FALSE);
}

static gboolean
menu_id_valid (const Item *it, gint32 id)
{
  if (id == ID_ROOT || id == ID_SEPARATOR || id == ID_CANCEL_ALL)
    return TRUE;
  if (id == ID_MORE)
    return it->more > 0;
  return id >= ID_FIRST_JOB && id < ID_FIRST_JOB + (gint32) it->lines->len;
}

/* the ids of the items present, in order */
static GArray *
menu_ids (const Item *it)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint32));
  gint32 id;

  for (guint i = 0; i < it->lines->len; i++)
    {
      id = ID_FIRST_JOB + (gint32) i;
      g_array_append_val (ids, id);
    }
  if (it->more > 0)
    {
      id = ID_MORE;
      g_array_append_val (ids, id);
    }
  id = ID_SEPARATOR;
  g_array_append_val (ids, id);
  id = ID_CANCEL_ALL;
  g_array_append_val (ids, id);
  return ids;
}

/* a{sv} of an item */
static GVariant *
menu_props (const Item *it, gint32 id)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
  if (id == ID_ROOT)
    g_variant_builder_add (&b, "{sv}", "children-display", g_variant_new_string ("submenu"));
  else if (id == ID_SEPARATOR)
    g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("separator"));
  else
    {
      char *raw = NULL, *label;

      if (id == ID_CANCEL_ALL)
        raw = g_strdup ("Cancel all");
      else if (id == ID_MORE)
        raw = g_strdup_printf ("… and %u more", it->more);
      else
        raw = g_strdup (g_ptr_array_index (it->lines, id - ID_FIRST_JOB));
      label = menu_label (raw);
      g_variant_builder_add (&b, "{sv}", "label", string_variant (label));
      g_variant_builder_add (&b, "{sv}", "enabled", g_variant_new_boolean (id == ID_CANCEL_ALL));
      g_free (label);
      g_free (raw);
    }
  return g_variant_builder_end (&b);
}

/* (ia{sv}av) of the root; with children unless depth is 0 */
static GVariant *
menu_layout (const Item *it, gint depth)
{
  GVariantBuilder kids;
  GArray *ids = menu_ids (it);

  g_variant_builder_init (&kids, G_VARIANT_TYPE ("av"));
  for (guint i = 0; depth != 0 && i < ids->len; i++)
    {
      gint32 id = g_array_index (ids, gint32, i);

      g_variant_builder_add (&kids, "v",
                             g_variant_new ("(i@a{sv}@av)", id, menu_props (it, id),
                                            g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0)));
    }
  g_array_free (ids, TRUE);
  return g_variant_new ("(i@a{sv}@av)", ID_ROOT, menu_props (it, ID_ROOT),
                        g_variant_builder_end (&kids));
}

static void
menu_handle_event (gint32 id, const char *event)
{
  if (id == ID_CANCEL_ALL && g_str_equal (event, "clicked"))
    nfc_cancel_all ();
}

static void
menu_refresh (Item *it);

static void
menu_method_call (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                  const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                  const char *method, GVariant *params, GDBusMethodInvocation *inv,
                  gpointer data)
{
  Item *it = data;

  if (g_str_equal (method, "GetLayout"))
    {
      gint32 parent, depth;

      g_variant_get (params, "(ii@as)", &parent, &depth, NULL);
      if (!menu_id_valid (it, parent))
        {
          g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "no such menu item");
          return;
        }
      /* a parent other than the root has no children; below the root depth 0 or 1 is
       * the same here (a flat menu) */
      g_dbus_method_invocation_return_value
        (inv, g_variant_new ("(u@(ia{sv}av))", it->revision,
                             parent == ID_ROOT
                               ? menu_layout (it, depth)
                               : g_variant_new ("(i@a{sv}@av)", parent, menu_props (it, parent),
                                                g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0))));
    }
  else if (g_str_equal (method, "GetGroupProperties"))
    {
      GVariantBuilder out;
      GVariant *want = g_variant_get_child_value (params, 0);
      GVariant *names = g_variant_get_child_value (params, 1);
      gsize n = g_variant_n_children (want);

      /* the menu has a dozen items: a longer list is a client gone wrong, and every
       * entry of it would cost a reply tree */
      if (n > MAX_CALL_ITEMS || g_variant_n_children (names) > MAX_CALL_ITEMS)
        {
          g_variant_unref (want);
          g_variant_unref (names);
          g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "too many ids or property names");
          return;
        }
      g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));
      if (n == 0) /* none named: all of them */
        {
          GArray *all = menu_ids (it);
          gint32 root = ID_ROOT;

          g_variant_builder_add (&out, "(i@a{sv})", root, menu_props (it, root));
          for (guint i = 0; i < all->len; i++)
            g_variant_builder_add (&out, "(i@a{sv})", g_array_index (all, gint32, i),
                                   menu_props (it, g_array_index (all, gint32, i)));
          g_array_free (all, TRUE);
        }
      else
        {
          /* each item at most once, in the order first named: the reply never has
           * more entries than the menu has items */
          gboolean seen[ID_CANCEL_ALL + 1] = { FALSE };

          for (gsize i = 0; i < n; i++)
            {
              gint32 id;

              g_variant_get_child (want, i, "i", &id);
              if (!menu_id_valid (it, id) || seen[id])
                continue;
              seen[id] = TRUE;
              g_variant_builder_add (&out, "(i@a{sv})", id, menu_props (it, id));
            }
        }
      g_variant_unref (want);
      g_variant_unref (names);
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(a(ia{sv}))", &out));
    }
  else if (g_str_equal (method, "GetProperty"))
    {
      gint32 id;
      const char *name;
      GVariant *props, *val = NULL;

      g_variant_get (params, "(i&s)", &id, &name);
      if (menu_id_valid (it, id))
        {
          props = g_variant_ref_sink (menu_props (it, id));
          val = g_variant_lookup_value (props, name, NULL);
          g_variant_unref (props);
        }
      if (val == NULL)
        {
          g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "no such property");
          return;
        }
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(v)", val));
      g_variant_unref (val);
    }
  else if (g_str_equal (method, "Event"))
    {
      gint32 id;
      const char *event;

      g_variant_get (params, "(i&s@vu)", &id, &event, NULL, NULL);
      /* answer first: cancelling may end jobs, and so end this item */
      g_dbus_method_invocation_return_value (inv, NULL);
      menu_handle_event (id, event);
    }
  else if (g_str_equal (method, "EventGroup"))
    {
      GVariantIter iter;
      GVariantBuilder errs;
      gint32 id;
      const char *event;
      GVariant *events = g_variant_get_child_value (params, 0);
      gboolean cancel = FALSE;

      if (g_variant_n_children (events) > MAX_CALL_ITEMS)
        {
          g_variant_unref (events);
          g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "too many events");
          return;
        }
      g_variant_builder_init (&errs, G_VARIANT_TYPE ("ai"));
      g_variant_iter_init (&iter, events);
      while (g_variant_iter_next (&iter, "(i&s@vu)", &id, &event, NULL, NULL))
        {
          if (!menu_id_valid (it, id))
            g_variant_builder_add (&errs, "i", id);
          else if (id == ID_CANCEL_ALL && g_str_equal (event, "clicked"))
            cancel = TRUE; /* once, however often it was sent */
        }
      /* answer first: cancelling may end jobs, and so end this item */
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(ai)", &errs));
      g_variant_unref (events);
      if (cancel)
        nfc_cancel_all ();
    }
  else if (g_str_equal (method, "AboutToShow"))
    {
      guint before = it->revision;

      menu_refresh (it); /* what is shown when it opens is what is running now */
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(b)", it->revision != before));
    }
  else if (g_str_equal (method, "AboutToShowGroup"))
    {
      guint before = it->revision;
      GVariantBuilder upd, errs;

      menu_refresh (it);
      g_variant_builder_init (&upd, G_VARIANT_TYPE ("ai"));
      g_variant_builder_init (&errs, G_VARIANT_TYPE ("ai"));
      if (it->revision != before)
        g_variant_builder_add (&upd, "i", ID_ROOT);
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(aiai)", &upd, &errs));
    }
  else
    g_dbus_method_invocation_return_error (inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "unknown method");
}

static GVariant *
menu_get_property (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                   const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                   const char *prop, GError **error G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  if (g_str_equal (prop, "Version"))
    return g_variant_new_uint32 (3);
  if (g_str_equal (prop, "TextDirection"))
    return g_variant_new_string ("ltr");
  if (g_str_equal (prop, "Status"))
    return g_variant_new_string ("normal");
  if (g_str_equal (prop, "IconThemePath"))
    return g_variant_new_strv (NULL, 0);
  return NULL;
}

/* ---- the item ---- */

static GVariant *
empty_pixmaps (void)
{
  return g_variant_new_array (G_VARIANT_TYPE ("(iiay)"), NULL, 0);
}

static GVariant *
sni_get_property (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                  const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                  const char *prop, GError **error G_GNUC_UNUSED, gpointer data)
{
  Item *it = data;

  if (g_str_equal (prop, "Category"))
    return g_variant_new_string ("ApplicationStatus");
  if (g_str_equal (prop, "Id"))
    return g_variant_new_string (TRAY_ID);
  if (g_str_equal (prop, "Title"))
    return g_variant_new_string (TRAY_TITLE);
  if (g_str_equal (prop, "Status"))
    return g_variant_new_string ("Active");
  if (g_str_equal (prop, "WindowId"))
    return g_variant_new_int32 (0);
  if (g_str_equal (prop, "IconThemePath") || g_str_equal (prop, "OverlayIconName") ||
      g_str_equal (prop, "AttentionIconName") || g_str_equal (prop, "AttentionMovieName") ||
      g_str_equal (prop, "IconAccessibleDesc") || g_str_equal (prop, "AttentionAccessibleDesc"))
    return g_variant_new_string ("");
  if (g_str_equal (prop, "Menu"))
    return g_variant_new_object_path (MENU_PATH);
  /* left click opens the menu as well: Activate does nothing */
  if (g_str_equal (prop, "ItemIsMenu"))
    return g_variant_new_boolean (TRUE);
  if (g_str_equal (prop, "IconName"))
    return g_variant_new_string (TRAY_ICON);
  if (g_str_equal (prop, "IconPixmap") || g_str_equal (prop, "OverlayIconPixmap") ||
      g_str_equal (prop, "AttentionIconPixmap"))
    return empty_pixmaps ();
  if (g_str_equal (prop, "ToolTip"))
    return g_variant_new ("(s@a(iiay)s@s)", TRAY_ICON, empty_pixmaps (), TRAY_TITLE,
                          string_variant (it->tooltip));
  return NULL;
}

static void
sni_method_call (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                 const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                 const char *method G_GNUC_UNUSED, GVariant *params G_GNUC_UNUSED,
                 GDBusMethodInvocation *inv, gpointer data G_GNUC_UNUSED)
{
  /* ContextMenu, Activate, SecondaryActivate, Scroll: the host draws the menu
   * from the Menu property; there is nothing else to do */
  g_dbus_method_invocation_return_value (inv, NULL);
}

static const GDBusInterfaceVTable sni_vtable = { sni_method_call, sni_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable menu_vtable = { menu_method_call, menu_get_property, NULL, { 0 } };

static void
item_emit (Item *it, const char *path, const char *iface, const char *signal, GVariant *params)
{
  if (it->conn != NULL && !it->dead)
    g_dbus_connection_emit_signal (it->conn, NULL, path, iface, signal, params, NULL);
  else if (params != NULL)
    g_variant_unref (g_variant_ref_sink (params));
}

/* take the jobs in flight: tooltip and menu lines, with the signals for a change */
static gboolean
item_apply (Item *it, const NfcActivity *a)
{
  char *tooltip = g_utf8_make_valid (a != NULL && a->summary != NULL ? a->summary : "", -1);
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  guint more = 0;
  gboolean layout_changed;

  if (a != NULL && a->lines != NULL)
    {
      for (guint i = 0; i < a->lines->len && i < MAX_JOB_LINES; i++)
        g_ptr_array_add (lines, g_utf8_make_valid (g_ptr_array_index (a->lines, i), -1));
      more = a->lines->len > MAX_JOB_LINES ? a->lines->len - MAX_JOB_LINES : 0;
      more += a->n_unstarted;
    }
  layout_changed = it->lines == NULL || it->more != more || it->lines->len != lines->len;
  for (guint i = 0; !layout_changed && i < lines->len; i++)
    layout_changed = !g_str_equal (lines->pdata[i], it->lines->pdata[i]);

  if (it->tooltip == NULL || !g_str_equal (it->tooltip, tooltip))
    {
      gboolean first = it->tooltip == NULL;

      g_free (it->tooltip);
      it->tooltip = tooltip;
      if (!first)
        item_emit (it, SNI_PATH, SNI_IFACE, "NewToolTip", NULL);
    }
  else
    g_free (tooltip);
  if (layout_changed)
    {
      gboolean first = it->lines == NULL;

      g_clear_pointer (&it->lines, g_ptr_array_unref);
      it->lines = lines;
      it->more = more;
      it->revision++;
      if (!first)
        item_emit (it, MENU_PATH, MENU_IFACE, "LayoutUpdated",
                   g_variant_new ("(ui)", it->revision, ID_ROOT));
    }
  else
    g_ptr_array_unref (lines);
  return layout_changed;
}

static void
menu_refresh (Item *it)
{
  NfcActivity a;

  if (nfc_activity_get (&a))
    item_apply (it, &a);
  else
    item_apply (it, NULL);
  nfc_activity_clear (&a);
}

/* ---- the bus ---- */

/* a RegisterStatusNotifierItem call, and the watcher it was meant for */
typedef struct {
  Item *it;
  guint gen;                /* it->watcher_gen when the call was made */
} Registration;

static void item_maybe_register (Item *it);

static void
register_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Registration *reg = data;
  Item *it = reg->it;
  GError *error = NULL;
  GVariant *v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  gboolean current = reg->gen == it->watcher_gen;

  it->registering = FALSE;
  if (v != NULL)
    {
      g_variant_unref (v);
      /* an answer from a watcher that has since been replaced says nothing about
       * the one there now */
      if (current)
        it->registered = TRUE;
    }
  else if (!it->dead && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    nfc_log ("tray: the StatusNotifierWatcher refused the icon: %s", error->message);
  g_clear_error (&error);
  /* the watcher changed while this call was out (watcher_appeared could not start a
   * call of its own then): tell the one that is there now */
  if (!current)
    {
      it->registered = FALSE;
      item_maybe_register (it);
    }
  g_free (reg);
  item_unref (it);
}

/* the name is ours and there is a watcher that does not know us yet */
static void
item_maybe_register (Item *it)
{
  Registration *reg;

  if (it->dead || it->conn == NULL || !it->name_owned || !it->watcher_present ||
      it->registered || it->registering)
    return;
  reg = g_new0 (Registration, 1);
  reg->it = item_ref (it);
  reg->gen = it->watcher_gen;
  it->registering = TRUE;
  g_dbus_connection_call (it->conn, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE,
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", it->bus_name), NULL,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, REGISTER_TIMEOUT_MS, it->cancel,
                          register_done, reg);
}

static void
bus_acquired (GDBusConnection *conn, const char *name G_GNUC_UNUSED, gpointer data)
{
  Item *it = data;

  if (it->dead || it->conn != NULL)
    return;
  it->conn = g_object_ref (conn);
  it->sni_reg = g_dbus_connection_register_object (conn, SNI_PATH, sni_info->interfaces[0],
                                                   &sni_vtable, item_ref (it), item_unref, NULL);
  if (it->sni_reg == 0)
    item_unref (it);
  it->menu_reg = g_dbus_connection_register_object (conn, MENU_PATH, menu_info->interfaces[0],
                                                    &menu_vtable, item_ref (it), item_unref, NULL);
  if (it->menu_reg == 0)
    item_unref (it);
}

static void
name_acquired (GDBusConnection *conn G_GNUC_UNUSED, const char *name G_GNUC_UNUSED, gpointer data)
{
  Item *it = data;

  it->name_owned = TRUE;
  item_maybe_register (it);
}

static void
name_lost (GDBusConnection *conn G_GNUC_UNUSED, const char *name G_GNUC_UNUSED, gpointer data)
{
  Item *it = data;

  it->name_owned = FALSE;
  it->registered = FALSE;
  it->watcher_gen++;
}

static void
watcher_appeared (GDBusConnection *conn G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
                  const char *owner G_GNUC_UNUSED, gpointer data)
{
  Item *it = data;

  it->watcher_present = TRUE;
  it->registered = FALSE; /* a new watcher knows nothing */
  it->watcher_gen++;
  item_maybe_register (it);
}

static void
watcher_vanished (GDBusConnection *conn G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
                  gpointer data)
{
  Item *it = data;

  it->watcher_present = FALSE;
  it->registered = FALSE;
  it->watcher_gen++;
}

static void
tray_export (void)
{
  Item *it = g_new0 (Item, 1);
  NfcActivity a;

  if (sni_info == NULL)
    {
      sni_info = g_dbus_node_info_new_for_xml (sni_xml, NULL);
      menu_info = g_dbus_node_info_new_for_xml (menu_xml, NULL);
      g_assert (sni_info != NULL && menu_info != NULL);
    }
  it->ref = 1; /* tray.item */
  it->cancel = g_cancellable_new ();
  it->bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-%u", (int) getpid (), ++tray.seq);
  if (nfc_activity_get (&a))
    item_apply (it, &a);
  else
    item_apply (it, NULL);
  nfc_activity_clear (&a);
  tray.item = it;
  it->owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, it->bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 bus_acquired, name_acquired, name_lost, item_ref (it),
                                 item_unref);
  it->watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                   watcher_appeared, watcher_vanished, item_ref (it), item_unref);
}

static void
tray_unexport (void)
{
  Item *it = tray.item;

  if (it == NULL)
    return;
  tray.item = NULL;
  it->dead = TRUE;
  g_cancellable_cancel (it->cancel);
  if (it->conn != NULL)
    {
      if (it->sni_reg != 0)
        g_dbus_connection_unregister_object (it->conn, it->sni_reg);
      if (it->menu_reg != 0)
        g_dbus_connection_unregister_object (it->conn, it->menu_reg);
      it->sni_reg = it->menu_reg = 0;
    }
  g_bus_unown_name (it->owner_id); /* the name goes: the watcher drops the icon */
  g_bus_unwatch_name (it->watch_id);
  item_unref (it);
}

/* ---- when ---- */

static gboolean tray_tick (gpointer data);

/* run tray_tick at due (monotonic ms) at the latest */
static void
tray_schedule (gint64 due)
{
  gint64 now = now_ms ();

  if (tray.timer_id != 0)
    {
      if (tray.timer_due <= due)
        return;
      g_source_remove (tray.timer_id);
    }
  tray.timer_due = due;
  tray.timer_id = g_timeout_add (due > now ? (guint) (due - now) : 0, tray_tick, NULL);
}

static gboolean
tray_tick (gpointer data G_GNUC_UNUSED)
{
  NfcActivity a;
  gint64 now = now_ms ();
  gboolean active;

  tray.timer_id = 0;
  tray.last_tick = now;
  if (!tray.enabled)
    return G_SOURCE_REMOVE;
  active = nfc_activity_get (&a);
  if (active)
    {
      tray.hide_at = 0;
      if (tray.item == NULL)
        {
          if (tray.active_since == 0)
            tray.active_since = now;
          if (now - tray.active_since < SHOW_DELAY_MS)
            tray_schedule (tray.active_since + SHOW_DELAY_MS);
          else
            tray_export ();
        }
      else
        item_apply (tray.item, &a);
    }
  else
    {
      tray.active_since = 0;
      if (tray.item != NULL)
        {
          if (tray.hide_at == 0)
            tray.hide_at = now + HIDE_DELAY_MS;
          if (now < tray.hide_at)
            tray_schedule (tray.hide_at);
          else
            {
              tray.hide_at = 0;
              tray_unexport ();
            }
        }
    }
  nfc_activity_clear (&a);
  return G_SOURCE_REMOVE;
}

static void
tray_jobs_changed (gpointer data G_GNUC_UNUSED)
{
  gint64 now = now_ms ();

  if (!tray.enabled)
    return;
  tray_schedule (MAX (now, tray.last_tick + THROTTLE_MS));
}

/* ---- API ---- */

gboolean
nfc_tray_config_enabled (const char *text)
{
  gboolean enabled = TRUE;
  char **lines;

  if (text == NULL)
    return TRUE;
  lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l != NULL; l++)
    {
      char *s = g_strstrip (*l);
      char *eq;

      if (*s == '\0' || *s == '#' || (eq = strchr (s, '=')) == NULL)
        continue;
      *eq = '\0';
      if (g_str_equal (g_strstrip (s), "tray"))
        {
          const char *v = g_strstrip (eq + 1);

          enabled = !(g_ascii_strcasecmp (v, "false") == 0 || g_ascii_strcasecmp (v, "no") == 0 ||
                      g_ascii_strcasecmp (v, "off") == 0 || g_str_equal (v, "0"));
        }
    }
  g_strfreev (lines);
  return enabled;
}

void
nfc_tray_start (gboolean enabled)
{
  nfc_tray_shutdown ();
  if (!enabled)
    return;
  tray.enabled = TRUE;
  tray.last_tick = 0;
  nfc_activity_set_observer (tray_jobs_changed, NULL);
  tray_jobs_changed (NULL); /* jobs already in flight */
}

void
nfc_tray_init (void)
{
  char *path = g_build_filename (g_get_user_config_dir (), "nautilus-file-converter", "config",
                                 NULL);
  char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    text = NULL;
  nfc_tray_start (nfc_tray_config_enabled (text));
  g_free (text);
  g_free (path);
}

void
nfc_tray_shutdown (void)
{
  if (tray.enabled)
    nfc_activity_set_observer (NULL, NULL);
  tray.enabled = FALSE;
  if (tray.timer_id != 0)
    {
      g_source_remove (tray.timer_id);
      tray.timer_id = 0;
    }
  tray.active_since = tray.hide_at = 0;
  tray_unexport ();
}

gboolean
nfc_tray_is_shown (void)
{
  return tray.item != NULL;
}
