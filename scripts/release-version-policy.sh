#!/usr/bin/env bash

# Stage1 releases are deliberately named ABI-transition publications. Keeping
# the next authorized tag as an exact allowlist prevents both reusing an
# existing tag and accidentally creating a stable tag before Stage2 is
# integrated and separately authorized.
ish_release_stage1_version_allowed() {
    [[ "$1" == "v0.4.0-abi.7" ]]
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
本维护版本新增无 shell、无 check-then-rename 竞争窗口的 guest 原子重命名能力。
`ish_embed_rename_noreplace` 通过与当前 runtime 精确匹配的内容寻址 supervisor 执行
Linux `renameat2(RENAME_NOREPLACE)`；目标已存在时返回 guest `EEXIST`，不会覆盖文件。
Swift `IshInstance.renameNoReplace` 提供对应类型化错误，并在链接旧 native binary 时
明确返回 unsupported。
公开 C ABI 版本仍为 1；这是向后兼容的新增符号。Swift 源仍不调用 retain/release；
完整 Swift lifecycle、类型化状态与 Terminal/VT 改造将在 Stage2 交付。
本 Release 不包含 RootFS，发布脚本也不会上传 RootFS。

This is not stable v0.4. It delivers the native lifecycle foundation: session
retain/release, a joinable kernel thread, soft-halt, and internal exact-match
wire protocol v4. This maintenance release adds guest-atomic rename without a
shell or a check-then-rename race. `ish_embed_rename_noreplace` invokes Linux
`renameat2(RENAME_NOREPLACE)` through the content-addressed supervisor that
exactly matches the running runtime. An existing destination returns guest
`EEXIST` and is never replaced. Swift `IshInstance.renameNoReplace` exposes a
typed error and reports unsupported when linked to an older native binary. The
public C ABI remains version 1; the new symbol is additive and backward
compatible. Swift source still does not call
retain/release; the complete Swift
lifecycle, typed statuses, and Terminal/VT changes remain Stage2. This Release
does not contain a RootFS, and the release script never uploads one.
EOF
            ;;
        *)
            ;;
    esac
}
