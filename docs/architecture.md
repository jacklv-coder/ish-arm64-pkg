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

`third_party/ish` 指向本项目维护的 `ish-arm64` fork，而不是在本地直接改写别人维护的
上游检出。原因是 join/soft-halt、嵌入式生命周期和 JIT 脏页一致性都必须修改 iSH 核心，
outer package 无法只靠 FFI 补齐；同时 PocketRoot 的构建、回归和发布必须固定到一个经过
审阅的精确 revision。通用修复仍可整理后回馈上游，但在上游接受并发布前，fork 让项目能
通过独立 PR、CI 和 gitlink 审计这些窄差异，不代表接管或重写上游项目。

## JIT 自修改代码一致性

iSH 的 ARM64 JIT 允许翻译块直链，因此普通 guest 写入不能只记录“最后一页”：同一块内的
跨页写或多次写会覆盖单槽，随后可能继续执行旧翻译。每个 TLB 现在维护与
`FIBER_PAGE_HASH_SIZE` 一致的 1024 位脏桶集合。C fast path、write miss、cross-page write
和 ARM64/x86 gadget 写路径用 `dirty_page` 保留当前精确页；切换页面时才把前一页 OR 进
位图。dispatcher drain 时，若此前没有桶位，最终页保持精确身份直接失效；若已经发生
切页，才把最终页补入位图并走保守的多页批处理。这样同页连续写不重复计算位图且不丢失
精确身份，同时任意多页写仍不会漏记；READ 不置位。这组字段只负责运行时失效，
dispatcher 排空后立即清理。

x86 的 `ptraceomatic`/`unicornomatic` 不能复用上述哈希桶做内存比对：相差 1024 页的地址
会落入同桶，而且运行时排空发生在工具比较之前。工具因此显式启用独立的 20-bit 精确页
位图（128 KiB，并带 2 KiB 二级摘要）；写路径只在页面切换时记录完整页号，最终页在 drain
前补入。运行时集合清空后，诊断集合仍保留到所有页比较成功才显式清理；比较失败不消费，
可以原样重试。普通执行的指针保持 `NULL`，不分配这块诊断内存。

kernel/host 通过 `mem_ptr(..., MEM_WRITE*)` 直接修改 guest memory 时采用前后两道失效：
取得可写指针前先失效，实际 `memcpy`、atomic RMW 或 `memset` 完成后、仍持有同一
`mem->lock` read 生命周期时再调用 `mem_did_write`。这样即使 compiler 恰好在两者之间
发布旧 bytes，后置失效仍会删除该 block。正常页使用 atomic occupancy 快速判断；无翻译
代码的数据页不争用全局 JIT mutex。越界、超长或地址回绕的内部报告保守地全量失效。

回到 dispatcher 后，runtime 在与翻译块 compile/insert 相同的锁内检查脏页并移入
jetsam，再以 `invalidate_gen` 清除 block/return cache。尚未发生页面切换的单页写序列仍
保留最终页的精确身份，因此按精确页过滤候选；显式 `asbestos_invalidate_page` 也使用同一
过滤逻辑。翻译块跨页时，`page[0]` 比较 `block->addr`，`page[1]` 必须比较
`block->end_addr`，所以失效第二页既能删除真实跨页 block，也不会误删同一哈希桶的远端
block。只有已经切换过页面的**多页**脏集合会把较早页折叠为哈希位图，并可能因桶碰撞
保守地多失效；这条路径不会漏掉必须失效的翻译。

检查、失效和脏集合清理不能在 coherence 锁外仅凭空桶 fast path，否则另一线程可能先
编译旧 bytes、等待插入，然后在脏集合清空后发布 stale block。实现只在 coherence read
side 内以 acquire 读取 atomic occupancy；确认无候选代码时可跳过全局 mutation mutex。
进入下一直链或 RET cache 目标前，汇编 guard 会把目标 block 的 `addr`/`end_addr` 与当前
精确脏页及较早页的保守桶集合比较。相交时才回到 dispatcher 排空脏状态并断开受影响的
chaining/cache；不相交的纯数据写可以继续直链，待处理脏状态不会丢失。该优化避免所有写
一律退出 dispatcher，但每次链跳仍有检查成本，写密集的旧 e2e workload 仍可能明显变慢；
量化基线见
[测试文档](testing.md#jit-性能基线)。

正常 task 在执行 JIT 时持有 `mem->lock` 的 read side，而 lazy mapping 可能临时释放 read
并申请 write。为避免它与脏页 drain/compile 锁形成反向等待，compiler 会在取得
`dirty_coherence_lock` 的 write side 前预解析当前指令允许访问的页；进入该锁后，decoder
切换为 no-fault TLB 读取，miss 只生成 guest fault，不再进入可能升级 `mem->lock` 的 MMU
slow path。x86 decoder 同时在任何第 16 个指令字节的内存读取前生成 `#UD`，A64 则在读取
前拒绝非 4-byte 对齐 PC；合法 A64 指令不会跨 4 KiB 页。这样 compile 从读取 guest bytes
到插入翻译块都与 drain 串行，又不会在内层锁中请求外层内存写锁。

符合 ARM64 规范的自修改代码在写入后执行 `DC ...`、`IC IVAU, Xt`、barrier 序列。数据
cache maintenance 在共享 host bytes 上仍是 NOP；`IC IVAU` 会把 Xt 指向的页加入**执行
该 IC 的线程**的 TLB 脏集合，然后结束当前翻译块并返回 dispatcher。这覆盖 writer 与
cache-maintenance executor 使用不同 TLB 的合法跨线程序列，并保证在跳到已修改目标前
消费脏集合；已直链 target 也不能先运行旧代码。没有执行规定 instruction-cache
maintenance 的 guest 自修改序列不承诺立即可见。模拟的
`CTR_EL0 == 0x84448004` 明确保持 DIC/IDC（bit 29/28）为 0，因此 guest 不能合法省略
DC/IC 步骤；该常量与两位的回归和 `IC IVAU` decoder boundary 一起测试。

PocketRoot Stage1 的 production artifact 只包含 **ARM64 guest**。x86 guest 代码仅作为
fork 的兼容与回归构建：同线程写入会在中央 block-chain 与 return-cache chain 边界检查
下一目标，仅在目标命中待处理代码脏页时退回 dispatcher；本阶段不承诺 x86 跨线程自修改
代码的全局发布语义，也不会把 x86 guest 切片打进 PocketRoot XCFramework。

## Boot 流程

1. 宿主验证 RootFS 路径、布局、来源与哈希，将可写的 `data/` + `meta.db` 目录传给
   `ish_embed_boot`。
2. C host 创建双向协议管道与日志管道，准备 instance 状态；`out_instance == NULL`
   会在任何安装或线程副作用前被拒绝。
3. 未指定 `supervisor_guest_path` 时，release XCFramework 将内嵌静态 AArch64
   supervisor 的实际 bytes 计算 SHA-256 并与构建元数据比对，通过后才原子安装到
   fakefs 的内容寻址私有路径，并复核完整字节和执行模式；
   不替换 RootFS 自带的 `/sbin/ishsv`。显式自定义路径会绕过这条默认 blob 安装与摘要
   校验，完全由调用方负责。SHA-256 这里只证明实际 bytes 与同一构建的固定元数据一致，
   不是数字签名，也不认证下载来源或发布者身份。
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
返回 `ISH_ERR_BUSY` 会恢复同一 handle 和 running 状态，成功才清空 handle；其他
shutdown 失败会隔离 handle，拒绝普通调用和再次 boot，但仍允许重试 shutdown 完成
native 清理。这样 boot/shutdown、两个 shutdown caller 以及 shutdown/call 均不会交错
释放旧 ABI handle。

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
关键字节 reserve 在编译期被要求至少容纳最小 lifecycle frame；有限 oneshot 和
streaming spawn 的 deadline 都从 API 入口开始，同时覆盖 instance gate、spawn gate 和
SPAWN 队列接纳。有限 streaming session 的 SPAWN、stdin close 与 terminate 采用保持
顺序的异步接纳，避免 control writer 停滞吞掉产品期限；session 会保留 SPAWN 的原生
绝对 deadline，stdin close 复用同一期限取得 writer gate，过期时返回
`ISH_ERR_TIMEOUT` 而不会重新开始等待。stdin close 遇到 active stdin write 时返回
`ISH_ERR_BUSY` 而不是等待；终止完成仍以 `EXITED` 为准。

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
- 嵌入式启动路径会在启动 PID 1 前挂载 supervisor 可见的根 procfs；收养 child 的扫描以
  这个根挂载为准，各 VM 的 procfs 仍是独立挂载，只服务对应 chroot 内的进程。
- 扫描、kill、reap 或两秒清理期限任一失败时，supervisor 采用 instance-wide fail-close：
  对该 guest 内进程执行兜底清理，退出且不发送成功 `EXITED`/`SHUTDOWN_ACK`。因此无法证明
  隔离边界已恢复时，协议会失败关闭而不是报告一次看似正常的 session 完成。

## Chroot 准备与隔离边界

绝对 `chroot_path` spawn 前，guest PID 1 会重新验证并按需修复必要的 device 节点、
devpts/procfs 挂载与传统的 `/root` home。它不永久缓存“该路径已准备”，所以同路径
RootFS 被替换或挂载丢失后仍会重新检查。supervisor 不创建任何 Codex CLI 配置或
工具专用目录。

Node.js/npm 是可选的通用 guest 能力：调用方可以在自己的可写 RootFS 中通过 `apk`
安装并运行，但 IshEmbed 不默认预装它们，也不自动安装任何 npm package。

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
返回成功时才清除 Swift handle；busy 恢复原 handle 与 running 状态，timeout 等终态失败
进入 quarantine，拒绝普通调用但允许 shutdown 清理重试。进入成功 shutdown 后，
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

runner 从验证、按需构建一直持锁到最后一次测试使用，关闭 validate→use 替换窗口。
复用允许正常运行造成的 meta/data 变化，但会重验四件套 receipt、recipe、数据库/
存储一致性和关键摘要。

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
