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

test_robust = executable('test-robust', 'test-robust.c',
  dependencies: core_dep,
)

# The tests use the helper from the build dir, not an installed one.
test_env = environment()
test_env.set('NFC_DECODE_HELPER', nfc_decode.full_path())
test_env.set('NFC_TEST_DATA', test_dir)

test('core-logic', test_core, timeout: 120, env: test_env, depends: nfc_decode)
test('robustness', test_robust, timeout: 300, env: test_env, depends: nfc_decode)
test('module-load-and-menu', test_module, args: [ext], depends: [ext, nfc_decode],
  timeout: 120, env: test_env)
test('module-probe-freeze', test_module, args: [ext, 'freeze'], depends: [ext, nfc_decode],
  timeout: 120, env: test_env)
test('conversions', test_conv, timeout: 1500, is_parallel: false, depends: nfc_decode,
  env: test_env)
