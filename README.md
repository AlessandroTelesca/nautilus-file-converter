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
| Image | png, jpg/jpeg, webp, avif, heic/heif, bmp, tiff, gif, ico, svg | PNG, JPG, WEBP, AVIF, ICO (max 256 px), BMP, TIFF, PDF | ImageMagick (+ GdkPixbuf for HEIC/AVIF input, ffmpeg/libaom for AVIF output) |
| Audio | mp3, wav, flac, ogg, opus, m4a, aac, wma | MP3 (LAME V2), FLAC, WAV, OGG (Vorbis), OPUS (128k), M4A (AAC 192k) | ffmpeg |
| Video | mp4, mkv, webm, mov, avi, wmv, flv, m4v | MP4 (H.264 CRF 23 + AAC), MKV (stream copy, falls back to re-encode), WebM (VP9 CRF 32 + Opus), GIF (12 fps, 480 px wide), MP3 / M4A (audio only) | ffmpeg |
| Document | doc, docx, odt, rtf, txt | PDF, DOCX, ODT | LibreOffice |
| Spreadsheet | xls, xlsx, ods, csv | PDF, XLSX, ODS, CSV (first sheet) | LibreOffice |
| Presentation | ppt, pptx, odp | PDF, PPTX, ODP | LibreOffice |

Notes:

* HEIC output is not offered (no encoder in the standard ImageMagick build).
* Transparency is flattened onto white for JPG, PDF and AVIF.
* GIF/TIFF/ICO/PDF sources: only the first frame/page is converted to images.
* HEIC/AVIF input is decoded in-process through GdkPixbuf (EXIF orientation is
  applied) because ImageMagick on Fedora cannot read them. AVIF output goes
  through ffmpeg (`libaom-av1`) after ImageMagick has flattened the image.
* Every ImageMagick output uses an explicit format prefix (`PNG:`, `JPEG:`, ...)
  so a name like `.avif` can never silently end up holding PNG data.
* A category is silently omitted if its tool is missing, and individual
  targets are omitted if the required codec is missing (e.g. no libvorbis).
  Tools are looked up once at startup; ffmpeg's encoders are probed once in the
  background.
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
* HEIC/AVIF input needs a GdkPixbuf HEIF loader. On Fedora 44 `gdk-pixbuf2`
  loads images through `glycin-loaders` and `libheif` (plus `libheif-freeworld`
  for HEVC-based HEIC); all three were confirmed installed with `rpm -q` on the
  author's machine. On other releases the package may instead be
  `heif-pixbuf-loader`; that package name is unverified.

## Install / uninstall

Nautilus only loads C extensions from its system directory
(`pkg-config --variable=extensiondir libnautilus-extension-4`, e.g.
`/usr/lib64/nautilus/extensions-4`), which is root-owned, so installing needs
`sudo`.

```
./install.sh      # meson setup + compile in ./build, `sudo meson install`,
                  # removes the old nautilus-python version if present,
                  # then restarts Nautilus (nautilus -q)
./uninstall.sh    # removes libnautilus-file-converter.so (sudo), nautilus -q
```

Reopen Files afterwards.

Manual build:

```
meson setup build --buildtype=release
meson compile -C build
sudo meson install -C build
```

## Behaviour

* **Output names**: written next to the source with the new extension.
  Existing files are never overwritten: `name.pdf`, then `name (1).pdf`,
  `name (2).pdf`, ... Output goes to a hidden temporary file in the destination
  directory first and is hard-linked into place under the first free name
  (`link()` fails with `EEXIST`, so concurrent conversions cannot clobber each
  other), then the temporary file is removed. No hidden files are left behind,
  on success or failure.
* **Asynchronous**: conversions run as `GSubprocess` children and never block
  Nautilus. Images/audio run up to CPU/2 files in parallel, video at most 2,
  documents one at a time.
* **Notifications**: a "Converting..." notification, replaced by a summary
  ("Converted 3 files to MP4") or, on failure, the file name and the last line
  of the tool's error output.
* **Log**: full command and stderr of every failure go to
  `~/.cache/nautilus-file-converter.log`.

## Tests

```
meson setup build
meson test -C build
```

* `core-logic`: classification, output naming, command lines.
* `module-load-and-menu`: loads the built `.so` without installing it, checks
  the three Nautilus entry points and that the registered type implements
  `NautilusMenuProvider`, then drives the real menu provider with fake
  `NautilusFileInfo` objects (single file, same-category selection, mixed
  categories, directory, non-`file://` URI, unknown type).
* `conversions`: generates fixtures with ImageMagick / ffmpeg / LibreOffice,
  runs every source-to-target pair for real and checks that the output exists,
  is non-empty and has the correct *real* type (magic bytes via `file`). Set
  `NFC_HEIC_SAMPLE=/path/photo.heic` to include HEIC input.

## License

MIT
