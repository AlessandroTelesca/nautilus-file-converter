/* nfc_publish_with_mode must always terminate.
 *
 *  - rename-eexist: with the LD_PRELOAD shim (tests/rename-eexist-shim.c) every
 *    rename() fails with EEXIST. In the last-resort mode (no link(), no
 *    renameat2) that used to make the name loop spin forever: it has to fail
 *    with an error within a second, and leave no placeholder behind. Skipped
 *    when the shim is not preloaded (NFC_SHIM_ACTIVE unset).
 *  - name-cap: when the first 10000 names are all taken, give up with an
 *    error (no shim needed).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfc-core.h"

static char *dir;

static guint
count_files (void)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  guint c = 0;

  while (d != NULL && g_dir_read_name (d) != NULL)
    c++;
  if (d != NULL)
    g_dir_close (d);
  return c;
}

static char *
make_tmp (void)
{
  char *p = g_build_filename (dir, ".tmp-source", NULL);

  g_assert_true (g_file_set_contents (p, "data", -1, NULL));
  return p;
}

static void
test_rename_eexist (void)
{
  char *tmp, *res;
  GError *error = NULL;
  gint64 t0;

  if (g_getenv ("NFC_SHIM_ACTIVE") == NULL)
    {
      g_test_skip ("rename() shim not preloaded");
      return;
    }
  tmp = make_tmp ();
  g_setenv ("NFC_SHIM_RENAME_EEXIST", "1", TRUE);
  t0 = g_get_monotonic_time ();
  res = nfc_publish_with_mode (tmp, dir, "out", "png", NFC_PUBLISH_NO_LINK_NO_RENAME2, &error);
  t0 = g_get_monotonic_time () - t0;
  g_unsetenv ("NFC_SHIM_RENAME_EEXIST");

  g_assert_null (res);
  g_assert_nonnull (error);
  g_print ("rename-eexist: failed in %.3f s: %s\n", t0 / 1e6, error->message);
  g_assert_cmpint (t0, <, G_USEC_PER_SEC);
  g_assert_true (g_file_test (tmp, G_FILE_TEST_EXISTS)); /* source untouched */
  g_assert_cmpuint (count_files (), ==, 1);              /* no placeholder left */
  g_clear_error (&error);

  /* shim off again: the same call works */
  res = nfc_publish_with_mode (tmp, dir, "out", "png", NFC_PUBLISH_NO_LINK_NO_RENAME2, &error);
  g_assert_no_error (error);
  g_assert_nonnull (res);
  g_assert_true (g_str_has_suffix (res, "/out.png"));
  g_assert_cmpuint (count_files (), ==, 1);
  g_unlink (res);
  g_free (res);
  g_free (tmp);
}

static void
test_name_cap (void)
{
  char *tmp = make_tmp (), *res;
  GError *error = NULL;
  gint64 t0;

  for (guint n = 0; n < 10000; n++)
    {
      char *name = nfc_candidate_name ("cap", "png", n);
      char *p = g_build_filename (dir, name, NULL);

      g_assert_true (g_file_set_contents (p, "x", -1, NULL));
      g_free (name);
      g_free (p);
    }
  t0 = g_get_monotonic_time ();
  res = nfc_publish_with_mode (tmp, dir, "cap", "png", NFC_PUBLISH_AUTO, &error);
  t0 = g_get_monotonic_time () - t0;
  g_assert_null (res);
  g_assert_nonnull (error);
  g_print ("name-cap: failed in %.3f s: %s\n", t0 / 1e6, error->message);
  g_assert_nonnull (strstr (error->message, "all taken"));
  g_assert_true (g_file_test (tmp, G_FILE_TEST_EXISTS));
  g_clear_error (&error);
  g_free (tmp);
}

int
main (int argc, char **argv)
{
  char *tmpl;
  int rc;

  g_test_init (&argc, &argv, NULL);
  alarm (20); /* a regression is an endless loop: fail instead of hanging */
  tmpl = g_build_filename (g_get_tmp_dir (), "nfc-publish-XXXXXX", NULL);
  dir = g_mkdtemp (tmpl);
  g_assert_nonnull (dir);

  g_test_add_func ("/publish/rename-eexist", test_rename_eexist);
  g_test_add_func ("/publish/name-cap", test_name_cap);
  rc = g_test_run ();
  {
    char *rm[] = { "rm", "-rf", dir, NULL };

    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  }
  g_free (dir);
  return rc;
}
