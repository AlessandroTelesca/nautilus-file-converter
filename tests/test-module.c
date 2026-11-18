/* Load test + menu-logic test.
 *
 * Loads the built libnautilus-file-converter.so with GModule (without
 * installing it), checks the three Nautilus entry points, registers the type
 * through a minimal GTypeModule, and drives the real menu provider with fake
 * NautilusFileInfo objects.
 */
#include <gmodule.h>
#include <glib/gstdio.h>
#include <nautilus-extension.h>
#include <string.h>

/* LeakSanitizer: libnautilus-extension's NautilusMenu keeps a reference to every
 * item appended to it and never releases it (even when the menu itself is
 * finalized), so every menu item an extension builds is leaked by the library,
 * whoever frees the lists it hands out. Items that have a signal handler stay
 * reachable through GLib's handler table and are not reported; the target items
 * with a preset submenu (no handler) would be. They are the library's leak, not
 * ours: the extension unrefs everything it creates. */
const char *__lsan_default_suppressions (void);
const char *
__lsan_default_suppressions (void)
{
  return "leak:nautilus_menu_item_new\n";
}


/* ---- minimal GTypeModule ---- */
typedef struct { GTypeModule parent; } TestModule;
typedef struct { GTypeModuleClass parent; } TestModuleClass;
G_DEFINE_TYPE (TestModule, test_module, G_TYPE_TYPE_MODULE)
static gboolean test_module_load (GTypeModule *m) { (void) m; return TRUE; }
static void test_module_unload (GTypeModule *m) { (void) m; }
static void test_module_init (TestModule *m) { (void) m; }
static void
test_module_class_init (TestModuleClass *k)
{
  GTypeModuleClass *c = G_TYPE_MODULE_CLASS (k);

  c->load = test_module_load;
  c->unload = test_module_unload;
}

/* ---- fake NautilusFileInfo ---- */
typedef struct { GObject parent; char *uri, *mime; gboolean is_dir; } FakeInfo;
typedef struct { GObjectClass parent; } FakeInfoClass;
static void fake_info_iface_init (NautilusFileInfoInterface *iface);
G_DEFINE_TYPE_WITH_CODE (FakeInfo, fake_info, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (NAUTILUS_TYPE_FILE_INFO, fake_info_iface_init))
static char *fi_uri (NautilusFileInfo *f) { return g_strdup (((FakeInfo *) f)->uri); }
static char *fi_mime (NautilusFileInfo *f) { return g_strdup (((FakeInfo *) f)->mime); }
static gboolean fi_dir (NautilusFileInfo *f) { return ((FakeInfo *) f)->is_dir; }
static void
fake_info_iface_init (NautilusFileInfoInterface *iface)
{
  iface->get_uri = fi_uri;
  iface->get_mime_type = fi_mime;
  iface->is_directory = fi_dir;
}
static void fake_info_init (FakeInfo *f) { (void) f; }
static void
fake_info_finalize (GObject *o)
{
  FakeInfo *f = (FakeInfo *) o;

  g_free (f->uri);
  g_free (f->mime);
  G_OBJECT_CLASS (fake_info_parent_class)->finalize (o);
}
static void
fake_info_class_init (FakeInfoClass *k)
{
  G_OBJECT_CLASS (k)->finalize = fake_info_finalize;
}

static FakeInfo *
fake_new (const char *uri, const char *mime, gboolean dir)
{
  FakeInfo *f = g_object_new (fake_info_get_type (), NULL);

  f->uri = g_strdup (uri);
  f->mime = g_strdup (mime);
  f->is_dir = dir;
  return f;
}

/* ---- helpers ---- */
static const char *module_path;
static GObject *provider;
static char *work;

/* Frees a list from nautilus_menu_get_items the way Nautilus does: a menu does not
 * release its items itself, whoever takes them releases them, and the items of
 * nested menus first. */
static void
free_items_deep (GList *items)
{
  for (GList *l = items; l; l = l->next)
    {
      NautilusMenu *sub = NULL;

      g_object_get (l->data, "menu", &sub, NULL);
      if (sub != NULL)
        {
          free_items_deep (nautilus_menu_get_items (sub));
          g_object_unref (sub);
        }
    }
  nautilus_menu_item_list_free (items);
}

/* returns the submenu labels joined by ',' or NULL when there is no menu */
static void
write_script (const char *path, const char *body)
{
  char *content = g_strconcat ("#!/bin/sh\n", body, NULL);

  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  g_assert_cmpint (g_chmod (path, 0755), ==, 0);
  g_free (content);
}

static char *
menu_labels (GList *infos)
{
  GList *items = nautilus_menu_provider_get_file_items (NAUTILUS_MENU_PROVIDER (provider), infos);
  NautilusMenu *sub = NULL;
  GString *s = g_string_new (NULL);
  char *label = NULL;

  if (items == NULL)
    {
      g_string_free (s, TRUE);
      return NULL;
    }
  g_assert_cmpuint (g_list_length (items), ==, 1);
  g_object_get (items->data, "label", &label, "menu", &sub, NULL);
  g_assert_cmpstr (label, ==, "Convert to");
  g_assert_nonnull (sub);
  {
    /* transfer full: the list and its items belong to the caller */
    GList *sub_items = nautilus_menu_get_items (sub);

    for (GList *l = sub_items; l; l = l->next)
      {
        char *lab = NULL;

        g_object_get (l->data, "label", &lab, NULL);
        if (s->len)
          g_string_append_c (s, ',');
        g_string_append (s, lab);
        g_free (lab);
      }
    free_items_deep (sub_items);
  }
  g_free (label);
  g_object_unref (sub);
  nautilus_menu_item_list_free (items);
  return g_string_free (s, FALSE);
}

/* The capability probes run in background threads and the menu never waits for
 * them (that is the point), so right after initialize() an image/audio/video
 * selection may get no menu yet. For positive expectations wait up to 5 s. */
static char *
menu_labels_wait (GList *infos)
{
  char *labels = menu_labels (infos);

  for (int i = 0; i < 250 && labels == NULL; i++)
    {
      g_usleep (20000);
      labels = menu_labels (infos);
    }
  return labels;
}

static char *
path_uri (const char *name)
{
  char *p = g_build_filename (work, name, NULL);
  char *u = g_filename_to_uri (p, NULL, NULL);

  g_free (p);
  return u;
}

static void
touch (const char *name)
{
  char *p = g_build_filename (work, name, NULL);

  g_file_set_contents (p, "x", -1, NULL);
  g_free (p);
}

static gboolean
has_label (const char *labels, const char *l)
{
  char **v = g_strsplit (labels, ",", -1);
  gboolean r = g_strv_contains ((const char *const *) v, l);

  g_strfreev (v);
  return r;
}


/* The items of the "Convert to" submenu (release with free_items_deep),
 * or NULL when there is no menu. */
static GList *
submenu_items (GList *infos)
{
  GList *items = nautilus_menu_provider_get_file_items (NAUTILUS_MENU_PROVIDER (provider), infos);
  NautilusMenu *sub = NULL;
  GList *sub_items;

  if (items == NULL)
    return NULL;
  g_assert_cmpuint (g_list_length (items), ==, 1);
  g_object_get (items->data, "menu", &sub, NULL);
  g_assert_nonnull (sub);
  sub_items = nautilus_menu_get_items (sub);
  g_object_unref (sub);
  nautilus_menu_item_list_free (items);
  return sub_items;
}

static GList *
submenu_items_wait (GList *infos)
{
  GList *items = submenu_items (infos);

  for (int i = 0; i < 250 && items == NULL; i++)
    {
      g_usleep (20000);
      items = submenu_items (infos);
    }
  return items;
}

static char *
item_prop (gpointer item, const char *prop)
{
  char *v = NULL;

  g_object_get (item, prop, &v, NULL);
  return v;
}

/* the item of this name, or NULL */
static NautilusMenuItem *
find_item (GList *items, const char *name)
{
  for (GList *l = items; l; l = l->next)
    {
      char *n = item_prop (l->data, "name");
      gboolean hit = g_strcmp0 (n, name) == 0;

      g_free (n);
      if (hit)
        return l->data;
    }
  return NULL;
}

/* The item has a submenu with exactly Best, High, Balanced, Small, named
 * "<name>::best" ... and with a tooltip each. */
static void
assert_preset_submenu (NautilusMenuItem *item, const char *name)
{
  static const char *const keys[] = { "best", "high", "balanced", "small" };
  static const char *const labels[] = { "Best", "High", "Balanced", "Small" };
  NautilusMenu *sub = NULL;
  GList *sub_items;
  guint i = 0;

  g_assert_nonnull (item);
  g_object_get (item, "menu", &sub, NULL);
  g_assert_nonnull (sub);
  sub_items = nautilus_menu_get_items (sub);
  g_assert_cmpuint (g_list_length (sub_items), ==, 4);
  for (GList *l = sub_items; l; l = l->next, i++)
    {
      char *n = item_prop (l->data, "name"), *lab = item_prop (l->data, "label");
      char *tip = item_prop (l->data, "tip"), *want = g_strconcat (name, "::", keys[i], NULL);
      NautilusMenu *none = NULL;

      g_assert_cmpstr (n, ==, want);
      g_assert_cmpstr (lab, ==, labels[i]);
      g_assert_nonnull (tip);
      g_assert_true (g_str_has_prefix (tip, labels[i]));
      g_object_get (l->data, "menu", &none, NULL);
      g_assert_null (none); /* the presets are leaves */
      g_free (n); g_free (lab); g_free (tip); g_free (want);
    }
  nautilus_menu_item_list_free (sub_items); /* the copy: the menu keeps its own items */
  g_object_unref (sub);
}

static gboolean
item_is_leaf (NautilusMenuItem *item)
{
  NautilusMenu *sub = NULL;

  g_assert_nonnull (item);
  g_object_get (item, "menu", &sub, NULL);
  if (sub != NULL)
    g_object_unref (sub);
  return sub == NULL;
}

/* Quality presets in the real menu: JPG, WEBP, AVIF, MP4, WebM and GIF are
 * submenus Best/High/Balanced/Small; PNG, BMP, MP3, MKV... are plain items. A
 * multi-file selection gets the same menu. What is offered depends on the tools
 * installed (the probes), so each target is checked when it is there. */
static void
test_preset_menus (void)
{
  GList *items, *l;
  char *u1, *u2;
  NautilusMenuItem *it;

  touch ("p1.png"); touch ("p2.png"); touch ("p3.jpg"); touch ("p4.wav"); touch ("p5.mkv");
  touch ("p6.mp4");

  /* single PNG: JPG (and WEBP, AVIF when offered) have presets; BMP/TIFF/PDF/ICO none */
  u1 = path_uri ("p1.png");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  items = submenu_items_wait (l);
  g_assert_nonnull (items);
  assert_preset_submenu (find_item (items, "FileConverter::image::jpg"),
                         "FileConverter::image::jpg");
  if ((it = find_item (items, "FileConverter::image::webp")) != NULL)
    assert_preset_submenu (it, "FileConverter::image::webp");
  if ((it = find_item (items, "FileConverter::image::avif")) != NULL)
    assert_preset_submenu (it, "FileConverter::image::avif");
  for (const char *const *k = (const char *const[]) { "bmp", "tiff", "pdf", "ico", NULL }; *k; k++)
    {
      char *name = g_strconcat ("FileConverter::image::", *k, NULL);

      if ((it = find_item (items, name)) != NULL)
        g_assert_true (item_is_leaf (it));
      g_free (name);
    }
  g_assert_null (find_item (items, "FileConverter::image::png")); /* own format */
  free_items_deep (items);

  /* two PNGs: the same menu */
  u2 = path_uri ("p2.png");
  l = g_list_append (l, fake_new (u2, "image/png", FALSE));
  items = submenu_items_wait (l);
  g_assert_nonnull (items);
  assert_preset_submenu (find_item (items, "FileConverter::image::jpg"),
                         "FileConverter::image::jpg");
  free_items_deep (items);
  g_list_free_full (l, g_object_unref);
  g_free (u1);
  g_free (u2);

  /* a JPEG: PNG and BMP are plain items, no JPG at all */
  u1 = path_uri ("p3.jpg");
  l = g_list_append (NULL, fake_new (u1, "image/jpeg", FALSE));
  items = submenu_items_wait (l);
  g_assert_nonnull (items);
  g_assert_null (find_item (items, "FileConverter::image::jpg"));
  g_assert_true (item_is_leaf (find_item (items, "FileConverter::image::png")));
  g_assert_true (item_is_leaf (find_item (items, "FileConverter::image::bmp")));
  assert_preset_submenu (find_item (items, "FileConverter::image::webp"),
                         "FileConverter::image::webp");
  free_items_deep (items);
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* audio: nothing has presets */
  u1 = path_uri ("p4.wav");
  l = g_list_append (NULL, fake_new (u1, "audio/x-wav", FALSE));
  items = submenu_items_wait (l);
  if (items != NULL)
    {
      for (GList *i = items; i; i = i->next)
        g_assert_true (item_is_leaf (i->data));
      if ((it = find_item (items, "FileConverter::audio::mp3")) != NULL)
        g_assert_true (item_is_leaf (it));
      free_items_deep (items);
    }
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* video: MP4, WebM, GIF have presets, MKV and the audio-only targets do not */
  u1 = path_uri ("p5.mkv");
  l = g_list_append (NULL, fake_new (u1, "video/x-matroska", FALSE));
  items = submenu_items_wait (l);
  if (items != NULL)
    {
      if ((it = find_item (items, "FileConverter::video::mp4")) != NULL)
        assert_preset_submenu (it, "FileConverter::video::mp4");
      if ((it = find_item (items, "FileConverter::video::webm")) != NULL)
        assert_preset_submenu (it, "FileConverter::video::webm");
      if ((it = find_item (items, "FileConverter::video::gif")) != NULL)
        assert_preset_submenu (it, "FileConverter::video::gif");
      if ((it = find_item (items, "FileConverter::video::mp3")) != NULL)
        g_assert_true (item_is_leaf (it));
      g_assert_null (find_item (items, "FileConverter::video::mkv")); /* own format */
      free_items_deep (items);
    }
  g_list_free_full (l, g_object_unref);
  g_free (u1);
  u1 = path_uri ("p6.mp4");
  l = g_list_append (NULL, fake_new (u1, "video/mp4", FALSE));
  items = submenu_items_wait (l);
  if (items != NULL)
    {
      if ((it = find_item (items, "FileConverter::video::mkv")) != NULL)
        g_assert_true (item_is_leaf (it));
      free_items_deep (items);
    }
  g_list_free_full (l, g_object_unref);
  g_free (u1);
}

static void
test_entry_points (void)
{
  GModule *m = g_module_open (module_path, G_MODULE_BIND_LOCAL);
  gpointer init, shut, list;

  g_assert_nonnull (m);
  g_assert_true (g_module_symbol (m, "nautilus_module_initialize", &init));
  g_assert_true (g_module_symbol (m, "nautilus_module_shutdown", &shut));
  g_assert_true (g_module_symbol (m, "nautilus_module_list_types", &list));
  g_assert_nonnull (init);
  g_assert_nonnull (shut);
  g_assert_nonnull (list);
  /* internal helpers must not leak out of the module */
  g_assert_true (!g_module_symbol (m, "nfc_classify", &init));
}

static void
test_register_and_menu (void)
{
  GModule *m = g_module_open (module_path, G_MODULE_BIND_LOCAL);
  void (*initialize) (GTypeModule *);
  void (*list_types) (const GType **, int *);
  void (*shutdown) (void);
  GTypeModule *tm = g_object_new (test_module_get_type (), NULL);
  const GType *types = NULL;
  int n = 0;
  gboolean ok = FALSE;
  char *labels, *u1, *u2;
  GList *l;
  FakeInfo *f1, *f2;

  g_assert_nonnull (m);
  g_assert_true (g_module_symbol (m, "nautilus_module_initialize", (gpointer *) &initialize));
  g_assert_true (g_module_symbol (m, "nautilus_module_list_types", (gpointer *) &list_types));
  g_assert_true (g_module_symbol (m, "nautilus_module_shutdown", (gpointer *) &shutdown));
  g_type_module_use (tm);
  initialize (tm);
  /* the probes finish within milliseconds; give all three time so the menus
   * printed below are complete (the assertions do not depend on this) */
  g_usleep (500000);
  list_types (&types, &n);
  g_assert_cmpint (n, ==, 1);
  g_assert_cmpuint (types[0], !=, 0);
  g_assert_true (g_type_is_a (types[0], NAUTILUS_TYPE_MENU_PROVIDER));
  g_print ("module type: %s implements NautilusMenuProvider: yes\n", g_type_name (types[0]));
  provider = g_object_new (types[0], NULL);
  g_assert_true (NAUTILUS_IS_MENU_PROVIDER (provider));
  (void) ok;

  touch ("a.png"); touch ("b.jpg"); touch ("c.wav"); touch ("d.xyz"); touch ("noext");
  touch ("e.odt"); touch ("f.mp4");
  {
    char *adir = g_build_filename (work, "adir.png", NULL);

    g_mkdir (adir, 0755);
    g_free (adir);
  }

  /* single image: no own format, other image targets present */
  u1 = path_uri ("a.png");
  f1 = fake_new (u1, "image/png", FALSE);
  l = g_list_append (NULL, f1);
  labels = menu_labels_wait (l);
  g_assert_nonnull (labels);
  g_print ("single png    -> %s\n", labels);
  g_assert_false (has_label (labels, "PNG"));
  g_assert_true (has_label (labels, "JPG"));
  g_assert_true (has_label (labels, "PDF"));
  g_free (labels);

  /* two images of the same category: neither own format is offered */
  u2 = path_uri ("b.jpg");
  f2 = fake_new (u2, "image/jpeg", FALSE);
  l = g_list_append (l, f2);
  labels = menu_labels_wait (l);
  g_assert_nonnull (labels);
  g_print ("png + jpg     -> %s\n", labels);
  g_assert_false (has_label (labels, "PNG"));
  g_assert_false (has_label (labels, "JPG"));
  g_assert_true (has_label (labels, "WEBP"));
  g_free (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u1);
  g_free (u2);

  /* mixed categories: no menu */
  u1 = path_uri ("a.png"); u2 = path_uri ("c.wav");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  l = g_list_append (l, fake_new (u2, "audio/x-wav", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1); g_free (u2);

  /* directory (even one named like an image) */
  u1 = path_uri ("adir.png");
  l = g_list_append (NULL, fake_new (u1, "inode/directory", TRUE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* a file selection that includes a directory */
  u1 = path_uri ("a.png"); u2 = path_uri ("adir.png");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  l = g_list_append (l, fake_new (u2, "inode/directory", TRUE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1); g_free (u2);

  /* non-file URIs */
  l = g_list_append (NULL, fake_new ("smb://host/share/a.png", "image/png", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  l = g_list_append (NULL, fake_new ("trash:///a.png", "image/png", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  l = g_list_append (NULL, fake_new ("sftp://host/a.png", "image/png", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);

  /* unknown type */
  u1 = path_uri ("d.xyz");
  l = g_list_append (NULL, fake_new (u1, "application/octet-stream", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* file that does not exist */
  u1 = path_uri ("missing.png");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* no extension: the mime type decides */
  u1 = path_uri ("noext");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  labels = menu_labels_wait (l);
  g_assert_nonnull (labels);
  g_assert_true (has_label (labels, "JPG"));
  g_free (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* documents and video get their own targets */
  u1 = path_uri ("e.odt");
  l = g_list_append (NULL, fake_new (u1, "application/vnd.oasis.opendocument.text", FALSE));
  labels = menu_labels (l);
  g_print ("single odt    -> %s\n", labels ? labels : "(none)");
  if (labels != NULL)
    {
      g_assert_false (has_label (labels, "ODT"));
      g_assert_true (has_label (labels, "PDF"));
    }
  g_free (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u1);
  u1 = path_uri ("f.mp4");
  l = g_list_append (NULL, fake_new (u1, "video/mp4", FALSE));
  labels = menu_labels (l);
  g_print ("single mp4    -> %s\n", labels ? labels : "(none)");
  if (labels != NULL)
    g_assert_false (has_label (labels, "MP4 (H.264)"));
  g_free (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u1);

  /* empty selection and an invalid element */
  g_assert_null (menu_labels (NULL));
  l = g_list_append (NULL, g_object_new (G_TYPE_OBJECT, NULL));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  /* [valid, invalid, valid]: no menu, and the entries after the invalid one are
   * freed too (a leak shows in the LeakSanitizer build) */
  u1 = path_uri ("a.png"); u2 = path_uri ("b.png");
  l = g_list_append (NULL, fake_new (u1, "image/png", FALSE));
  l = g_list_append (l, g_object_new (G_TYPE_OBJECT, NULL));
  l = g_list_append (l, fake_new (u2, "image/png", FALSE));
  g_assert_null (menu_labels (l));
  g_list_free_full (l, g_object_unref);
  g_free (u1); g_free (u2);

  test_preset_menus ();

  /* no background items */
  {
    FakeInfo *folder = fake_new ("file:///tmp", "inode/directory", TRUE);

    g_assert_null (nautilus_menu_provider_get_background_items (NAUTILUS_MENU_PROVIDER (provider),
                                                                NAUTILUS_FILE_INFO (folder)));
    g_object_unref (folder);
  }

  g_object_unref (provider);
  shutdown ();
}

/* QC finding 1, end to end through the real menu provider: with a fake ffmpeg
 * that hangs for 20 s first in PATH, right-click on a plain .txt used to take
 * 20 s (and a truly hung tool froze Nautilus for good). Runs in its own process
 * (mode "freeze") because the probe state is once-per-process. */
static void
test_freeze (void)
{
  GModule *m = g_module_open (module_path, G_MODULE_BIND_LOCAL);
  void (*initialize) (GTypeModule *);
  void (*list_types) (const GType **, int *);
  void (*shutdown) (void);
  GTypeModule *tm = g_object_new (test_module_get_type (), NULL);
  const GType *types = NULL;
  int n = 0;
  char *u, *labels;
  GList *l;
  gint64 t0, us;
  gboolean got_jpg = FALSE;

  g_assert_nonnull (m);
  g_assert_true (g_module_symbol (m, "nautilus_module_initialize", (gpointer *) &initialize));
  g_assert_true (g_module_symbol (m, "nautilus_module_list_types", (gpointer *) &list_types));
  g_assert_true (g_module_symbol (m, "nautilus_module_shutdown", (gpointer *) &shutdown));
  g_type_module_use (tm);
  t0 = g_get_monotonic_time ();
  initialize (tm);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 100000);
  list_types (&types, &n);
  provider = g_object_new (types[0], NULL);

  touch ("a.txt"); touch ("b.png"); touch ("c.mp4");

  /* plain text file: the very first right-click, probes still running */
  u = path_uri ("a.txt");
  l = g_list_append (NULL, fake_new (u, "text/plain", FALSE));
  t0 = g_get_monotonic_time ();
  labels = menu_labels (l);
  us = g_get_monotonic_time () - t0;
  g_print ("txt right-click with a hung ffmpeg: %" G_GINT64_FORMAT " us -> %s\n", us,
           labels ? labels : "(no menu)");
  g_assert_cmpint (us, <, 100000);
  g_assert_nonnull (labels);
  g_assert_true (has_label (labels, "PDF"));
  g_free (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u);

  /* images: quick, and the menu appears as soon as magick has been probed,
   * although ffmpeg is still hanging (only AVIF needs ffmpeg) */
  u = path_uri ("b.png");
  l = g_list_append (NULL, fake_new (u, "image/png", FALSE));
  for (int i = 0; i < 300 && !got_jpg; i++)
    {
      t0 = g_get_monotonic_time ();
      labels = menu_labels (l);
      us = g_get_monotonic_time () - t0;
      g_assert_cmpint (us, <, 100000);
      if (labels != NULL)
        {
          got_jpg = has_label (labels, "JPG");
          g_assert_false (has_label (labels, "AVIF"));
          g_free (labels);
        }
      if (!got_jpg)
        g_usleep (10000);
    }
  g_assert_true (got_jpg);
  g_list_free_full (l, g_object_unref);
  g_free (u);

  /* video needs ffmpeg: no menu, immediately */
  u = path_uri ("c.mp4");
  l = g_list_append (NULL, fake_new (u, "video/mp4", FALSE));
  t0 = g_get_monotonic_time ();
  labels = menu_labels (l);
  us = g_get_monotonic_time () - t0;
  g_assert_cmpint (us, <, 100000);
  g_assert_null (labels);
  g_list_free_full (l, g_object_unref);
  g_free (u);

  g_object_unref (provider);
  shutdown ();
}

int
main (int argc, char **argv)
{
  int rc;

  g_test_init (&argc, &argv, NULL);
  if (argc < 2)
    g_error ("usage: test-module <path to libnautilus-file-converter.so>");
  module_path = argv[1];
  work = g_dir_make_tmp ("nfc-menu-XXXXXX", NULL);
  g_assert_nonnull (work);
  if (argc > 2 && g_str_equal (argv[2], "freeze"))
    {
      /* fakes first in PATH, before the module looks the tools up */
      char *bin = g_build_filename (work, "bin", NULL), *p, *path;

      g_mkdir (bin, 0755);
      p = g_build_filename (bin, "ffmpeg", NULL);
      write_script (p, "exec sleep 20\n");
      g_free (p);
      p = g_build_filename (bin, "magick", NULL);
      write_script (p, "echo '      PNG* PNG       rw-   fake'\necho '     JPEG* JPEG      rw-   fake'\n");
      g_free (p);
      p = g_build_filename (bin, "soffice", NULL);
      write_script (p, "exit 0\n");
      g_free (p);
      path = g_strconcat (bin, ":", g_getenv ("PATH"), NULL);
      g_setenv ("PATH", path, TRUE);
      g_free (path);
      g_free (bin);
      g_test_add_func ("/module/probe-freeze", test_freeze);
    }
  else
    {
      g_test_add_func ("/module/entry-points", test_entry_points);
      g_test_add_func ("/module/register-and-menu", test_register_and_menu);
    }
  rc = g_test_run ();
  {
    char *rm[] = { "rm", "-rf", work, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (work);
  return rc;
}
