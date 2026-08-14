# IshEmbed 技术文档导航

简体中文｜[English](README.en.md)

本文档集说明 IshEmbed 的用途、Stage1 ABI 过渡状态、实现边界、测试方法与发布事务。
中文是主文档；英文镜像用于协作与发布复核。

## 先读：不要混淆四个版本面

1. **公开 C ABI**：权威定义为 [`include/ishembed.h`](../include/ishembed.h)，Stage1
   仍是 ABI 1，只增加兼容性符号。
2. **内部 wire protocol**：权威定义为 [`protocol/proto.h`](../protocol/proto.h)，
   Stage1 是 host 与内嵌 supervisor 精确匹配的 v4。
3. **Swift 源与 manifest binary**：Stage1 Swift 保持 v0.3.3 ABI 兼容且不调用
   retain/release。manifest 当前固定已公开的 `v0.4.0-abi.11`；只有
   `v0.4.0-abi.12` 资产公开并验证后，发布事务才将其切到维护 binary。
4. **RootFS 与 PocketRoot**：RootFS 是独立资产；PocketRoot 是上层产品。两者均不因
   runtime PR 或 Release 自动完成。

`v0.4.0-abi.11` 与维护版本 `v0.4.0-abi.12` 都是 native-first 过渡预发布，不是稳定
v0.4。完整 Swift lifecycle、类型化状态、Terminal callback 队列和 VT parser 改造
属于 Stage2。

## 推荐阅读顺序

1. [根目录 README](../README.md)：项目用途、安装状态与当前 Swift 用法。
2. [架构与生命周期](architecture.md)：Swift、C ABI、wire v4、线程和 guest PID 1
   如何协作。
3. [测试与验收](testing.md)：各门禁证明什么，以及 Stage1 发布前后的真链接边界。
4. [故障排查](troubleshooting.md)：从 manifest、符号、协议、RootFS、session 与构建
   层逐级定位。
5. [发布事务](releasing.md)：如何发布 ABI 过渡资产、更新 manifest，并在失败时恢复。
6. [变更日志](../CHANGELOG.md)：Stage1 已交付与 Stage2 未交付项。

## 文档索引

| 文档 | 回答的问题 |
| --- | --- |
| [架构与生命周期](architecture.md) | runtime 怎样实现？ABI 1 与 wire v4 有什么区别？ |
| [测试与验收](testing.md) | native、sanitizer、Swift/iOS 18、文档与供应链门禁各证明什么？ |
| [故障排查](troubleshooting.md) | boot、链接、协议、输出、shutdown 或发布失败时从哪里查？ |
| [发布事务](releasing.md) | 为什么维护源码合入后 manifest 仍是 `v0.4.0-abi.11`？何时变成 `v0.4.0-abi.12`？ |
| [变更日志](../CHANGELOG.md) | Stage1 的范围和兼容边界是什么？ |

## 一页架构

```text
SwiftUI / UIKit / PocketRoot
              │  Stage1: v0.3.3-compatible Swift wrapper
              ▼
public C ABI: include/ishembed.h (ABI version 1)
              │  host lifecycle + bounded I/O pumps
              ▼
internal wire: protocol/proto.h (exact-match v4)
              │
              ▼
guest PID 1 supervisor ── child processes / PTY / chroot
              │
              ▼
fakefs RootFS (independently sourced and installed)
```

Swift 对象不是另一套 runtime。它们包装 C handle；C 层管理线程、session 表、队列和
协议；guest PID 1 管理 Linux child、PTY、信号与回收；fakefs 持久化 guest 文件系统。

`third_party/ish` 是固定到精确 revision 的项目 fork，不是对别人上游工作树的直接修改。
嵌入式生命周期和 JIT 一致性需要进入 iSH core，outer package 无法独立实现；fork 用
独立 PR/CI 保存窄差异，通用修复仍可回馈上游。详见
[架构文档](architecture.md#分层与职责)。

## Stage1 的关键不变量

- 每个宿主进程只允许一次有效 instance lifecycle。
- `ish_embed_spawn` 返回的 C session handle 拥有一个 owner reference；新增
  retain/release 允许 native 调用安全借用。Stage1 Swift 尚未采用 native borrow，而是
  用兼容 v0.3.3 的 call gate 先 detach、拒绝新调用，再等待已进入调用后 close。
- call gate 防止 close/call UAF，但不会取消永久不返回的非 read C 调用；调用方仍应先
  停止并等待任务。
- shutdown 只在所有 session 已关闭、spawn/runOneshot 等活动 instance 调用已退出后成功；
  Swift 对 `BUSY` 恢复运行态，其他失败隔离普通调用但保留 shutdown 清理重试，成功后进入
  consumed 终态并拒绝二次 boot。
- host/supervisor wire 必须同为 v4，不做跨版本协商。
- 默认 supervisor 安装前校验实际内嵌 bytes 的 SHA-256 与同一构建元数据/内容寻址路径；
  自定义路径绕过该默认门禁。摘要一致不等于数字签名或来源认证。
- JIT 单页写及显式 `invalidate_page` 按精确页过滤；仅多页哈希位图可能因碰撞保守多
  失效。下一直链/RET 目标命中待处理代码脏页时才回到 dispatcher；纯数据写可继续直链。
- RootFS 不属于 package/release，禁止将其混入 Corresponding Source。
- 当前 `v0.4.0-abi.12` 源码变更不携带 RootFS 或预构建二进制；XCFramework 必须由
  后续发布事务生成。

## 权威来源

| 内容 | 权威文件 |
| --- | --- |
| 公开 C ABI 与错误码 | [`include/ishembed.h`](../include/ishembed.h) |
| host ↔ supervisor 帧布局 | [`protocol/proto.h`](../protocol/proto.h) |
| Swift API | [`Sources/IshEmbed/`](../Sources/IshEmbed/) |
| 当前平台、binary URL/checksum | [`Package.swift`](../Package.swift) |
| native 构建 | [`scripts/build-ios.sh`](../scripts/build-ios.sh) |
| 发布事务 | [`scripts/release.sh`](../scripts/release.sh) |

若文档与代码不一致，以这些权威文件为准，并在同一变更中修正文档及英文镜像。
