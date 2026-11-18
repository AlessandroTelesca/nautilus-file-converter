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

/* returns the submenu labels joined by ',' or NULL when there is no menu */
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
  for (GList *l = nautilus_menu_get_items (sub); l; l = l->next)
    {
      char *lab = NULL;

      g_object_get (l->data, "label", &lab, NULL);
      if (s->len)
        g_string_append_c (s, ',');
      g_string_append (s, lab);
      g_free (lab);
    }
  g_free (label);
  g_object_unref (sub);
  nautilus_menu_item_list_free (items);
  return g_string_free (s, FALSE);
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
  g_mkdir (g_build_filename (work, "adir.png", NULL), 0755);

  /* single image: no own format, other image targets present */
  u1 = path_uri ("a.png");
  f1 = fake_new (u1, "image/png", FALSE);
  l = g_list_append (NULL, f1);
  labels = menu_labels (l);
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
  labels = menu_labels (l);
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
  labels = menu_labels (l);
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
  g_test_add_func ("/module/entry-points", test_entry_points);
  g_test_add_func ("/module/register-and-menu", test_register_and_menu);
  rc = g_test_run ();
  {
    char *cmd_dir = g_strdup (work);
    GDir *d = g_dir_open (cmd_dir, 0, NULL);
    const char *n;

    while (d && (n = g_dir_read_name (d)))
      {
        char *p = g_build_filename (cmd_dir, n, NULL);

        g_rmdir (p);
        g_unlink (p);
        g_free (p);
      }
    if (d)
      g_dir_close (d);
    g_rmdir (cmd_dir);
    g_free (cmd_dir);
  }
  return rc;
}
