/* LD_PRELOAD shim for test-publish: while NFC_SHIM_RENAME_EEXIST is set in the
 * environment, rename() fails with EEXIST, like a (buggy) filesystem answering
 * "exists" to an overwriting rename. Otherwise it calls the real rename(). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int
rename (const char *from, const char *to)
{
  static int (*real) (const char *, const char *);

  if (getenv ("NFC_SHIM_RENAME_EEXIST") != NULL)
    {
      errno = EEXIST;
      return -1;
    }
  if (real == NULL)
    real = (int (*) (const char *, const char *)) dlsym (RTLD_NEXT, "rename");
  return real (from, to);
}
