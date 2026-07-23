#!/usr/bin/env bash

# Stage1 releases are deliberately named ABI-transition publications. Keeping
# the next authorized tag as an exact allowlist prevents both reusing an
# existing tag and accidentally creating a stable tag before Stage2 is
# integrated and separately authorized.
ish_release_stage1_version_allowed() {
    [[ "$1" == "v0.4.0-abi.2" ]]
}

# Call only after the release entry point has validated strict SemVer.
ish_release_github_prerelease() {
    case "$1" in
        *-*) printf 'true\n' ;;
        *)   printf 'false\n' ;;
    esac
}

# Emit the extra bilingual notice only for the native ABI-transition series.
# The release entry point validates strict SemVer before calling this helper.
ish_release_abi_transition_notes() {
    case "$1" in
        v*-abi.*)
            cat <<'EOF'

## Native ABI 过渡预发布 / Native ABI transition prerelease

这不是稳定 v0.4。本预发布交付 native lifecycle 基础：session
retain/release、可等待 kernel thread、soft-halt 与内部精确匹配 wire protocol v4。
本维护版本还修复 procfs 读取与 task 地址空间退出之间的锁生命周期竞态。
公开 C ABI 版本仍为 1。Swift 源仍保持 v0.3.3 ABI 兼容且不调用 retain/release；
完整 Swift lifecycle、类型化状态与 Terminal/VT 改造将在 Stage2 交付。
本 Release 不包含 RootFS，发布脚本也不会上传 RootFS。

This is not stable v0.4. It delivers the native lifecycle foundation: session
retain/release, a joinable kernel thread, soft-halt, and internal exact-match
wire protocol v4. This maintenance release also fixes lock-lifecycle races
between procfs reads and task address-space teardown. The public C ABI remains
version 1. Swift source remains v0.3.3-ABI compatible and does not call
retain/release; the complete Swift
lifecycle, typed statuses, and Terminal/VT changes remain Stage2. This Release
does not contain a RootFS, and the release script never uploads one.
EOF
            ;;
        *)
            ;;
    esac
}
