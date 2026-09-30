# PodLens

听得懂的英文播客。跨平台桌面播客应用：用你自己的大模型 API key，把英文单集转写、逐句翻译、自动总结——为"读得懂、听不懂"的听众而生。

[English](README.md) · **中文**

[![CI](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE) ![Version](https://img.shields.io/badge/version-1.1.0-C15F3C)

## 下载

从 [GitHub Releases](https://github.com/turinglambdaai/podlens/releases) 获取最新版：

| 平台 | 产物 |
|---|---|
| macOS 14+（Apple Silicon） | `PodLens-v*-macos.dmg`（ad-hoc 签名——首次启动右键 → 打开） |
| Windows 10+ | `podlens-*-windows-x64.msi` 安装器（含开始菜单/桌面快捷方式）或 `PodLens-v*-windows-x64.zip` 便携版 |

应用内通过 Ed25519 签名清单自动更新（[docs/UPDATE.md](docs/UPDATE.md)）。

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

PodLens 基于 [Rivet](https://github.com/turinglambdaai/rivet) 构建：一个共享的 Racket 后端（订阅解析、媒体库、AI 流水线、更新客户端）以进程内方式嵌进每个第一方原生宿主。不是 Electron，不是 web 套壳。

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

## 1.1 都有什么

- 订阅 RSS 播客源（RSS 2.0 + iTunes 标签），刷新时检测新单集
- 单集下载到本地缓存；倍速播放（1.0–2.0×），从上次进度继续
- 逐句转写稿，跟随播放高亮（点句子即跳转）
- 逐句对照翻译，双语 / 只看译文 / 只看原文三种视图
- 结构化总结（TL;DR、要点、引用、话题）
- 自带 key：OpenAI、DeepSeek、Groq、SiliconFlow、Ollama，任意 OpenAI 兼容端点
- 内置经典英文播客精选目录（科技/科学/商业/设计），「发现」面板一键添加，绝不自动订阅；每个条目发布前都用 `scripts/verify-catalog.rkt` 实测可达
- agent 友好的 CLI，与 GUI 共用同一核心（`add`、`episodes`、`transcribe`、`translate`、`summarize`、`show`、`--json`、退出码 0/1/2）
- 签名的应用内更新（Ed25519 清单 + SHA-256，见 [docs/UPDATE.md](docs/UPDATE.md)）

## 快速开始

### 1. 配置 API key

打开设置，填写：

- `api-base`——如 `https://api.openai.com/v1`（或 DeepSeek/Groq/Ollama 的地址）
- `api-key`——你的 key，保存在本机 `~/.podlens/config.json`，从不同步

### 2. 开听

添加播客 RSS 地址，选一集，点 **转写 → 翻译 → 总结**。或者在终端：

```bash
git clone https://github.com/turinglambdaai/podlens && cd podlens
raco pkg install --auto --no-docs --link /path/to/rivet
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
│   └── core/               # feeds、library、config、openai、pipeline、i18n
├── macos-host/             # SwiftUI 宿主（播放器、逐句稿、更新服务）
├── windows/                # WinUI 3 宿主（C++/WinRT code-behind）
├── tests/                  # 18 个后端测试（含假 OpenAI 服务器，无需真实 key）
├── scripts/                # update-keys.sh、make-update-manifest.sh
├── docs/                   # UPDATE.md（更新契约）、发布手册
├── site/                   # podlens.jrtx.site（GitHub Pages）
└── .github/workflows/      # ci.yml · release.yml · pages.yml
```

## 开发

前置：[Racket CS](https://racket-lang.org/)（stable）并 link Rivet 包；平台工具链（macOS 装 Xcode CLT，Windows 装 VS 2022 WinUI 工作负载）。

```bash
raco pkg install --auto --no-docs --link /path/to/rivet
raco rivet doctor
raco rivet dev          # 构建并启动当前平台宿主
raco test tests/        # 后端测试（假服务器，不需要真实 key）
```

## 诚实缺口

- **Windows 播放很基础**——每集播放/暂停走平台播放器；尚无波形、无缝衔接、章节标记
- **macOS 构建为 ad-hoc 签名**——CI 配置公证证书之前，首次启动需要右键 → 打开
- **翻译成本上不封顶**——选中单集的每一句都会走你的 API，长单集花费真金白银
- **一次一种目标语言**——改 `target-lang` 后需要重新翻译

## 路线图

- [x] RSS 订阅 + 单集缓存 + 播放进度
- [x] ASR / 翻译 / 总结流水线，任务进度事件
- [x] SwiftUI + WinUI 3 宿主共用一份 RVT1 契约
- [x] Ed25519 签名更新通道
- [ ] macOS 公证 + Windows Authenticode 签名发布
- [ ] 章节标记与分章总结
- [ ] 收听统计与生词导出

## 许可

[AGPL-3.0](LICENSE)。
