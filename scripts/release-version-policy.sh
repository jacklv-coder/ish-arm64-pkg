#!/usr/bin/env bash

# Stage1 releases are deliberately named ABI-transition publications. Keeping
# the next authorized tag as an exact allowlist prevents both reusing an
# existing tag and accidentally creating a stable tag before Stage2 is
# integrated and separately authorized.
ish_release_stage1_version_allowed() {
    [[ "$1" == "v0.4.0-abi.13" ]]
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
本维护系列已加入无 shell、无 check-then-rename 竞争窗口的 guest 原子重命名能力。
`ish_embed_rename_noreplace` 通过与当前 runtime 精确匹配的内容寻址 supervisor 执行
Linux `renameat2(RENAME_NOREPLACE)`；目标已存在时返回 guest `EEXIST`，不会覆盖文件。
Swift `IshInstance.renameNoReplace` 提供对应类型化错误，并在链接旧 native binary 时
明确返回 unsupported。
本维护系列提供单次 stdin write/close 的显式 timeout API。每次调用的 deadline
与原始 SPAWN deadline 取更早值；控制 writer 停滞时有界返回，且超时调用不会发布
late frame。本版本还更新固定的 iSH kernel：强制 guest task teardown 会串行化，
group exit 开始后拒绝创建替代 interval timer，避免 host pthread 或 timer callback
在 task/address-space 已进入最终回收阶段后继续持有它们。最终清理统一采用
`pids_lock -> ptrace.lock` 全局锁顺序，避免与 wait4/ptrace lookup 形成 ABBA 死锁。
强制分离的 fd table 还会逐项记录延迟 descriptor 引用；共享 host handle 只在最后
一个可运行 owner 退出后关闭，从而唤醒阻塞 syscall，同时保留复制 fd table 等外部
owner 仍在使用的 descriptor。poll/socket/futex/condition/vfork/native wait 会观察
强制分离；native output worker 在 handler 运行前发布，并可在终端背压时定向取消。
handler 会先恢复 cwd、释放 fd/argv，再完成 task 退出；宿主 `SIGTERM` 不再用作
pthread 终止手段。
本版本还让破坏性 guest wait 保留 zombie，直到所有强制分离的 host pthread 与较晚的
thread-group member 都释放旧进程资源。`WNOWAIT` 仍可用于观察，最后一个 owner 会唤醒
waiter，避免 replacement embedded process 过早复用 runtime。
本维护版进一步为 Apple 平台采用写者优先的内部读写锁：已有写者排队时，后到的读者
不能持续插队，从而避免代码脏页失效路径在高并发 guest 读操作下长期阻塞。supervisor
在 `WNOWAIT` 已观察到 zombie、但 destructive `waitpid(WNOHANG)` 暂时返回 0 时，也会在
原清理期限内重试，等待 guest thread group 完成资源静止，而不是错误地 fail-close VM。
本维护版还补齐 AArch64 AdvSIMD `REV16` 向量指令：64 位 `.8B` 形式遵循架构语义清零
目标寄存器高 64 位，128 位 `.16B` 形式交换每个 16 位元素内的两个字节。该指令会在
Rust/TLS 网络路径中出现，缺失时 guest 会以 `SIGILL` 退出。
公开 C ABI 版本仍为 1；这些变更向后兼容。Swift 源仍不调用 retain/release；
完整 Swift lifecycle、类型化状态与 Terminal/VT 改造将在 Stage2 交付。
本 Release 不包含 RootFS，发布脚本也不会上传 RootFS。

This is not stable v0.4. It delivers the native lifecycle foundation: session
retain/release, a joinable kernel thread, soft-halt, and internal exact-match
wire protocol v4. This maintenance series includes guest-atomic rename without a
shell or a check-then-rename race. `ish_embed_rename_noreplace` invokes Linux
`renameat2(RENAME_NOREPLACE)` through the content-addressed supervisor that
exactly matches the running runtime. An existing destination returns guest
`EEXIST` and is never replaced. Swift `IshInstance.renameNoReplace` exposes a
typed error and reports unsupported when linked to an older native binary.
This maintenance series also includes explicit per-call timeout APIs for streaming
stdin write/close. Each call uses the earlier of its own deadline and the
original SPAWN deadline; a stalled control writer returns boundedly and a timed
out call publishes no late frame. This release also advances the pinned iSH
kernel to serialize forced guest-task teardown and reject replacement interval timers
once group exit starts. That prevents a host pthread or timer callback
from retaining task/address-space state after the teardown path has made it
eligible for disposal. Final cleanup follows the global
`pids_lock -> ptrace.lock` order to prevent an ABBA deadlock with wait4/ptrace
lookup. Force-detached fd tables also account for every deferred descriptor
reference. A shared host handle closes only after the last runnable owner exits,
waking blocked syscalls without invalidating a descriptor still retained by a
copied table or another external owner. Poll, socket, futex, condition, vfork,
and native waits observe forced detachment. Native output workers are published
before handlers run and can be cancelled under terminal backpressure; handlers
restore cwd and release retained fds/argv before completing task exit.
Host `SIGTERM` is no longer used to terminate a pthread.
Destructive guest waits now keep a zombie process visible until every
force-detached host pthread and late thread-group member has released the old
process. Observational `WNOWAIT` remains available, while the final owner wakes
the waiter before a replacement embedded process can start.
This maintenance release also uses an internal writer-preferring rwlock on
Apple platforms. Once a writer is queued, later readers cannot barge forever,
so code-dirty invalidation cannot starve behind sustained concurrent guest
reads. If `WNOWAIT` has observed a zombie but destructive `waitpid(WNOHANG)`
temporarily returns zero, the supervisor now retries within the existing
cleanup deadline while the guest thread group finishes releasing resources,
rather than incorrectly fail-closing the VM.
This maintenance release also implements the AArch64 AdvSIMD vector `REV16`
instruction. The 64-bit `.8B` form clears the destination register's upper
64 bits as required by the architecture, while `.16B` swaps the two bytes in
every 16-bit element. Rust/TLS network paths can emit this instruction; without
it, the guest exits with `SIGILL`.
The public C ABI remains version 1; these changes are backward compatible.
Swift source still does not call
retain/release; the complete Swift
lifecycle, typed statuses, and Terminal/VT changes remain Stage2. This Release
does not contain a RootFS, and the release script never uploads one.
EOF
            ;;
        *)
            ;;
    esac
}
