project('nautilus-file-converter', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.60.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'],
)

glib_dep    = dependency('glib-2.0', version: '>= 2.74')
gio_dep     = dependency('gio-2.0')
gobject_dep = dependency('gobject-2.0')
pixbuf_dep  = dependency('gdk-pixbuf-2.0')
nautilus_dep = dependency('libnautilus-extension-4')

extension_dir = nautilus_dep.get_variable(pkgconfig: 'extensiondir',
  default_value: get_option('libdir') / 'nautilus' / 'extensions-4')

# Pure logic + async runner: no Nautilus dependency, also linked by the tests.
core_lib = static_library('nfc-core',
  ['src/nfc-core.c', 'src/nfc-convert.c'],
  dependencies: [glib_dep, gio_dep, gobject_dep, pixbuf_dep],
  include_directories: include_directories('src'),
)

core_dep = declare_dependency(
  link_with: core_lib,
  dependencies: [glib_dep, gio_dep, gobject_dep, pixbuf_dep],
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
