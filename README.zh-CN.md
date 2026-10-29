# PodLens

听得懂的英文播客。跨平台桌面播客应用：用你自己的大模型 API key，把英文单集转写、逐句翻译、自动总结——为"读得懂、听不懂"的听众而生。

[![CI](https://github.com/turlamai/podlens/actions/workflows/ci.yml/badge.svg)](https://github.com/turlamai/podlens/actions/workflows/ci.yml) [![Release](https://img.shields.io/github/v/release/turlamai/podlens)](https://github.com/turlamai/podlens/releases/latest) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

[English](README.md) · **中文**

## 开始使用

免费 BYOK 应用。不提供 PodLens 托管 AI 订阅；模型用量由配置的服务商收费。 提供 macOS、Windows、Linux 桌面预览；移动与伴随端尚未交付。Linux 当前通过 PODLENS_LANG 选择语言。

[安装、首次使用、隐私与备份恢复](docs/USER-GUIDE.zh-CN.md) · [English](docs/USER-GUIDE.md)

## 安装

从 [Releases](https://github.com/turlamai/podlens/releases/latest) 下载：

| 平台 | 下载 | 更新 |
|---|---|---|
| macOS 14+（Apple Silicon 与 Intel） | `podlens-<version>-macos-<arch>.dmg`——`arm64` / `x64`（ad-hoc 签名——首次启动右键 → 打开） | 应用内更新（签名清单），或重新安装新版 DMG |
| Windows 10+ x64 | `podlens-<version>-windows-x64.msi` 安装器（含开始菜单/桌面快捷方式），或 `podlens-<version>-windows-x64.zip` 便携版 | 便携版：应用内下载并换装（签名清单）；MSI：重新运行新版安装器（应用内会提示） |
| Linux x64 / arm64 | `podlens-<version>-linux-x64.AppImage` / `podlens-<version>-linux-arm64.AppImage`（自带 GTK4 依赖）、`podlens-<version>-linux-<发行版架构>.deb`（随包附带 GStreamer 插件闭包含 `gst-libav`，保证 m4a/mp3 可播）、`.rpm`，或便携版 `tar.gz`/`.zip` | tar.gz/AppImage：应用内检查并下载（签名清单）；deb/rpm：用系统包管理器升级 |

每个发布都带 `SHA256SUMS` 校验清单和 Sigstore 构建来源证明（`gh attestation verify <file> -R turlamai/podlens`）。

macOS 版本仅为 ad-hoc 签名——首次启动若被 Gatekeeper 拦截，右键应用选「打开」。应用内通过 Ed25519 签名清单自动更新（[docs/UPDATE.md](docs/UPDATE.md)）。

## 为什么做 PodLens？

听力是多数非英语母语者最后一堵墙：播客没有翻译，没人帮你总结一小时的内容，停下来查词又毁掉节奏。PodLens 用一条逐集流水线解决：

- **转写**——调用任意 OpenAI 兼容的 `/audio/transcriptions` 端点（Whisper 等），输出带时间戳的逐句稿
- **翻译**——逐句对齐翻译成中文（或英文），与原文对照显示
- **总结**——每集自动生成一句话总结、5–8 条要点、值得记的话和话题标签

转写、翻译、总结都缓存在本地磁盘：一集只处理一次，结果永远是你的。

### 和主流播放器对比

| | PodLens | Apple 播客 | Overcast |
|---|---|---|---|
| 原生 UI | **SwiftUI / WinUI 3** | AppKit | web 套壳 |
| 逐句稿 | **按需生成，归你所有** | 仅部分播客 | 仅部分播客 |
| 翻译 | **逐句对照，本地缓存** | — | — |
| 总结 | **TL;DR + 要点** | AI 回顾（仅美区） | — |
| 模型 | **BYOK，任意 OpenAI 兼容端点** | 仅苹果 | — |

## 工作原理

PodLens 基于 [Rivet](https://github.com/turlamai/rivet) 构建：一个共享的 Racket 后端（订阅解析、媒体库、AI 流水线、更新客户端）以进程内方式嵌进每个第一方原生宿主。不是 Electron，不是 web 套壳。

```text
                 Racket 应用核心
      订阅 · 媒体库 · ASR/翻译/总结 · 更新
                          │
                    RVT1 协议
                类型化 RPC / 事件
                    ┌─────┴─────┐
                  macOS       Windows
                 SwiftUI      WinUI 3
```

- 后端把 Racket CS 嵌入应用进程内——没有后台守护进程，没有控制台窗口
- 播放走平台媒体栈；音频缓存在 `~/.podlens/audio/`
- UI 只与 `app/backend.rkt` 中的类型化契约对话——改契约就是一次三端同步发布

## 都有什么

- **一键听懂**：下载 → 转写 → 翻译 → 总结一次排队完成——已完成的阶段重跑时自动跳过，动手前还显示句数/字符数成本估算
- 订阅 RSS 播客源（RSS 2.0 + iTunes 标签），刷新检测新单集，每 30 分钟自动刷新（macOS 弹新单集系统通知）
- 播放：1.0–3.0× 倍速、±15/30 秒跳转、睡眠定时器、章节跳转（Podcasting 2.0 `podcast:chapters`）、连续播放接下一集、启动续播、系统媒体键 + Control Center（macOS）/ SMTC（Windows）/ MPRIS（Linux）
- 逐句转写稿跟随播放高亮（点句子即跳转）；逐句对照翻译，双语 / 只看译文 / 只看原文三种视图
- 结构化总结（TL;DR、要点、引用、话题），附 feed 自带的节目介绍
- 媒体库基础：已播/未播标记、每档未播徽章、单集任务进度
- 「发现」面板：经典英文播客精选目录 + 全目录搜索（iTunes Search API，无需密钥，结果自带 Apple 权威 feed 地址）——绝不自动订阅
- 先试听再订阅：发现页每张卡片一键试听最新单集——只拉取 feed、缓存试听音频，不入库不订阅（`feed-peek` / `preview-audio`；CLI 对应 `peek`、`preview`）
- 自带 key（BYOK）：OpenAI、DeepSeek、Groq、SiliconFlow、Ollama，任意 OpenAI 兼容端点
- agent 友好的 CLI，与 GUI 共用同一核心（`add`、`episodes`、`pipeline`、
  `estimate`、`transcribe`、`translate`、`summarize`、`show`、`export`、
  `find`、`done`、`search`、`peek`、`preview`、`--json`、退出码 0/1/2）
- 签名的应用内更新（Ed25519 清单 + SHA-256，见 [docs/UPDATE.md](docs/UPDATE.md)）
- [使用手册](https://podlens.jrtx.site/guide.html)：五分钟上手、BYOK 配置与常见问题

## 快速开始

### 1. 配置 API key

打开设置，填写：

- `api-base`——如 `https://api.openai.com/v1`（或 DeepSeek/Groq/Ollama 的地址）
- `api-key`——你的 key，保存在本机 `~/.podlens/config.json`，从不同步

### 2. 开听

添加播客 RSS 地址，选一集，点 **转写 → 翻译 → 总结**。或者在终端：

```bash
git clone https://github.com/turlamai/podlens && cd podlens
raco pkg install --auto --no-docs 'https://github.com/turlamai/rivet.git#v0.11.0'
racket app/cli.rkt add "https://feeds.example.com/show.xml"
racket app/cli.rkt episodes <feed-id>
racket app/cli.rkt transcribe <episode-id>
racket app/cli.rkt show <episode-id> --json
```

CLI 就是 GUI 内嵌的那份 Racket 源码，无头运行。（自带运行时的单文件 CLI
在路线图上；仓库内运行时 `--help`、`list`、`doctor`、`config` 不需要 API key。）

## 仓库结构

```text
podlens/
├── rivet.rktd              # 发布身份、版本、部署目标
├── app/
│   ├── backend.rkt         # RVT1 契约（21 个 RPC、5 个事件、1 个 State）
│   ├── update.rkt          # 签名清单更新检查
│   ├── cli.rkt             # agent 向 CLI（--json、退出码）
│   └── core/               # feeds、library、config、openai、pipeline、chapters、i18n
├── macos-host/             # SwiftUI 宿主（播放器、逐句稿、更新服务）
├── windows/                # WinUI 3 宿主（C++/WinRT code-behind）
├── linux/                  # GTK4 宿主（GStreamer 播放；GeneratedBackend.hpp 为生成物）
├── tests/                  # 41 个后端测试（含假 OpenAI 服务器，无需真实 key）
├── scripts/                # update-keys.sh、make-update-manifest.sh、make-icons.py
├── docs/                   # UPDATE.md（更新契约）· DESIGN-ROADMAP.md（M1–M5）
├── site/                   # podlens.jrtx.site（GitHub Pages）
└── .github/workflows/      # ci.yml · release.yml · pages.yml
```

## 开发

前置：[Racket CS](https://racket-lang.org/)（stable）并安装 pinned 的 Rivet 包；平台工具链（macOS 装 Xcode CLT，Windows 装 VS 2022 WinUI 工作负载，Linux 装 GTK4 与 GStreamer 开发包）。

```bash
raco pkg install --auto --no-docs 'https://github.com/turlamai/rivet.git#v0.11.0'
raco rivet doctor
raco rivet dev          # 构建并启动当前平台宿主
raco test tests/        # 后端测试（假服务器，不需要真实 key）
```

## 诚实缺口

- **无波形、无缝衔接或 Up Next 队列**——seek、±15/30 秒跳转、1.0–3.0× 倍速、睡眠定时器、章节跳转和连续播放已有；可拖拽排序的队列没有
- **章节仅支持 Podcasting 2.0 feed 指针**——内嵌 ID3/M4A 章节原子未解析；分章总结刻意不做（LLM 账单 ×N）
- **翻译成本只受你的用量约束**——选中单集的每一句都走你的 API，长单集花费真金白银。操作区会先显示句数/字符数估算，`podlens estimate <id>` 也能打印
- **一次一种目标语言**——改 `target-lang` 后流水线会重新翻译
- **macOS 构建为 ad-hoc 签名**——CI 配置公证证书之前，首次启动需要右键 → 打开

## 路线图

完整设计路线图（M1 一致性基线 → M2 逐句稿即产品 → M3 实用性闭环 → M4 移动伴随 → M5 沉淀回 Rivet）见 [docs/DESIGN-ROADMAP.md](docs/DESIGN-ROADMAP.md)。

- [x] RSS 订阅 + 单集缓存 + 播放进度
- [x] ASR / 翻译 / 总结流水线，任务进度事件
- [x] 一键听懂（pipeline RPC）+ 成本估算
- [x] SwiftUI + WinUI 3 宿主共用一份 RVT1 契约
- [x] Ed25519 签名更新通道
- [x] 系统媒体键（Control Center / SMTC）、跳转、睡眠定时器、3× 倍速
- [x] 章节标记（Podcasting 2.0）、已播标记、未播徽章、连续播放、启动续播
- [ ] macOS 公证 + Windows Authenticode 签名发布
- [ ] 分章总结、收听统计与生词导出
- [ ] 移动端伴随——iOS / iPadOS + Android 宿主、watchOS 遥控
      （设计见 [docs/DESIGN-ROADMAP.md](docs/DESIGN-ROADMAP.md)）

## 许可

[AGPL-3.0](LICENSE)。
