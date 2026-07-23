# IshEmbed 故障排查

简体中文｜[English](troubleshooting.en.md)

先判断故障发生在 manifest binary、源码、本地 XCFramework、RootFS 还是 PocketRoot
产品层。Stage1 最常见的误判，是把“合入 ABI 过渡源码”和“已经发布过渡 binary”当成
同一件事。

## 第一步：记录版本矩阵

```sh
git rev-parse HEAD
git submodule sync -- third_party/ish
git submodule update --init --checkout -- third_party/ish
git submodule status third_party/ish
rg 'ISH_EMBED_ABI_VERSION' include/ishembed.h
rg 'ISH_PROTO_VERSION' protocol/proto.h
rg -n 'url:|checksum:' Package.swift
git status --short
```

`git submodule sync` 会把旧 checkout 缓存在 `.git/config` 中的子模块 URL 更新为当前
`.gitmodules` 的 SSH-over-443 地址；修改 `.gitmodules` 本身不会自动更新这份缓存。

同时记录 Xcode/Swift/Zig 版本、运行平台、XCFramework 来源与 checksum，以及 RootFS 的
来源、大小和 SHA-256。没有这组信息，后续日志可能来自不同状态。

当前正确组合是：C ABI 1、wire v4、Swift 不调用 retain/release、manifest 指向已公开
的 `v0.4.0-abi.1`。发布 `v0.4.0-abi.2` 后，只有 manifest URL/checksum 应切到维护资产。

## 链接缺少 retain/release 或其他符号

症状：linker 报 `ish_embed_session_retain`、`ish_embed_session_release`、join/soft-halt
相关 undefined symbol。

排查：

```sh
nm -gU path/to/libIshKernel.a | awk '{print $NF}' | sort -u \
  | rg 'ish_embed_session_(retain|release)|ish_ffi_task_join|soft_halt'
```

- 如果 Stage1 Swift 源出现 retain/release 调用，说明误合入 Stage2 代码；Stage1 应恢复
  旧 ABI 兼容 Swift，而不是扩大本次维护发布范围。
- 如果检查的是本地 Stage1 XCFramework 而符号缺失，说明构建用了旧 commit、旧 gitlink
  或缓存；用隔离 build 路径重建。
- 如果 `v0.4.0-abi.2` 已发布但 manifest 仍是 `v0.4.0-abi.1`，检查 release
  commit/default branch fast-forward 是否完成；不要手工猜 checksum。

## Package.swift 看起来“还没更新”

维护 PR 合入到 Release 发布前，manifest 固定 `v0.4.0-abi.1` 是预期状态。发布脚本先
从合入 commit 重建/验证资产，再创建只更新 manifest 的 release commit，先公开并校验
资产，最后 fast-forward 默认分支。这样默认分支不会引用 404 URL。

只有已经确认 `v0.4.0-abi.2` Release 公开后，manifest 仍旧才是异常。此时检查：

```sh
gh release view v0.4.0-abi.2 --repo jacklv-coder/ish-arm64-pkg
git ls-remote --tags origin refs/tags/v0.4.0-abi.2
git fetch origin
git log --oneline --decorate -5 origin/main
```

## Boot 失败

按层排查：

1. RootFS 参数必须是包含 `data/` 与 `meta.db` 的可写 fakefs 根目录，而不是 tar.gz、
   Alpine 目录或宿主 `/`。
2. 检查 sandbox 权限、剩余空间、文件保护和路径生命周期。
3. 默认 supervisor 路径依赖 release XCFramework 的内嵌 blob；安装前会对实际 bytes
   计算 SHA-256，并要求摘要与构建元数据及内容寻址 guest path 一致。自定义
   `supervisorGuestPath` 会跳过内嵌安装和这项默认摘要校验，调用方必须保证
   guest-absolute path、静态 AArch64 ELF、执行模式、完整性、来源与 wire v4 匹配。
4. 保留 `kernelLogFD` 指向的日志。日志是 best-effort，不能替代返回码。

若失败来自本地 `scripts/run-host-tests.sh`，先运行
`scripts/build-rootfs.sh --print-identity`，再运行
`scripts/build-rootfs.sh --verify-bundle build`。runner 只复用 receipt/recipe 匹配、SQLite
行类型/16-byte stat/唯一 root、meta/data 路径一致、关键 arm64 摘要正确的四件套。锁文件
会永久保留；是否占用以内核 `flock` 为准，不能因文件存在而删除/接管。替换时 exchange
保持 `build/fs` 可见，失败回滚当前代际，成功后旧代际随 staging 删除，不再生成
`fs.previous.*`。若 SIGKILL/断电发生在多路径发布中，receipt-last 会让不完整代际验证失败，
下次重建；不要手工拼接 receipt、tar 或目录。

不要把 `ROOTFS_RECEIPT` 或 `--verify-bundle` 的成功解读成“当前 `fs` 等于 tar”。receipt
绑定的是静态 marker 和初始 `fs.tar.gz`/`SHA256SUMS`；运行态合法写入后，验证器会检查
当前树结构、数据库/关键身份及初始归档完整性，但不会比较当前树与 tar 的每个字节。
初始 tar 不是当前树恢复备份，也不能提交或随 Release 分发。

Stage1 Swift 公开错误仍是 `IshError.raw(code, message)`；不要按 Stage2 typed status 写
排查逻辑。C 状态的权威列表见 [`include/ishembed.h`](../include/ishembed.h)。

## `ISH_ERR_SUPERVISOR_INSTALL`

默认路径只有在“实际内嵌 bytes 的 SHA-256、64 位小写构建摘要、
`/sbin/.ishsv-ishembed-sha256-<digest>` 路径”三者一致后才调用安装 FFI。该错误通常说明
blob 与生成的元数据来自不同构建、bytes 被改动，或复用了旧 build cache；应在隔离目录
重建并运行 `scripts/verify-ios-artifact.sh`，不要手改摘要或内容寻址路径绕过失败。

显式 `supervisorGuestPath` 本来就绕过默认 blob 安装与这项摘要门禁，不能把它当作修复
默认资产不一致的捷径；选择自定义路径后，来源、签名、实际文件摘要、执行模式和 wire v4
兼容性全部由调用方建立。默认 SHA-256 比对只证明 bytes 与同一次构建的固定元数据一致，
不是数字签名，也不认证下载来源或发布者。本源码 PR 不携带 RootFS 或预构建 binary；测试
本地二进制必须由当前检出构建，正式二进制只能来自通过门禁的后续 Release。

## `ISH_ERR_PROTOCOL` / 握手失败

host 与 supervisor 必须精确使用 wire v4。常见原因：

- 新 host library 配了 RootFS 中旧 `/sbin/ishsv` 的自定义路径；
- 只重建了 supervisor 或只替换了 library；
- App 缓存了旧 XCFramework；
- pipe 中出现非帧字节、截断 frame 或 payload 超过 1 MiB。

优先使用 XCFramework 内嵌 supervisor 的默认路径，并从同一次干净构建取得两端。
公开 C ABI 1 与内部 wire v4 同时出现是正常的，不要把 ABI 常量改成 4 来“修复”握手。

## 固定新 iSH 后，旧 e2e 变慢或超时

先区分“持续有包安装/编译进度但超过时间预算”和“完全无进度的死锁”。JIT 一致性修复让
直链/RET cache 在下一目标命中待处理代码脏页时返回 dispatcher，排空脏页并断开受影响的
chaining/cache；纯数据写可继续直链，但每次链跳仍有检查成本。写密集的旧 Alpine e2e
仍可能明显变慢。

最新精确页修复只缩小失效范围：未发生切页的单页写和显式
`asbestos_invalidate_page` 会精确比较 block 页，跨页 block 的第二页使用 `end_addr`；只有
发生页面切换后的多页哈希位图可能因桶碰撞保守多失效。因此，单页碰撞测试通过能证明
“没有误杀远端同桶 block”，不能证明普通写不再返回 dispatcher，也不能据此声称吞吐已
完全恢复；代码页相交仍必须返回 dispatcher。复现时保留完整进度日志，运行测试文档中的
`dirty-page-trace`/`dirty_page_test` 与
性能基线；不要通过关闭失效、恢复直链或无限延长 CI timeout 掩盖正确性/性能问题。

## Spawn/chroot 失败

- `argv` 必须非空，guest 可执行文件和 cwd 必须存在。
- `chrootPath` 应为 guest 内绝对路径，例如 `/srv/vms/default`，不是宿主 URL。
- guest PID 1 会在每次绝对 chroot spawn 前重新检查 device、devpts/procfs 和必需目录；
  失败时查看 supervisor 日志及 RootFS 可写性/完整性。
- chroot 不是对抗恶意代码的沙箱。权限/网络/资源策略仍由 PocketRoot 实现。

## session 未收到 `EXITED`，日志出现 instance fail-close

这是 supervisor 无法证明 guest 后代已全部清理时的保守失败，不是一次正常 session 退出。
tracked group 与 TTY foreground job 清理后，PID 1 还会处理 double-fork/`setsid` 后由
subreaper 收养的未跟踪 child，并要求连续两次 `/proc` 扫描为空。扫描、精确 PID kill、
reap 或两秒期限失败时，supervisor 会对该 guest instance 执行兜底清理并退出，不发送
成功 `EXITED`/`SHUTDOWN_ACK`。

- 不要为缺失的成功帧自行合成退出码，也不要继续复用该 instance；完成 host 侧 close/
  shutdown 并重新 boot。
- 保留 guest log 中的 fail-close 原因，检查 procfs、iSH 子进程回收和自定义 supervisor
  是否与当前 XCFramework 同源。
- 若业务命令会 daemonize，仍应设置上层超时；该清理是隔离完整性的最后防线，不是后台
  服务生命周期 API。

## 输出停止或 `ISH_ERR_OUTPUT_LIMIT`

native 上限是最后一道内存保护：单 payload 1 MiB，每 session 未消费 4 MiB/4096 帧，
oneshot stdout 8 MiB、stderr 4 MiB。

- 流式 session 必须从独立任务持续 `read`，不能等待 child 退出后才消费。
- 检查 App 是否在 main actor 执行阻塞 read 或重 CPU 解析。
- 对高输出命令设置产品级更小配额，超限后关闭 stdin/terminate/close。
- stdin 写入也有有界队列；guest 不读取时，不要无限生产数据。

Stage1 没有交付新的 Terminal callback drop 事件或 VT parser 上限；如果日志提到这些 API，
说明运行的是 Stage2 候选源码而不是本阶段。

## `ISH_ERR_CONTROL_LIMIT`

这表示 host→guest 全局控制预算（4 MiB/256 个完整 wire frame）暂时无法接纳下一帧，通常是
supervisor 停止读取或控制管道严重拥塞。失败的下一帧未被接纳，但一次大
`ish_embed_session_write` 会拆成多帧，更早的 chunk 可能已送达，不能把整次调用当作原子写。

总预算内有 4 KiB/16 帧仅供内部 close/shutdown 生命周期清理；普通调用不能侵占。关键帧
仍无法接纳、writer 失败或 session close 在 1 秒内未确认退出时，runtime 会关闭 control
direction，让 guest 通过 EOF 清理全部 child；此后新调用返回 not-running，调用方应完成
session close 和 instance shutdown，而不是继续复用实例。

- 暂停生产新输入，不要用无延迟循环重试；先继续消费 session 事件/输出并观察退出状态。
- 如果 supervisor 已停止响应，按产品超时策略 terminate/close session，而不是无限积压。
- 日志出现该错误后若要重试业务协议，应由上层使用幂等命令或显式 offset/ack 处理部分写入。

## close/read/write 并发崩溃或异常

native C API 的 retain/release 已支持“已借用调用与 close 交错”。但 Stage1 Swift 为了
链接 v0.3.3 binary，尚未使用新增引用函数，而是通过 `IshSessionCallGate` 自动阻止
close/call UAF：close 先 detach handle、拒绝新调用，再等待所有已进入 C 调用结束后
执行 native close。因此不需要调用方用自己的锁串行每一次 I/O。

`read(timeout: nil)` 使用 100 ms C read 轮询，通常在当前 poll 返回后重新经过 gate 并
观察 close。但 close 没有固定上界：如果 v0.3.3 或自定义 supervisor 下某个已经进入的
非 read C 调用（例如阻塞 write）永久不返回，close 也会一直等待。

调用方仍应先停止并等待同一 session 的读写/信号任务，再调用幂等 `close()`，close 后
不再发起新操作。完整 native borrow/cancel/typed lifecycle 等待 Stage2；不要在 Stage1
单独加入 retain/release 调用造成旧 binary 链接失败。

## shutdown 返回 busy

`ISH_ERR_BUSY` 表示仍有 live session 或活动 instance call。按下面顺序处理：

1. 停止并等待 spawn/runOneshot；
2. 结束/等待 read、write、signal；
3. 对每个 session 执行 close；
4. 再 shutdown。

Stage1 Swift 的 instance gate 让 oneshot 在调用期持 lease，并把 spawn lease 保留到
session 的 native close 完成。只要存在 lease，shutdown 就在进入 v0.3.3 native 前立即
busy；并发 boot/shutdown 也不会交错。native `BUSY` 保留运行态，可完成上述清理后重试；
timeout 等终态失败隔离普通调用，但仍保留 shutdown 清理重试。该门禁防止旧 ABI UAF，
但不会取消调用，所以仍必须先等待它们结束。过渡
native 下同类状态也可能返回 busy 或相应错误。

成功 shutdown 后不要在同一进程再次 boot。底层 iSH 仍有进程级/TLS 状态；一次 lifecycle
是明确架构约束。

## iOS 构建或测试失败

- 确认 arm64 macOS、完整 Xcode、可用 iOS 18+ simulator。
- 不要复用来自另一 commit/gitlink/Zig 版本的 build 目录。
- `build-check` 若提示缺少 `-DISH_DISABLE_SKIP_BRK=1`，应对这个专用目录安全 reconfigure
  或重建；不能绕过门禁继续使用会吞掉 fatal guest SIGTRAP 的缓存。
- `Package.swift` 的最低 iOS、actual final link 的 deployment target 和 XCFramework
  slice 必须都为 iOS 18/arm64。
- `test-swift-ios.sh --manifest-binary` 测旧/已发布 pin；无参数或传路径测试本地新 binary。
  两者回答不同问题。
- sanitizer 失败先看第一个 stack；不要被后续取消/泄漏噪音遮住根因。

## 文档或脚本门禁失败

```sh
scripts/check-docs.sh
scripts/test-check-docs.sh
bash -n scripts/*.sh
scripts/test-release-version-policy.sh
git diff --check
```

- 双语文件名必须成对，中文页包含指向同名英文镜像的 English 切换链接，英文页包含
  指回同名中文主文档的简体中文切换链接。
- 删除文档时同步删除所有链接；Stage1 不应恢复 v0.4 migration 文档，因为完整 Swift
  API 尚未交付。
- ABI 专用 release notes 只应出现在 `v*-abi.*`，普通 rc 或稳定标签不得误带。

## Release 失败

不要立即删 tag/draft 或重跑同一标签。先保存脚本输出的 staging path、Release id、
release commit、tag raw OID 和 digest，再按[发布指南](releasing.md#失败与恢复)检查远端。
发布 PATCH 已尝试后的网络失败属于不确定状态，自动清理可能删除已经公开且有效的对象。

## 提交问题报告

问题报告至少包含：父仓库 commit、iSH gitlink、manifest URL/checksum、本地/Release
XCFramework digest、C ABI/wire 版本、iOS/Xcode/Zig 版本、RootFS manifest（不上传受限
资产）、最小复现、返回码、日志以及对应测试命令。移除 token、私钥、个人路径和业务数据。
