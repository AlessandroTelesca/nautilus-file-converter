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
# sanitizer runtimes (quarantines, shadow memory) inflate peak RSS: the tests that
# measure it skip that check then
if get_option('b_sanitize') != 'none'
  test_env.set('NFC_TEST_SANITIZED', '1')
endif

test('core-logic', test_core, timeout: 120, env: test_env, depends: nfc_decode)
test('robustness', test_robust, timeout: 300, env: test_env, depends: nfc_decode)
test('module-load-and-menu', test_module, args: [ext], depends: [ext, nfc_decode],
  timeout: 120, env: test_env)
test('module-probe-freeze', test_module, args: [ext, 'freeze'], depends: [ext, nfc_decode],
  timeout: 120, env: test_env)
test('conversions', test_conv, timeout: 1500, is_parallel: false, depends: nfc_decode,
  env: test_env)

# nfc_publish_with_mode must terminate even if rename() answers EEXIST: an
# LD_PRELOAD shim (built here) makes rename() fail on demand.
rename_shim = shared_library('rename-eexist-shim', 'rename-eexist-shim.c',
  dependencies: dependency('dl', required: false),
)
test_publish = executable('test-publish', 'test-publish.c',
  dependencies: core_dep,
)
publish_env = environment()
publish_env.set('LD_PRELOAD', rename_shim.full_path())
publish_env.set('NFC_SHIM_ACTIVE', '1')
test('publish-terminates', test_publish, timeout: 120, env: publish_env,
  depends: rename_shim)

test_decode = executable('test-decode', 'test-decode.c',
  dependencies: [glib_dep, dependency('zlib')],
)
test('decode-limits', test_decode, timeout: 120, env: test_env, depends: nfc_decode)
