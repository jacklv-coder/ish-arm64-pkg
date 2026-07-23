# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the C glue that turns iSH into an embeddable runtime:
#   - libishembed.a: host-side embedding glue (pumps, sessions, lifecycle)
#   - libishffi.a:   FFI shims into iSH internals
#
# iSH itself is a separate Meson project under third_party/ish/ (managed
# as a git submodule pointing at jacklv-coder/ish-arm64). scripts/build-ios.sh
# orchestrates both.
#
# Usage (host build):
#   meson setup build native -Dish_src=$(realpath third_party/ish)
#   ninja -C build
#
# For iOS, see scripts/build-ios.sh.

project('ishembed', 'c',
    default_options: [
        'default_library=static',
        'c_std=gnu11',
        'warning_level=1',
    ])

ish_src_dir = get_option('ish_src')
if ish_src_dir == ''
    error('-Dish_src=<path to iSH source root> is required')
endif

# iSH internal include paths needed by the FFI shim. We pass them as
# raw -I flags rather than include_directories() because Meson refuses
# to take an absolute path inside the source tree (the third_party/ish
# submodule lives inside the project).
#
# Use ONLY the repo root; never add `kernel/` or `fs/` here, otherwise
# macOS's libc `<time.h>` resolves to `third_party/ish/kernel/time.h`
# and explodes.
guest_arch = get_option('guest_arch')
ish_cflags = [
    '-I' + ish_src_dir,
    '-I' + ish_src_dir / 'vdso' / guest_arch,
]

embed_includes = include_directories(
    'include',
    'protocol',
    'ffi',
)

cc = meson.get_compiler('c')
threads = dependency('threads')

# ish_ffi.c includes kernel headers that depend on
# -DGUEST_X86 / -DGUEST_ARM64.
add_project_arguments('-DGUEST_' + guest_arch.to_upper() + '=1', language: 'c')

libishffi = static_library('ishffi',
    ['ffi/ish_ffi.c'],
    include_directories: [embed_includes],
    c_args: ish_cflags,
    install: false,
)

libishembed_sources = ['host/ishembed.c', 'host/ishembed_sha256.c']
libishembed_c_args = []
supervisor_blob_src = get_option('supervisor_blob_src')
if supervisor_blob_src != ''
    libishembed_sources += supervisor_blob_src
    libishembed_c_args += '-DISH_EMBED_BUNDLED_SUPERVISOR=1'
endif

libishembed = static_library('ishembed',
    libishembed_sources,
    include_directories: [embed_includes],
    dependencies: [threads],
    c_args: libishembed_c_args,
    install: false,
)

ishembed_dep = declare_dependency(
    link_with: [libishembed, libishffi],
    include_directories: [embed_includes],
    dependencies: [threads],
)

# Host smoke test executable (linked against iSH static libs from
# ish_build_dir if present).
ish_build_dir = get_option('ish_build')
if ish_build_dir != ''
    libish_a = cc.find_library('ish',
        dirs: [ish_build_dir],
        required: false)
    libish_emu_a = cc.find_library('ish_emu',
        dirs: [ish_build_dir],
        required: false)
    libfakefs_a = cc.find_library('fakefs',
        dirs: [ish_build_dir],
        required: false)
    sqlite3 = cc.find_library('sqlite3', required: false)
    if libish_a.found() and libish_emu_a.found() and libfakefs_a.found() and sqlite3.found()
        ish_test_deps = [
            ishembed_dep,
            libish_a, libish_emu_a, libfakefs_a, sqlite3, threads,
        ]
        executable('ishembed_smoke',
            ['c-tests/smoke.c'],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            install: false,
        )
        executable('procfs_test',
            ['c-tests/procfs_test.c'],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            install: false,
        )
        executable('codex_test',
            ['c-tests/codex_test.c'],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            install: false,
        )
        executable('provision_codex',
            [
                'c-tests/provision_codex.c',
                'c-tests/provision_codex_format.c',
            ],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            install: false,
        )
        dirty_page_test = executable('dirty_page_test',
            ['c-tests/dirty_page_test.c'],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            c_args: ish_cflags,
            install: false,
        )
        test('dirty_page_invalidation', dirty_page_test, timeout: 10)
    endif
endif

# protocol unit tests are pure-C, no iSH deps
proto_test = executable('proto_test',
    ['c-tests/proto_test.c'],
    include_directories: [embed_includes],
    install: false,
)
test('proto_test', proto_test)

# Pure-C boundary regression for the validated npm package/version tuple used
# by provision_codex. This must reject truncation rather than install a prefix.
provision_codex_format_test = executable('provision_codex_format_test',
    [
        'c-tests/provision_codex_format.c',
        'c-tests/provision_codex_format_test.c',
    ],
    install: false,
)
test('provision_codex_format', provision_codex_format_test)

# Dependency-free SHA-256 known-answer tests protect the runtime check applied
# to the generated bundled-supervisor bytes before they enter fakefs.
sha256_test = executable('sha256_test',
    ['host/ishembed_sha256.c', 'c-tests/sha256_test.c'],
    install: false,
)
test('sha256_known_answers', sha256_test)

# Host-only deterministic lifecycle tests. The test file supplies a fake FFI
# and supervisor pipe peer, so no iSH static libraries or RootFS are required.
lifecycle_test_sources = [
    'host/ishembed.c',
    'host/ishembed_sha256.c',
    'c-tests/lifecycle_test.c',
]
lifecycle_test_c_args = [
    '-DISH_EMBED_HELLO_TIMEOUT_MS=25',
    '-DISH_EMBED_TESTING=1',
    '-DISH_EMBED_BUNDLED_SUPERVISOR=1',
    '-DISH_EMBED_MAX_CONTROL_QUEUE_BYTES=131072u',
    '-DISH_EMBED_MAX_CONTROL_QUEUE_FRAMES=4u',
    '-DISH_EMBED_CONTROL_CRITICAL_RESERVE_BYTES=16u',
    '-DISH_EMBED_CONTROL_CRITICAL_RESERVE_FRAMES=1u',
]
lifecycle_test = executable('lifecycle_test',
    lifecycle_test_sources,
    include_directories: [embed_includes],
    dependencies: [threads],
    c_args: lifecycle_test_c_args,
    install: false,
)
test('lifecycle_bundled_supervisor_digest_mismatch',
    lifecycle_test,
    args: ['bundled-supervisor-digest-mismatch'], timeout: 5)
test('lifecycle_bundled_supervisor_path_mismatch',
    lifecycle_test,
    args: ['bundled-supervisor-path-mismatch'], timeout: 5)
test('lifecycle_boot_timeout', lifecycle_test,
    args: ['boot-timeout'], timeout: 5)
test('lifecycle_bad_hello_ack', lifecycle_test,
    args: ['bad-hello-ack'], timeout: 5)
test('lifecycle_install_failure', lifecycle_test,
    args: ['install-failure'], timeout: 5)
test('lifecycle_custom_supervisor', lifecycle_test,
    args: ['custom-supervisor'], timeout: 5)
test('lifecycle_boot_null_output', lifecycle_test,
    args: ['boot-null-output'], timeout: 5)
test('lifecycle_stdin_close_order', lifecycle_test,
    args: ['stdin-close-order'], timeout: 10)
test('lifecycle_control_frame_limit', lifecycle_test,
    args: ['control-frame-limit'], timeout: 10)
test('lifecycle_control_critical_close', lifecycle_test,
    args: ['control-critical-close'], timeout: 10)
test('lifecycle_control_same_session_close', lifecycle_test,
    args: ['control-same-session-close'], timeout: 10)
test('lifecycle_control_exited_same_session_close', lifecycle_test,
    args: ['control-exited-same-session-close'], timeout: 10)
test('lifecycle_signal_close_order', lifecycle_test,
    args: ['signal-close-order'], timeout: 10)
test('lifecycle_resize_close_order', lifecycle_test,
    args: ['resize-close-order'], timeout: 10)
test('lifecycle_terminate_close_order', lifecycle_test,
    args: ['terminate-close-order'], timeout: 10)
test('lifecycle_control_critical_oneshot', lifecycle_test,
    args: ['control-critical-oneshot'], timeout: 10)
test('lifecycle_control_preblocked_oneshot', lifecycle_test,
    args: ['control-preblocked-oneshot'], timeout: 10)
test('lifecycle_control_byte_limit', lifecycle_test,
    args: ['control-byte-limit'], timeout: 10)
test('lifecycle_control_byte_reserve', lifecycle_test,
    args: ['control-byte-reserve'], timeout: 10)
test('lifecycle_control_spawn_gate', lifecycle_test,
    args: ['control-spawn-gate'], timeout: 10)
test('lifecycle_control_oneshot_spawn_lock', lifecycle_test,
    args: ['control-oneshot-spawn-lock'], timeout: 10)
test('lifecycle_supervisor_error', lifecycle_test,
    args: ['supervisor-error'], timeout: 5)
test('lifecycle_close_race', lifecycle_test,
    args: ['close-race'], timeout: 30)
test('lifecycle_backlog', lifecycle_test,
    args: ['backlog'], timeout: 15)
test('lifecycle_frame_backlog', lifecycle_test,
    args: ['frame-backlog'], timeout: 15)
test('lifecycle_backlog_control_pressure', lifecycle_test,
    args: ['backlog-control-pressure'], timeout: 20)
test('lifecycle_borrow_shutdown', lifecycle_test,
    args: ['borrow-shutdown'], timeout: 10)
test('lifecycle_double_shutdown', lifecycle_test,
    args: ['double-shutdown'], timeout: 10)
test('lifecycle_active_call', lifecycle_test,
    args: ['active-call'], timeout: 10)
test('lifecycle_broken_control', lifecycle_test,
    args: ['broken-control'], timeout: 10)
test('lifecycle_protocol_fatal', lifecycle_test,
    args: ['protocol-fatal'], timeout: 10)
test('lifecycle_protocol_fatal_control_pressure', lifecycle_test,
    args: ['protocol-fatal-control-pressure'], timeout: 20)
foreach malformed_event : ['spawned', 'stdout', 'stderr', 'exited', 'shutdown-ack', 'pong', 'unknown']
    test('lifecycle_malformed_' + malformed_event, lifecycle_test,
        args: ['malformed-event', malformed_event], timeout: 10)
endforeach
test('lifecycle_output_allocation_failure', lifecycle_test,
    args: ['output-allocation-failure'], timeout: 10)
test('lifecycle_spawn_argument_bound', lifecycle_test,
    args: ['spawn-argument-bound'], timeout: 10)
test('lifecycle_shutdown_drain', lifecycle_test,
    args: ['shutdown-drain'], timeout: 10)
test('lifecycle_log_backpressure', lifecycle_test,
    args: ['log-backpressure'], timeout: 10)
test('lifecycle_oneshot_timeout', lifecycle_test,
    args: ['oneshot-timeout'], timeout: 10)
test('lifecycle_oneshot_output', lifecycle_test,
    args: ['oneshot-output'], timeout: 10)

# The supervisor runs inside Linux/iSH, but its stdin queue has a host-only
# deterministic harness. Keep it out of iOS cross builds while running it in
# native CI with the same sanitizer/werror settings as the lifecycle suite.
if not meson.is_cross_build()
    supervisor_stdin_test = executable('supervisor_stdin_test',
        ['c-tests/supervisor_stdin_test.c'],
        c_args: ['-DISH_SUPERVISOR_TESTING=1'],
        install: false,
    )
    test('supervisor_stdin', supervisor_stdin_test, timeout: 10)
endif
