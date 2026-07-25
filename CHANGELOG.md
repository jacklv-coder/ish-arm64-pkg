# 变更日志

[English](CHANGELOG.en.md)

本仓库以中文变更日志为主，并同步维护英文镜像。

## 未发布

- RootFS builder 升级到 schema v3，使用固定 `SOURCE_DATE_EPOCH` 的确定性 tar/gzip
  归档器；新增本地双构建候选生成器和不上传制品的 CI 一致性门禁。候选仍明确标记为
  未获分发批准，不改变 XCFramework Release 的 RootFS 排除策略。

## v0.4.0-abi.6（计划中的 Stage1 维护预发布）

这是 `v0.4.0-abi.5` 之后的兼容性维护版本，仍是 prerelease，**不是稳定
v0.4.0**。

- Swift wrapper 现在从 `runOneshot`/`spawn` API 入口建立绝对 deadline，并在
  argv/env/cwd/chroot 封送完成后重新计算传给 native 的剩余毫秒，避免参数封送重启
  完整 timeout、使 SPAWN 在产品期限后才被接纳。剩余不足 1 ms 时直接返回
  `ISH_ERR_TIMEOUT`，不会把 `0` 误解释为无超时。
- 有限 streaming session 现在保留 SPAWN 的原生绝对 deadline；stdin close 在控制队列
  入队时复用同一期限，无法在期限前取得 writer gate 时返回 `ISH_ERR_TIMEOUT`，不会在
  SPAWN 已成功后重新开始一段 admission 等待。
- 公开 C ABI 仍为 1，wire protocol 仍为 v4，未新增公开符号；RootFS 不进入 Release。
  本版本不实现原生 Agent Loop，也不会在 iOS App 内安装或运行 Codex CLI。Node.js/npm
  仍可作为独立的可选 guest 包使用。

## v0.4.0-abi.5（已发布的 Stage1 维护预发布）

这是 `v0.4.0-abi.4` 之后的兼容性维护版本，仍是 prerelease，**不是稳定
v0.4.0**。

- 有限 `timeout_ms` 的 streaming `ish_embed_spawn` 现在从 API 入口覆盖 instance gate、
  SPAWN staging gate 与控制队列接纳；deadline 前无法取得任一 gate 时返回
  `ISH_ERR_TIMEOUT`，不会创建 session。
- 有限 timeout session 的 SPAWN、stdin close 与 terminate 使用保持顺序的有界异步接纳，
  因此被阻塞的 control writer 不会消耗产品命令期限。stdin close 遇到正在持有顺序锁的
  stdin write 时返回 `ISH_ERR_BUSY`，不会在其后无限等待；调用方仍须读取权威 `EXITED`
  才能确认命令终止。
- `timeout_ms == 0` 的既有 streaming API 继续保持同步写入语义。公开 C ABI 仍为 1，
  wire protocol 仍为 v4，未新增公开符号。
- host lifecycle 测试新增有限 streaming 的 instance/staging/queue gate 超时、writer
  停滞下 `SPAWN → STDIN_CLOSE → TERMINATE → EXITED` 的有界顺序回归，以及 active write
  后 stdin close 保持有界的回归。

## v0.4.0-abi.4（已发布的 Stage1 维护预发布）

这是 `v0.4.0-abi.3` 之后的兼容性维护版本，仍是 prerelease，**不是稳定
v0.4.0**。

- `third_party/ish` 更新到 `c36dfd2`。嵌入启动线程和每个 guest task 线程会明确解除
  iSH 内部 SIGUSR1 屏蔽，避免宿主 App 线程继承的 signal mask 使内部中断永久 pending。
- guest signal 现在可可靠打断 `poll`、`nanosleep` 等阻塞中的宿主 syscall，使命令取消、
  超时终止及后续 runtime 恢复不再依赖 supervisor 轮询或 App 级 workaround。
- Linux x86、macOS arm64、iOS device/simulator 及真实 RootFS 路径覆盖 signal mask、
  取消、取消后恢复和 shutdown 回归。
- 公开 C ABI 仍为 1，wire protocol 仍为 v4，Swift API 不变；RootFS 不进入 Release。
- 本版本不实现原生 Agent Loop，也不会在 iOS App 内安装或运行 Codex CLI。
  Node.js/npm 仍可作为独立的可选 guest 包使用。

## v0.4.0-abi.3（已发布的 Stage1 维护预发布）

这是 `v0.4.0-abi.2` 之后的兼容性维护版本，仍是 prerelease，**不是稳定
v0.4.0**。

- `third_party/ish` 更新到 `5f7535e`，将 guest `uname` 的所有固定宽度字段限制为
  Linux ABI 的 65 字节并保证 NUL 结尾，避免较长的宿主 hostname 在 Xcode 16 /
  iOS 18 加固 libc 下触发 `__strcpy_chk` SIGTRAP。
- Linux、macOS arm64 和 iOS 构建路径新增或复用了有界截断、缓冲区完整性及真实
  guest `uname -a` 回归。
- 公开 C ABI、wire protocol 和 Swift API 均不变；RootFS 不进入 Release。
- 本版本不实现原生 Agent Loop，也不会在 iOS App 内安装或运行 Codex CLI。
  Node.js/npm 仍可作为独立的可选 guest 包使用。

## v0.4.0-abi.2（已发布的 Stage1 维护预发布）

这是 `v0.4.0-abi.1` 之后的兼容性维护版本，仍是 prerelease，**不是稳定
v0.4.0**。

- `third_party/ish` 更新到 `71f940a`，在 task 地址空间退出期间用
  `general_lock` 串行化 `/proc/<pid>/statm`、`maps` 和 `mem` 访问，避免 Darwin
  销毁仍有等待者的 `pthread_rwlock` 时触发 SIGTRAP。
- task 的 `general_lock` 在 PID 表发布 task 指针前初始化，避免 procfs 观察到尚未完成
  锁初始化的 task。
- 公开 C ABI、wire protocol 和 Swift API 均不变；RootFS 不进入 Release。
- 本版本不实现原生 Agent Loop，也不会在 iOS App 内安装或运行 Codex CLI。
  Node.js/npm 仍可作为独立的可选 guest 包使用。

## v0.4.0-abi.1（已发布的 ABI 过渡预发布）

这是 native-first / Swift-second 发布序列的 Stage1，**不是稳定 v0.4.0**。

### 发布事务

- `v0.4.0-abi.1` 已通过发布事务生成 XCFramework、对应源码和只更新 manifest 的
  release commit；`Package.swift` 当前固定该公开资产。
- RootFS 不提交、不打包，也不由 release 脚本上传；预构建 XCFramework 与 guest
  binary 只由发布事务生成，不直接提交到源码分支。

### Stage1 已交付：native runtime

- `third_party/ish` 使用绝对 SSH-over-443 URL，SwiftPM 与干净 checkout 不再把相对子模块地址
  误解析到本地 package repository cache，并保持仓库的 SSH 拉取约定；无 SSH 私钥的
  GitHub hosted CI 只在子模块 checkout 命令内使用公开 HTTPS 只读重写。
- 公开 C ABI 版本仍为 `ISH_EMBED_ABI_VERSION == 1`；新增 session retain/release、
  `ISH_ERR_BUSY`、输出积压保护和 supervisor 安装错误等兼容性符号/状态。
- kernel 线程可等待，shutdown 通过 soft-halt 协作退出并 join；实例生命周期仍限制为
  每个宿主进程一次有效 boot/shutdown。
- host 使用独立 control writer、event reader、guest-log drain 和 log-sink writer，
  对帧、输出积压、stdin 队列和日志队列设置固定上限。host→guest 控制帧从接纳到实际释放
  全程计入 4 MiB/256 帧总预算，其中 4 KiB/16 帧保留给关键生命周期消息；普通调用超限
  返回 `ISH_ERR_CONTROL_LIMIT`。关键 close/shutdown/oneshot 终止链路采用有界异步接纳，
  无法确认清理时转入 control EOF 停机；spawn 的 payload 构建也串行化，避免预算外并发
  暂存大块内存。
- 内部 host ↔ guest wire protocol 升级为精确匹配 v4，并增加 `SESSION_CLOSE`；
  wire v4 与公开 C ABI 1 是两个不同版本面。
- 默认 boot 在安装内嵌 supervisor 前，对实际 blob bytes 计算 SHA-256，并要求它与构建
  摘要及 `/sbin/.ishsv-ishembed-sha256-<digest>` 内容寻址路径一致；不匹配在安装前返回
  `ISH_ERR_SUPERVISOR_INSTALL`。显式 `supervisorGuestPath` 绕过默认 blob 安装与摘要门禁，
  由调用方负责。该校验保证同一构建内的完整性，不是签名或来源/发布者认证。
- iSH JIT 脏页 drain 与 compile/insert 串行，host/kernel 写使用前后失效，ARM64
  `IC IVAU` 会发布目标页并退出直链。单页写与显式 `asbestos_invalidate_page` 现在按精确
  页过滤；跨页 block 的第二页用 `end_addr` 判断，只有多页哈希位图可能因碰撞保守多
  失效。新增单页碰撞和跨页 `end_addr` 回归。直链/RET cache 仅在下一目标 block 命中待处理
  代码脏页时回到 dispatcher；纯数据写可以继续直链，但仍保留待消费脏状态和一致性检查
  成本。
- live TTY close 会清理 tracked shell、前台 job 与 transport；direct child 通过
  readiness pipe 确认不可迁移的 PID/PGID，退出后由 `waitid(..., WNOWAIT)` 保留
  identity、先清理同组后台进程，再由 `waitpid` 回收并清零 PID/PGID。PID 1 还会按精确
  PID 清理 double-fork/`setsid` 后由 subreaper 收养的未跟踪后代，并在连续两次扫描为空后
  才报告成功退出；扫描/kill/reap/期限失败会使 guest instance fail-close，不发送伪成功
  `EXITED`/`SHUTDOWN_ACK`。
- host 集成测试固定 Alpine 开发 RootFS 输入的官方 URL 与 SHA-256；该 pin 只保证测试
  输入可复现。schema v2 recipe 覆盖 builder、supervisor/protocol、iSH 工作树/子模块、
  fakefsify 来源和 Alpine pin；artifact seal 绑定实际关键二进制及初始 meta/data。builder
  在稳定 `flock` 内使用唯一同卷 staging；`fs`、tar、checksums 与 receipt 组成事务，替换
  采用 exchange，receipt 最后提交，失败逆序回滚，成功丢弃旧代际。runner 从验证到使用
  完成持续持锁，复核 SQLite 行类型/16-byte stat/唯一 root、meta/data 全路径与关键摘要；
  派生 Codex 身份绑定 clean receipt/内容、package、exact/tag 请求分类、VM/bin、provision
  输入与实际版本；exact 版本必须逐字匹配，tag 绑定解析结果，package bin/guest executable/
  全局入口映射也属于复用门禁。
  该机制不授权提交或发布 RootFS，也不替代签名、来源、最终内容摘要和许可证审查。
  receipt 是绑定静态 marker 与初始 `fs.tar.gz`/`SHA256SUMS` 的 lineage/初始快照记录；
  运行态 mutation 后的 `--verify-bundle` 会分别验证当前树结构/关键身份和初始归档完整性，
  不证明当前 `fs` 与 tar 字节相同。初始 tar 不是当前树备份或发布资产。
- `ish_embed_boot(..., NULL)` 在产生安装或启动副作用前返回无效参数。
- chrooted spawn 会重新验证必要的 device、devpts/procfs 与运行目录状态。
- production 与 host-test iSH 构建显式定义 `ISH_DISABLE_SKIP_BRK=1`，不会启用 fork 中实验性
  arm64 BRK 恢复而吞掉 fatal guest SIGTRAP；旧 Meson cache 缺宏会被拒绝。
- `third_party/ish` 固定项目维护的 `ish-arm64` fork：join/soft-halt、嵌入生命周期与 JIT
  一致性必须修改 iSH core，无法仅在 outer package 完成。窄差异通过独立 PR/CI 和精确
  gitlink 审计，通用修复仍可回馈上游；不会直接改写别人维护的上游工作树。
- chroot 中缺失的 Codex 配置会原子创建默认值；已有普通文件保留自定义内容并收紧为
  `0600`，符号链接和其他非普通文件会被安全拒绝。

### Stage1 Swift 兼容边界

- Swift 源保持 v0.3.3 ABI 兼容，不调用新增的 C retain/release 符号。
- `IshInstanceCallGate` 串行化 boot/shutdown，并为 oneshot 与存活 session 持有 instance
  lease；只要存在 lease，shutdown 就在进入 v0.3.3 native 前返回 `ISH_ERR_BUSY`。spawn
  将 lease 无缝转交给 session，native close 完成后才释放。`BUSY` 恢复 running handle，
  其他 shutdown 失败进入仅允许 shutdown 清理重试的 quarantine；成功 shutdown 进入
  consumed 终态，二次 boot 不会进入 native。
- Swift oneshot/read 在 native 前拒绝 NaN/无穷 timeout；正的亚毫秒 oneshot timeout
  向上取整为 1 ms，避免意外变成无超时。
- `IshSessionCallGate` 用 Swift 层 `NSCondition` 与 `activeCalls` 防止 `close()` 回收仍被
  已进入 C 调用使用的 handle。close 先 detach、拒绝新调用，再等待活动调用结束；
  `read(timeout: nil)` 以 100 ms 内部轮询，使 close 后的阻塞 read 能退出等待。
- `IshTerminal.setEventHandler` 保持原有非 `@Sendable` 公开签名，通过私有
  `@unchecked Sendable` box 隔离严格并发下的 queue crossing，避免破坏调用方源码。
- 公开错误仍是 `IshError.raw(Int32, String)`，现有 `IshInstance`、`IshSession`、
  `IshVM` 与 Terminal/VT 接口维持当前兼容形态。
- close 会等待所有已进入的 C 调用；若 v0.3.3 或自定义 supervisor 下某个非 read 调用
  永久不返回，close 也会持续等待，因此调用方仍应先停止并等待任务。完整 native
  borrow/cancel、类型化 status、Terminal callback 有界投递以及 VT parser 加固属于
  Stage2，本版本未交付。

## v0.3.3

Stage1 首次过渡 Release 发布前曾引用此 binary；历史详情以对应标签为准。
