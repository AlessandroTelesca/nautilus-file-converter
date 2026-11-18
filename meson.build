project('nautilus-file-converter', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.62.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'],
)

glib_dep    = dependency('glib-2.0', version: '>= 2.74')
gio_dep     = dependency('gio-2.0')
gobject_dep = dependency('gobject-2.0')
pixbuf_dep  = dependency('gdk-pixbuf-2.0')
nautilus_dep = dependency('libnautilus-extension-4')

extension_dir = nautilus_dep.get_variable(pkgconfig: 'extensiondir',
  default_value: get_option('libdir') / 'nautilus' / 'extensions-4')

# Where the out-of-process decoder is installed (and where the extension looks
# for it): <prefix>/<libexecdir>, /usr/libexec by default.
libexec_dir = get_option('prefix') / get_option('libexecdir')

conf = configuration_data()
conf.set_quoted('NFC_DECODE_HELPER_PATH', libexec_dir / 'nfc-decode')
config_h = configure_file(output: 'config.h', configuration: conf)

# Out-of-process HEIC/AVIF decoding and GdkPixbuf format listing. This is the
# only code linked against gdk-pixbuf: the extension itself must not be.
nfc_decode = executable('nfc-decode', 'src/nfc-decode.c',
  dependencies: [pixbuf_dep],
  install: true,
  install_dir: libexec_dir,
)

# Pure logic + async runner: no Nautilus and no gdk-pixbuf dependency, also
# linked by the tests.
core_lib = static_library('nfc-core',
  ['src/nfc-core.c', 'src/nfc-convert.c', 'src/nfc-util.c', 'src/nfc-header.c',
   'src/nfc-tray.c'],
  dependencies: [glib_dep, gio_dep, gobject_dep],
  include_directories: include_directories('src', '.'),
)

core_dep = declare_dependency(
  link_with: core_lib,
  dependencies: [glib_dep, gio_dep, gobject_dep],
  include_directories: include_directories('src'),
)

ext = shared_module('nautilus-file-converter',
  'src/nfc-extension.c',
  dependencies: [core_dep, nautilus_dep],
  name_prefix: 'lib',
  link_args: ['-Wl,--exclude-libs,ALL'],
  install: true,
  install_dir: extension_dir,
)

subdir('tests')
