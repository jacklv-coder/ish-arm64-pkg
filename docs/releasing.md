# IshEmbed 发布指南

简体中文｜[English](releasing.en.md)

本文用于维护者发布 XCFramework 与匹配的 Corresponding Source。Stage1 的目标标签是
`v0.4.0-abi.1`：这是 ABI 过渡 prerelease，**不是稳定 v0.4.0**。执行发布会创建公开
GitHub Release 和更新默认分支，必须在获得明确发布授权后进行。

## Stage1 发布前后状态

### PR 合入后、Release 发布前

- `Package.swift` 仍固定已发布的 v0.3.3 URL/checksum；
- Swift 源保持 v0.3.3 ABI 兼容，不调用 retain/release；
- 仓库中已有公开 C ABI 1 的增量实现和内部 wire v4 源码；
- 没有可供使用方安装的 Stage1 binary。

这个中间状态是刻意设计的：默认分支不会先暴露一个尚未公开、会返回 404 的资产 URL。

### `v0.4.0-abi.1` 成功发布后

- release commit 只改 `Package.swift`，固定到新 XCFramework URL/checksum；
- GitHub prerelease 包含 `libIshKernel.xcframework.zip` 与
  `IshEmbed-corresponding-source.tar.gz`；
- 公开 C ABI 仍是 1，内部 wire 仍是 v4；
- Swift 仍是旧 ABI 兼容层，完整 Swift lifecycle/typed status/Terminal/VT 改造留 Stage2；
- RootFS 仍不在任何发布资产中。

## 前置条件

1. 在默认分支执行，工作树必须干净并与 SSH remote 的默认分支完全一致。
2. remote 的 fetch/push URL 必须是 GitHub SSH；发布脚本会拒绝 HTTPS 和非目标仓库。
3. `gh auth status --hostname github.com` 成功，且账号具有创建 Release、tag 和更新默认
   分支的权限。
4. 安装 `git`、`gh`、`swift`、`zip`、`shasum`、`python3`、`curl`、`zig`、Meson、Ninja、
   LLVM/lld 等构建工具。
5. `third_party/ish` 已初始化，父仓库 gitlink 指向已审查的固定 revision。
6. 重要 diff 已通过 Luna CR，P1/P2 已清零；CI、iOS 18 真链接、native sanitizer、
   文档与供应链门禁均通过。
7. 已明确确认本次只发布 runtime/Corresponding Source，不发布 RootFS。

## 发布前本地门禁

```sh
git diff --check
bash -n scripts/*.sh
scripts/check-docs.sh
scripts/test-check-docs.sh
scripts/test-release-version-policy.sh
scripts/test-release-tag-cas.sh
scripts/test-source-policy.sh
scripts/run-host-tests.sh
scripts/test-swift-ios.sh --manifest-binary
PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH" \
  scripts/build-ios.sh
scripts/verify-ios-artifact.sh
scripts/test-swift-ios.sh --local-binary
```

`--manifest-binary` 证明 Stage1 Swift 仍能链接发布前固定的 v0.3.3 binary；
`--local-binary` 证明相同 Swift 能链接待发布的 ABI 过渡 XCFramework。两者缺一不可。

## 执行

确认标签不存在且获得发布授权后：

```sh
scripts/release.sh v0.4.0-abi.1
```

脚本根据 SemVer 后缀设置 GitHub `prerelease=true`。只有 `v*-abi.*` 标签会附加专用
中英文说明，明确它不是稳定 v0.4，列出 native lifecycle/retain-release/
join-soft-halt/wire v4，并说明 Swift 和 RootFS 边界。
除了 SemVer 检查，Stage1 版本策略还会硬性拒绝除 `v0.4.0-abi.1` 以外的任何标签。
因此即使误输入 `v0.4.0`，也会在任何 tag、draft 或资产写入前失败。

不要用 `v0.4.0` 代替过渡标签。稳定标签必须等 Stage2 合入、迁移与回归完成后另行决定。

## 发布事务做了什么

1. 校验严格 SemVer、SSH remote、GitHub 登录、干净默认分支、远端 head/tag/Release 状态。
2. 先用 manifest binary 对当前 Swift 做 iOS 18 真链接与 XCTest。
3. 在隔离 worktree 从固定 commit/submodule 重建 XCFramework，用与 CI 相同的
   `verify-ios-artifact.sh` 校验 Info.plist 实际引用路径、两个 arm64 slice 的逐 Mach-O member
   platform/minos、无 warning 的 iOS 18 最终链接、ABI 符号、supervisor 从 ELF 到生成数组
   和两份链接产物的逐字节一致性、hash/path 及许可文件，再用该本地 binary 做 XCTest。
4. 计算 SwiftPM/SHA-256；在隔离 worktree 只更新 `Package.swift` URL/checksum 并创建
   release commit。主工作树不被改写。
5. 从 release commit 打包 Corresponding Source，验证父仓库、固定 iSH、Zig 与 musl
   记录，并通过 RootFS/嵌套归档拒绝策略。
6. 将 release commit 推到唯一 staging ref，创建私有 draft，上传并核对两个资产的
   数量、大小、digest 与状态。
7. 以 absent-ref lease/CAS 创建唯一 annotated final tag；把已验证 draft 切换到最终
   标签并发布 prerelease。
8. 从公开 URL 重新下载两个资产并复核 checksum。
9. 最后才 fast-forward 默认分支到只改 manifest 的 release commit，避免默认分支提前
   引用 404 资产；随后清理持有明确 ownership 的 staging 对象。

## 资产与许可

Release 必须恰好包含：

| 资产 | 内容 |
| --- | --- |
| `libIshKernel.xcframework.zip` | device/simulator arm64 runtime 与内嵌 supervisor |
| `IshEmbed-corresponding-source.tar.gz` | 重建该 binary 所需父仓库、固定 iSH revision、musl 等源码 |

RootFS 既不是 binary，也不是 Corresponding Source。脚本不会读取或上传 RootFS，源码策略
会按路径、名称、magic 和嵌套归档检测拒绝疑似 RootFS 内容。RootFS 的来源、hash、许可
和分发由 PocketRoot 独立管理。

## 发布后验收

```sh
gh release view v0.4.0-abi.1 --repo jacklv-coder/ish-arm64-pkg
git fetch origin --tags
git show v0.4.0-abi.1:Package.swift
git pull --ff-only origin main
scripts/test-swift-ios.sh --manifest-binary
```

同时确认：

- Release 标记为 prerelease，说明包含 ABI 过渡中英文段落；
- final tag 指向 release commit，commit 仅修改 `Package.swift`；
- 两个资产公开可下载且 digest 与 release 输出一致；
- manifest URL 使用同一标签，checksum 匹配公开 zip；
- `ISH_EMBED_ABI_VERSION` 仍为 1，binary 导出 retain/release 与 join/soft-halt 所需符号；
- Release 不含 RootFS。

## 失败与恢复

脚本按“只删除能证明归属的对象”设计：

- 发布尝试前的普通失败会清理隔离 worktree/临时目录；
- 已验证 draft 之后失败会保留 draft、staging ref 和本地资产，供人工检查；
- final tag push 返回不确定时不删除任何远端对象；
- 一旦 publication PATCH 已尝试，网络错误会造成结果不确定，脚本不自动删除 tag 或
  Release；必须先查询 Release id、tag raw OID/peeled commit、assets 与默认分支；
- Release 已公开但默认分支 fast-forward 失败时，不能重发同标签；先验证资产，再将默认
  分支安全 fast-forward 到既有 release commit。

任何恢复都不得强推覆盖未知对象，也不得按名称盲删 tag/draft。保存脚本打印的 staging
路径、Release id、commit、tag object OID 与 digest，再进行人工处理。

## Stage2 进入条件

只有 `v0.4.0-abi.1` 公开资产、manifest 更新和发布后真链接全部通过，Stage2 才能合入
Swift retain/release lifecycle、typed status 与 Terminal/VT 改造。Stage2 仍需独立 CR、
测试、文档和发布决策；ABI 过渡 prerelease 本身不能宣称稳定 v0.4 已完成。
