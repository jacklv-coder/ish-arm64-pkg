# IshEmbed 架构与生命周期

简体中文｜[English](architecture.en.md)

本文描述 Stage1 native ABI 过渡实现。公开 C ABI 仍为版本 1，内部 wire protocol 为
精确匹配 v4；Stage1 Swift wrapper 保持 v0.3.3 ABI 兼容。完整 Swift lifecycle 与
Terminal/VT 改造不在本文的已交付范围内。

## 分层与职责

```text
PocketRoot / SwiftUI / UIKit
          │
          ▼
Sources/IshEmbed
v0.3.3-compatible Swift API
          │
          ▼
include/ishembed.h                    public C ABI = 1
          │
          ▼
host/ishembed.c + ffi/ish_ffi.c       instance/session/threads/queues
          │
          ▼
protocol/proto.h                      internal exact-match wire = 4
          │
          ▼
supervisor/ishsv.c                    guest PID 1 / child / PTY / chroot
          │
          ▼
third_party/ish + fakefs RootFS       emulator/kernel state + persistence
```

- Swift 层提供 `IshInstance`、`IshSession`、`IshVM` 及当前 Terminal/VT 接口。
- C host 层拥有 instance、session table、I/O pumps、队列、引用计数和 shutdown 协调。
- FFI 只暴露嵌入所需的 iSH 内部能力。Stage1 固定的 iSH revision 包含 joinable kernel
  thread 与 soft-halt 支持；这是 runtime 生命周期所需的窄补丁，不是改写上游产品功能。
  该 fork 还带有实验性 arm64 BRK 跳过逻辑；production 与 host-test iSH 构建都显式定义
  `ISH_DISABLE_SKIP_BRK=1`，保留 fatal guest SIGTRAP 语义，不能让断言/陷阱伪造寄存器后继续。
- guest supervisor 作为 Linux PID 1 管理命令、PTY、信号、stdin 与 child reaping。
- RootFS 只提供 fakefs 持久化内容，由宿主独立安装，不属于 Release。

## Boot 流程

1. 宿主验证 RootFS 路径、布局、来源与哈希，将可写的 `data/` + `meta.db` 目录传给
   `ish_embed_boot`。
2. C host 创建双向协议管道与日志管道，准备 instance 状态；`out_instance == NULL`
   会在任何安装或线程副作用前被拒绝。
3. 未指定 `supervisor_guest_path` 时，release XCFramework 将内嵌静态 AArch64
   supervisor 原子安装到 fakefs 的内容寻址私有路径，并复核完整字节和执行模式；
   不替换 RootFS 自带的 `/sbin/ishsv`。自定义路径完全由调用方负责。
4. iSH kernel 在 joinable pthread 上启动，进入 guest PID 1 supervisor。
5. host 与 supervisor 交换 `HELLO`/`HELLO_ACK`，同时精确校验公开 ABI 版本 1 与
   wire protocol v4。只有握手成功后 boot 才返回 instance handle。

失败路径会停止已启动的 pumps、请求 kernel soft-halt 并 join 可等待线程。底层 iSH
仍使用进程级/TLS 状态，因此每个宿主进程只支持一次有效 instance lifecycle；成功
shutdown 后不要再次 boot。

## 公开 C ABI 1 与内部 wire v4

两个数字解决不同问题：

- `ISH_EMBED_ABI_VERSION == 1` 描述 App/Swift 与 XCFramework 导出 C 符号、结构体和
  调用约定的兼容面。Stage1 只做可兼容的增量，因此不提高 ABI 常量。
- `ISH_PROTO_VERSION == 4` 描述 host 与打包在同一 XCFramework 中的 guest supervisor
  帧协议。它们必须一起构建与发布，因此 v4 采用精确匹配而非跨版本协商。

每个 wire frame 有 12 字节 header（magic、version、type、flags、payload length、
session id）和最多 1 MiB payload。固定宽度 payload 字段按协议指定端序编码，不能直接
依赖 C struct 布局。v4 新增 `SESSION_CLOSE`，用于完整清理 live session。

## Session 生命周期与引用

`ish_embed_spawn` 返回的 C handle 拥有一个 owner reference：

1. native 调用开始前可用 `ish_embed_session_retain` 获取 borrow；成功后必须 release。
2. `ish_embed_session_close` 原子阻止新调用并消费 owner reference。
3. 已经成功 retain 的调用可以与 close 交错并完成；最后一个 reference 释放后才回收内存。
4. direct child 先成为不可离开其 process group 的 session leader，并通过专用 pipe
   向 supervisor 确认 `pid == pgid`；supervisor 只在确认后保存并使用独立 PGID。
   `waitid(..., WNOWAIT)` 观察到 leader 退出后，先清理同 group 的后台进程与 TTY
   foreground job，再用 `waitpid` 真正回收并立即清零 PID/PGID；后续 close 只释放
   残留资源，不会拿已释放的数值 identity 发信号。

这是 Stage1 **native 能力**。Stage1 Swift 源为兼容 v0.3.3 binary，不调用 retain/release，
而是用内部 `IshSessionCallGate` 提供旧 ABI 可用的 Swift 侧保护：

1. `withRaw` 在 `NSCondition` 下确认 handle 尚未 detach，并增加 `activeCalls`；
2. `close()` 先 detach handle，使所有新调用立即失败；
3. close 再等待 `activeCalls == 0`，最后才调用 native close，因此不会释放已进入 C 调用
   仍在使用的 v0.3.3 handle；
4. `read(timeout: nil)` 不再把一次 C read 永久阻塞，而是按 100 ms 调用 bounded read，
   在每次轮询间重新经过 gate，使 close 能让读循环退出。

该 gate 自动避免 close 与已进入 C 调用的 UAF，不再要求调用方用锁手工串行每次 I/O。
但 close 的语义是“等待”而不是“取消”：如果 v0.3.3 或自定义 supervisor 下某个非 read
C 调用永久不返回，`activeCalls` 不会归零，close 也会等待。调用方仍应先停止并等待任务。
全面采用 native retain/release 的 borrow/cancel/typed lifecycle 属于 Stage2。

旧 binary 同样没有 instance retain。`IshInstanceCallGate` 因此把状态序列化为
idle/booting/running/shutting-down，并为每次 oneshot 持有调用 lease。spawn 成功时不经过
零计数窗口，直接把 lease 转交给 `IshSession`；只有 native session close 返回后才释放。
shutdown 会先原子关闭新调用入口：只要仍有 oneshot 或 session lease，就立即返回
`ISH_ERR_BUSY` 而不进入 v0.3.3 native shutdown；没有 lease 时才独占 shutdown。native
失败会恢复同一 handle 和 running 状态，成功才清空 handle。这样 boot/shutdown、两个
shutdown caller 以及 shutdown/call 均不会交错释放旧 ABI handle。

## 线程与反压

host 侧把可能互相阻塞的职责分开：

- control writer 独占 host→guest fd，并按序发送控制帧；
- event reader 独占 guest→host fd，校验帧并分发到 session inbox；
- guest-log drain 持续排空不可信的 guest stderr；
- log-sink writer 将 best-effort 日志写向调用方 fd，慢日志接收器不能阻塞协议进度。

关键上限以 [`include/ishembed.h`](../include/ishembed.h) 和源码为准：协议 payload 1 MiB，
每 session 未消费输出 4 MiB/4096 帧，一次性 stdout 8 MiB、stderr 4 MiB。supervisor
还为每 session 维护有界 stdin 队列并处理 partial write/`EAGAIN`。达到上限会返回可观察
错误或终止 session，而不是无限增长宿主内存。

host→guest 控制路径另有 4 MiB/256 帧的全局总预算，其中 4 KiB/16 帧只供内部关键生命
周期消息使用。完整 wire frame（含 12 字节 header）从进入队列前的接纳检查起，到 queued、
in-flight 或同步 waiter 最终释放为止都计入预算；检查发生在复制 payload 之前。普通调用
无法接纳下一帧时返回 `ISH_ERR_CONTROL_LIMIT`，该帧不会进入管道，但同一次多帧
`session_write` 中更早的 chunk 可能已经送达。

`SESSION_CLOSE`、`SHUTDOWN` 与 oneshot 的 `STDIN_CLOSE → TERM → SIGKILL` 使用 reserve-aware
异步接纳，不会让调用线程在阻塞 writer 后无限等待。session close 若无法接纳、writer
失败或 1 秒内未观察到退出，会把 runtime 转入 shutting-down 并关闭 control direction；
guest PID 1 以 EOF 清理全部 child。spawn 的 measure/build/send/free 由单独 gate 串行化，
防止多个约 1 MiB 的 staging payload 在控制预算之外同时存在；stop、EOF 与错误 drain 都
走同一释放和记账路径。
关键字节 reserve 在编译期被要求至少容纳最小 lifecycle frame；有限 oneshot
deadline 从 API 入口开始，同时覆盖 spawn gate 等待和 SPAWN 队列接纳。

Stage1 Swift 尚未交付新的 typed status 和 Terminal callback 有界投递；不能把 native
积压上限误写成 Stage2 Swift callback 策略已完成。

`IshTerminal.setEventHandler(queue:_:)` 的公开 closure 仍保持原非 `@Sendable` 签名。
实现只在私有 `@unchecked Sendable` box 内承接既有 handlerQueue crossing，使严格并发
编译通过而不对调用方造成源码破坏；这不等于 Stage2 callback 队列/丢弃策略已交付。

## 信号、PTY 与回收

- pipe session 的 signal 发送给 tracked command process group。
- TTY 的 Ctrl+C/Ctrl+Z 等通常由 Swift 写入控制字节，经 tty layer 定向到 foreground
  process group；direct signal 仍只针对 tracked group。
- resize 对 PTY 执行窗口更新并触发 SIGWINCH；非 TTY session 可忽略。
- terminate 先 SIGTERM，supervisor 使用固定约 1.5 秒窗口，再按需 SIGKILL；现有
  `grace_ms` 参数为兼容形态。
- v4 session close 对 live TTY 同时处理 shell group、foreground job 与 transport。
  leader 正常退出也先由 `waitid(..., WNOWAIT)` 保留 zombie 作为 identity anchor，
  用 spawn 时经专用 pipe 确认的不可变 PGID SIGKILL 残余 group，再 `waitpid` 回收、
  清除 identity 并发布退出事件；
  因此同 process group 的后台子进程不会在 leader 之后逃逸，也不存在 reap 后 PGID
  复用误杀窗口。
- supervisor 还是 child subreaper。double-fork/`setsid` 脱离 tracked group 的后代最终会
  被 PID 1 收养；它会排除仍受其他 session/TTY 管理的进程，按精确 PID 杀死并回收未跟踪
  child，再要求连续两次 `/proc` 扫描为空，才发送成功 `EXITED`。
- 扫描、kill、reap 或两秒清理期限任一失败时，supervisor 采用 instance-wide fail-close：
  对该 guest 内进程执行兜底清理，退出且不发送成功 `EXITED`/`SHUTDOWN_ACK`。因此无法证明
  隔离边界已恢复时，协议会失败关闭而不是报告一次看似正常的 session 完成。

## Chroot 准备与隔离边界

绝对 `chroot_path` spawn 前，guest PID 1 会重新验证并按需修复必要的 device 节点、
devpts/procfs 挂载与运行目录。它不永久缓存“该路径已准备”，所以同路径 RootFS 被替换
或挂载丢失后仍会重新检查。

`/root/.codex/config.toml` 缺失时，supervisor 会在同目录完整写入临时文件，再以不可覆盖
的原子操作发布默认配置。已有的非符号链接普通文件会保留全部内容，仅把权限收紧为
`0600`；符号链接、目录、FIFO 等非普通文件会使 spawn 在 fork 前失败，不会被覆盖或跟随。

chroot 只隔离文件系统视图。所有 session 共享同一个 iSH kernel、宿主进程、内存预算
和攻击面，不能把它声明为强安全沙箱。PocketRoot 仍需命令 allowlist、资源预算与产品
层取消策略。

## Shutdown

推荐顺序：

1. 停止创建新命令；
2. 取消/等待所有 `spawn`、`runOneshot`、读取与写入任务；
3. 关闭全部 session；
4. 调用 `ish_embed_shutdown` / `IshInstance.shutdown()`；
5. 不再使用 instance，也不在同一进程重新 boot。

存在 live session 或活动 instance call 时，native shutdown 返回 `ISH_ERR_BUSY`。
Stage1 Swift 的 instance gate 还在进入 native 前执行旧 ABI 兼容保护：oneshot 和每个存活
session 都持有 lease；有 lease 时立即 busy，并发 boot/shutdown 也被状态机拒绝。因此
v0.3.3 shutdown 不会释放已经被 spawn/runOneshot/session 使用的 instance。只有 native
返回成功时才清除 Swift handle；任何失败（包括 busy）都恢复原 handle 与 running 状态，
调用方可在关闭 session/等待任务后重试。进入成功 shutdown 后，
runtime 请求 supervisor 退出、关闭/排空 pumps、启用 iSH soft-halt，等待并 join kernel
pthread，最后释放 instance。

该门禁防止旧 C runtime 的 instance UAF，但不取消业务调用：busy 后仍必须停止并等待
oneshot，关闭 session，再重试 shutdown。过渡 native runtime 也会对自己的 live
session/active call 返回 `ISH_ERR_BUSY` 或相应错误，调用方应遵守同一清理顺序。

## RootFS 与发布资产

RootFS、XCFramework 与 Corresponding Source 是三个独立对象：

| 对象 | 内容 | 本仓库 Release |
| --- | --- | --- |
| XCFramework | host runtime、固定 iSH 代码、内嵌 supervisor | 是 |
| Corresponding Source | 重建 XCFramework 所需父仓库、iSH revision、musl 源码等 | 是，与 binary 成对 |
| RootFS | Alpine 用户空间与 fakefs 数据 | 否，独立来源/哈希/许可/安装 |

发布脚本和源码策略会拒绝 RootFS 混入发布资产。PocketRoot 应固定它自己的 RootFS
清单，并使用同卷 staging、布局校验和原子替换。

仓库内的 host-test builder 使用 schema v2
`build/fs/.ishembed-rootfs-identity`，且 runner 永不 `source` 该数据文件。recipe 前缀覆盖
builder、supervisor/protocol、iSH revision/工作树/递归子模块、fakefsify 来源与 Alpine pin；
artifact 字段绑定实际 fakefsify、AArch64 supervisor、BusyBox 和初始 meta/data seal。

并发协调使用永久稳定的普通锁文件及内核 `flock`；文件存在不代表锁被占用，owner 中的
PID、进程启动身份和随机 token 只用于认证嵌套调用。builder 在唯一同卷 staging 生成
`fs`、tar、checksums 与 receipt。SQLite quick-check、正整数 inode、16-byte stat BLOB、
唯一空 root、meta/data 全路径、AArch64 ELF 与 seal 通过后，首次目标用 no-replace，已有
目标用 exchange；receipt 最后发布并提交整个代际。rename→journal 的可捕获信号会延迟，
失败逆序回滚，成功删除 staging 中的旧代际。SIGKILL/断电不能保证多路径回滚，但
receipt-last 会使半发布代际验证失败而不被复用。

runner 从验证、按需构建/provision 一直持锁到最后一次测试使用，关闭 validate→use 替换
窗口。复用允许正常运行造成的 meta/data 变化，但会重验四件套 receipt、recipe、数据库/
存储一致性和关键摘要。`fs-codex` 身份另外绑定 clean marker/receipt/当前内容、包名、
exact/tag 请求分类、VM/bin、provision script/binary、verifier 和实际 package.json 版本。
exact SemVer 必须逐字匹配实际版本；tag 可解析成不同实际版本，但两者都写入身份。验证器还
要求 package bin target 与全局入口在 host backing 和 fakefs metadata 中安全存在、guest
可执行，并且 symlink/hardlink 精确映射到同一 target；任何失配都重新 provision。它只是
开发测试输入身份，不是 PocketRoot 产品 RootFS manifest，也不
提供发布许可或最终安装完整性保证。

`ROOTFS_RECEIPT` 的语义是 lineage/初始快照提交记录：它绑定静态 identity marker、初始
`fs.tar.gz` 及其 `SHA256SUMS`，而不是给可变的当前 `fs` 做逐字节封存。测试或 provision
产生合法 meta/data 写入后，`--verify-bundle` 一方面复核当前 `fs` 的结构、数据库/存储
一致性与关键身份，另一方面复核初始 tar/sums 未被篡改；它不证明当前树字节等于 tar。
初始 tar 既不是当前树备份，也不属于 package、Corresponding Source 或 Release 资产。

## Stage2 边界

以下内容明确不属于 Stage1：

- Swift 全面使用 native session retain/release 的 borrow/cancel/typed lifecycle；
- `IshError` 类型化 status 与新错误映射；
- Terminal callback 有界队列、drop 事件与 handler generation 语义；
- VT 增量 UTF-8、CSI/OSC parser 上限与相关 API 改造。

这些变更必须在过渡 binary 已公开、manifest 已固定且真链接测试通过后单独 CR、测试和发布。
