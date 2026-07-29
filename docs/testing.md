# IshEmbed 测试与验收

简体中文｜[English](testing.en.md)

Stage1 验收的核心不是“某个测试跑过”，而是同时证明 native ABI 过渡、旧 Swift 兼容、
iOS 18 二进制、内部 wire v4 和发布供应链处于同一可解释状态。

## 测试矩阵

| 层 | 主要门禁 | 证明内容 | 不证明什么 |
| --- | --- | --- | --- |
| 协议/摘要单元 | `proto_test`、`supervisor_stdin_test`、`sha256_test` | v4 帧解析、边界、stdin partial write/反压、SHA-256 标准向量与畸形元数据拒绝 | 真正 iSH boot |
| 生命周期 | `lifecycle_test` | retain/release、close/read/write/signal 交错、shutdown/busy、PID identity、错误内嵌摘要/路径在安装前失败 | Stage2 Swift borrow 已完成 |
| JIT 脏页 | `dirty_page_test`、`dirty-page-trace` | READ 不置位、fast/miss/cross-page 写合并、单页碰撞精确过滤、显式跨页 `end_addr` 失效、多页保守桶、host/kernel 后置失效、精确 tracer、跨 TLB `IC IVAU` 与 ARM64/x86 直链边界 | 非规范的无 cache-maintenance 自修改代码立即可见；x86 跨线程全局发布；原写路径吞吐 |
| native 集成 | `internal-signal-mask`、`procfs_test`、`ishembed_smoke` | 嵌入/guest task 内部 SIGUSR1 mask、fakefs、spawn、procfs、真实 guest `uname -a`、通用命令链路 | RootFS 来源/许可可信；特定用户工具兼容性 |
| sanitizer | ASan/UBSan，必要时 TSan | 已覆盖路径上的越界、UAF、未定义行为和数据竞争 | 所有调度组合都无缺陷 |
| Swift RootFS-free | instance/session gate、shutdown retry、公开 API smoke | oneshot/session lease 阻止旧 ABI UAF、失败保留 handle、旧公开签名可编译 | 任意 C 调用都可取消或 close 始终有界 |
| Swift manifest 真链接 | `test-swift-ios.sh --manifest-binary` | Stage1 Swift 与当前 `v0.4.0-abi.6` binary 链接 | `v0.4.0-abi.7` 修复已公开 |
| Swift local 真链接 | `test-swift-ios.sh --local-binary` | 同一 Swift 与待发布维护 XCFramework 链接 | GitHub 资产已发布 |
| XCFramework | `build-ios.sh` + symbol/final-link 检查 | device/simulator arm64、最低 iOS 18、必需符号 | App 产品逻辑 |
| 文档/脚本 | docs 正负门禁、shell syntax、策略测试 | 双语链接、失败诊断、release notes/version/tag/source policy | 文档本身等于实现 |

## 先确认 Stage1 状态

`v0.4.0-abi.7` 发布前应同时满足：

- `ISH_EMBED_ABI_VERSION` 为 1；
- `ISH_PROTO_VERSION` 为 4；
- Swift 源不调用 `ish_embed_session_retain/release`；
- `Package.swift` 仍固定已公开的 `v0.4.0-abi.6`；
- 本地构建的新 XCFramework 导出 retain/release、join/soft-halt 等必需符号；
- RootFS 没有出现在 Git diff、XCFramework、source archive 或 Release 清单中。

发布后才把“manifest 固定到 `v0.4.0-abi.7`”加入预期。不能用发布后的预期否定
发布前仍引用已验证 `v0.4.0-abi.6` 的正确状态。

## 快速元数据与脚本门禁

```sh
git diff --check
bash -n scripts/*.sh
scripts/check-docs.sh
scripts/test-check-docs.sh
scripts/test-build-ios-path-safety.sh
scripts/test-run-host-tests.sh
scripts/test-release-version-policy.sh
scripts/test-release-tag-cas.sh
scripts/test-source-policy.sh
swift package dump-package >/dev/null
```

- `check-docs.sh` 检查每个中文文档的英文镜像、语言切换和本地链接/anchor；
  `test-check-docs.sh` 用故意损坏的 fixture 证明门禁会失败。
- release version policy 测试验证 prerelease 判定及 `v*-abi.*` 专用双语 release notes；
  不访问网络。
- tag CAS 测试验证只有持有精确 raw object OID/commit 的事务才能删除或发布 tag。
- source policy 的正负 fixture 拒绝 RootFS、嵌套归档、逃逸路径与不合规资产。
- CI 的 PR diff 从 base SHA 覆盖到当前 HEAD；push diff 从事件 `before` SHA 覆盖到 HEAD，
  全零或本地不可解析的 `before` 则从空树检查当前完整受跟踪树，避免多 commit push 只检查
  最后一个 commit。

## Native 单元与生命周期

标准构建：

```sh
meson setup build-test-ish "$PWD/third_party/ish" \
  -Dguest_arch=arm64 \
  -Dc_args=-DISH_DISABLE_SKIP_BRK=1 \
  -Db_lundef=false
ninja -C build-test-ish libish.a libish_emu.a libfakefs.a
meson setup build-test \
  -Dish_src="$PWD/third_party/ish" \
  -Dish_build="$PWD/build-test-ish" \
  -Dguest_arch=arm64 \
  -Db_lundef=false \
  -Dwerror=true
meson compile -C build-test
meson test -C build-test --print-errorlogs
```

重点覆盖：

- v4 header/payload 上限、未知类型、截断和 exact-version 拒绝；
- owner reference、retain/release 配对、close 后新调用失败；
- 已借用 read/write/signal 与 close 的确定性交错；
- spawn readiness pipe 确认不可变 leader/PGID、`waitid(..., WNOWAIT)` 下 reap 前
  清理同组后台子进程，并在
  `waitpid` 后同时清零 PID/PGID，避免后台进程逃逸与复用 identity 误杀；
- Linux 实进程 double-fork + `setsid` 回归覆盖：未跟踪且由 subreaper 收养的后代在普通
  leader 退出和显式 close 两条路径都被精确清理；另一个 tracked session 不会被误认；
  `/proc` 扫描/清理失败会 fail-close，且不得发送成功 `EXITED`/shutdown ACK；
- TTY foreground group 的 close 清理与 slot 释放；
- `ish_embed_boot(..., NULL)` 无副作用拒绝；
- 默认 supervisor 会在安装前对实际内嵌 bytes 计算 SHA-256，并同时绑定 64 位小写摘要和
  `/sbin/.ishsv-ishembed-sha256-<digest>` 路径；错误摘要/路径均在调用安装 FFI 前以
  `ISH_ERR_SUPERVISOR_INSTALL` 失败。显式自定义路径不走默认 blob 安装/摘要校验；这些
  测试证明构建内一致性，不证明数字签名或来源认证；
- shutdown 在 live session/active call 时返回 busy，正常路径 soft-halt 并 join；
- control queue 的普通/关键 frame/byte 上限、饱和时 close/有限 oneshot 的有界 EOF
  fallback、有限 streaming 的 instance/staging/queue gate deadline、writer 停滞下
  stdin-close 复用原始 SPAWN deadline 的回归、有界 stdin-close/terminate、active write
  后返回 busy 的 stdin-close、stop/finish 精确释放，以及阻塞 reader 下 spawn staging
  gate；测试使用较小预算让溢出与复用路径可确定复现；
- stdin queue partial write、`EAGAIN`、上限和错误传播；
- TLB READ 不污染脏集合，C fast/write-miss/cross-page 写保留所有页，并验证当前末页延迟
  到 drain、切页时保留前页；单页同桶碰撞回归证明写入无代码的碰撞页不会删除远端 block
  或推进 generation，而写入真实代码页只失效一次；显式 `asbestos_invalidate_page` 回归
  证明跨页 block 的第二页通过 `end_addr` 命中，同时同桶远端 block 保留。发生切页后的
  多页路径使用哈希位图，两个不同桶及同桶碰撞可以保守批量失效，消费后不会重复推进
  generation；确定性线程回归证明 drain 会等待持有
  同一锁的 compile/insert，并在其发布后失效 stale block；`mem_did_write` 回归覆盖 pre/write
  窗口后发布、跨页、地址回绕全量兜底和 data-only 空 occupancy；x86 精确 tracer 回归还证明
  运行时集合 drain 后诊断页仍完整保留、相差 1024 页的同桶地址可分别枚举、重复遍历不消费
  且只有显式成功路径才清理；模拟 `CTR_EL0` 的 DIC/IDC 均为 0，ARM64 真实 gadget 回归使用
  独立 writer/executor TLB，证明 `IC IVAU, Xt` 先发布
  Xt 页、再结束当前翻译块，使 dispatcher 在直链目标运行前排空脏集合；x86 真实 gadget
  回归建立 `writer -> source -> target` 双直链并证明写后不会执行旧 target，另有 16-prefix
  页尾回归证明 decoder 在第 16 byte 的 unmapped-page 读取前生成 `#UD`；ARM64 高地址相邻页
  TLB 别名回归证明页尾最后一条对齐 A64 指令正常运行，而未对齐 PC 直接报错且不会预取、
  驱逐起始页；
- chroot 准备只修复通用 `/dev`、devpts、procfs 与 `/root`，不会创建 Codex CLI
  配置或其他工具专用状态。

## JIT 性能基线

在 Apple M3 Pro、release/O3/NDEBUG、Alpine 3.19.1 ARM64 seed 上，以 `d9629015` 为基线，
对每个 oneshot 执行 20,000 次 shell 算术/变量写循环，并按 baseline/current 反向交错采样：

- 单任务中位数 `2.6798s -> 2.8044s`，增加 `4.65%`；
- 4 个同时放行的并发任务中位数 `2.9490s -> 3.0594s`，增加 `3.74%`；
- `ishembed_smoke` 两侧共 20 次全部通过，固定等待造成的波动较大，不用于声称吞吐提升。

后续目标页 guard 样本在同一机器上测得 shell 循环约 `2.77s -> 3.44s`（约 `24%`），
legacy SSE2 e2e 约 `5.67s -> 7.08s`（约 `25%`）。这些是本地短样本，不替代稳定基准、
PocketRoot 交互负载评估或发布 SLA。最终实现不会让所有普通写一律回到 dispatcher：只有
下一直链/RET 目标与待处理代码脏页相交时才退出并排空，纯数据写可继续直链；但每次链跳
仍有一致性检查成本。后续修改应复用相同 seed、release 配置和交错顺序，并增加多轮统计，
避免把系统负载误判为代码变化。

## Sanitizer

```sh
meson setup build-asan-ish "$PWD/third_party/ish" \
  -Dguest_arch=arm64 \
  -Dc_args=-DISH_DISABLE_SKIP_BRK=1 \
  -Db_sanitize=address,undefined \
  -Db_lundef=false
ninja -C build-asan-ish libish.a libish_emu.a libfakefs.a
meson setup build-asan \
  -Dish_src="$PWD/third_party/ish" \
  -Dish_build="$PWD/build-asan-ish" \
  -Dguest_arch=arm64 \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Dwerror=true
meson compile -C build-asan
meson test -C build-asan --print-errorlogs --repeat 5
```

outer build 传入可用的 `-Dish_build` 后会生成 `dirty_page_test`；Meson 本身不证明该目录
与 outer build 使用相同 sanitizer。上面的标准命令和 CI 会为 iSH/outer 显式使用相同配置，
并另用 `build-ci-ish-tsan`、`build-ci-tsan` 目录以 `-Db_sanitize=thread` 执行同一套测试。
不要在同一 build 目录切换 sanitizer，也不要把不同 sanitizer 的 iSH 与 outer build 混用；
Meson cache 和不同 runtime 混用会使结果不可解释。失败时保存完整 test log 和第一个
sanitizer stack。

## Host/RootFS 集成

```sh
# 只查看版本、官方下载 URL、固定 SHA-256 与预期 recipe 身份；不下载、不构建：
scripts/build-rootfs.sh --print-inputs
scripts/build-rootfs.sh --print-identity
scripts/build-rootfs.sh --verify-rootfs build/fs
scripts/build-rootfs.sh --verify-bundle build
scripts/test-deterministic-rootfs-tar.sh
scripts/prepare-rootfs-candidate.sh --verify-only
scripts/run-host-tests.sh --smoke
```

runner 会明确把 `scripts/alpine-rootfs-pin.sh` 传给 builder，并要求 build-check、build-host
与 RootFS recipe 的 guest 架构都是 `arm64`。该清单固定经审阅的 Alpine 版本、架构和
SHA-256，因此干净检出可直接运行，同时仍在解包前强制校验下载内容。

builder 在稳定普通文件的内核 `flock` 内，以唯一同卷 staging 生成 schema v4 marker；
确定性归档器将两层 tar/gzip 的 owner、顺序和时间归一到固定
`SOURCE_DATE_EPOCH`。recipe 覆盖列出的、经过审阅的
源码与 pin 输入；递归子模块摘要只保留规范化 status/object/path，不包含会随本地
fetch 改变的 ref 描述，也不声称哈希未提交的嵌套工作树内容。artifact 字段另外绑定
supervisor、BusyBox 与初始 meta/data seal；实际 fakefsify 二进制、host/tool 证据，
以及链接 library 的加载路径、版本和可获取文件摘要写入候选外部环境 receipt。
发布前执行完整 seal 验证；复用时执行
四件套 receipt、SQLite quick-check、正整数 inode、16-byte stat BLOB、唯一空 root、meta/data
全路径一致性及 AArch64 supervisor/BusyBox 摘要验证。`fs`、tar、checksums 先发布，receipt
最后提交；替换使用 exchange 保持 fs 可见，失败逆序回滚，成功不保留旧代际。runner 从
验证到最后一次消费都持有 RootFS 锁，因此并发 builder 不能在 use 阶段换树。合法运行态
写入仍可复用，复制/篡改 marker 不足以通过。

receipt 只表示 lineage/初始快照：它绑定静态 marker 与初始 `fs.tar.gz`/`SHA256SUMS`。
运行态 mutation 后，`--verify-bundle` 的通过条件是“当前 `fs` 结构、数据库/关键身份有效，
且初始归档完整”，不是“当前 `fs` 与 tar 逐字节相同”。测试和排查都不得把初始 tar 当作
当前树备份；它也不进入提交、package、source archive 或 Release。

测试其他版本时，必须一起设置 `ALPINE_VERSION` 与经过独立复核的 `ALPINE_SHA256`；摘要
缺失、格式错误或不匹配都会失败。`scripts/test-run-host-tests.sh` 用隔离 fixture 覆盖首次
构建、四件套复用、旧架构/pin/缺标记、实际 supervisor 篡改、畸形 SQLite、空/损坏/PID
复用锁、两个并发 builder、运行态 mutation、每个发布步骤故障与 TERM journal gap、use 期
锁竞争、consumer 后台进程的锁 FD 隔离和状态聚合。
`scripts/test-deterministic-rootfs-tar.sh` 用不同宿主 mtime 的目录、可执行文件、symlink
和 hardlink 验证归档字节一致与 no-replace。独立 CI job 再调用
`scripts/prepare-rootfs-candidate.sh --verify-only`，以一个已记录摘要的 `fakefsify`
在按 host 架构固定的可信工具目录下完成两次真实 RootFS 构建，并比较完整 tar、receipt、
identity、SQLite 和 data；另生成
`ROOTFS_BUILD_ENVIRONMENT.json` 记录 host/tool/链接 library 证据。临时 RootFS 和环境 receipt
在 job 结束前删除且不上传；两个显式 `--output` 调用可再比较相同 `fs.tar.gz`。

host-test 与 production iOS 构建都必须在 iSH `c_args` 中包含
`-DISH_DISABLE_SKIP_BRK=1`。runner 对旧 `build-check` 做 Meson introspection；缺少该宏会以
65 失败并要求安全 reconfigure，不能静默复用会跳过 fatal guest SIGTRAP 的缓存；真实
`procfs_test` 还执行 `kill -TRAP $$`，要求 shell 以 SIGTRAP 结束且不能继续输出。

这些测试使用本地开发 RootFS 证明 boot/spawn/procfs/命令路径。固定清单只解决测试输入
可复现性，不授权将 RootFS 提交或发布，也不代替资产来源、签名、最终内容摘要和许可证
复核。

## iOS 18 XCFramework

```sh
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/verify-ios-artifact.sh
```

验收项：

1. `Info.plist` 只有 device/simulator 两条记录，二者严格绑定
   `LibraryPath`/`BinaryPath=libIshKernel.a`、`HeadersPath=Headers`；slice 内路径经过规范化，
   且 library、headers 及父路径都不是 symlink；
2. 两个 library 都只有 arm64；除 archive 符号索引外，每个 Mach-O member 都必须被
   `otool` 解析，其 `LC_BUILD_VERSION` 分别是 iOS/iOS Simulator 且 `minos` 必须严格为 18.0；
   实际 C final link 分别使用
   `arm64-apple-ios18.0` 与 `arm64-apple-ios18.0-simulator`，linker warning 视为失败，
   `vtool` 精确报告对应平台与 iOS 18；
3. 导出 session retain/release、shutdown、bundled supervisor、kernel join/soft-halt
   所需符号，不导出已废弃 FFI；
4. 内嵌 supervisor 是 little-endian ELF64 AArch64 静态 ET_EXEC，无 `PT_INTERP`/
   `DT_NEEDED`；生成 C 数组、两个 slice 真链接后的 Mach-O blob 均与该 ELF 逐字节一致，
   length/hash/path 也绑定同一内容；
5. XCFramework `Licenses/` 与仓库 LICENSE/NOTICE/release docs/iSH/musl notices 一致。

CI 与隔离 Release 构建都调用同一个 `verify-ios-artifact.sh`，因此 Release 不会只做比 PR
更弱的 `Info.plist`/Swift 验证。脚本验证的是本次构建目录里的实际 supervisor 与
XCFramework 字节。

## Swift 两个真链接边界

`Tests/IshEmbedTests` 中不需要 RootFS 的测试覆盖：

- instance gate 串行化 boot/shutdown，oneshot lease 或存活 session 使 shutdown 在进入旧
  native 前立即 busy；spawn lease 直到 native session close 完成才释放；
- shutdown 的 busy 状态恢复同一 running handle；timeout 等终态失败隔离普通调用但允许
  shutdown 清理重试；成功 shutdown 消耗进程唯一 lifecycle，后续 boot 在进入 native 前
  即被拒绝；
- oneshot/read 的 NaN/无穷 timeout 在进入 native 前返回 invalid-argument；spawn
  budget 会扣除 Swift 参数封送时间，剩余不足 1 ms 时在 native entry 前返回 timeout，
  不会退化成无超时；
- session gate 先 detach、拒绝新调用，并等待已进入调用结束后才执行 native close；
- 现有公开 API 的源码兼容 smoke，包括可写 `keyEncoder`；严格并发编译同时验证
  `setEventHandler` 保持原非 `@Sendable` 公开形态。

这些测试不写死总数，且不声称 close 总能在固定时间完成。nil-timeout read 通常最多等待
当前 100 ms poll 后重过 gate；其他已进入 C 调用的返回时间决定 close 上界，旧/自定义
supervisor 下永久阻塞的非 read 调用会让 close 继续等待。

发布前先验证当前 manifest binary：

```sh
scripts/test-swift-ios.sh --manifest-binary
```

再验证本地 Stage1 binary：

```sh
scripts/test-swift-ios.sh \
  build/xcframework/libIshKernel.xcframework
```

脚本在隔离 package 中选择 iOS 18+ arm64 simulator，启用 complete strict concurrency
和 warnings-as-errors，运行 XCTest，并拒绝“全部测试 skipped”的假成功。

Stage1 的 Swift 目标是旧 ABI 兼容，不应在这里加入 native retain/release、typed status
或新 Terminal/VT 行为的断言。完整 borrow/cancel/typed lifecycle 测试随 Stage2 引入。
维护 Release 发布后，再运行 `--manifest-binary` 证明更新后的 manifest 能从公开 URL
真链接。

## CR 与提交门禁

重要 push 前、PR 合并前使用 Codex CR 检查：

- diff 是否超出 Stage1 native ABI 范围；
- session/instance 并发、引用、shutdown 与错误路径；
- wire header/payload、长度运算和有界队列；
- iSH gitlink 与许可证/对应源码；
- 双语文档是否把 Stage2 写成已交付；
- manifest 的发布前/发布后状态是否描述一致。

P1/P2 清零后才能 push 或合并。测试通过不能替代 CR，CR 也不能替代真构建和 sanitizer。
