# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the C glue that turns iSH into an embeddable runtime:
#   - libishembed.a: host-side embedding glue (pumps, sessions, lifecycle)
#   - libishffi.a:   FFI shims into iSH internals
#
# iSH itself is a separate Meson project under third_party/ish/ (managed
# as a git submodule pointing at Lolendor/ish-arm64). scripts/build-ios.sh
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

libishembed = static_library('ishembed',
    ['host/ishembed.c'],
    include_directories: [embed_includes],
    dependencies: [threads],
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
            ['c-tests/provision_codex.c'],
            dependencies: ish_test_deps,
            include_directories: [embed_includes],
            install: false,
        )
    endif
endif

# protocol unit tests are pure-C, no iSH deps
proto_test = executable('proto_test',
    ['c-tests/proto_test.c'],
    include_directories: [embed_includes],
    install: false,
)
test('proto_test', proto_test)
