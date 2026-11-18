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

static void
free_top (GList *items)
{
  if (items != NULL) /* the library asserts on an empty list */
    nautilus_menu_item_list_free (items);
}

/* The top-level items of the selection ("Convert to", "Compress", "Resize", "Upscale"): never
 * any other, each at most once, in that order ("Convert to" first). */
static GList *
top_items (GList *infos)
{
  GList *items = nautilus_menu_provider_get_file_items (NAUTILUS_MENU_PROVIDER (provider), infos);
  guint n_convert = 0, n_compress = 0, n_resize = 0, n_upscale = 0, pos = 0;

  for (GList *l = items; l; l = l->next, pos++)
    {
      char *label = NULL;

      g_object_get (l->data, "label", &label, NULL);
      if (g_strcmp0 (label, "Convert to") == 0)
        {
          n_convert++;
          g_assert_cmpuint (pos, ==, 0);
        }
      else if (g_strcmp0 (label, "Compress") == 0)
        {
          n_compress++;
          g_assert_cmpuint (n_resize, ==, 0);
          g_assert_cmpuint (n_upscale, ==, 0);
        }
      else if (g_strcmp0 (label, "Resize") == 0)
        {
          n_resize++;
          g_assert_cmpuint (n_upscale, ==, 0);
        }
      else if (g_strcmp0 (label, "Upscale") == 0)
        n_upscale++;
      else
        g_error ("unexpected top-level item '%s'", label);
      g_free (label);
    }
  g_assert_cmpuint (n_convert, <=, 1);
  g_assert_cmpuint (n_compress, <=, 1);
  g_assert_cmpuint (n_resize, <=, 1);
  g_assert_cmpuint (n_upscale, <=, 1);
  return items;
}

/* the top-level item with this label (borrowed from items), or NULL */
static NautilusMenuItem *
find_top (GList *items, const char *want)
{
  for (GList *l = items; l; l = l->next)
    {
      char *label = NULL;
      gboolean hit;

      g_object_get (l->data, "label", &label, NULL);
      hit = g_strcmp0 (label, want) == 0;
      g_free (label);
      if (hit)
        return l->data;
    }
  return NULL;
}

/* the items of the submenu of the top-level item "label" (release with
 * free_items_deep), or NULL when there is no such item */
static GList *
top_submenu (GList *infos, const char *label)
{
  GList *items = top_items (infos);
  NautilusMenuItem *top = find_top (items, label);
  NautilusMenu *sub = NULL;
  GList *sub_items = NULL;

  if (top != NULL)
    {
      g_object_get (top, "menu", &sub, NULL);
      g_assert_nonnull (sub);
      sub_items = nautilus_menu_get_items (sub);
      g_object_unref (sub);
    }
  free_top (items);
  return sub_items;
}

static char *
menu_labels (GList *infos)
{
  GString *s = g_string_new (NULL);
  GList *sub_items = top_submenu (infos, "Convert to");

  if (sub_items == NULL)
    {
      g_string_free (s, TRUE);
      return NULL;
    }
  {
    /* transfer full: the list and its items belong to the caller */
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
  return top_submenu (infos, "Convert to");
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
 * "<name>::best" ... and with a tooltip each. Small's label says what it does to
 * the size: "Small (max 2560 px)" for an image target, "Small (max 1080p)" for
 * MP4/WebM, "Small (480 px)" for GIF; the item NAME stays "...::small". */
static void
assert_preset_submenu (NautilusMenuItem *item, const char *name)
{
  static const char *const keys[] = { "best", "high", "balanced", "small" };
  const char *small = strstr (name, "::image::") != NULL ? "Small (max 2560 px)"
                      : g_str_has_suffix (name, "::gif") ? "Small (480 px)"
                      : "Small (max 1080p)";
  const char *const labels[] = { "Best", "High", "Balanced", small };
  static const char *const tip_starts[] = { "Best", "High", "Balanced", "Small" };
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
      g_assert_true (g_str_has_prefix (tip, tip_starts[i]));
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

/* The names, labels and tooltips of the Compress submenu of a selection: High,
 * Balanced, Small, as leaves, named FileConverter::Compress::<preset> */
static void
assert_compress_submenu (GList *infos)
{
  static const char *const keys[] = { "high", "balanced", "small" };
  static const char *const labels[] = { "High", "Balanced", "Small (max 2560 px / 1080p)" };
  static const char *const tip_starts[] = { "High", "Balanced", "Small" };
  GList *items = top_items (infos);
  NautilusMenuItem *top = find_top (items, "Compress");
  NautilusMenu *sub = NULL;
  GList *sub_items;
  char *name = item_prop (top, "name"), *tip = item_prop (top, "tip");
  guint i = 0;

  g_assert_nonnull (top);
  g_assert_cmpstr (name, ==, "FileConverter::Compress");
  g_assert_nonnull (tip);
  g_assert_nonnull (strstr (tip, "original")); /* says that the original stays */
  g_object_get (top, "menu", &sub, NULL);
  g_assert_nonnull (sub);
  sub_items = nautilus_menu_get_items (sub);
  g_assert_cmpuint (g_list_length (sub_items), ==, 3);
  for (GList *l = sub_items; l; l = l->next, i++)
    {
      char *n = item_prop (l->data, "name"), *lab = item_prop (l->data, "label");
      char *t = item_prop (l->data, "tip"), *want = g_strconcat ("FileConverter::Compress::", keys[i], NULL);

      g_assert_cmpstr (n, ==, want);
      g_assert_cmpstr (lab, ==, labels[i]);
      g_assert_nonnull (t);
      g_assert_true (g_str_has_prefix (t, tip_starts[i]));
      g_assert_true (item_is_leaf (l->data));
      g_free (n); g_free (lab); g_free (t); g_free (want);
    }
  nautilus_menu_item_list_free (sub_items);
  g_object_unref (sub);
  g_free (name);
  g_free (tip);
  nautilus_menu_item_list_free (items);
}

/* Is "Compress" offered for this selection? The probes run in the background
 * and the menu never waits: wait (up to 12 s, a probe's own timeout is 10 s)
 * until it appears; when it should NOT be there, until the Convert menu of the
 * same selection is (so that the probes of the category have finished). */
static gboolean
compress_offered_wait (GList *infos, gboolean expect)
{
  GList *items = NULL;
  gboolean got = FALSE;

  for (int i = 0; i < 600; i++)
    {
      items = top_items (infos);
      got = find_top (items, "Compress") != NULL;
      if (got || (!expect && find_top (items, "Convert to") != NULL))
        break;
      free_top (items);
      items = NULL;
      g_usleep (20000);
    }
  free_top (items);
  return got;
}

static GList *
sel1 (const char *name, const char *mime, gboolean dir)
{
  char *u = path_uri (name);
  GList *l = g_list_append (NULL, fake_new (u, mime, dir));

  g_free (u);
  return l;
}

static GList *
sel_add (GList *l, const char *name, const char *mime)
{
  char *u = path_uri (name);

  l = g_list_append (l, fake_new (u, mime, FALSE));
  g_free (u);
  return l;
}

/* "Compress" next to "Convert to": offered only when every selected file is a
 * JPG, WEBP, AVIF, MP4/M4V, WebM or GIF. */
static void
test_compress_menus (void)
{
  GList *l;
  char *ffmpeg_path = g_find_program_in_path ("ffmpeg");
  gboolean have_ffmpeg = ffmpeg_path != NULL;

  touch ("c1.jpg"); touch ("c2.jpeg"); touch ("c3.png"); touch ("c4.webp");
  touch ("c5.wav"); touch ("c6.docx"); touch ("c7.mkv"); touch ("c8.heic"); touch ("c9.bmp");
  touch ("v1.mp4"); touch ("v2.webm"); touch ("v3.m4v"); touch ("g1.gif"); touch ("a1.avif");
  g_mkdir_with_parents ("compress-dir", 0755);

  /* a JPG: yes, with its three presets; "Convert to" is still first */
  l = sel1 ("c1.jpg", "image/jpeg", FALSE);
  g_assert_true (compress_offered_wait (l, TRUE));
  assert_compress_submenu (l);
  g_list_free_full (l, g_object_unref);

  /* .jpeg and two JPGs: yes */
  l = sel1 ("c2.jpeg", "image/jpeg", FALSE);
  g_assert_true (compress_offered_wait (l, TRUE));
  l = sel_add (l, "c1.jpg", "image/jpeg");
  g_assert_true (compress_offered_wait (l, TRUE));
  assert_compress_submenu (l);
  g_list_free_full (l, g_object_unref);

  /* a PNG has no presets: no Compress (but "Convert to" is there) */
  l = sel1 ("c3.png", "image/png", FALSE);
  g_assert_false (compress_offered_wait (l, FALSE));
  g_list_free_full (l, g_object_unref);

  /* JPG + PNG: not every file qualifies */
  l = sel1 ("c1.jpg", "image/jpeg", FALSE);
  l = sel_add (l, "c3.png", "image/png");
  g_assert_false (compress_offered_wait (l, FALSE));
  g_list_free_full (l, g_object_unref);

  /* BMP, HEIC: no */
  l = sel1 ("c9.bmp", "image/bmp", FALSE);
  g_assert_false (compress_offered_wait (l, FALSE));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("c8.heic", "image/heic", FALSE);
  g_assert_false (compress_offered_wait (l, FALSE));
  g_list_free_full (l, g_object_unref);

  /* a directory, alone or with a JPG: no menu at all */
  l = sel1 ("compress-dir", "inode/directory", TRUE);
  g_assert_null (top_submenu (l, "Compress"));
  g_assert_null (top_submenu (l, "Convert to"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("c1.jpg", "image/jpeg", FALSE);
  l = g_list_append (l, fake_new ("file:///tmp", "inode/directory", TRUE));
  g_assert_null (top_submenu (l, "Compress"));
  g_list_free_full (l, g_object_unref);

  /* audio, a document: no */
  l = sel1 ("c5.wav", "audio/x-wav", FALSE);
  g_assert_null (top_submenu (l, "Compress"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("c6.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", FALSE);
  g_assert_null (top_submenu (l, "Compress"));
  g_list_free_full (l, g_object_unref);

  /* mixed formats of one category are fine: each is compressed in its own format
   * (WEBP needs an ImageMagick that writes it: when "Convert to" of a PNG offers
   * WEBP) */
  {
    GList *p = sel1 ("c3.png", "image/png", FALSE);
    char *labels = menu_labels_wait (p);

    g_assert_nonnull (labels);
    if (has_label (labels, "WEBP"))
      {
        l = sel1 ("c1.jpg", "image/jpeg", FALSE);
        l = sel_add (l, "c4.webp", "image/webp");
        g_assert_true (compress_offered_wait (l, TRUE));
        assert_compress_submenu (l);
        g_list_free_full (l, g_object_unref);
      }
    g_free (labels);
    g_list_free_full (p, g_object_unref);
  }

  /* video and GIF need ffmpeg (and its encoders: when it is there they are) */
  l = sel1 ("c7.mkv", "video/x-matroska", FALSE); /* MKV: no presets of its own */
  g_assert_false (compress_offered_wait (l, FALSE) && have_ffmpeg);
  g_list_free_full (l, g_object_unref);
  if (have_ffmpeg)
    {
      GList *m;

      for (const char *const *f = (const char *const[]) { "v1.mp4", "v2.webm", "v3.m4v", NULL }; *f; f++)
        {
          l = sel1 (*f, g_str_has_suffix (*f, "webm") ? "video/webm" : "video/mp4", FALSE);
          /* offered only if the needed encoder (libx264/libopenh264, libvpx-vp9) exists */
          if (compress_offered_wait (l, TRUE))
            assert_compress_submenu (l);
          g_list_free_full (l, g_object_unref);
        }
      /* MP4 + MKV: no */
      m = sel1 ("v1.mp4", "video/mp4", FALSE);
      m = sel_add (m, "c7.mkv", "video/x-matroska");
      g_assert_false (compress_offered_wait (m, FALSE));
      g_list_free_full (m, g_object_unref);
      /* GIF (an image) and a JPG together */
      l = sel1 ("g1.gif", "image/gif", FALSE);
      if (compress_offered_wait (l, TRUE))
        assert_compress_submenu (l);
      g_list_free_full (l, g_object_unref);
    }
  g_free (ffmpeg_path);
}

/* ---- Resize ---- */

/* The names (after "FileConverter::Resize::") of the Resize submenu, joined by
 * ',' (every item a leaf, its label and tooltip returned through the out
 * arguments when wanted), or NULL when there is no Resize item. Waits (up to 12 s)
 * for it to appear; when it should NOT be there, until the Convert menu of the
 * same selection is, so that the probes of the category have finished. */
static char *
resize_names_wait (GList *infos, gboolean expect, char **labels_out, char **tips_out)
{
  GList *items = NULL;
  NautilusMenuItem *top = NULL;
  GString *names = NULL, *labels = g_string_new (NULL), *tips = g_string_new (NULL);

  for (int i = 0; i < 600; i++)
    {
      items = top_items (infos);
      top = find_top (items, "Resize");
      if (top != NULL || (!expect && find_top (items, "Convert to") != NULL))
        break;
      free_top (items);
      items = NULL;
      g_usleep (20000);
    }
  if (top != NULL)
    {
      NautilusMenu *sub = NULL;
      GList *sub_items;
      char *name = item_prop (top, "name");

      g_assert_cmpstr (name, ==, "FileConverter::Resize");
      g_free (name);
      g_object_get (top, "menu", &sub, NULL);
      g_assert_nonnull (sub);
      sub_items = nautilus_menu_get_items (sub);
      names = g_string_new (NULL);
      for (GList *l = sub_items; l; l = l->next)
        {
          char *n = item_prop (l->data, "name"), *lab = item_prop (l->data, "label");
          char *tip = item_prop (l->data, "tip");

          g_assert_true (g_str_has_prefix (n, "FileConverter::Resize::"));
          g_assert_true (item_is_leaf (l->data));
          g_string_append_printf (names, "%s%s", names->len ? "," : "",
                                  n + strlen ("FileConverter::Resize::"));
          g_string_append_printf (labels, "%s%s", labels->len ? "|" : "", lab);
          g_string_append_printf (tips, "%s%s", tips->len ? "|" : "", tip);
          g_free (n); g_free (lab); g_free (tip);
        }
      nautilus_menu_item_list_free (sub_items);
      g_object_unref (sub);
    }
  free_top (items);
  if (labels_out != NULL)
    *labels_out = g_string_free (labels, FALSE);
  else
    g_string_free (labels, TRUE);
  if (tips_out != NULL)
    *tips_out = g_string_free (tips, FALSE);
  else
    g_string_free (tips, TRUE);
  return names != NULL ? g_string_free (names, FALSE) : NULL;
}

/* make an image of w x h pixels with ImageMagick (tests/ have no binary fixtures) */
static gboolean
make_image (const char *magick, const char *name, guint w, guint h)
{
  char *p = g_build_filename (work, name, NULL);
  char *size = g_strdup_printf ("%ux%u", w, h);
  char *fmt = g_str_has_suffix (name, ".jpg") ? g_strdup_printf ("JPEG:%s", p)
                                              : g_strdup_printf ("PNG:%s", p);
  char *argv[] = { (char *) magick, "-size", size, "xc:gray", fmt, NULL };
  int status = -1;
  gboolean ok = g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                              NULL, NULL, NULL, NULL, &status, NULL) && status == 0;

  g_free (p); g_free (size); g_free (fmt);
  return ok;
}

/* "Resize" after "Convert to" and "Compress": only presets that make a picture
 * smaller (sizes read from the file headers), the fixed list where the size is
 * not known, 1080p/720p/480p for video. */
static void
test_resize_menus (void)
{
  char *magick = g_find_program_in_path ("magick");
  char *ffmpeg = g_find_program_in_path ("ffmpeg");
  char *names, *labels, *tips;
  GList *l;
  const char *all_images = "4k,1440p,1080p,720p,480p,50pct,25pct";

  touch ("r-garbage.png"); touch ("r-video.mp4"); touch ("r-heic.heic"); touch ("r-svg.svg");
  touch ("r-doc.docx");
  g_mkdir_with_parents ("resize-dir", 0755);
  if (magick != NULL)
    {
      g_assert_true (make_image (magick, "r-4k.png", 3840, 2160));
      g_assert_true (make_image (magick, "r-1000x800.jpg", 1000, 800));
      g_assert_true (make_image (magick, "r-20.png", 20, 20));
      g_assert_true (make_image (magick, "r-30.png", 30, 30));
      g_assert_true (make_image (magick, "r-720p.png", 1280, 720));

      /* a 4K PNG: everything smaller than 3840 px, not 4K itself; names, labels
       * and the exact size in the tooltip */
      l = sel1 ("r-4k.png", "image/png", FALSE);
      names = resize_names_wait (l, TRUE, &labels, &tips);
      g_assert_cmpstr (names, ==, "1440p,1080p,720p,480p,50pct,25pct");
      g_assert_cmpstr (labels, ==, "1440p (2560 px)|1080p (1920 px)|720p (1280 px)|"
                                   "480p (854 px)|50%|25%");
      g_assert_nonnull (strstr (tips, "2560 × 1440 px (aspect ratio kept)")); /* 1440p */
      g_assert_nonnull (strstr (tips, "1920 × 1080 px (aspect ratio kept)")); /* 1080p, 50% */
      g_assert_nonnull (strstr (tips, "960 × 540 px (aspect ratio kept)"));   /* 25% */
      g_free (names); g_free (labels); g_free (tips);
      g_list_free_full (l, g_object_unref);

      /* a 1000 x 800 JPG: 720p is 1280 px on the long side, which is larger */
      l = sel1 ("r-1000x800.jpg", "image/jpeg", FALSE);
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "480p,50pct,25pct");
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* a 1280 x 720 PNG is not larger than 720p: 480p and the percentages */
      l = sel1 ("r-720p.png", "image/png", FALSE);
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "480p,50pct,25pct");
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* 30 x 30: 50% is 15 px (< 16), so only... nothing; 20 x 20 likewise */
      l = sel1 ("r-20.png", "image/png", FALSE);
      g_assert_null (resize_names_wait (l, FALSE, NULL, NULL));
      g_list_free_full (l, g_object_unref);
      l = sel1 ("r-30.png", "image/png", FALSE);
      g_assert_null (resize_names_wait (l, FALSE, NULL, NULL));
      g_list_free_full (l, g_object_unref);

      /* a selection: a preset that makes at least one file smaller is shown */
      l = sel1 ("r-4k.png", "image/png", FALSE);
      l = sel_add (l, "r-20.png", "image/png");
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "1440p,1080p,720p,480p,50pct,25pct");
      g_free (names);
      l = sel_add (l, "r-1000x800.jpg", "image/jpeg"); /* a JPG and PNGs: still resizable */
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "1440p,1080p,720p,480p,50pct,25pct");
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* two files that are both too small: no Resize */
      l = sel1 ("r-20.png", "image/png", FALSE);
      l = sel_add (l, "r-30.png", "image/png");
      g_assert_null (resize_names_wait (l, FALSE, NULL, NULL));
      g_list_free_full (l, g_object_unref);

      /* a small file and one of unknown size (garbage): the fixed list */
      l = sel1 ("r-20.png", "image/png", FALSE);
      l = sel_add (l, "r-garbage.png", "image/png");
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, all_images);
      g_free (names);
      g_list_free_full (l, g_object_unref);
    }

  /* a file whose size cannot be read: the fixed image list */
  if (magick != NULL)
    {
      l = sel1 ("r-garbage.png", "image/png", FALSE);
      names = resize_names_wait (l, TRUE, &labels, NULL);
      g_assert_cmpstr (names, ==, all_images);
      g_assert_true (g_str_has_prefix (labels, "4K (3840 px)|1440p (2560 px)|"));
      g_free (names); g_free (labels);
      g_list_free_full (l, g_object_unref);
    }

  /* a video: the size is not known at right-click time, so the fixed list of
   * short-side presets (offered when ffmpeg has an H.264 encoder) */
  if (ffmpeg != NULL)
    {
      l = sel1 ("r-video.mp4", "video/mp4", FALSE);
      names = resize_names_wait (l, TRUE, &labels, NULL);
      if (names != NULL)
        {
          g_assert_cmpstr (names, ==, "1080p,720p,480p");
          g_assert_cmpstr (labels, ==, "1080p|720p|480p");
        }
      g_free (names); g_free (labels);
      g_list_free_full (l, g_object_unref);
    }

  /* HEIC and SVG cannot be written back in their own format: no Resize; a
   * document: none */
  l = sel1 ("r-heic.heic", "image/heic", FALSE);
  g_assert_null (resize_names_wait (l, FALSE, NULL, NULL));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("r-svg.svg", "image/svg+xml", FALSE);
  g_assert_null (resize_names_wait (l, FALSE, NULL, NULL));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("r-doc.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            FALSE);
  g_assert_null (top_submenu (l, "Resize"));
  g_list_free_full (l, g_object_unref);

  /* a directory, alone or with a picture: no menu at all */
  l = sel1 ("resize-dir", "inode/directory", TRUE);
  g_assert_null (top_submenu (l, "Resize"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("r-garbage.png", "image/png", FALSE);
  l = g_list_append (l, fake_new ("file:///tmp", "inode/directory", TRUE));
  g_assert_null (top_submenu (l, "Resize"));
  g_list_free_full (l, g_object_unref);

  /* images and a video together: no menu */
  l = sel1 ("r-garbage.png", "image/png", FALSE);
  l = sel_add (l, "r-video.mp4", "video/mp4");
  g_assert_null (top_submenu (l, "Resize"));
  g_list_free_full (l, g_object_unref);

  g_free (magick);
  g_free (ffmpeg);
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
/* ---- Upscale ---- */

/* The names (after "FileConverter::Upscale::") of the Upscale submenu, joined by
 * ',' (every item a leaf; its labels and tooltips through the out arguments when
 * wanted), or NULL when there is no Upscale item. As resize_names_wait. */
static char *
upscale_names_wait (GList *infos, gboolean expect, char **labels_out, char **tips_out)
{
  GList *items = NULL;
  NautilusMenuItem *top = NULL;
  GString *names = NULL, *labels = g_string_new (NULL), *tips = g_string_new (NULL);

  for (int i = 0; i < 600; i++)
    {
      items = top_items (infos);
      top = find_top (items, "Upscale");
      if (top != NULL || (!expect && find_top (items, "Convert to") != NULL))
        break;
      free_top (items);
      items = NULL;
      g_usleep (20000);
    }
  if (top != NULL)
    {
      NautilusMenu *sub = NULL;
      GList *sub_items;
      char *name = item_prop (top, "name");

      g_assert_cmpstr (name, ==, "FileConverter::Upscale");
      g_free (name);
      g_object_get (top, "menu", &sub, NULL);
      g_assert_nonnull (sub);
      sub_items = nautilus_menu_get_items (sub);
      names = g_string_new (NULL);
      for (GList *l = sub_items; l; l = l->next)
        {
          char *n = item_prop (l->data, "name"), *lab = item_prop (l->data, "label");
          char *tip = item_prop (l->data, "tip");

          g_assert_true (g_str_has_prefix (n, "FileConverter::Upscale::"));
          g_assert_true (item_is_leaf (l->data));
          g_string_append_printf (names, "%s%s", names->len ? "," : "",
                                  n + strlen ("FileConverter::Upscale::"));
          g_string_append_printf (labels, "%s%s", labels->len ? "|" : "", lab);
          g_string_append_printf (tips, "%s%s", tips->len ? "|" : "", tip);
          g_free (n); g_free (lab); g_free (tip);
        }
      nautilus_menu_item_list_free (sub_items);
      g_object_unref (sub);
    }
  free_top (items);
  if (labels_out != NULL)
    *labels_out = g_string_free (labels, FALSE);
  else
    g_string_free (labels, TRUE);
  if (tips_out != NULL)
    *tips_out = g_string_free (tips, FALSE);
  else
    g_string_free (tips, TRUE);
  return names != NULL ? g_string_free (names, FALSE) : NULL;
}

/* the labels of the top-level items, in order, joined by ',' */
static char *
top_labels (GList *infos)
{
  GList *items = top_items (infos);
  GString *s = g_string_new (NULL);

  for (GList *l = items; l; l = l->next)
    {
      char *label = item_prop (l->data, "label");

      g_string_append_printf (s, "%s%s", s->len ? "," : "", label);
      g_free (label);
    }
  free_top (items);
  return g_string_free (s, FALSE);
}

/* "Upscale" after "Resize": 2× ... 8× for the pictures Upscayl takes, only the scales whose
 * result stays within 32768 px per side and 400 megapixels (sizes from the file
 * headers), all of them where the size is not known; none for SVG, ICO, video, documents,
 * folders. (The module of this process found a fake upscayl-bin: see main.) */
static void
test_upscale_menus (void)
{
  char *magick = g_find_program_in_path ("magick");
  char *names, *labels, *tips, *tl;
  GList *l;
  const char *all = "2x,3x,4x,5x,6x,8x";

  touch ("u-garbage.png"); touch ("u-video.mp4"); touch ("u-svg.svg"); touch ("u-ico.ico");
  touch ("u-doc.docx"); touch ("u-noise.mp3");
  g_mkdir_with_parents ("upscale-dir", 0755);
  if (magick != NULL)
    {
      g_assert_true (make_image (magick, "u-640x480.png", 640, 480));
      g_assert_true (make_image (magick, "u-5000x4000.png", 5000, 4000));
      g_assert_true (make_image (magick, "u-3840.png", 3840, 2160));
      g_assert_true (make_image (magick, "u-4100x50.jpg", 4100, 50));
      g_assert_true (make_image (magick, "u-20000x20.png", 20000, 20));
      g_assert_true (make_image (magick, "u-64.jpg", 64, 64));

      /* 640 x 480: every scale, the exact size in the tooltip, after Resize */
      l = sel1 ("u-640x480.png", "image/png", FALSE);
      names = upscale_names_wait (l, TRUE, &labels, &tips);
      g_assert_cmpstr (names, ==, all);
      g_assert_cmpstr (labels, ==, "2×|3×|4×|5×|6×|8×");
      g_assert_nonnull (strstr (tips, "1280 × 960 px"));   /* 2× */
      g_assert_nonnull (strstr (tips, "2560 × 1920 px"));  /* 4× */
      g_assert_nonnull (strstr (tips, "3200 × 2400 px"));  /* 5× */
      g_assert_nonnull (strstr (tips, "5120 × 3840 px"));  /* 8× */
      g_free (names); g_free (labels); g_free (tips);
      tl = top_labels (l);
      g_assert_cmpstr (tl, ==, "Convert to,Resize,Upscale"); /* a PNG has no Compress */
      g_free (tl);
      g_list_free_full (l, g_object_unref);

      /* a JPG has all four (Compress, Resize: smaller; Upscale: larger), Upscale last */
      l = sel1 ("u-4100x50.jpg", "image/jpeg", FALSE);
      names = upscale_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "2x,3x,4x,5x,6x"); /* 8×: 32800 px */
      g_free (names);
      tl = top_labels (l);
      g_assert_cmpstr (tl, ==, "Convert to,Compress,Resize,Upscale");
      g_free (tl);
      g_list_free_full (l, g_object_unref);

      /* a 5000 x 4000 picture: 4× is 320 MP; 5× (500 MP), 6× and 8× are too big */
      l = sel1 ("u-5000x4000.png", "image/png", FALSE);
      names = upscale_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "2x,3x,4x");
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* 3840 x 2160: 6× is 298 MP, 8× 530 MP */
      l = sel1 ("u-3840.png", "image/png", FALSE);
      names = upscale_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, "2x,3x,4x,5x,6x");
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* 20000 x 20: even 2× is 40000 px: no Upscale, the rest of the menu is there */
      l = sel1 ("u-20000x20.png", "image/png", FALSE);
      g_assert_null (upscale_names_wait (l, FALSE, NULL, NULL));
      tl = top_labels (l);
      g_assert_cmpstr (tl, ==, "Convert to,Resize");
      g_free (tl);
      g_list_free_full (l, g_object_unref);

      /* a selection: a scale that fits one file is shown (the others are skipped when it runs);
       * the tooltips then say no exact size */
      l = sel1 ("u-5000x4000.png", "image/png", FALSE);
      l = sel_add (l, "u-64.jpg", "image/jpeg");
      names = upscale_names_wait (l, TRUE, NULL, &tips);
      g_assert_cmpstr (names, ==, all);
      g_assert_null (strstr (tips, " px"));
      g_free (names); g_free (tips);
      g_list_free_full (l, g_object_unref);
      /* two files that are both too big for the scale: none (for 20000 x 20 and 5000 x 4000: 2x, 3x, 4x) */
      l = sel1 ("u-20000x20.png", "image/png", FALSE);
      l = sel_add (l, "u-20000x20.png", "image/png");
      g_assert_null (upscale_names_wait (l, FALSE, NULL, NULL));
      g_list_free_full (l, g_object_unref);

      /* a file whose size cannot be read: every scale (the job reads it and skips if too big) */
      l = sel1 ("u-garbage.png", "image/png", FALSE);
      names = upscale_names_wait (l, TRUE, NULL, NULL);
      g_assert_cmpstr (names, ==, all);
      g_free (names);
      g_list_free_full (l, g_object_unref);

      /* every other picture format Upscayl needs a conversion for is offered too */
      touch ("u-a.bmp"); touch ("u-a.tiff"); touch ("u-a.gif"); touch ("u-a.webp");
      {
        const struct { const char *name, *mime; } more[] = {
          { "u-a.bmp", "image/bmp" }, { "u-a.tiff", "image/tiff" }, { "u-a.gif", "image/gif" },
          { "u-a.webp", "image/webp" },
        };

        for (guint i = 0; i < G_N_ELEMENTS (more); i++)
          {
            l = sel1 (more[i].name, more[i].mime, FALSE);
            names = upscale_names_wait (l, TRUE, NULL, NULL);
            g_assert_cmpstr (names, ==, all);
            g_free (names);
            g_list_free_full (l, g_object_unref);
          }
      }
    }

  /* a vector picture and an icon: nothing to enlarge */
  l = sel1 ("u-svg.svg", "image/svg+xml", FALSE);
  g_assert_null (upscale_names_wait (l, FALSE, NULL, NULL));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-ico.ico", "image/vnd.microsoft.icon", FALSE);
  g_assert_null (upscale_names_wait (l, FALSE, NULL, NULL));
  g_list_free_full (l, g_object_unref);
  /* a video, audio, a document: none (no Upscale item at all) */
  l = sel1 ("u-video.mp4", "video/mp4", FALSE);
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-noise.mp3", "audio/mpeg", FALSE);
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-doc.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", FALSE);
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  /* a directory, alone or with a picture; a picture and a video; a picture and an SVG */
  l = sel1 ("upscale-dir", "inode/directory", TRUE);
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-garbage.png", "image/png", FALSE);
  l = g_list_append (l, fake_new ("file:///tmp", "inode/directory", TRUE));
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-garbage.png", "image/png", FALSE);
  l = sel_add (l, "u-video.mp4", "video/mp4");
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);
  l = sel1 ("u-garbage.png", "image/png", FALSE);
  l = sel_add (l, "u-svg.svg", "image/svg+xml");
  g_assert_null (top_submenu (l, "Upscale"));
  g_list_free_full (l, g_object_unref);

  g_free (magick);
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
  test_compress_menus ();
  test_resize_menus ();
  test_upscale_menus ();

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

/* Without upscayl-bin (or without its models) there is no Upscale item, whatever the
 * selection, and everything else is as before. Runs in its own process (mode
 * "no-upscayl" or "no-models"): the tools are looked up once per process. There the
 * environment of main has made the backend not found, or found without its models. */
static void
test_no_upscayl (void)
{
  GModule *m = g_module_open (module_path, G_MODULE_BIND_LOCAL);
  void (*initialize) (GTypeModule *);
  void (*list_types) (const GType **, int *);
  void (*shutdown) (void);
  GTypeModule *tm = g_object_new (test_module_get_type (), NULL);
  const GType *types = NULL;
  int n = 0;
  char *magick = g_find_program_in_path ("magick"), *tl, *names;
  GList *l;

  g_assert_nonnull (m);
  g_assert_true (g_module_symbol (m, "nautilus_module_initialize", (gpointer *) &initialize));
  g_assert_true (g_module_symbol (m, "nautilus_module_list_types", (gpointer *) &list_types));
  g_assert_true (g_module_symbol (m, "nautilus_module_shutdown", (gpointer *) &shutdown));
  g_type_module_use (tm);
  initialize (tm);
  list_types (&types, &n);
  provider = g_object_new (types[0], NULL);
  if (magick != NULL)
    {
      g_assert_true (make_image (magick, "n-640x480.jpg", 640, 480));
      l = sel1 ("n-640x480.jpg", "image/jpeg", FALSE);
      /* Resize shows up when the probes have finished: then Upscale would have, too */
      names = resize_names_wait (l, TRUE, NULL, NULL);
      g_assert_nonnull (names);
      g_free (names);
      tl = top_labels (l);
      g_assert_cmpstr (tl, ==, "Convert to,Compress,Resize");
      g_free (tl);
      g_assert_null (top_submenu (l, "Upscale"));
      g_list_free_full (l, g_object_unref);
    }
  g_free (magick);
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
  /* The backend of Upscale: a fake upscayl-bin (never run here) with its models, found through
   * NFC_UPSCAYL_BIN; the modes "no-upscayl" (not found) and "no-models" (found, but the
   * models directory is empty) take it away. The user's configuration and log stay out of it. */
  {
    char *cfg = g_build_filename (work, "xdg-config", NULL), *cache = g_build_filename (work, "xdg-cache", NULL);
    char *bin = g_build_filename (work, "upscayl", "bin", NULL), *models = g_build_filename (work, "upscayl", "models", NULL);
    char *exe = g_build_filename (bin, "upscayl-bin", NULL), *p;
    const char *mode = argc > 2 ? argv[2] : "";

    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_mkdir_with_parents (bin, 0755);
    g_mkdir_with_parents (models, 0755);
    write_script (exe, "exit 0\n");
    for (const char *ext = "param"; ext != NULL; ext = g_str_equal (ext, "param") ? "bin" : NULL)
      {
        char *name = g_strdup_printf ("upscayl-standard-4x.%s", ext);

        p = g_build_filename (models, name, NULL);
        if (!g_str_equal (mode, "no-models"))
          g_file_set_contents (p, "x", -1, NULL);
        g_free (p);
        g_free (name);
      }
    g_setenv ("NFC_UPSCAYL_BIN", g_str_equal (mode, "no-upscayl") ? "/nonexistent/upscayl-bin" : exe, TRUE);
    g_free (cfg);
    g_free (cache);
    g_free (bin);
    g_free (models);
    g_free (exe);
  }
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
  else if (argc > 2 && (g_str_equal (argv[2], "no-upscayl") || g_str_equal (argv[2], "no-models")))
    g_test_add_func ("/module/no-upscale", test_no_upscayl);
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
