# nautilus-file-converter

A "Convert to" submenu for Nautilus (GNOME Files), similar to the Windows app
*File Converter*. Select one or more files, right-click, choose a target
format; the converted copies appear next to the originals. For JPG, WEBP, AVIF,
MP4, WebM and GIF the target opens a small submenu of quality presets (Best, High,
Balanced, Small; see "Quality presets" below). A second entry, "Reduce file size"
(High, Balanced, Small), makes a smaller copy of a JPG, WEBP, AVIF, MP4, WebM or
GIF in the same format (see "Reduce file size" below), and "Resize" (1080p, 720p, 50 %, ...;
only sizes smaller than the file) makes a copy at a smaller pixel size (see "Resize"
below). With Upscayl installed a further entry, "Upscale" (2×, 3×, 4×, 5×, 6×, 8×), makes
an AI-enlarged copy of an image on the GPU (see "Upscale" below).

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
* **Small**: the smallest acceptable result. Its menu label says what it does to
  the size: "Small (max 2560 px)" for JPG, WEBP and AVIF, "Small (max 1080p)" for
  MP4 and WebM, "Small (480 px)" for GIF (the same in Reduce file size; a mixed selection such as JPG + GIF gets a plain
  "Small"); the item name stays `...::small`. When it really scaled a picture
  down, the success banner gives the new size: "photo.png → photo.jpg (Small,
  2560×1707)". It also shrinks: images larger than
  2560 px on the long side are scaled to 2560 px (JPG, WEBP, AVIF), video whose
  short side is above 1080 is scaled to 1080p (MP4, WebM; a 4K clip becomes
  1920x1080), GIF is 480 px wide at 10 fps with 128 colours. High and Balanced
  never change the picture size (GIF: High 720 px at 15 fps, Balanced 640 px at
  12 fps; Best 960 px at 15 fps; those are *maximum* rates, the frame rate is
  `min(source, preset)` so a 10 fps source keeps its 10 fps instead of getting
  frames duplicated). The menu tooltips say the same.

Settings (the source classes are as above: *graphics* = lossless sources,
*photo* = lossy sources):

| Target | Best | High | Balanced | Small |
|---|---|---|---|---|
| JPG graphics | q99, 4:4:4 | q95, 4:4:4, progressive | q85, 4:4:4, progressive | q75, 4:2:0, progressive, max 2560 px |
| JPG photo | q95, 4:4:4 | q92, 4:2:0, progressive | q80, 4:2:0, progressive | q70, 4:2:0, progressive, max 2560 px |
| WEBP graphics | lossless, method 4 | lossy q95 *or* the lossless file of Best, whichever is smaller | lossy q88 *or* lossless, whichever is smaller | lossy q75 *or* lossless, whichever is smaller, max 2560 px |
| WEBP photo | lossy q90, sharp YUV | q85, sharp YUV | q78, sharp YUV | q65, sharp YUV, max 2560 px |
| AVIF graphics (4:4:4) | CRF 9 | CRF 18 | CRF 26 | CRF 34, max 2560 px |
| AVIF photo (4:2:0) | CRF 12 | CRF 20 | CRF 28 | CRF 36, max 2560 px |
| MP4 libx264 | CRF 18, slow | CRF 23, slow | CRF 28, slow | CRF 34, medium, max 1080p |
| MP4 libopenh264 | 100 % of the bitrate | 60 % | 40 % | 25 %, max 1080p |
| WebM VP9 | CRF 28 | CRF 36 | CRF 44 | CRF 52, max 1080p |
| GIF | 960 px, 15 fps | 720 px, 15 fps | 640 px, 12 fps | 480 px, 10 fps, 128 colours |

WEBP graphics (High, Balanced, Small) are encoded twice and the smaller file is
kept: a lossy WEBP (q95 / q88 / q75, method 6, `webp:use-sharp-yuv=true`,
`webp:alpha-quality=100`) and the lossless one of Best. Flat text
screenshots (a terminal) are smaller lossless than at any lossy quality (a
1160x747 terminal screenshot: lossless 33 KB, lossy q75 72 KB, q95 130 KB), so
for them every preset is Best's file; screenshots with gradients, shadows or
photos inside are 3 to 10 times smaller lossy. The lossless encode is only made
for images below 25 megapixels (it costs time and memory: bigger images get the lossy encode alone) and if it fails the lossy encode is run
alone. The choice is the general "keep only what is smaller" rule of the runner,
also used by Compress (below). An earlier version posterised the pixels
(6 or 5 bits per channel, a 128-colour palette) and encoded them losslessly
instead; that is gone.

(`webp:near-lossless` has no effect in ImageMagick. The define
`webp:sharp-yuv` that Best's photo preset passes is not an ImageMagick option
and has never done anything; `webp:use-sharp-yuv` is the real one and is used for
the graphics presets only, so that Best's commands stay as they were.)

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
| WEBP, game screenshot | 27 %, 0.954, - | 17 %, 0.942, - | 9 %, 0.910, - |
| WEBP, terminal | 100 %, 1.000, 1.000 | 100 %, 1.000, 1.000 | 100 %, 1.000, 1.000 |
| WEBP, UI | 31 %, 0.988, 0.994 | 19 %, 0.982, 0.983 | 12 %, 0.963, 0.962 |
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
* AVIF of flat graphics shrinks little (terminal: 58 to 73 % of Best) because
  Best is already tiny there; WEBP of the terminal does not shrink at all: the
  lossless file (33 KB) beats every lossy one, so each preset gives Best's file
  (lossy q95 would be 130 KB). The larger savings are on game-like screenshots,
  UIs with shadows or gradients and photos. The WEBP SSIM of the game screenshot
  (0.954 / 0.942 / 0.910) is lower than the 0.990 / 0.967 / 0.939 of the earlier
  posterised-lossless presets, at 27 / 17 / 9 % of Best's size instead of
  69 / 51 / 23 %: lossy WEBP (4:2:0 chroma) cannot keep the hard colour edges of
  that screenshot, but at the same size it is better (the earlier Balanced
  was 51 % at 0.967, the new High 27 % at 0.954).
* Photographs with fine sensor noise (the phone photo) lose SSIM fast under
  WEBP/AVIF at Balanced and Small because the noise is smoothed away; the
  picture still looks clean. SSIM is a harsh judge of that.
* libopenh264 has no constant-quality mode, only a target bitrate, so the
  presets scale the bitrate and the size follows the content: a low-motion
  screencast at 25 % of Best's bitrate is still 60 % of its size. libx264 is
  the main path and behaves as the table shows.

### Reduce file size (Compress)

Next to "Convert to" the menu has **Reduce file size** (not "Compress", which Nautilus
uses for archives; the item names stay `FileConverter::Compress...`), with the presets **High /
Balanced / Small** (item names `FileConverter::Compress::<high|balanced|small>`).
It makes a smaller copy of a file in the *same* format, with the settings of
that preset from the table above.

* **Offered** only when every selected file is a JPG/JPEG, WEBP, AVIF, MP4/M4V,
  WebM or GIF (the formats that have presets): not for PNG, HEIC, MKV, audio,
  documents, folders, and not for a mixed selection such as JPG + PNG. A selection
  of several of these formats (JPG + WEBP, MP4 + WebM) is fine: each file is
  compressed in its own format.
* **What it does**: JPG, WEBP and AVIF as above (the photo settings, since all
  three are lossy sources); MP4/M4V is re-encoded as H.264 (libx264 or libopenh264),
  WebM as VP9, GIF with the GIF presets (a `.gif` is an image to Nautilus but
  is compressed with the video tooling, palette and all). Small also scales
  down: images beyond 2560 px, video above 1080p, GIF to 480 px.
* **Name**: `photo (compressed).jpg`, through the normal no-overwrite
  publishing (`photo (compressed) (1).jpg` if that exists). `.jpeg` gives
  `.jpg`, `.m4v` gives `.mp4`. The original is never touched.
* **Only if smaller**: the copy is kept only when it is strictly smaller than
  the original. Otherwise it is deleted (nothing is left behind, not even the
  hidden temporary file), the file counts as skipped, not failed, and the
  notification says so. Re-encoding an already well-compressed file at a high
  quality often makes it bigger: "photo.jpg is already as small as it gets".
* **Notifications**: "Reducing file size…", then "photo.jpg → photo (compressed).jpg
  (Balanced), −62%" (title "File size reduced"), or "Compressed 3 files (Balanced), −54%" (the percentage is
  the saving over all compressed files), plus a line "photo.jpg is already as
  small as it gets" / "2 files are already as small as they get" for skipped
  files, and "Failed: ..." lines as usual.

The rule behind the last point is a general runner feature, so that further
features (resizing, upscaling) can reuse it: `NfcConvertOptions` of
`nfc_convert_file_opts_async` carries a `reference_size` (`NFC_REFERENCE_SOURCE`,
`NFC_REFERENCE_NONE` or a size in bytes) and an optional `name_suffix`; an output
that is not strictly smaller than the reference ends the job with
`NFC_CONVERT_ERROR_NOT_SMALLER` after deleting it. The same rule
(`nfc_is_smaller`) picks between two candidate encodes inside one conversion (the
internal step `NFC_STEP_KEEP_SMALLEST`, used by the WEBP graphics presets).

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
* `Upscayl` (optional, for Upscale only): its backend `upscayl-bin` and models, from the
  [release RPM](https://github.com/upscayl/upscayl/releases) (`/opt/Upscayl`) or anywhere
  `upscayl-bin` is on the `PATH` with a `models` directory beside its `bin` directory;
  a Vulkan-capable GPU and driver
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

### Resize

After "Convert to" and "Reduce file size" the menu has **Resize**: a copy at a smaller
pixel size, in the *same* format, next to the original (item names
`FileConverter::Resize::<key>`).

* **Images** (PNG, JPG, WEBP, AVIF, BMP, TIFF, and animated GIF): **4K (3840 px),
  1440p (2560 px), 1080p (1920 px), 720p (1280 px), 480p (854 px), 50 %, 25 %**
  (keys `4k 1440p 1080p 720p 480p 50pct 25pct`). The named sizes are the **long
  side**, the aspect ratio is kept, so a 3840x2160 picture becomes exactly
  1920x1080 and a portrait 2160x3840 one 1080x1920. 50 % and 25 % scale both
  sides. HEIC (no HEIC encoder), SVG (vector) and ICO are not offered.
* **Videos** (MP4/M4V, MKV, WebM, MOV, AVI): **1080p, 720p, 480p** by the **short
  side** (video convention: 1080p is 1080 lines, a portrait phone video becomes
  1080 px wide), even dimensions.
* **Only presets that make the picture smaller are offered.** For an image the
  menu knows its size: `get_file_items` reads the first 64 KiB of each selected
  local file (the only place the extension reads file content on the main thread;
  a bounded read of a regular file, nothing blocks on a FIFO, at most 32 files)
  and parses the PNG, JPEG, GIF, WebP or BMP header in C (`src/nfc-header.c`).
  A 3840x2160 PNG offers 1440p ... 25 % but not 4K; a 1000x800 JPG offers 480p, 50
  %, 25 % (720p is 1280 px, which is larger); a 20x20 icon offers nothing (50 % is
  10 px, below the 16 px minimum of the percentages), so there is no Resize entry.
  With several files a preset is offered when it makes at least one of them smaller.
  Where the size cannot be read (AVIF, TIFF, JPEG with a header block beyond 64
  KiB, a damaged file) and for every video (probing needs a process, never done
  in the menu) the fixed list is shown, and the job decides when it runs.
* **When it runs**, the size is probed (`identify -ping`, `nfc-decode --info`,
  `ffprobe`) and a file the preset would not make smaller is skipped, not failed:
  "photo.jpg is already 1080p or smaller", "icon.png is too small to resize to
  50%". Nothing is written for it.
* **Quality**: the Best settings of the format. PNG, BMP and TIFF stay lossless,
  JPG is q95 4:4:4 (photo) or q99 4:4:4 (the source class rules of the table above),
  WEBP is lossless for graphics and lossy q90 for photos, AVIF CRF 9/12. Resampling
  is ImageMagick's Lanczos (`-filter Lanczos -resize 1920x1920>`, which never
  enlarges) and the alpha channel is kept (JPG flattens it on white, as always).
  GIF is scaled by ffmpeg (`scale=...:flags=lanczos` and the same palette chain
  as the GIF presets) without touching frames or timing.
* **Video** is re-encoded (`scale=w=...:h=...:flags=lanczos`, short side, even
  sizes): MP4, M4V, MOV, MKV and AVI with libx264 CRF 18 slow (libopenh264 with
  the bitrate of the *output* size when that is all ffmpeg has), WebM with VP9 CRF
  28. The **audio is copied** (`-c:a copy`); when the container cannot hold the
  source's codec the next attempt re-encodes it (AAC 192k, Opus 128k for WebM).
  AVI gets no B-frames (`-bf 0`) because the format cannot carry them reliably.
  `.m4v` is written as `.mp4` (ffmpeg's `.m4v` muxer is raw H.264).
* **Name**: `photo (1080p).jpg`, `photo (50%).png`, `clip (720p).mp4` through
  the normal no-overwrite publishing (`photo (1080p) (1).jpg` if that exists). No
  byte-size rule applies: the point is fewer pixels, the file may even grow
  (a flat PNG).
* **Notifications**: "Resizing…", then "photo.jpg → photo (1080p).jpg, 3840×2160
  → 1920×1080", or "Resized 3 files to 1080p", plus "photo.jpg is already
  1080p or smaller" / "2 files are already 1080p or smaller" for the skipped
  ones and "Failed: ..." lines.

For features that reuse this: `nfc_analyze_selection` fills `sel->dims` (an
`NfcDims` per file, 0 x 0 when unknown) and `sel->resize` (the presets to offer);
`nfc_header_parse` / `nfc_header_read_file` are the header parsers;
`NfcConvertOptions.resize` makes a job a Resize (it probes the size and ends with
`NFC_CONVERT_ERROR_NOT_SMALLER` before running any converter when the preset would
not shrink the source); `nfc_convert_result_dims` hands the probed source size to
the callback; `nfc_resize_output_size` is the one rule for "what size, and is it
smaller".

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
* **The one read**: for Resize the menu reads the first 64 KiB of each selected
  image (at most 32 files, regular local files only, opened `O_NONBLOCK` so a FIFO
  cannot hang it) to learn its size; microseconds from the page cache (13 us per file
  measured). Everything else about a file is learned by a process, after the click.
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

### Upscale

With [Upscayl](https://upscayl.org) installed the menu has a last entry, **Upscale**
(item names `FileConverter::Upscale::<key>`): **2×, 3×, 4×, 5×, 6×, 8×** (keys `2x 3x 4x
5x 6x 8x`). Every selected image becomes a copy that many times as wide and tall,
named `photo (4x).png` (the normal no-overwrite publishing: `photo (4x) (1).png` if
that exists), made by Upscayl's own backend, `upscayl-bin` (Real-ESRGAN through
ncnn/Vulkan, so it needs a GPU with a Vulkan driver; a CPU Vulkan device such as
llvmpipe works too, a few times slower than the GPU in a quick test). Nothing in the
menu talks to the GPU: the entry is only a menu item until it is clicked.

* **When it is offered.** `upscayl-bin` is looked up once, when the module loads:
  `$NFC_UPSCAYL_BIN` (tests), then Upscayl's install
  (`/opt/Upscayl/resources/bin/upscayl-bin`), then `upscayl-bin` on the `PATH`. Its
  models are `<bindir>/../models` (`<name>.param` + `<name>.bin`). No binary, no
  models directory or no usable model: no Upscale entry, and the rest of the menu is
  untouched. Images only: PNG, JPG, WEBP, AVIF, HEIC, BMP, TIFF and GIF (its first
  frame); not SVG (vector: nothing to enlarge), not ICO, no video.
* **Model.** `upscayl-standard-4x` by default. To use another installed one create
  `$XDG_CONFIG_HOME/nautilus-file-converter/config` (`~/.config/...`), a plain
  `key=value` file with `#` comments:

  ```
  upscale-model=remacri-4x
  ```

  The name must be a model whose `.param` and `.bin` files exist in the models
  directory (`digital-art-4x`, `high-fidelity-4x`, `remacri-4x`, `ultramix-balanced-4x`,
  `ultrasharp-4x`, `upscayl-lite-4x`, `upscayl-standard-4x` with Upscayl 2.15); anything
  else is logged and ignored (the default is used). The file is read once, with the
  tool paths: restart Nautilus after editing it.
* **How the scales are made.** One pass is `upscayl-bin -s N`: the 4× model enlarges
  four times and the backend resizes that to N. For 2× and 3× this was compared with a
  4× pass shrunk by ImageMagick's Lanczos (against ground truth made by shrinking
  real pictures: SSIM and PSNR were better for `-s N` on two of three images at 2× and
  all three at 3×, by eye
  they are indistinguishable), so the simpler one-process form is used. The bigger
  factors chain passes, the second one on the first one's output: **6× = 3× then 2×,
  8× = 4× then 2×**. **5×** has no integer product: it is made as 6× and shrunk by
  5/6 with Lanczos. Against the alternatives (one 4× pass stretched by 1.25, which is
  also what `-s 5` does inside the backend, and 8× shrunk) 6× → 5× kept the most
  detail (Laplacian variance 18 against 9.5 on the photo, whose original is 29.3;
  100 against 8.7 on a text screenshot) at a PSNR about 1 dB lower, for the price of a
  second pass. The sizes are exact (`width × factor`), the passes never round.
* **Input.** An ImageMagick step first writes the first frame as an upright 8-bit PNG
  (`-auto-orient`, which the backend does not apply to JPG and WEBP, and which also
  takes care of CMYK, 16-bit, palette and gray files); the backend itself reads only
  JPG, PNG and WEBP. HEIC and AVIF are decoded by `nfc-decode` as for every other
  conversion. The result of the passes is written by ImageMagick at the Best settings
  of its format.
* **Output format.** The source's own for JPG (q95 4:4:4, flattened on white), PNG and
  WEBP (as in Resize: lossy q90 for a WEBP, which counts as a photo); a transparent
  WEBP or PNG **keeps its alpha** (the backend upscales the alpha channel separately:
  checked on a transparent sticker, the result's alpha matches a Lanczos enlargement of
  the original's to 53 dB PSNR). HEIC, AVIF, BMP, TIFF and GIF become PNG. A WEBP
  whose result would exceed WebP's 16383 px per side becomes a PNG as well, and the
  notification says so. Embedded colour profiles are not carried over (the backend
  drops them; the result is plain sRGB).
* **Limits.** An Upscale never makes a picture wider or taller than **32768 px or
  larger than 400 megapixels**, and the entry only offers the scales within that for
  the selected pictures, using the header sizes the Resize entry reads (5000×4000 offers
  2×, 3×, 4×; 640×480 offers all six). With several files a scale is offered when it
  fits at least one; where no size is known (TIFF, a damaged file) every scale is
  shown, and the job reads the real size first: a file that would be too big is skipped,
  not failed ("photo.png is too big to upscale 8× (40000×32000 px; the limit is 32768
  px per side or 400 megapixels)"), and nothing runs for it.
* **One at a time.** The GPU is one resource: at most **one Upscale job runs at a time**,
  whichever batch it belongs to (the others wait in the admission queue of "Memory"
  below; a waiting Upscale job does not hold up the other conversions), and a batch
  upscales its files one after the other. The jobs go through the same memory
  admission: peak memory is estimated as the picture the passes make (6× for 5×) times
  the B/px of the output format, plus 1 GiB for `upscayl-bin` itself (measured about 1.0
  GiB resident for a small picture and 1.3 GiB for a 5120×3840 result; the backend
  keeps the full 4× image of its last pass in memory, about 3.6 B per pixel of it).
  Intermediate PNGs of a result of 25 MP or more live in the disk-backed work
  directory like other big jobs.
* **Timeout.** Per step (the prepare, each pass, the final encode): 120 s plus 4 s per
  output megapixel, at most 2 hours (measured about 3 s per megapixel of a pass's
  *input* on an RX 9070 XT, so an 8× pass of a result at the 400 MP limit takes about
  5 minutes there). On a timeout the process group is killed like any other step and the
  job fails with "timed out (the GPU may be busy or too slow)".
* **Failures.** `upscayl-bin` exits 0 after most of its errors (unreadable input, a
  missing model) and only prints them, so the runner treats a pass that wrote nothing
  as failed and reads its message: "no usable GPU: Upscale needs a Vulkan-capable
  graphics card and driver" (`vkCreateInstance failed`, `Invalid GPU Device`), "the GPU
  ran out of memory (try a smaller image or a lower scale)" (`vkAllocateMemory
  failed`), "the GPU stopped responding", "the Upscale model files are missing (check
  upscale-model in the config)", "the Upscale backend could not read the image". The
  lines it prints about every Vulkan device are ignored.
* **Notifications**: "Upscaling… photo.png (4×)", then "photo.png → photo (4x).png,
  640×480 → 2560×1920" (the sizes of the files themselves, so an EXIF-rotated photo is
  right), or "Upscaled 3 files 4×", plus a line for each file that was too big and
  "Failed: ..." lines.

Measured on a Radeon RX 9070 XT (RADV), 640×480 source, whole pass chain: 2× 1.9 s,
3× 2.0 s, 4× 2.2 s, 5× 11.7 s, 6× 10.7 s, 8× 16.9 s (a 128×96 source: 0.7 s for
2×–4×, 1.8 s for 5×/6×, 2.2 s for 8×); about 1.2 GiB of VRAM (a 1.7 GiB desktop was
already using it), 1.0 to 1.3 GiB RSS.

For features that reuse this: `NfcConvertOptions.upscale` makes a job an Upscale (it
probes the size, ends with `NFC_CONVERT_ERROR_TOO_BIG` before running anything when the
result is too big, switches a WEBP that WebP cannot hold to PNG, and takes the GPU slot);
`nfc_upscale_output_size` is the one rule for "what size, and does it fit";
`nfc_build_attempts_upscale` builds the steps; `nfc_upscale_resolve` finds the models and
the configured model; `NfcSelection.upscale` holds the scales to offer.

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
(no fallback attempt follows a timeout; Upscale steps have their own, see "Upscale"). `NFC_TEST_TIMEOUT_SECS` and
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
  files to JPG (Balanced)", "photo.png → photo.jpg (Small)". Compress has its
  own texts (see "Reduce file size"), and so has Resize (see "Resize"). When the Small
  preset scaled a picture down the size is added: "photo.png → photo.jpg (Small,
  2560×1707)".
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
  multi-file selection; and "Reduce file size" (High, Balanced, Small): offered for a
  JPG, `.jpeg`, two JPGs, JPG + WEBP, MP4, M4V, WebM and GIF, not for PNG, BMP, HEIC,
  JPG + PNG, MP4 + MKV, MKV, audio, a document, a directory. And "Resize" after
  "Reduce file size": generated 3840x2160 PNG (1440p ... 25 %, no 4K), 1000x800 JPG (480p, 50
  %, 25 %), 20x20 and 30x30 PNGs (no Resize at all), a video (fixed 1080p/720p/480p),
  a selection (a preset that shrinks at least one file), an unreadable size (the
  fixed list), HEIC, SVG, a document, a directory.
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
  Compress: every compressible format (JPG, `.jpeg`, WEBP, AVIF, MP4, `.m4v`,
  WebM, GIF) is compressed with High, Balanced and Small from a generated fixture:
  same real type, named `<name> (compressed).<ext>`, smaller than the original,
  sizes decreasing from High to Small, the original unchanged (checksum) and no
  file left in the directory. A tiny q40 JPG compressed with High/Balanced is
  "already as small as it gets": no file, no hidden temporary file, the error is
  `NFC_CONVERT_ERROR_NOT_SMALLER`; an explicit reference size of 1 byte discards
  a conversion, one of 1 TiB keeps it under the plain name. A second and third
  compress of the same file become `(compressed) (1)` and `(2)` and never touch
  the first. A JPG + WEBP + GIF batch compresses each in its own format; the
  notification texts (one file, three files, skipped, one good + one skipped, one
  good + one broken) are compared with a fake `notify-send`. WEBP graphics: a
  flat text image comes out at exactly Best's (lossless) size for every preset, a
  plasma image at less than half of it, decreasing, with no `.alt` file left.
  Resize: a 4K PNG to 1080p is exactly 1920x1080 and named `pic (1080p).png`, a JPG to
  50 %, a WEBP with transparency to 720p (alpha kept), a 10 fps GIF to 480p (same
  frame count and rate), BMP, TIFF, AVIF, a 1080p MP4 / MKV to 720p and a 720p
  WebM/MOV/AVI/M4V to 480p (exact size, video codec as before, audio stream
  copied), a portrait video (short side), a video without audio; a second resize
  becomes `(480p) (1)`; sources that are already smaller (an image, an exact match,
  a 720p video to 720p, a 30 px image to 50 %) are skipped with the plain message and
  leave nothing; the notification texts are compared with a fake `notify-send`; the
  GIF frame rate is `min(source, preset)` (10, 30 and 12 fps sources). `core-logic`
  fuzzes the header parsers (truncation and byte flips of real PNG, JPEG, GIF, WebP
  lossy / lossless / extended and BMP files, plus random bytes; under ASan nothing
  may read out of bounds), and `robustness` checks that the header read neither
  blocks on a FIFO nor reads more than 64 KiB of a 100 GiB file.
  Upscale: `core-logic` covers the tool discovery (a fake install found through
  `NFC_UPSCAYL_BIN`, the model of a config file in `XDG_CONFIG_HOME`), the config
  parser and the model validation (valid, unknown, a path, empty, a missing `.bin`,
  missing default), the scale rule (640×480 offers all six, 5000×4000 only 2×–4×, the
  exact 400 MP and 32768 px boundaries, no overflow), the output format (PNG for
  AVIF/HEIC/BMP/TIFF/GIF, the WEBP → PNG switch at 16383 px) and the argv of every
  scale (pass chains, the 5/6 shrink, JPG/WEBP encoders, paths with `%`), the memory
  estimate, the timeout and the failure messages for `upscayl-bin`'s own texts.
  `module-load-and-menu` checks the entry with a fake `upscayl-bin` (every scale for
  640×480 with its labels and exact sizes in the tooltips, hidden scales for 5000×4000, 3840×2160
  and a 4100 px strip, none for 20000×20, a selection, an unreadable file, BMP/TIFF/GIF/WEBP; absent
  for SVG, ICO, video, audio, a document, a folder, mixed selections; always after
  Resize) and `module-no-upscayl` / `module-no-models` that nothing appears without the binary or the
  models. `conversions` runs the **real** `upscayl-bin` on tiny pictures (2×/3×/4× on
  128×96, 5×/6×/8× on 64×48, a JPG, a transparent WEBP and PNG, AVIF/BMP/TIFF/GIF to
  PNG, an EXIF-rotated JPG, CMYK, 16-bit and gray sources, the `(2x) (1)` naming), checking
  exact size, real type and nothing left behind, and with a fake `upscayl-bin` the failure
  texts (no Vulkan device, out of VRAM, unreadable input and missing model with exit 0, silent
  exit 0), the timeout kills the process group, and two jobs at once run one after the other.
  These rows say SKIPPED, and pass, when there is no `upscayl-bin`, no working Vulkan device
  (a probe picture is upscaled first) or `NFC_TEST_SKIP_GPU` is set.
  `NFC_TEST_ONLY=upscale` runs just these (the rest takes minutes).

## License

MIT
