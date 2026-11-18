test_dir = meson.current_source_dir()

test_conv = executable('test-conversions', 'test-conversions.c',
  dependencies: core_dep,
)

test_module = executable('test-module', 'test-module.c',
  dependencies: [core_dep, nautilus_dep, dependency('gmodule-2.0')],
)

test_core = executable('test-core', 'test-core.c',
  dependencies: core_dep,
)

test('core-logic', test_core, timeout: 120)
test('module-load-and-menu', test_module, args: [ext], depends: ext, timeout: 120)
test('conversions', test_conv, timeout: 1500, is_parallel: false,
  env: ['NFC_TEST_DATA=' + test_dir])
