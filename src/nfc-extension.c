/* nautilus-file-converter: Nautilus glue (menu provider + module entry points).
 *
 * Select files, right-click, "Convert to" > <format> [> <quality preset>], or
 * "Compress" > <quality preset> (a smaller copy in the same format), or
 * "Resize" > <size> (a smaller copy at a given pixel size), or "Upscale" > 2× .. 8×
 * (an AI-enlarged copy, made by Upscayl's backend). All
 * real work happens in nfc-core.c / nfc-convert.c.
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
  NfcPreset preset;
  gboolean compress; /* Compress (key, label, ext unused) instead of Convert to */
  char *resize_key;  /* Resize to this preset (NfcResizePreset.key); NULL: not a resize */
  char *upscale_key; /* Upscale by this preset (NfcUpscalePreset.key); NULL: not an upscale */
  char *key, *label, *ext;
  GPtrArray *paths; /* char *, owned */
  GPtrArray *fmts;  /* static strings */
} ActivateData;

static void
activate_data_free (gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  ActivateData *d = data;

  g_free (d->key);
  g_free (d->resize_key);
  g_free (d->upscale_key);
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
  if (d->upscale_key != NULL)
    nfc_batch_run_upscale (d->upscale_key, (const char *const *) d->paths->pdata,
                           (const char *const *) d->fmts->pdata, d->paths->len, NULL, NULL);
  else if (d->resize_key != NULL)
    nfc_batch_run_resize (d->cat, d->resize_key, (const char *const *) d->paths->pdata,
                          (const char *const *) d->fmts->pdata, d->paths->len, NULL, NULL);
  else if (d->compress)
    nfc_batch_run_compress (d->cat, d->preset, (const char *const *) d->paths->pdata,
                            (const char *const *) d->fmts->pdata, d->paths->len, NULL, NULL);
  else
    nfc_batch_run_preset (d->cat, d->key, d->preset, d->label, d->ext,
                          (const char *const *) d->paths->pdata,
                          (const char *const *) d->fmts->pdata, d->paths->len, NULL, NULL);
}

/* the selected files (paths and formats) of the item's action */
static void
activate_data_copy_selection (ActivateData *d, const NfcSelection *sel)
{
  d->paths = g_ptr_array_new_null_terminated (8, g_free, TRUE);
  d->fmts = g_ptr_array_new_null_terminated (8, NULL, TRUE);
  for (guint p = 0; p < sel->paths->len; p++)
    {
      g_ptr_array_add (d->paths, g_strdup (sel->paths->pdata[p]));
      g_ptr_array_add (d->fmts, sel->fmts->pdata[p]);
    }
}

/* A menu item that converts the whole selection to target t with this preset (t
 * NULL: compresses every file in its own format with it) */
static NautilusMenuItem *
convert_item_new (const NfcSelection *sel, const NfcTarget *t, NfcPreset preset,
                  const char *name, const char *label, const char *tip)
{
  NautilusMenuItem *item = nautilus_menu_item_new (name, label, tip, NULL);
  ActivateData *d = g_new0 (ActivateData, 1);

  d->cat = sel->cat;
  d->preset = preset;
  d->compress = t == NULL;
  d->key = g_strdup (t != NULL ? t->key : "");
  d->label = g_strdup (t != NULL ? t->label : "Compress");
  d->ext = g_strdup (t != NULL ? t->ext : "");
  activate_data_copy_selection (d, sel);
  g_signal_connect_data (item, "activate", G_CALLBACK (on_activate), d,
                         activate_data_free, 0);
  return item;
}

/* One entry of the "Convert to" menu. Targets with quality presets get a
 * submenu "Best / High / Balanced / Small" (item names
 * "FileConverter::<category>::<key>::<preset>"); all others are plain items
 * that convert as Best. Pure bookkeeping: nothing here waits. */
static NautilusMenuItem *
target_item_new (const NfcSelection *sel, const NfcTarget *t)
{
  char *name = g_strdup_printf ("FileConverter::%s::%s", nfc_category_name (sel->cat), t->key);
  NautilusMenuItem *item;

  if (!nfc_target_has_presets (sel->cat, t->key))
    {
      item = convert_item_new (sel, t, NFC_PRESET_BEST, name, t->label, NULL);
      g_free (name);
      return item;
    }

  item = nautilus_menu_item_new (name, t->label, "Choose a quality preset", NULL);
  {
    NautilusMenu *sub = nautilus_menu_new ();

    for (guint p = 0; p < NFC_PRESET_N; p++)
      {
        char *pname = g_strdup_printf ("%s::%s", name, nfc_preset_key ((NfcPreset) p));
        NautilusMenuItem *pi = convert_item_new (sel, t, (NfcPreset) p, pname,
                                                 nfc_preset_menu_label (sel->cat, t->key, (NfcPreset) p),
                                                 nfc_preset_tooltip (sel->cat, t->key,
                                                                     (NfcPreset) p));

        nautilus_menu_append_item (sub, pi);
        g_object_unref (pi);
        g_free (pname);
      }
    nautilus_menu_item_set_submenu (item, sub);
    g_object_unref (sub);
  }
  g_free (name);
  return item;
}

/* The "Compress" entry: a submenu High / Balanced / Small (item names
 * "FileConverter::Compress::<preset>"). */
static NautilusMenuItem *
compress_item_new (const NfcSelection *sel)
{
  NautilusMenuItem *top = nautilus_menu_item_new ("FileConverter::Compress", "Compress",
                                                  "Make a smaller copy next to the original; "
                                                  "nothing is made if it would not be smaller",
                                                  NULL);
  NautilusMenu *sub = nautilus_menu_new ();

  for (NfcPreset p = NFC_PRESET_HIGH; p < NFC_PRESET_N; p++)
    {
      char *name = g_strdup_printf ("FileConverter::Compress::%s", nfc_preset_key (p));
      NautilusMenuItem *pi = convert_item_new (sel, NULL, p, name,
                                               nfc_preset_menu_label (sel->cat, NULL, p),
                                               nfc_compress_tooltip (p));

      nautilus_menu_append_item (sub, pi);
      g_object_unref (pi);
      g_free (name);
    }
  nautilus_menu_item_set_submenu (top, sub);
  g_object_unref (sub);
  return top;
}

/* The "Resize" entry: a submenu of the presets that make the selection smaller
 * (item names "FileConverter::Resize::<key>", e.g. "...::1080p", "...::50pct").
 * The tooltip says the exact size when one file of known size is selected. */
static NautilusMenuItem *
resize_item_new (const NfcSelection *sel)
{
  NautilusMenuItem *top = nautilus_menu_item_new ("FileConverter::Resize", "Resize",
                                                  "Make a smaller copy next to the original, "
                                                  "same format, at a given size", NULL);
  NautilusMenu *sub = nautilus_menu_new ();
  gboolean one = sel->paths->len == 1 && sel->dims != NULL && sel->dims->len == 1;

  for (guint k = 0; k < sel->resize->len; k++)
    {
      const NfcResizePreset *p = sel->resize->pdata[k];
      char *name = g_strdup_printf ("FileConverter::Resize::%s", p->key);
      const NfcDims *d = one ? &g_array_index (sel->dims, NfcDims, 0) : NULL;
      char *tip = nfc_resize_tooltip (p, d != NULL ? d->width : 0, d != NULL ? d->height : 0);
      NautilusMenuItem *item = nautilus_menu_item_new (name, p->label, tip, NULL);
      ActivateData *data = g_new0 (ActivateData, 1);

      data->cat = sel->cat;
      data->resize_key = g_strdup (p->key);
      data->key = g_strdup ("");
      data->label = g_strdup ("Resize");
      data->ext = g_strdup ("");
      activate_data_copy_selection (data, sel);
      g_signal_connect_data (item, "activate", G_CALLBACK (on_activate), data,
                             activate_data_free, 0);
      nautilus_menu_append_item (sub, item);
      g_object_unref (item);
      g_free (tip);
      g_free (name);
    }
  nautilus_menu_item_set_submenu (top, sub);
  g_object_unref (sub);
  return top;
}

/* The "Upscale" entry: a submenu 2× / 3× / 4× / 5× / 6× / 8× (item names
 * "FileConverter::Upscale::<key>", e.g. "...::4x") of the presets whose result fits
 * the limits. The tooltip says the exact size when one file of known size is
 * selected. */
static NautilusMenuItem *
upscale_item_new (const NfcSelection *sel)
{
  NautilusMenuItem *top = nautilus_menu_item_new ("FileConverter::Upscale", "Upscale",
                                                  "Make an AI-upscaled copy next to the original "
                                                  "(Upscayl, runs on the GPU)", NULL);
  NautilusMenu *sub = nautilus_menu_new ();
  gboolean one = sel->paths->len == 1 && sel->dims != NULL && sel->dims->len == 1;

  for (guint k = 0; k < sel->upscale->len; k++)
    {
      const NfcUpscalePreset *p = sel->upscale->pdata[k];
      char *name = g_strdup_printf ("FileConverter::Upscale::%s", p->key);
      const NfcDims *d = one ? &g_array_index (sel->dims, NfcDims, 0) : NULL;
      char *tip = nfc_upscale_tooltip (p, d != NULL ? d->width : 0, d != NULL ? d->height : 0);
      NautilusMenuItem *item = nautilus_menu_item_new (name, p->label, tip, NULL);
      ActivateData *data = g_new0 (ActivateData, 1);

      data->cat = sel->cat;
      data->upscale_key = g_strdup (p->key);
      data->key = g_strdup ("");
      data->label = g_strdup ("Upscale");
      data->ext = g_strdup ("");
      activate_data_copy_selection (data, sel);
      g_signal_connect_data (item, "activate", G_CALLBACK (on_activate), data,
                             activate_data_free, 0);
      nautilus_menu_append_item (sub, item);
      g_object_unref (item);
      g_free (tip);
      g_free (name);
    }
  nautilus_menu_item_set_submenu (top, sub);
  g_object_unref (sub);
  return top;
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
  if (ok && sel.targets->len > 0)
    {
      NautilusMenuItem *top = nautilus_menu_item_new ("FileConverter::Convert", "Convert to",
                                                      NULL, NULL);
      NautilusMenu *sub = nautilus_menu_new ();

      if (top != NULL && sub != NULL)
        {
          for (guint k = 0; k < sel.targets->len; k++)
            {
              NautilusMenuItem *item = target_item_new (&sel, sel.targets->pdata[k]);

              nautilus_menu_append_item (sub, item);
              g_object_unref (item);
            }
          nautilus_menu_item_set_submenu (top, sub);
          result = g_list_append (result, top);
        }
      else
        g_clear_object (&top);
      g_clear_object (&sub);
    }
  if (ok && sel.can_compress)
    result = g_list_append (result, compress_item_new (&sel));
  if (ok && sel.resize != NULL && sel.resize->len > 0)
    result = g_list_append (result, resize_item_new (&sel));
  if (ok && sel.upscale != NULL && sel.upscale->len > 0)
    result = g_list_append (result, upscale_item_new (&sel));
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
