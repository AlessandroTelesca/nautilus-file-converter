# nautilus-file-converter

A "Convert to" submenu for Nautilus (GNOME Files), similar to the Windows app
*File Converter*. Select one or more files, right-click, choose a target
format; the converted copies appear next to the originals. For JPG, WEBP, AVIF,
MP4, WebM and GIF the target opens a small submenu of quality presets (Best, High,
Balanced, Small; see "Quality presets" below).

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
| Image | png, jpg/jpeg, webp, avif, heic/heif, bmp, tiff, gif, ico, svg | PNG, JPG, WEBP, AVIF, ICO (256/48/32/16 px, never enlarged), BMP, TIFF, PDF (see "Image quality" below) | ImageMagick (+ the `nfc-decode` helper, GdkPixbuf, for HEIC/AVIF input; ffmpeg/libaom for AVIF output) |
| Audio | mp3, wav, flac, ogg, opus, m4a, aac, wma | MP3 (LAME V2), FLAC, WAV, OGG (Vorbis), OPUS (128k), M4A (AAC 192k) | ffmpeg |
| Video | mp4, mkv, webm, mov, avi, wmv, flv, m4v | MP4 (H.264 CRF 18, preset slow, + AAC 192k; libopenh264 at about 3 bits per pixel per second where libx264 is missing), MKV (stream copy, falls back to re-encode), WebM (VP9 CRF 28 + Opus 128k), GIF (15 fps, up to 960 px wide), MP3 / M4A (audio only; a video without an audio track fails with "it has no audio track") | | ffmpeg |
| Document | doc, docx, odt, rtf, txt | PDF, DOCX, ODT | LibreOffice |
| Spreadsheet | xls, xlsx, ods, csv | PDF, XLSX, ODS, CSV (first sheet) | LibreOffice |
| Presentation | ppt, pptx, odp | PDF, PPTX, ODP | LibreOffice |

Notes:

* HEIC output is not offered (no encoder in the standard ImageMagick build).
* HEIC input is offered only when it can really be decoded: glycin and libheif
  list `heic` even without an HEVC decoder (stock Fedora without
  `libheif-freeworld`), so `nfc-decode --formats` decodes a built-in 16x16
  HEVC sample (a synthetic gradient) and leaves `heic` out of the list if that
  fails. If such a file is converted anyway, the toast says "can't decode HEIC:
  the HEVC decoder is missing (install libheif-freeworld from RPM Fusion)".
* The toast of a failed conversion shows the line of the tool's stderr that
  names the cause (not simply the last one), without program names and
  ImageMagick's `@ error/file.c/Function/NNN` suffix, and in plain words for
  known causes (missing HEVC decoder, a broken ffmpeg decoder, "image too large
  for available memory", "disk full"). The full stderr is in the log.
* Notifications all have urgency normal (a critical one stays until dismissed
  and the later "Converting…" / "Conversion complete" banners would queue
  behind it); the success icon is `object-select-symbolic` (`emblem-ok-symbolic`
  is gone from Adwaita 50).
* MP4 (and the MKV re-encode) need an H.264 encoder: libx264, or libopenh264,
  which stock Fedora's `ffmpeg-free` has (with the `openh264` package). With
  both, libx264 is tried first and libopenh264 is the next attempt.
  libopenh264 only has a bitrate mode: 3 bits per pixel per second, from the
  picture size (`ffprobe`), 0.5 to 40 Mbit/s (6.2 at 1080p; 1080p is assumed if
  `ffprobe` is missing).
* Transparency is flattened onto white for JPG, PDF and AVIF.
* GIF/TIFF/ICO/PDF sources: only the first frame/page is converted to images.
* HEIC/AVIF input is decoded by `nfc-decode`, a small helper executable that
  uses GdkPixbuf (EXIF orientation is applied), because ImageMagick on Fedora
  cannot read them. It is a separate process on purpose: these are untrusted
  files and a crash in libheif/glycin must not take Nautilus down; the
  extension itself does not link gdk-pixbuf. If the helper is not installed,
  HEIC/AVIF input is simply not offered. AVIF output goes through ffmpeg
  (`libaom-av1`) after ImageMagick has flattened the image.
  The helper refuses images above 100 megapixels or 30000 pixels on a side
  (read from the header, before any pixel is allocated; exit status 1 and a
  message on stderr), so a tiny file cannot claim a gigantic picture. There is
  no address-space rlimit because glycin starts sandboxed loader processes.
* Every ImageMagick output uses an explicit format prefix (`PNG:`, `JPEG:`, ...)
  so a name like `.avif` can never silently end up holding PNG data, and every
  input gets an explicit coder prefix chosen from the file's classified type
  (`PNG:`, `JPEG:`, `RSVG:`, ...), so ImageMagick never picks a coder by
  sniffing content (an image-named file that is really PostScript, MVG or MSL
  does not reach Ghostscript or the script coders). The flip side: a file whose
  extension lies about its format (a PNG named `.jpg`) fails to convert.
  ImageMagick also runs with resource limits: memory a quarter of the RAM
  (between 1 and 8 GiB), map twice that, disk 16 GiB, time 600 s. A larger
  image spills to a mapped file and then to disk instead of failing: a
  19200 x 10800 RGBA screenshot (207 megapixels, 3.3 GB of pixel cache) used to
  die with "cache resources exhausted" under the old 1 GiB / 2 GiB / 4 GiB
  limits and now converts to every target (peak 5.7 GB RSS, 8.4 GB for WEBP;
  each parallel conversion of such an image needs that much). See
  "Memory: admission and temp files" below for how several of them are kept from
  running at once. WEBP cannot
  store more than 16383 pixels per side: wider images are shrunk to fit. File names are passed
  with `%` escaped, so names like `a%d.png` or `100%.png` work.
* **SVG input uses `RSVG:`**, never the plain `SVG:` coder. Depending on the
  build, ImageMagick delegates `SVG:` to an external renderer (Inkscape on
  Fedora: a child process per conversion that follows some references). `RSVG:`
  is librsvg inside the `magick` process: no helper process, much faster, and it
  does not embed `file://`, `text:` or relative `href` targets. The coder is
  chosen from `magick -list format` (the probe records readable coders too); if
  `RSVG` is not listed the fallback is ImageMagick's own `MSVG:` renderer, which
  refuses explicit coders inside the SVG. No `policy.xml` is installed or
  needed.
* A category is silently omitted if its tool is missing, and individual
  targets are omitted if the required codec is missing (e.g. no libvorbis).
  Tools are looked up once at startup; ffmpeg's encoders, ImageMagick's
  formats and GdkPixbuf's loaders are probed in the background (see
  "Never blocks Nautilus" below).
* LibreOffice runs headless with its own throw-away profile, so conversion
  works while LibreOffice is open.

### Image and video quality

The settings were chosen by measurement (SSIM against the source, on real
screenshots and photos), not by habit. Sources are split into *photos* (jpg,
heic, avif, webp: lossy, camera or phone) and *graphics* (everything else:
screenshots, drawings, icons, scans), because text and colour edges need much
more than a photo does.

| Target | Graphics source (png, bmp, tiff, gif, ico, svg) | Photo source |
|---|---|---|
| JPG | quality 99, 4:4:4 | quality 95, 4:4:4 |
| WEBP | lossless (smaller than the source PNG for every screenshot tried) | lossy quality 90, method 6, sharp YUV |
| AVIF | libaom 4:4:4, CRF 9, 8-bit | libaom 4:2:0, CRF 12, 8-bit |
| PDF, PNG, BMP, TIFF | lossless | lossless |
| ICO | 256, 48, 32 and 16 px entries that do not enlarge the source (a 16 px icon gives one 16 px entry, 87x98 gives 98, 48, 32, 16), aspect ratio kept (padded transparently to a square) | same |

* JPG is always 4:4:4, stated explicitly (ImageMagick only uses it on its own
  from quality 90 up). 4:2:0 smears the colour edges of screenshots.
* AVIF: RGB is converted to YCbCr with the BT.709 matrix in full range and the
  file is tagged exactly that, so viewers do not guess BT.601 / limited range
  and shift the colours. Odd image sizes are kept as they are (the old
  even-size scale resampled every odd-sized image by a pixel). If libaom
  refuses the 4:4:4 encode of a gigantic image a second attempt uses 4:2:0.
  `-tiles` is not set: AV1 tiles are at most 4096 px wide and libaom picks as
  many as the width needs.
* WEBP stores at most 16383 pixels per side: larger images are shrunk to fit.
* PDF: ImageMagick embeds PNG-type sources losslessly (Flate) and JPEG
  sources as JPEG at their own quality; nothing is recompressed to a lower
  quality.
* PDF page size: a screenshot has no resolution, so ImageMagick would write 72
  dpi and a 1920 px capture would become a 26 inch page. The source resolution
  is read with the size probe (`identify -ping -format '%x %y %U'`); when there
  is none (units undefined, or the 72 dpi that everything stamps on a file),
  `-units PixelsPerInch -density N` is set so that the page fits an A4 sheet
  (landscape for a wide image, portrait for a tall one; N is the smallest whole
  dpi that does it, but at least 96, so an icon stays small): 1920 x 1080 gets
  165 dpi, a 837.8 x 471.3 pt page instead of 1920 x 1080 pt. Only the density
  is set, the pixels are embedded untouched. An image with a real resolution (a
  300 dpi scan, a 96 dpi screen capture that says so) keeps it, and SVG keeps
  its 192 dpi rendering.
* HEIC input: decoded by GdkPixbuf (glycin) to an 8-bit sRGB PNG with the EXIF
  orientation applied; the colour profile is applied by the loader (the pixels
  differ from a raw decode by the P3 to sRGB conversion). Bit depth above 8 is
  lost. This is the only loss before the target encoder.
* Video: H.264 CRF 18 with `-preset slow` (libx264; libopenh264 at a bitrate if that is all there is), 4:2:0, `+faststart`, audio AAC
  192k; VP9 CRF 28 with `-deadline good -cpu-used 2 -row-mt 1`, Opus 128k;
  GIF at 15 fps up to 960 px wide with a per-frame difference palette and
  ordered (Bayer 5) dithering. Error diffusion (Sierra) was measured at 5 times
  the size and a lower SSIM, no dithering at the same size with banding.
  Odd frame sizes are cropped by one pixel, not scaled.
* Audio settings are unchanged: MP3 `-q:a 2`, Opus 128k, AAC 192k.
* A video with no audio track converts to MP4, MKV, WebM and GIF as usual
  (audio is mapped optionally). Converting it to MP3 or M4A fails at once with
  "it has no audio track" instead of ffmpeg's `Stream map '' matches no
  streams`. The audio is detected by ffmpeg's own error (no `ffprobe` pre-step:
  the menu never probes files, and ffmpeg fails in about 0.1 s anyway).

### Quality presets

JPG, WEBP, AVIF, MP4, WebM and GIF are submenus: **Best / High / Balanced /
Small** (menu item names `FileConverter::<category>::<target>::<preset>`, e.g.
`FileConverter::image::jpg::balanced`). Every other target (PNG, BMP, TIFF, ICO,
PDF, MKV, all audio and the documents) stays one click and converts as it always
did. With several files selected the preset applies to all of them. The preset is
not encoded in the output name (`photo.jpg`, then `photo (1).jpg`, as before),
and the notifications name it when it is not Best ("Converted 3 files to JPG
(Balanced)").

* **Best** is what the extension has always done; its commands are unchanged
  (the tests compare them with a copy of the previous ones).
* **High**: visually identical on screenshots and photos.
* **Balanced**: a good default for sharing.
* **Small**: the smallest acceptable result. It also shrinks: images larger than
  2560 px on the long side are scaled to 2560 px (JPG, WEBP, AVIF), video whose
  short side is above 1080 is scaled to 1080p (MP4, WebM; a 4K clip becomes
  1920x1080), GIF is 480 px wide at 10 fps with 128 colours. High and Balanced
  never change the picture size (GIF: High 720 px at 15 fps, Balanced 640 px at
  12 fps; Best 960 px at 15 fps). The menu tooltips say the same.

Settings (the source classes are as above: *graphics* = lossless sources,
*photo* = lossy sources):

| Target | Best | High | Balanced | Small |
|---|---|---|---|---|
| JPG graphics | q99, 4:4:4 | q95, 4:4:4, progressive | q85, 4:4:4, progressive | q75, 4:2:0, progressive, max 2560 px |
| JPG photo | q95, 4:4:4 | q92, 4:2:0, progressive | q80, 4:2:0, progressive | q70, 4:2:0, progressive, max 2560 px |
| WEBP graphics | lossless | lossless, 6 bits per channel | lossless, 5 bits per channel | lossless, 128-colour palette, max 2560 px |
| WEBP photo | lossy q90 | q85 | q78 | q65, max 2560 px |
| AVIF graphics (4:4:4) | CRF 9 | CRF 18 | CRF 26 | CRF 34, max 2560 px |
| AVIF photo (4:2:0) | CRF 12 | CRF 20 | CRF 28 | CRF 36, max 2560 px |
| MP4 libx264 | CRF 18, slow | CRF 23, slow | CRF 28, slow | CRF 34, medium, max 1080p |
| MP4 libopenh264 | 100 % of the bitrate | 60 % | 40 % | 25 %, max 1080p |
| WebM VP9 | CRF 28 | CRF 36 | CRF 44 | CRF 52, max 1080p |
| GIF | 960 px, 15 fps | 720 px, 15 fps | 640 px, 12 fps | 480 px, 10 fps, 128 colours |

Why WEBP graphics are lossless in every preset: for flat screenshots (a
terminal, a UI) the lossless file is already smaller than any lossy WEBP (a
1160x747 terminal screenshot: lossless 32 KB, lossy q60 59 KB, q98 135 KB), so
a lossy preset would make "Small" larger than "Best". ImageMagick's
`webp:near-lossless` has no effect. Instead the information in the pixels is
reduced (6 or 5 bits per channel, or a 128-colour palette) and the result is
encoded losslessly, which never makes the file larger.

Measured (SSIM from ffmpeg's `ssim` filter, RGB for images and 4:2:0 luma+chroma
for video, against the source; the "text" column is SSIM of a 400x200 crop of the
text; size is relative to Best's output for the same file; `-` = not applicable,
the crop is only taken from the text screenshots). A Small result that was
scaled down is compared with the source scaled the same way. Sources:
`shot-1080p.png` (game screenshot), `shot-terminal.png` (text-heavy),
`Bildschirmfoto ... .png` (965x283 UI), `photo-phone.jpg` (3024x4032),
`photo-big.jpg` (4191x6286), a phone HEIC (2268x4032), two 1080p GNOME
screencasts (v01 7.4 s, v02 11.4 s), `obs-cut15s.mkv` (HEVC 1080p60, 15 s) and
a generated 720p test clip.

| Target, source | High: size, SSIM, text | Balanced: size, SSIM, text | Small: size, SSIM, text |
|---|---|---|---|
| JPG, game screenshot | 45 %, 0.967, - | 23 %, 0.932, - | 13 %, 0.900, - |
| JPG, terminal | 64 %, 0.995, 0.989 | 42 %, 0.979, 0.957 | 29 %, 0.958, 0.918 |
| JPG, UI | 53 %, 0.984, 0.982 | 32 %, 0.971, 0.967 | 22 %, 0.948, 0.941 |
| JPG, phone photo / big photo / HEIC | 73 / 53 / 68 %, 0.984 / 0.986 / 0.981 | 41 / 26 / 40 %, 0.941 / 0.980 / 0.959 | 13 / 4 / 14 %, 0.928 / 0.969 / 0.931 |
| WEBP, game screenshot | 69 %, 0.990, - | 51 %, 0.967, - | 23 %, 0.939, - |
| WEBP, terminal | 85 %, 0.999, 1.000 | 77 %, 0.994, 0.996 | 75 %, 0.996, 1.000 |
| WEBP, UI | 56 %, 0.993, 0.995 | 43 %, 0.978, 0.989 | 29 %, 0.991, 0.995 |
| WEBP, phone photo / big photo / HEIC | 62 / 67 / 69 %, 0.927 / 0.976 / 0.956 | 34 / 46 / 44 %, 0.896 / 0.971 / 0.929 | 9 / 9 / 15 %, 0.915 / 0.964 / 0.896 |
| AVIF, game screenshot | 64 %, 0.966, - | 45 %, 0.951, - | 28 %, 0.922, - |
| AVIF, terminal | 73 %, 0.998, 0.998 | 64 %, 0.998, 0.997 | 58 %, 0.996, 0.996 |
| AVIF, UI | 73 %, 0.988, 0.994 | 60 %, 0.981, 0.984 | 50 %, 0.968, 0.951 |
| AVIF, phone photo / big photo / HEIC | 82 / 60 / 57 %, 0.971 / 0.978 / 0.954 | 22 / 37 / 30 %, 0.876 / 0.972 / 0.917 | 6 / 6 / 8 %, 0.905 / 0.963 / 0.868 |
| MP4 x264, v01 / v02 / obs / generated | 56 / 59 / 56 / 71 %, 0.985 / 0.991 / 0.990 / 0.997 | 31 / 34 / 33 / 37 %, 0.973 / 0.985 / 0.983 / 0.989 | 15 / 16 / 20 / 19 %, 0.941 / 0.967 / 0.974 / 0.979 |
| MP4 openh264, v01 / v02 / generated | 61 / 81 / 62 %, 0.946 / 0.974 / 0.980 | 41 / 71 / 42 %, 0.915 / 0.969 / 0.971 | 26 / 60 / 32 %, 0.863 / 0.962 / 0.960 |
| WebM, v01 / v02 / obs / generated | 62 / 57 / 52 / 68 %, 0.977 / 0.983 / 0.990 / 0.996 | 36 / 33 / 28 / 37 %, 0.960 / 0.975 / 0.986 / 0.991 | 18 / 20 / 16 / 17 %, 0.934 / 0.963 / 0.981 / 0.986 |
| GIF, v01 / v02 / generated | 60 / 60 / 60 % | 41 / 44 / 39 % | 17 / 18 / 18 % |

Best's own SSIM for reference: JPG 0.991 / 0.999 / 0.996 (game, terminal, UI),
WEBP graphics 1.000, AVIF 0.978 / 0.999 / 0.991, x264 0.991 to 0.999.

Where the numbers fall short of the intent, and why:

* JPG of a screenshot cannot reach SSIM 0.985 at half of Best's size: q99 4:4:4
  is a very high bar for a format that rings around text. High (q95) keeps text
  at 0.98 to 0.99; Balanced and Small are visibly softer around small text
  (terminal text crop 0.957 and 0.918). Use WEBP or AVIF for screenshots.
* WEBP and AVIF of flat graphics shrink little (terminal: 58 to 85 % of Best)
  because Best is already tiny there; the larger savings are on game-like
  screenshots and photos.
* Photographs with fine sensor noise (the phone photo) lose SSIM fast under
  WEBP/AVIF at Balanced and Small because the noise is smoothed away; the
  picture still looks clean. SSIM is a harsh judge of that.
* libopenh264 has no constant-quality mode, only a target bitrate, so the
  presets scale the bitrate and the size follows the content: a low-motion
  screencast at 25 % of Best's bitrate is still 60 % of its size. libx264 is
  the main path and behaves as the table shows.

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
  for HEVC-based HEIC). Tested on Fedora 44 with all three installed. On other
  releases the package may instead be `heif-pixbuf-loader`; that package name
  is unverified.

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

### Memory: admission and temp files

One 200 megapixel image needs 6 to 8 GB while it converts, and the runner starts
up to `ncpu/2` images at once, so selecting several big ones could take far more
memory than the machine has. Image jobs therefore go through an admission step
(all of it bookkeeping in the main loop; nothing blocks):

1. **Size, without decoding pixels.** `magick identify -ping` (own process,
   asynchronous, 30 s timeout) reads width, height and resolution from the
   header; for HEIC/AVIF the helper does it (`nfc-decode --info`). If the probe
   fails the job simply runs without an estimate (it counts as small).
2. **Estimate.** peak bytes = pixels x B/px, with B/px 30 for most targets (jpg,
   png, ico, bmp, tiff, pdf), 40 for AVIF and 44 for WEBP. Calibrated on the
   measured 19200 x 10800 RGBA PNG (207 MP): 5.7 GB = 27.5 B/px, AVIF 7.6 GB =
   36.7 B/px, WEBP 8.4 GB = 40.5 B/px, plus 8-10 % margin. SVG is rendered at
   192 dpi, so its pixels count 64/9 times.
3. **Admission.** A job starts when it is small (below 25 megapixels: never held
   back, so ordinary use is unchanged), or when nothing else is running (a single
   huge file always runs, alone), or when the estimates of all running jobs plus
   its own fit in 60 % of `MemAvailable` (`/proc/meminfo`, read when the decision
   is made). Otherwise it waits in a FIFO queue that is looked at again whenever
   a conversion ends and every 2 s. The accounting is global: two batches started
   one after the other share it. The usual worker cap (`ncpu/2`) still applies.
4. **Where ImageMagick spills.** `/tmp` is a RAM-backed tmpfs on most current
   systems (`findmnt -T /tmp` says `tmpfs`), so pixel data ImageMagick cannot
   keep in memory would still be RAM. Image jobs run `magick` with
   `MAGICK_TEMPORARY_PATH` (and `MAGICK_TMPDIR`) set to
   `$XDG_CACHE_HOME/nautilus-file-converter/tmp` (`~/.cache/...`, on the disk,
   `findmnt -T ~/.cache` says `ext4`; mode 0700, created on demand). The work
   directory of a job on an image of 25 megapixels or more (it holds `decoded.png`
   or `flat.png`, hundreds of MB) is made there too; everything else uses
   `$TMPDIR`. Entries in that directory older than one day (left by a crash or a
   kill) are removed once per session, in a background thread. If it cannot be
   created, the system temp directory is used.

`NFC_TEST_MEMAVAILABLE_KB` replaces the `MemAvailable` figure; it exists for the
tests and is only honoured when set.


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
  of the tool's error output. A preset other than Best is named: "Converted 3
  files to JPG (Balanced)", "photo.png → photo.jpg (Small)".
* **Output cap**: a tool's stdout and stderr are read incrementally and only the
  last 64 KiB of each is kept, so a tool that prints without end cannot grow
  memory, the log or the notification (the toast text is also shortened to
  300 bytes). A single log entry is at most 64 KiB (the middle is cut).
* **Log**: full command and stderr of every failure, and probe timeouts, go to
  `$XDG_CACHE_HOME/nautilus-file-converter.log` (`~/.cache/...`). The file is
  created with mode 0600 (and narrowed if it existed with wider permissions).
  Above 1 MiB (or when one entry would cross it) it is rotated to
  `nautilus-file-converter.log.1` (one backup, replaced on each rotation).

## Tests

```
meson setup build
meson test -C build
```

* `core-logic`: classification, output naming, command lines (input coders, `%`
  escaping, resource limits, LibreOffice URIs), no-overwrite publishing in all
  three strategies, and the `nfc-decode` helper (formats, a decode, `--info`, garbage
  input, usage errors); the size-probe parser, the memory estimate (checked
  against the calibration numbers), the admission decision (including a fake
  `NFC_TEST_MEMAVAILABLE_KB`), the PDF page-fit arguments and the spill directory
  (mode, location, removal of old leftovers).
* `robustness`: all with fake tools and a `TMPDIR` containing a space and `%`:
  a fake `ffmpeg` that hangs for 20 s must not slow the menu logic (< 100 ms
  for every selection) and is killed by the probe timeout and logged; a fake
  `soffice` of the `sleep 30 & exec sleep 30` kind must fail within timeout +
  2 s with no surviving process from its group and no leaked work directory;
  partial output and hidden temp files are removed on timeout and on failure;
  in-flight jobs are cancelled on shutdown; the log is 0600 and rotates, and
  one entry never exceeds 64 KiB; a fake tool printing 10 MB to stderr and
  stdout fails the job with its last stderr line while the log grows by at most
  one capped entry. Admission: four generated 12000x8000 PNGs (96 MP, estimate
  2.9 GB each) are converted as one batch with a fake `magick` that logs when
  each conversion starts and ends: with little memory they run strictly one at a
  time and all succeed, with room for two never more than two overlap, with plenty
  all start together; small images always run in parallel even with no memory
  at all; ImageMagick gets the spill directory; a shutdown while jobs wait ends
  them all as cancelled and leaves nothing behind. `NFC_TEST_SKIP_HUGE` skips the
  huge-image batches.
* `publish-terminates`: with an `LD_PRELOAD` shim that makes `rename()` fail
  with `EEXIST`, publishing in the last-resort mode returns an error within a
  second instead of looping forever; and when the first 10000 names are all
  taken it gives up with an error.
* `decode-limits`: `nfc-decode` refuses (exit 1, quickly, with a message) PNGs
  of 20000x20000, just over 100 MP, and over 30000 pixels on a side, and still
  decodes exactly 100 MP, a small PNG and (with `NFC_HEIC_SAMPLE`) a real HEIC.
* `module-load-and-menu`: loads the built `.so` without installing it, checks
  the three Nautilus entry points and that the registered type implements
  `NautilusMenuProvider`, then drives the real menu provider with fake
  `NautilusFileInfo` objects (single file, same-category selection, mixed
  categories, directory, non-`file://` URI, unknown type, an invalid entry in
  the middle of the list); and the preset menus: JPG/WEBP/AVIF/MP4/WebM/GIF are
  submenus with the items Best, High, Balanced, Small (names `...::<preset>`, a
  tooltip each), PNG, BMP, MP3, MKV and the rest are plain items, also for a
  multi-file selection.
* `module-probe-freeze`: the same, end to end, with a hanging fake `ffmpeg`
  first in `PATH`: right-clicking a `.txt` returns in microseconds.
* `conversions`: generates fixtures with ImageMagick / ffmpeg / LibreOffice,
  runs every source-to-target pair for real and checks that the output exists,
  is non-empty and has the correct *real* type (magic bytes via `file`). Set
  `NFC_HEIC_SAMPLE=/path/photo.heic` to include HEIC input. Runs under a
  `TMPDIR` containing a space and `%`, and includes file names such as `a%d.png`,
  `%[filename].png`, `100%.png`, `dir%d/x.png`, `a[1].png`, `a*b.png`,
  `@list.png`, `a:b.png`, and a fake `notify-send` that rejects a leading-dash
  argument unless it follows `--`. An SVG that references a secret PNG through
  `file://`, `text:` and relative
  `href`s must convert without embedding it, and a fake `inkscape` first in
  `PATH` must never be run.
  Quality and robustness cases from real failures: a generated 19200x10800 RGBA
  PNG (the size of a real upscaled screenshot, which used to fail with "cache
  resources exhausted") converts to every image target (skipped with a notice
  when less than 16 GiB RAM is available or `NFC_TEST_SKIP_HUGE` is set; it
  needs 6-8 GB); a video without an audio track converts to MP4, MKV, WebM and
  GIF (odd frame size included) and fails to MP3/M4A with "it has no audio
  track"; JPG output is 4:4:4, an odd-sized PNG keeps its exact size in AVIF
  (4:4:4, BT.709 full range tagged), WEBP of a PNG is pixel-identical, ICO of a
  non-square image has square 256/48/32/16 entries. `core-logic` asserts the
  exact quality arguments of every target.
  PDF page size: PNGs of 1920x1080 (no density), a tall phone screenshot, an icon,
  and 300 and 96 dpi ones are converted to PDF; `pdfinfo` must show an A4-sized
  page for the ones without density and the source resolution for the others,
  and `pdfimages -list` the original pixel size.
  Presets: every preset-capable target is converted once per preset from a
  screenshot-like and a photo fixture (real type checked, sizes must not grow
  from Best to Small), Small must shrink a 3200 px image to 2560 and 1440p/portrait
  video to 1080p while the other presets leave the size alone, and a batch with a
  fake `notify-send` must name the preset in both banners. `core-logic` holds the
  exact arguments of every target x preset x source class and a hard-coded copy of
  the previous (Best) commands.

## License

MIT
