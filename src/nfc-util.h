/* nautilus-file-converter: small helpers shared by the core and the runner
 * (logging, child process setup). GLib/GIO only. */
#pragma once

#include <gio/gio.h>
#include <sys/types.h>

G_BEGIN_DECLS

/* Append a timestamped line to $XDG_CACHE_HOME/nautilus-file-converter.log.
 * The file is created with mode 0600 (and narrowed if it already exists with
 * wider permissions); above 1 MiB it is rotated to ...log.1 (single backup). */
void nfc_log (const char *format, ...) G_GNUC_PRINTF (1, 2);

/* A launcher whose children
 *  - get /dev/null as stdin,
 *  - run in their own session / process group (so a timeout can kill the
 *    whole tree with nfc_kill_group(), not only the direct child),
 *  - are killed by the kernel when the spawning thread dies (Linux
 *    PR_SET_PDEATHSIG; note: that is the parent THREAD, not the process, so
 *    spawn from a thread that outlives the child; the main thread for
 *    conversions, the probe thread for probes). */
GSubprocessLauncher *nfc_launcher_new (GSubprocessFlags flags);

/* Process group id of a subprocess made by nfc_launcher_new (0 if unknown or
 * already reaped). */
pid_t nfc_subprocess_pgid (GSubprocess *proc);

/* SIGKILL every process of group pgid (no-op for pgid <= 1). */
void nfc_kill_group (pid_t pgid);

G_END_DECLS
