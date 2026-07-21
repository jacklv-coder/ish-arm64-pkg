# IshEmbed 测试与验收

简体中文｜[English](testing.en.md)

Stage1 验收的核心不是“某个测试跑过”，而是同时证明 native ABI 过渡、旧 Swift 兼容、
iOS 18 二进制、内部 wire v4 和发布供应链处于同一可解释状态。

## 测试矩阵

| 层 | 主要门禁 | 证明内容 | 不证明什么 |
| --- | --- | --- | --- |
| 协议单元 | `proto_test`、`supervisor_stdin_test` | v4 帧解析、边界、stdin partial write/反压 | 真正 iSH boot |
| 生命周期 | `lifecycle_test` | retain/release、close/read/write/signal 交错、shutdown/busy、PID identity | Stage2 Swift borrow 已完成 |
| native 集成 | `procfs_test`、`ishembed_smoke`、`codex_test` | fakefs、spawn、procfs、命令链路 | RootFS 来源/许可可信 |
| sanitizer | ASan/UBSan，必要时 TSan | 已覆盖路径上的越界、UAF、未定义行为和数据竞争 | 所有调度组合都无缺陷 |
| Swift RootFS-free | instance/session gate、shutdown retry、公开 API smoke | oneshot/session lease 阻止旧 ABI UAF、失败保留 handle、旧公开签名可编译 | 任意 C 调用都可取消或 close 始终有界 |
| Swift manifest 真链接 | `test-swift-ios.sh --manifest-binary` | Stage1 Swift 仍与发布前 v0.3.3 binary 链接 | 新 native 符号已公开 |
| Swift local 真链接 | `test-swift-ios.sh --local-binary` | 同一 Swift 与待发布过渡 XCFramework 链接 | GitHub 资产已发布 |
| XCFramework | `build-ios.sh` + symbol/final-link 检查 | device/simulator arm64、最低 iOS 18、必需符号 | App 产品逻辑 |
| 文档/脚本 | docs 正负门禁、shell syntax、策略测试 | 双语链接、失败诊断、release notes/version/tag/source policy | 文档本身等于实现 |

## 先确认 Stage1 状态

发布前应同时满足：

- `ISH_EMBED_ABI_VERSION` 为 1；
- `ISH_PROTO_VERSION` 为 4；
- Swift 源不调用 `ish_embed_session_retain/release`；
- `Package.swift` 仍固定 v0.3.3；
- 本地构建的新 XCFramework 导出 retain/release、join/soft-halt 等必需符号；
- RootFS 没有出现在 Git diff、XCFramework、source archive 或 Release 清单中。

发布后才把“manifest 固定到 `v0.4.0-abi.1`”加入预期。不能用发布后的预期否定
发布前的正确状态。

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
meson setup build-test \
  -Dish_src="$PWD/third_party/ish" \
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
- shutdown 在 live session/active call 时返回 busy，正常路径 soft-halt 并 join；
- control queue 的普通/关键 frame/byte 上限、饱和时 close/有限 oneshot 的有界 EOF fallback、
  stop/finish 精确释放，以及阻塞 reader 下 spawn staging gate；测试使用较小预算让溢出与
  复用路径可确定复现；
- stdin queue partial write、`EAGAIN`、上限和错误传播；
- chroot Codex 配置保留自定义内容并修正为 `0600`、删除后重建默认值，以及不覆盖
  symlink、目录和 FIFO 等恶意类型。

## Sanitizer

```sh
meson setup build-asan \
  -Dish_src="$PWD/third_party/ish" \
  -Dguest_arch=arm64 \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Dwerror=true
meson compile -C build-asan
meson test -C build-asan --print-errorlogs --repeat 5
```

CI 还会在独立 `build-ci-tsan` 目录用 `-Db_sanitize=thread` 执行同一套测试。不要在同一
build 目录切换 sanitizer；Meson cache 和不同 runtime 混用会使结果不可解释。失败时保存
完整 test log 和第一个 sanitizer stack。

## Host/RootFS 集成

```sh
# 只查看版本、官方下载 URL、固定 SHA-256 与预期 recipe 身份；不下载、不构建：
scripts/build-rootfs.sh --print-inputs
scripts/build-rootfs.sh --print-identity
scripts/build-rootfs.sh --verify-rootfs build/fs
scripts/build-rootfs.sh --verify-bundle build
scripts/run-host-tests.sh --no-codex --smoke
# 完整 Codex guest 流程（需要联网准备独立测试 RootFS）：
scripts/run-host-tests.sh
```

runner 会明确把 `scripts/alpine-rootfs-pin.sh` 传给 builder，并要求 build-check、build-host
与 RootFS recipe 的 guest 架构都是 `arm64`。该清单固定经审阅的 Alpine 版本、架构和
SHA-256，因此干净检出可直接运行，同时仍在解包前强制校验下载内容。

builder 在稳定普通文件的内核 `flock` 内，以唯一同卷 staging 生成 schema v2 marker；
recipe 覆盖列出的、经过审阅的
源码与 pin 输入，但不保证 toolchain/libarchive 版本，递归子模块记录也只绑定
gitlink/status，不声称哈希未提交的嵌套工作树内容。artifact 字段另外绑定实际
fakefsify、supervisor、BusyBox 与初始 meta/data seal。发布前执行完整 seal 验证；复用时执行
四件套 receipt、SQLite quick-check、正整数 inode、16-byte stat BLOB、唯一空 root、meta/data
全路径一致性及 AArch64 supervisor/BusyBox 摘要验证。`fs`、tar、checksums 先发布，receipt
最后提交；替换使用 exchange 保持 fs 可见，失败逆序回滚，成功不保留旧代际。runner 从
验证到最后一次消费都持有 RootFS/Codex 锁，因此并发 builder 不能在 use 阶段换树。合法
运行态写入仍可复用，复制/篡改 marker 不足以通过。`fs-codex` 身份绑定 clean receipt/内容、
包、exact/tag 请求分类、VM/bin、provision 输入与实际安装版本；exact 错装不同版本会在发布
前失败，tag 会绑定解析后的版本。复用还验证 package bin target、guest executable mode 与
全局入口映射；失配时强制 provision，成功返回却没有匹配布局/身份会使 runner 失败。

receipt 只表示 lineage/初始快照：它绑定静态 marker 与初始 `fs.tar.gz`/`SHA256SUMS`。
运行态 mutation 后，`--verify-bundle` 的通过条件是“当前 `fs` 结构、数据库/关键身份有效，
且初始归档完整”，不是“当前 `fs` 与 tar 逐字节相同”。测试和排查都不得把初始 tar 当作
当前树备份；它也不进入提交、package、source archive 或 Release。

测试其他版本时，必须一起设置 `ALPINE_VERSION` 与经过独立复核的 `ALPINE_SHA256`；摘要
缺失、格式错误或不匹配都会失败。`scripts/test-run-host-tests.sh` 用隔离 fixture 覆盖首次
构建、四件套复用、旧架构/pin/缺标记、实际 supervisor 篡改、畸形 SQLite、空/损坏/PID
复用锁、两个并发 builder、运行态 mutation、每个发布步骤故障与 TERM journal gap、use 期
锁竞争、consumer 后台进程的锁 FD 隔离，以及 `fs-codex` base/请求/实际版本漂移、exact
错装、tag 解析、bin target/global entry 缺失和状态聚合。
纯 C `provision_codex_format` 回归另外覆盖输入策略允许的 194 字节 scoped package 与
128 字节版本/tag 组合（323 字节 npm target），并证明少一个字节的缓冲区会失败而不是
静默截断。fixture 还会断开全局入口的真实 host hardlink、同时保留 fakefs inode，验证这种
host backing 与数据库不一致也不能通过复用。

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
   `otool` 解析，其 `LC_BUILD_VERSION` 分别是 iOS/iOS Simulator 且 minos 不高于 18；
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
- shutdown 失败（包括 busy 对应状态）保留同一 handle，允许清理后重试；成功 shutdown
  消耗进程唯一 lifecycle，后续 boot 在进入 native 前即被拒绝；
- oneshot/read 的 NaN/无穷 timeout 在进入 native 前返回 invalid-argument，正的亚毫秒
  oneshot timeout 至少转换为 1 ms；
- session gate 先 detach、拒绝新调用，并等待已进入调用结束后才执行 native close；
- 现有公开 API 的源码兼容 smoke，包括可写 `keyEncoder`；严格并发编译同时验证
  `setEventHandler` 保持原非 `@Sendable` 公开形态。

这些测试不写死总数，且不声称 close 总能在固定时间完成。nil-timeout read 通常最多等待
当前 100 ms poll 后重过 gate；其他已进入 C 调用的返回时间决定 close 上界，旧/自定义
supervisor 下永久阻塞的非 read 调用会让 close 继续等待。

发布前先验证旧 binary：

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
过渡 Release 发布后，再运行 `--manifest-binary` 证明更新后的 manifest 能从公开 URL
真链接。

## CR 与提交门禁

重要 push 前、PR 合并前使用 GPT-5.6 Luna 检查：

- diff 是否超出 Stage1 native ABI 范围；
- session/instance 并发、引用、shutdown 与错误路径；
- wire header/payload、长度运算和有界队列；
- iSH gitlink 与许可证/对应源码；
- 双语文档是否把 Stage2 写成已交付；
- manifest 的发布前/发布后状态是否描述一致。

P1/P2 清零后才能 push 或合并。测试通过不能替代 CR，CR 也不能替代真构建和 sanitizer。
