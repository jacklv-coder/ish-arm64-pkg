# IshEmbed

简体中文｜[English](README.en.md)

IshEmbed 是面向 iOS 宿主应用的嵌入式 Linux 运行时 Swift Package。它基于
[iSH](https://github.com/ish-app/ish) 的用户态模拟器、Linux 系统调用层和 fakefs，
让 SwiftUI/UIKit 应用可以启动一个 Linux 用户空间、执行一次性命令、管理流式会话，
并在同一 fakefs 中维护多个持久化 chroot 目录树。

本仓库是底层运行时组件，不是最终用户应用。PocketRoot 在它之上负责经过校验的
RootFS 安装、产品级命令策略、Swift Concurrency 隔离和界面。项目最低支持 iOS 18，
发布的 device 与 simulator 二进制切片均为 arm64。

## 当前阶段：Native ABI 过渡

当前分支是计划发布为 `v0.4.0-abi.1` 的 **Stage1 native ABI 过渡**，不是稳定
`v0.4.0`，也不是完整 v0.4 Swift API。请同时区分下面四个版本面：

| 版本面 | Stage1 合入、发布前 | 发布 `v0.4.0-abi.1` 后 |
| --- | --- | --- |
| 公开 C ABI | `ISH_EMBED_ABI_VERSION == 1`；增加兼容性符号 | 仍为 ABI 1；新符号随过渡 XCFramework 发布 |
| 内部 wire protocol | host 与内嵌 supervisor 精确匹配 v4 | 仍为 v4；它不是公开 C ABI 版本 |
| `Package.swift` | 仍固定上游 v0.3.3 URL/checksum | 发布事务生成只改 manifest 的 release commit，固定到过渡二进制 |
| Swift 源 | 保持 v0.3.3 ABI 兼容，不调用 retain/release | 仍保持旧 ABI 用法，可与过渡二进制组合 |

Stage1 的 native runtime 已加入 session retain/release、可等待 kernel 线程、soft-halt、
严格 v4 协议和完整 session close 等底层能力。现有 Swift wrapper 刻意不调用新增
retain/release；它通过兼容 v0.3.3 的 Swift call gate 同时保护 instance 与 session：
oneshot 在调用期持有 instance lease，spawn 将同一 lease 转交给 session，直到 native close
完成才释放。存在活动调用或 session 时，shutdown 会先返回 busy，绝不进入旧 binary 的释放
路径；`BUSY` 恢复运行态，其他 shutdown 失败保留仅供清理重试的 handle。完整 native
borrow/cancel、类型化状态、
Terminal callback 队列与 VT parser 改造属于 **Stage2**，本阶段尚未交付。

RootFS 不提交到本仓库、不包含在 XCFramework 或 GitHub Release 中。应用必须通过独立
流程固定来源、大小、SHA-256、许可证和安装事务。

## 能力与边界

- 每个宿主进程只支持一个有效的 `IshInstance` 生命周期，可并发运行多个 command session。
- 支持一次性命令与流式 session；流式 session 可读写标准流、关闭 stdin、发送信号、
  调整 PTY 尺寸并等待退出。
- `/srv/vms/<name>` 下的目录树可作为持久化 chroot。它隔离文件视图，但不是硬件虚拟机，
  也不是针对恶意代码的安全边界。
- native reader 对协议帧和每个 session 的输出积压设置硬上限；host→guest 控制队列也有
  4 MiB/256 帧总预算，其中 4 KiB/16 帧保留给 close/shutdown 等关键生命周期消息；普通
  调用无法接纳下一帧时返回 `ISH_ERR_CONTROL_LIMIT`。业务层仍应设置更严格的输出配额、
  超时与取消策略。
- 发布 XCFramework 内嵌静态 AArch64 guest supervisor。默认 boot 会先对**实际内嵌
  bytes** 计算 SHA-256，与同一构建生成的摘要及内容寻址 guest path 一致后，才安装到
  fakefs；不匹配会以 `ISH_ERR_SUPERVISOR_INSTALL` 失败。显式 `supervisorGuestPath` 会
  绕过默认 blob 的安装与这项摘要门禁，由调用方负责来源、完整性和协议兼容性。该摘要
  比对不是数字签名，也不认证下载来源或发布者身份。
- 固定的 iSH fork 对 JIT 写路径采用正确性优先的一致性：单页写和显式
  `asbestos_invalidate_page` 按精确页过滤，跨页 block 的第二页按 `end_addr` 判断；只有
  多页脏集合的哈希位图可能因碰撞保守地多失效。直链/RET cache 会在下一目标 block 与
  待处理代码脏页相交时回到 dispatcher；纯数据写可继续直链，但仍保留待消费脏状态。
  这避免了无关数据写一律打断直链，写密集路径仍有真实的一致性检查成本。
- guest PID 1 不只清理 tracked process group；double-fork/`setsid` 后由 subreaper 收养的
  未跟踪后代也必须按精确 PID 清理并连续两次扫描为空，之后才发布成功退出。无法完成或
  证明清理时，整个 guest instance 会 fail-close，不会伪造成功的 `EXITED`/shutdown ACK。

## 为什么维护 iSH fork

`third_party/ish` 固定到本项目维护的 `ish-arm64` fork。join/soft-halt、嵌入式生命周期与
JIT 脏页一致性必须修改模拟器核心，无法只在 outer package 或 Swift 层实现；fork 让这些
窄差异拥有独立 PR、CI 和精确 gitlink，PocketRoot 的构建与发布也因此可复现。我们不会在
本地直接改写别人维护的上游仓库；适合通用化的修复仍可回馈
[iSH upstream](https://github.com/ish-app/ish)，但在上游接受并发布前由 fork 承担项目门禁。
当前这组源码、测试和文档变更不纳入 RootFS，也不提交任何预构建 XCFramework/guest
binary；二进制只能在后续发布事务通过后生成和发布。

## 安装状态

在 `v0.4.0-abi.1` 真正发布前，本 fork 没有可安装的 Stage1 制品。当前
[`Package.swift`](Package.swift) 仍指向 v0.3.3；这是发布事务开始前的预期状态，不能把
工作树中的新 native 源码误认为 manifest 已经引用的新二进制。

过渡 Release 发布并验证后，可在 Xcode 的 **File → Add Package Dependencies…** 中使用：

```text
https://github.com/jacklv-coder/ish-arm64-pkg
```

请选择明确包含 `libIshKernel.xcframework.zip`、对应源码归档，并且 manifest URL/checksum
与同一标签匹配的版本。业务工程不需要安装 Meson、Zig 或 LLVM。

## Swift 使用方式

Stage1 沿用 v0.3.3 兼容的 Swift API。宿主应用先将经过校验的 fakefs RootFS 安装到可写
目录；目录顶层应包含 `data/` 与 `meta.db`。

```swift
import Foundation
import IshEmbed

let instance = IshInstance.shared
let rootfs = try installedRootFSURL() // 由宿主实现校验和原子安装
try instance.boot(.init(rootfsPath: rootfs.path))

let result = try instance.runOneshot(
    .init(argv: ["/bin/echo", "hello"], timeout: 10)
)
print(String(decoding: result.stdoutData, as: UTF8.self))
```

`IshSpawnOptions.timeout` 只由 `runOneshot` 使用。有限超时从 API 入口开始，包含
SPAWN staging gate 和控制队列接纳；如果 runtime 无法确认命令已清理，会转入
shutting-down 状态而不是遗留无主 guest 进程。NaN/正负无穷会在进入 native 前返回
`ISH_ERR_INVALID_ARG (-13)`；正但小于 1 ms 的值向上取整为 1 ms，不会退化成“无超时”。

只有当已校验的 RootFS manifest 明确包含 `/srv/vms/.template` 时，才可使用
`ensureDefaultVM()` 和 `spawn(in:)` 这组 VM helper。流式 session 必须持续消费输出，
并确保最终关闭：

```swift
let vm = try instance.ensureDefaultVM()
let session = try instance.spawn(
    in: vm,
    .init(argv: ["/bin/sh"], allocateTTY: false)
)
defer { session.close() }

try session.write(Data("printf 'ready\\n'\n".utf8))
try session.closeStdin()

readLoop: while true {
    switch try session.read(timeout: nil) {
    case .data(let data, _, _):
        FileHandle.standardOutput.write(data)
    case .exited(let code, let signal):
        print("exit=\(code), signal=\(signal)")
        break readLoop
    }
}
```

`read(timeout:)` 的有限等待到期时会抛出 code 为 `ISH_ERR_TIMEOUT (-12)` 的
`IshError.raw`，不会返回“无事件”。非有限 timeout 同样返回 `ISH_ERR_INVALID_ARG (-13)`；
`nil` 才表示无限等待。

取消时可先关闭 stdin，再调用 `terminate()`；TTY 的 Ctrl+C 可用 `interrupt()`。调用
`close()` 前仍应先停止并等待该 session 的任务；session gate 会等待已进入的 C 调用，不能
强制取消永久不返回的非 read 调用。instance gate 会让 oneshot 和存活 session 持有 lease；
此时 `shutdown()` 立即抛出 `ISH_ERR_BUSY`，不会调用 v0.3.3 的 native free。先停止并等待
spawn/runOneshot，再关闭所有 session，然后调用 `try instance.shutdown()`；`BUSY` 可在清理
业务调用后重试，其他失败会隔离普通调用但保留 shutdown 清理重试。Stage1 Swift 的
`IshError.raw(Int32, String)` 仍是公开错误形式；类型化状态转换留到 Stage2。

## C ABI 与 wire protocol

[`include/ishembed.h`](include/ishembed.h) 是公开 C ABI 的权威定义。Stage1 增加了
`ish_embed_session_retain`、`ish_embed_session_release` 等符号，但 ABI 版本常量仍为 1，
现有 Swift 源也尚未使用这些新增符号。

[`protocol/proto.h`](protocol/proto.h) 是 host 与 guest PID 1 之间内部帧协议的权威定义。
wire v4 要求两端精确匹配，增加 `SESSION_CLOSE`；wire 版本不等于公开 C ABI 版本。

## 文档

- [技术文档导航](docs/README.md)
- [架构与生命周期](docs/architecture.md)
- [测试与验收](docs/testing.md)
- [故障排查](docs/troubleshooting.md)
- [发布事务](docs/releasing.md)
- [变更日志](CHANGELOG.md)

文档以中文为主，并同步维护英文镜像。

## 维护者构建与验证

```sh
brew install meson ninja sqlite libarchive llvm lld zig
git submodule update --init
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/build-rootfs.sh --print-inputs
scripts/build-rootfs.sh --print-identity
scripts/build-rootfs.sh --verify-bundle build
scripts/run-host-tests.sh
scripts/verify-ios-artifact.sh
scripts/test-swift-ios.sh --local-binary
scripts/check-docs.sh
```

iSH 内部的嵌套子模块不是本 package 构建前置；上述命令只初始化本仓库固定的
`third_party/ish`，避免隐式抓取未参与构建的其他仓库。

干净检出第一次运行 host tests 时，会使用 `scripts/alpine-rootfs-pin.sh` 中经过审阅的
Alpine 版本、架构和 SHA-256，并从脚本固定的官方下载地址自动准备开发 RootFS。
`--print-inputs` 可在下载前审计这些输入；尝试其他 Alpine 版本时必须同时显式提供经过
复核的 `ALPINE_SHA256`。
builder 在稳定普通锁文件上持有内核 `flock`，并在唯一同卷 staging 中生成
`fs`、`fs.tar.gz`、`SHA256SUMS` 与 `ROOTFS_RECEIPT`。首次发布使用 no-replace rename，
替换使用原子 exchange 保持 `build/fs` 始终可见；receipt 最后发布，是四件套的提交点。
任何可捕获信号或中途失败都会逆序回滚，成功后旧代际随 staging 删除，不保留
`fs.previous.*`。`--print-identity` 给出 recipe 前缀，覆盖 builder、supervisor、协议头、
iSH revision/工作树/子模块、fakefsify 来源和 Alpine pin；marker 再绑定实际 fakefsify、
arm64 supervisor、BusyBox 与初始 meta/data seal。runner 从验证到最后一个 consumer 都持有
RootFS/Codex 锁，并验证 receipt、recipe、SQLite 行类型/16-byte stat/root、meta/data 全路径、
关键摘要；合法运行态写入仍可复用。`build/fs-codex` 身份还绑定 clean receipt/当前内容、
包名、exact/tag 请求分类、VM/bin、provision 输入与实际安装版本；exact SemVer 必须与实际
版本逐字相等，tag 则记录解析后的实际版本。复用还会核对 `package.json` bin、guest 可执行
模式以及安全映射到 package target 的全局入口，任一失配即重新 provision。该身份只用于本地
开发测试，不能替代产品 RootFS 的来源、许可证、最终内容摘要和安装事务。
`ROOTFS_RECEIPT` 是 lineage/初始快照凭据：它绑定静态 identity marker 与构建时的初始
`fs.tar.gz`/`SHA256SUMS`。运行态写入改变 `fs` 后，`--verify-bundle` 会验证当前树结构、
数据库/关键二进制身份以及初始归档的完整性，但不证明当前 `fs` 字节等于 tar；该 tar
不是当前树备份，也不是可提交或发布的资产。
`scripts/build-rootfs.sh` 的输出不是 package/release 资产，固定摘要也不代表授权分发。
完整测试矩阵与两阶段真链接门禁见[测试文档](docs/testing.md)。

## 源码布局

```text
ish-arm64-pkg/
├── Package.swift                    SwiftPM manifest 与二进制固定值
├── Sources/CIshEmbed/               C module map
├── Sources/IshEmbed/                v0.3.3 兼容 Swift wrapper
├── include/ishembed.h               公开 C ABI
├── protocol/proto.h                 内部 host ↔ supervisor wire protocol
├── host/                            lifecycle、session 与 I/O pump
├── ffi/                             对 iSH 内部能力的窄 FFI
├── supervisor/                      静态 AArch64 guest PID 1
├── c-tests/                         native/protocol/lifecycle 测试
├── scripts/                         构建、测试、合规与发布事务
└── third_party/ish/                 固定 revision 的 iSH 子模块
```

## 许可证

本仓库与 iSH 派生代码采用 GPL-3.0-or-later。发布 XCFramework 时必须同时提供匹配的
Corresponding Source。详情见 [LICENSE](LICENSE)、[NOTICE.md](NOTICE.md) 与
[发布指南](docs/releasing.md)。RootFS 需独立完成来源和许可证审查。
