/* nfc-decode: decode an image with GdkPixbuf in its own process.
 *
 *   nfc-decode <input> <output.png>   decode (EXIF orientation applied), write PNG
 *   nfc-decode --formats              list the GdkPixbuf formats that can be read
 *
 * Why a separate executable: HEIC/AVIF files are untrusted input and the
 * loaders (libheif, glycin, ...) are large C/Rust codebases. A crash or hang
 * in there must not take the Nautilus process down, so the extension only ever
 * runs this helper through GSubprocess (with a timeout) and never links
 * gdk-pixbuf itself.
 *
 * Exit status: 0 ok, 1 decode/write error (message on stderr), 2 usage.
 */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <string.h>

static int
list_formats (void)
{
  GSList *formats = gdk_pixbuf_get_formats ();

  for (GSList *l = formats; l; l = l->next)
    {
      GdkPixbufFormat *f = l->data;
      char *name;

      if (gdk_pixbuf_format_is_disabled (f))
        continue;
      name = gdk_pixbuf_format_get_name (f);
      printf ("%s\n", name);
      g_free (name);
    }
  g_slist_free (formats);
  return 0;
}

static int
decode (const char *in, const char *out)
{
  GError *error = NULL;
  GdkPixbuf *p, *o;
  gboolean ok;
  int rc = 0;

  p = gdk_pixbuf_new_from_file (in, &error);
  if (p == NULL)
    {
      fprintf (stderr, "cannot decode %s: %s\n", in, error->message);
      g_error_free (error);
      return 1;
    }
  o = gdk_pixbuf_apply_embedded_orientation (p);
  if (o == NULL)
    o = g_object_ref (p);
  ok = gdk_pixbuf_save (o, out, "png", &error, NULL);
  if (!ok)
    {
      fprintf (stderr, "cannot write %s: %s\n", out, error->message);
      g_error_free (error);
      rc = 1;
    }
  g_object_unref (o);
  g_object_unref (p);
  return rc;
}

int
main (int argc, char **argv)
{
  if (argc == 2 && strcmp (argv[1], "--formats") == 0)
    return list_formats ();
  if (argc == 3 && argv[1][0] != '-')
    return decode (argv[1], argv[2]);
  fprintf (stderr, "usage: nfc-decode <input> <output.png> | nfc-decode --formats\n");
  return 2;
}
