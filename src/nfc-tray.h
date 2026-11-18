/* nautilus-file-converter: a tray icon while a job is in flight (GLib/GIO only).
 *
 * A minimal StatusNotifierItem (the protocol of the desktop's tray: KDE, GNOME's
 * AppIndicator extension, ...) spoken directly over GDBus, with a minimal
 * com.canonical.dbusmenu for its menu: one disabled line per job in flight and
 * "Cancel all". No GTK, no libdbusmenu, no libappindicator.
 *
 * The icon appears (after a short delay, so that a conversion of a few hundred
 * milliseconds never shows it) when the first job starts and disappears (after a
 * short debounce) when the last one has ended. Everything is asynchronous and on
 * the main thread. Without a StatusNotifierWatcher on the session bus, or when
 * any bus call fails, nothing happens and nothing is reported. The watcher
 * appearing later registers the icon then.
 *
 * Switched off by "tray=false" in ~/.config/nautilus-file-converter/config.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The value of "tray=" in the text of a configuration file (lines "key=value",
 * "#" comments, blanks trimmed; the last tray line wins): FALSE for false, no, off
 * or 0 (any case), TRUE otherwise, also when absent or text is NULL. */
gboolean nfc_tray_config_enabled (const char *config_text);

/* Start observing the jobs (nfc_activity_set_observer). enabled FALSE: do nothing
 * at all. Call once, from the main thread. */
void nfc_tray_start (gboolean enabled);

/* nfc_tray_start with the setting of the user's configuration file (read once, here). */
void nfc_tray_init (void);

/* Remove the icon and stop observing. Safe to call without a start, and twice. */
void nfc_tray_shutdown (void);

/* Is the item currently on the bus (whether or not a watcher took it)? */
gboolean nfc_tray_is_shown (void);

G_END_DECLS
