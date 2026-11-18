/* nautilus-file-converter: Nautilus glue (menu provider + module entry points).
 *
 * Select files, right-click, "Convert to" > <format>. All real work happens in
 * nfc-core.c / nfc-convert.c.
 */
#include <nautilus-extension.h>

#include "nfc-convert.h"
#include "nfc-core.h"

#define NFC_TYPE_PROVIDER (nfc_provider_get_type ())

typedef struct {
  GObject parent_instance;
} NfcProvider;

typedef struct {
  GObjectClass parent_class;
} NfcProviderClass;

GType nfc_provider_get_type (void);
static void nfc_provider_register_type (GTypeModule *module);

static void nfc_provider_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (NfcProvider, nfc_provider, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                                               nfc_provider_menu_provider_iface_init))

typedef struct {
  NfcCategory cat;
  char *key, *label, *ext;
  GPtrArray *paths; /* char *, owned */
  GPtrArray *fmts;  /* static strings */
} ActivateData;

static void
activate_data_free (gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  ActivateData *d = data;

  g_free (d->key);
  g_free (d->label);
  g_free (d->ext);
  g_ptr_array_unref (d->paths);
  g_ptr_array_unref (d->fmts);
  g_free (d);
}

static void
on_activate (NautilusMenuItem *item G_GNUC_UNUSED, gpointer user_data)
{
  ActivateData *d = user_data;

  if (d == NULL || d->paths->len == 0)
    return;
  nfc_batch_run (d->cat, d->key, d->label, d->ext,
                 (const char *const *) d->paths->pdata,
                 (const char *const *) d->fmts->pdata, d->paths->len, NULL, NULL);
}

/* Runs on the Nautilus main thread: it must never wait. nfc_analyze_selection
 * only stats the selected files and consults capability probes that run in
 * background threads; targets whose probe is still running are left out (see
 * nfc-core.h), so right after startup an image/audio/video selection may get no
 * menu for a moment, while document menus work at once. */
static GList *
nfc_provider_get_file_items (NautilusMenuProvider *provider G_GNUC_UNUSED, GList *files)
{
  NfcSelItem *items;
  char **uris, **mimes;
  guint n = 0, i = 0;
  NfcSelection sel;
  GList *result = NULL;
  gboolean ok;

  for (GList *l = files; l; l = l->next)
    n++;
  if (n == 0)
    return NULL;

  items = g_new0 (NfcSelItem, n);
  uris = g_new0 (char *, n + 1);
  mimes = g_new0 (char *, n + 1);
  for (GList *l = files; l; l = l->next, i++)
    {
      NautilusFileInfo *f = l->data;

      if (!NAUTILUS_IS_FILE_INFO (f))
        {
          /* an invalid entry: no menu (a NULL uri is rejected by the analysis) */
          items[i].uri = NULL;
          continue;
        }
      uris[i] = nautilus_file_info_get_uri (f);
      mimes[i] = nautilus_file_info_get_mime_type (f);
      items[i].uri = uris[i];
      items[i].mime = mimes[i];
      items[i].is_dir = nautilus_file_info_is_directory (f);
    }

  ok = nfc_analyze_selection (items, n, &sel);
  if (ok)
    {
      NautilusMenuItem *top = nautilus_menu_item_new ("FileConverter::Convert", "Convert to",
                                                      NULL, NULL);
      NautilusMenu *sub = nautilus_menu_new ();

      if (top != NULL && sub != NULL)
        {
          for (guint k = 0; k < sel.targets->len; k++)
            {
              const NfcTarget *t = sel.targets->pdata[k];
              char *name = g_strdup_printf ("FileConverter::%s::%s",
                                            nfc_category_name (sel.cat), t->key);
              NautilusMenuItem *item = nautilus_menu_item_new (name, t->label, NULL, NULL);
              ActivateData *d = g_new0 (ActivateData, 1);

              d->cat = sel.cat;
              d->key = g_strdup (t->key);
              d->label = g_strdup (t->label);
              d->ext = g_strdup (t->ext);
              d->paths = g_ptr_array_new_null_terminated (8, g_free, TRUE);
              d->fmts = g_ptr_array_new_null_terminated (8, NULL, TRUE);
              for (guint p = 0; p < sel.paths->len; p++)
                {
                  g_ptr_array_add (d->paths, g_strdup (sel.paths->pdata[p]));
                  g_ptr_array_add (d->fmts, sel.fmts->pdata[p]);
                }
              g_signal_connect_data (item, "activate", G_CALLBACK (on_activate), d,
                                     activate_data_free, 0);
              nautilus_menu_append_item (sub, item);
              g_object_unref (item);
              g_free (name);
            }
          nautilus_menu_item_set_submenu (top, sub);
          result = g_list_append (result, top);
        }
      else
        g_clear_object (&top);
      g_clear_object (&sub);
    }
  nfc_selection_clear (&sel);
  /* explicit loop over n: an invalid entry in the middle leaves NULL holes, where
   * g_strfreev() would stop and leak every later entry */
  for (guint k = 0; k < n; k++)
    {
      g_free (uris[k]);
      g_free (mimes[k]);
    }
  g_free (uris);
  g_free (mimes);
  g_free (items);
  return result;
}

static GList *
nfc_provider_get_background_items (NautilusMenuProvider *provider G_GNUC_UNUSED,
                                   NautilusFileInfo *current_folder G_GNUC_UNUSED)
{
  return NULL;
}

static void
nfc_provider_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
  iface->get_file_items = nfc_provider_get_file_items;
  iface->get_background_items = nfc_provider_get_background_items;
}

static void
nfc_provider_init (NfcProvider *self G_GNUC_UNUSED)
{
}

static void
nfc_provider_class_init (NfcProviderClass *klass G_GNUC_UNUSED)
{
}

static void
nfc_provider_class_finalize (NfcProviderClass *klass G_GNUC_UNUSED)
{
}

void
nautilus_module_initialize (GTypeModule *module)
{
  nfc_tools_init ();
  nfc_provider_register_type (module);
  /* probe ffmpeg encoders / ImageMagick formats / pixbuf loaders in background
   * threads (each with a timeout); get_file_items never waits for them */
  nfc_probe_start ();
}

void
nautilus_module_shutdown (void)
{
  /* kill in-flight conversions and remove their temp files (best effort) */
  nfc_shutdown ();
}

void
nautilus_module_list_types (const GType **types, int *num_types)
{
  static GType type_list[1];

  type_list[0] = NFC_TYPE_PROVIDER;
  *types = type_list;
  *num_types = 1;
}
