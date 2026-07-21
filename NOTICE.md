# 许可与第三方通知

[English](#english)

**修改通知：本组件及固定的 iSH fork 已于 2026 年为 PocketRoot 修改。**

IshEmbed 是为 PocketRoot 集成和修改的运行时组件。本仓库以及固定的
`third_party/ish` fork 包含为 arm64 guest、宿主嵌入 API、生命周期管理和隔离所做的
修改；具体修改日期、作者和内容以 Git 历史为准。

- 本仓库代码按 GNU GPL v3 或更高版本发布，完整条款见 [LICENSE](LICENSE)。
- `third_party/ish` 固定到明确的 Git revision。iSH 的许可说明见
  `third_party/ish/LICENSE.md`，通过 Apple App Store 分发时还必须阅读
  `third_party/ish/LICENSE.IOS`。
- 随 XCFramework 内嵌的 guest supervisor 由 Zig 以 `aarch64-linux-musl` 静态链接。
  musl 的许可与作者通知见 `third_party/licenses/musl-COPYRIGHT`；每个发布版本的
  `IshEmbed-corresponding-source.tar.gz` 还包含该次工具链提供的 musl 源码快照。
- Xcode SDK、Apple 系统 SQLite 以及 Zig、Meson、Ninja、Clang 等通用构建工具不会
  作为发布资产再分发。
- RootFS 不是 XCFramework 的一部分，也不属于对应源码归档。RootFS 的来源、内容、
  哈希、出口限制和许可证必须通过独立门禁；`scripts/release.sh` 不会读取、上传或发布
  RootFS。
- XCFramework 的 `Licenses/` 按本仓库相对路径保存本文件、`LICENSE`、中英文发布指南、
  iSH 许可与 musl 通知，使本文件中的相对链接在二进制归档内仍然有效。

发布二进制时必须同时发布同一版本的 `IshEmbed-corresponding-source.tar.gz`，并保留
其中及本文件中的许可和修改通知。发布流程见 [docs/releasing.md](docs/releasing.md)。

以上为工程合规记录，不构成法律意见。

## English

**Modification notice: this component and the pinned iSH fork were modified
for PocketRoot in 2026.**

IshEmbed is a runtime component integrated and modified for PocketRoot. This
repository and the pinned `third_party/ish` fork contain changes for the arm64
guest, host embedding API, lifecycle management, and containment. Git history
is the authoritative record of each change, date, and author.

- Repository code is distributed under GNU GPL v3 or later; see
  [LICENSE](LICENSE).
- `third_party/ish` is pinned to an exact Git revision. Read
  `third_party/ish/LICENSE.md` and, for Apple App Store distribution,
  `third_party/ish/LICENSE.IOS`.
- The guest supervisor embedded in the XCFramework is statically linked for
  `aarch64-linux-musl` by Zig. The musl license and attribution are in
  `third_party/licenses/musl-COPYRIGHT`; every release's
  `IshEmbed-corresponding-source.tar.gz` also contains the musl source snapshot
  exposed by that release toolchain.
- The Xcode SDK, Apple system SQLite, and general-purpose build tools such as
  Zig, Meson, Ninja, and Clang are not redistributed as release assets.
- RootFS is neither part of the XCFramework nor the Corresponding Source
  archive. Its provenance, contents, hash, export constraints, and licenses
  remain behind a separate gate; `scripts/release.sh` never reads, uploads, or
  publishes a RootFS.
- The XCFramework's `Licenses/` tree preserves repository-relative paths for
  this file, `LICENSE`, both release guides, the iSH licenses, and the musl
  notice, so this file's relative links remain valid inside the binary archive.

When distributing the binary, publish the matching
`IshEmbed-corresponding-source.tar.gz` and preserve the license and modification
notices in that archive and this file. See
[docs/releasing.en.md](docs/releasing.en.md) for the release procedure.

This is an engineering compliance record, not legal advice.
