# nautilus-file-converter

A "Convert to" submenu for Nautilus (GNOME Files), similar to the Windows app
*File Converter*. Select one or more files, right-click, choose a target
format; the converted copies appear next to the originals.

Written in C against `libnautilus-extension-4` (Nautilus 43+ / GTK 4) and built
with meson. There is no scripting-language runtime involved: no Python, no
`nautilus-python`.

## Supported conversions

The submenu appears only when every selected item is a regular local file
(`file://`, not a directory) and all of them are in the same category. The
file's own format is never offered. Files are classified by extension, with the
MIME type as a fallback.

| Category | Input | Targets | Backend |
|---|---|---|---|
| Image | png, jpg/jpeg, webp, avif, heic/heif, bmp, tiff, gif, ico, svg | PNG, JPG, WEBP, AVIF, ICO (max 256 px), BMP, TIFF, PDF | ImageMagick (+ the `nfc-decode` helper, GdkPixbuf, for HEIC/AVIF input; ffmpeg/libaom for AVIF output) |
| Audio | mp3, wav, flac, ogg, opus, m4a, aac, wma | MP3 (LAME V2), FLAC, WAV, OGG (Vorbis), OPUS (128k), M4A (AAC 192k) | ffmpeg |
| Video | mp4, mkv, webm, mov, avi, wmv, flv, m4v | MP4 (H.264 CRF 23 + AAC), MKV (stream copy, falls back to re-encode), WebM (VP9 CRF 32 + Opus), GIF (12 fps, 480 px wide), MP3 / M4A (audio only) | ffmpeg |
| Document | doc, docx, odt, rtf, txt | PDF, DOCX, ODT | LibreOffice |
| Spreadsheet | xls, xlsx, ods, csv | PDF, XLSX, ODS, CSV (first sheet) | LibreOffice |
| Presentation | ppt, pptx, odp | PDF, PPTX, ODP | LibreOffice |

Notes:

* HEIC output is not offered (no encoder in the standard ImageMagick build).
* Transparency is flattened onto white for JPG, PDF and AVIF.
* GIF/TIFF/ICO/PDF sources: only the first frame/page is converted to images.
* HEIC/AVIF input is decoded by `nfc-decode`, a small helper executable that
  uses GdkPixbuf (EXIF orientation is applied), because ImageMagick on Fedora
  cannot read them. It is a separate process on purpose: these are untrusted
  files and a crash in libheif/glycin must not take Nautilus down; the
  extension itself does not link gdk-pixbuf. If the helper is not installed,
  HEIC/AVIF input is simply not offered. AVIF output goes through ffmpeg
  (`libaom-av1`) after ImageMagick has flattened the image.
* Every ImageMagick output uses an explicit format prefix (`PNG:`, `JPEG:`, ...)
  so a name like `.avif` can never silently end up holding PNG data, and every
  input gets an explicit coder prefix chosen from the file's classified type
  (`PNG:`, `JPEG:`, `SVG:`, ...), so ImageMagick never picks a coder by
  sniffing content (an image-named file that is really PostScript, MVG or MSL
  does not reach Ghostscript or the script coders). The flip side: a file whose
  extension lies about its format (a PNG named `.jpg`) fails to convert.
  ImageMagick also runs with resource limits (`-limit memory 1GiB map 2GiB
  disk 4GiB time 600`); larger images spill to disk. File names are passed
  with `%` escaped, so names like `a%d.png` or `100%.png` work.
* A category is silently omitted if its tool is missing, and individual
  targets are omitted if the required codec is missing (e.g. no libvorbis).
  Tools are looked up once at startup; ffmpeg's encoders, ImageMagick's
  formats and GdkPixbuf's loaders are probed in the background (see
  "Never blocks Nautilus" below).
* LibreOffice runs headless with its own throw-away profile, so conversion
  works while LibreOffice is open.

## Dependencies

Build (Fedora / Nobara):

```
sudo dnf install gcc meson nautilus-devel gdk-pixbuf2-devel
```

Runtime tools (each one is optional; a missing tool just hides its category):

```
sudo dnf install ImageMagick ffmpeg libreoffice libnotify
```

* `ImageMagick` (`magick`, or `convert` for IM6): image conversion
* `ffmpeg` (the full build, e.g. from RPM Fusion, for libx264/libmp3lame/libopus...)
* `libreoffice` (provides `soffice`): documents
* `libnotify` (`notify-send`): result notifications, replaced in place using
  `--print-id` / `--replace-id`
* HEIC/AVIF input needs a GdkPixbuf HEIF loader (used by `nfc-decode`). On Fedora 44 `gdk-pixbuf2`
  loads images through `glycin-loaders` and `libheif` (plus `libheif-freeworld`
  for HEVC-based HEIC); all three were confirmed installed with `rpm -q` on the
  author's machine. On other releases the package may instead be
  `heif-pixbuf-loader`; that package name is unverified.

## Install / uninstall

Nautilus only loads C extensions from its system directory
(`pkg-config --variable=extensiondir libnautilus-extension-4`, e.g.
`/usr/lib64/nautilus/extensions-4`), which is root-owned, so installing needs
`sudo`. Two files are installed:

| File | Where |
|---|---|
| `libnautilus-file-converter.so` | the Nautilus extension dir (above) |
| `nfc-decode` (HEIC/AVIF decoder, GdkPixbuf format list) | `<prefix>/<libexecdir>`, i.e. `/usr/libexec` with `--prefix=/usr` |

```
./install.sh      # meson setup (--prefix=/usr) + compile in ./build AS YOU,
                  # then `sudo meson install --no-rebuild`, removes the old
                  # nautilus-python version if present, then nautilus -q
./uninstall.sh    # sudo rm of the .so and nfc-decode (paths from pkg-config
                  # and the configured meson libexecdir), then nautilus -q
```

`install.sh` refuses to run as root: it calls `sudo` itself, only for the
install step, so `./build` is never left with root-owned `.ninja_log` or
`.ninja_deps`. `uninstall.sh` builds nothing; only the `rm` is privileged.
Reopen Files afterwards.

Manual build:

```
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
sudo meson install --no-rebuild -C build
```

The extension finds the helper at the install path compiled in from the meson
prefix (`config.h`); `NFC_DECODE_HELPER=/path/to/nfc-decode` overrides it (the
tests use that to run from the build directory).

## Never blocks Nautilus

The extension runs inside the Nautilus process, so nothing it does may block
the main thread or crash the process:

* **Right-click menu** (`get_file_items`): never waits and never runs a tool.
  It stats the selected files and consults capability probes that run in
  background threads (one each for `ffmpeg -encoders`, `magick -list format`
  and `nfc-decode --formats`). Documents, spreadsheets and presentations need
  no probe and get their menu immediately. Image, audio and video targets
  appear as soon as the probe they need has finished (normally well under a
  second after Nautilus starts); until then those targets are left out, and a
  selection with nothing known yet gets no menu at all (rather than an empty or
  half-built one). Because the probes are independent, a hung `ffmpeg` does not
  hide image targets that only need ImageMagick.
* **Probe timeout**: each probe is killed (with its whole process group) after
  10 s. A probe that times out or fails counts as "tool unavailable" for the
  rest of the session and is logged.
* **Conversions** are asynchronous `GSubprocess` children, each in its own
  process group and session. Orphans: on Linux every child gets
  `PR_SET_PDEATHSIG=SIGKILL`, so the kernel kills it if Nautilus dies (this is
  tied to the spawning thread, which for conversions is the main thread). On a
  clean module shutdown, in-flight jobs are killed, and their work directories
  and hidden temp files are removed (best effort).
* **Decoding untrusted HEIC/AVIF** happens in the `nfc-decode` process, never
  inside Nautilus.

### Timeouts

Per tool invocation (a conversion can have two or three: decode, flatten,
encode): images 600 s, audio and documents 1800 s, **video 1800 s plus 60 s per
100 MiB of input, capped at 4 h** (so a long film can finish, while a hung
ffmpeg is eventually killed). On a timeout the step is cancelled, the whole
process group is killed with SIGKILL (LibreOffice's `soffice` is a wrapper
script whose `soffice.bin` child would otherwise keep running and hold the
pipes open), the temp files are removed, and the job fails with "timed out"
(no fallback attempt follows a timeout). `NFC_TEST_TIMEOUT_SECS` and
`NFC_TEST_PROBE_TIMEOUT_SECS` override the timeouts; they exist for the tests
and are only honoured when set.

## Behaviour

* **Output names**: written next to the source with the new extension.
  Existing files are never overwritten: `name.pdf`, then `name (1).pdf`,
  `name (2).pdf`, ... Output goes to a hidden temporary file in the destination
  directory first and is hard-linked into place under the first free name
  (`link()` fails with `EEXIST`, so concurrent conversions cannot clobber each
  other), then the temporary file is removed. On filesystems without hard links
  it uses `renameat2(RENAME_NOREPLACE)`, and where that is unsupported too an
  `O_CREAT|O_EXCL` placeholder followed by `rename()`; in no case is an
  existing file replaced. No hidden files are left behind, on success, failure,
  timeout or shutdown.
* **Asynchronous**: conversions run as `GSubprocess` children and never block
  Nautilus. Images/audio run up to CPU/2 files in parallel, video at most 2,
  documents one at a time.
* **Notifications**: a "Converting..." notification, replaced by a summary
  ("Converted 3 files to MP4") or, on failure, the file name and the last line
  of the tool's error output.
* **Log**: full command and stderr of every failure, and probe timeouts, go to
  `$XDG_CACHE_HOME/nautilus-file-converter.log` (`~/.cache/...`). The file is
  created with mode 0600 (and narrowed if it existed with wider permissions).
  Above 1 MiB it is rotated to `nautilus-file-converter.log.1` (one backup,
  replaced on each rotation).

## Tests

```
meson setup build
meson test -C build
```

* `core-logic`: classification, output naming, command lines (input coders, `%`
  escaping, resource limits, LibreOffice URIs), no-overwrite publishing in all
  three strategies, and the `nfc-decode` helper (formats, a decode, garbage
  input, usage errors).
* `robustness`: all with fake tools and a `TMPDIR` containing a space and `%`:
  a fake `ffmpeg` that hangs for 20 s must not slow the menu logic (< 100 ms
  for every selection) and is killed by the probe timeout and logged; a fake
  `soffice` of the `sleep 30 & exec sleep 30` kind must fail within timeout +
  2 s with no surviving process from its group and no leaked work directory;
  partial output and hidden temp files are removed on timeout and on failure;
  in-flight jobs are cancelled on shutdown; the log is 0600 and rotates.
* `module-load-and-menu`: loads the built `.so` without installing it, checks
  the three Nautilus entry points and that the registered type implements
  `NautilusMenuProvider`, then drives the real menu provider with fake
  `NautilusFileInfo` objects (single file, same-category selection, mixed
  categories, directory, non-`file://` URI, unknown type).
* `module-probe-freeze`: the same, end to end, with a hanging fake `ffmpeg`
  first in `PATH`: right-clicking a `.txt` returns in microseconds.
* `conversions`: generates fixtures with ImageMagick / ffmpeg / LibreOffice,
  runs every source-to-target pair for real and checks that the output exists,
  is non-empty and has the correct *real* type (magic bytes via `file`). Set
  `NFC_HEIC_SAMPLE=/path/photo.heic` to include HEIC input. Runs under a
  `TMPDIR` containing a space and `%`, and includes file names such as `a%d.png`,
  `%[filename].png`, `100%.png`, `dir%d/x.png`, `a[1].png`, `a*b.png`,
  `@list.png`, `a:b.png`, and a fake `notify-send` that rejects a leading-dash
  argument unless it follows `--`.

## License

MIT
