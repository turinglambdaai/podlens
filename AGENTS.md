# AGENTS.md

指引给 AI agent（及开发者）：如何理解、构建、改动 PodLens。

## 这是什么

PodLens 是**原生**播客应用（Rivet 架构）：解决「听不懂英文播客」——转写、逐句翻译、总结。一份 Racket 应用核心以进程内方式嵌进各平台第一方 UI，**不共享任何 UI 代码**，靠 `app/backend.rkt` 的类型化契约保持一致：

| 平台 | 技术栈 | 目录 | 验证状态 |
|---|---|---|---|
| macOS 14+ | SwiftUI + Rivet（嵌入式 Racket CS） | `macos-host/` | ✅ 构建+打包+启动已验证 |
| Windows 10+ | WinUI 3 (C++/WinRT) + Rivet | `windows/` | 源码完成，CI 构建验证 |

GUI 与 CLI 共用同一份 Racket 核心：无参数启动 GUI；CLI 走源码方式
`racket app/cli.rkt <command>`（单文件自带运行时的 CLI 在路线图上）。

底层框架是 [Rivet](https://github.com/turinglambdaai/rivet)（同组织仓库）：RVT1 协议（typed RPC / Events / State）、嵌入式 Racket CS、构建编排。Rivet 迭代很快：**每次动工前先把本地 checkout 更新到 origin/main 再开发**；遇到 Rivet 的问题直接向上游提 issue 或 PR，不在 PodLens 里绕过或本地 hack。

## 快速命令

```bash
# 每次开发前：先把本地 rivet checkout 更新到 origin/main（Rivet 迭代很快）
git -C /path/to/rivet fetch origin && git -C /path/to/rivet reset --hard origin/main
# link（首次或 link 断了）：link rivet 与 rivet-cli 两个子包
raco pkg install --auto --no-docs --link /path/to/rivet/rivet /path/to/rivet/rivet-cli

# 后端测试（假 OpenAI 服务器，无需真实 key）
raco test tests/

# 构建并启动当前平台宿主（自动重新生成类型化客户端）
raco rivet dev

# 打包 + 自检
raco rivet package

# CLI 冒烟（隔离数据目录）
PODLENS_DATA_DIR=$(mktemp -d) racket app/cli.rkt doctor

# 更新签名密钥（仅首次；私钥永不入库）
scripts/update-keys.sh
```

## 契约（改任何行为前必读）

| 位置 | 内容 |
|---|---|
| `app/backend.rkt` | RVT1 契约：21 个 RPC、5 个事件（notify/job-progress/episodes-changed/open-url/update-available）、State `version`。行布局是位置参数字符串，注释在文件头 |
| `docs/UPDATE.md` | 更新契约：清单 JSON schema、Ed25519 签名对象、资产命名、客户端行为 |
| `rivet.rktd` | 发布身份唯一真源：version/build/identifier/最低系统版本。**tag 必须等于其中 version** |
| CHANGELOG.md | 每个 tag 必须有同名小节，release 前置检查会拒绝 |

## 数据布局

```
~/.podlens/            （测试/多实例用 PODLENS_DATA_DIR 覆盖）
├── config.json        设置（api-base/api-key/模型/语言），带校验与默认值
├── library.json       订阅 + 单集 + 播放进度（原子写）
├── audio/<id>.<ext>   音频缓存（<id> 是 40 位 hex）
└── transcripts/<id>.json  逐句稿 + 翻译 + 总结
```

宿主找本地音频不用加 RPC：按 `<episodeId>` 前缀扫 `audio/` 目录。

## 改动规则

- **改共享行为**（CLI 语义、管线、文案）：先改 `app/core/` + `tests/`，跑 `raco test tests/`；backend 契约变更 = 三端同步发布
- **改 UI**：macOS 在 `macos-host/Sources/RivetHost/`，Windows 在 `windows/`；`GeneratedBackend.swift/.hpp` 是生成物，不要手改
- **版本发布**：`rivet.rktd` version + CHANGELOG 小节 + 打 `v*` tag，CI 完成 packaging、Windows MSI（`raco rivet release`，WiX，含开始菜单/桌面快捷方式与 per-product UpgradeCode）、便携 zip、签名清单与 GitHub Release
- **应用图标**：`scripts/make-icons.py` 从代码绘制源图并生成 `assets/branding/app.ico` + `app.icns`（已配进 `rivet.rktd`）；改样式只改脚本里的 `draw_master()` 后重跑，不要手改生成物
- **i18n**：三份独立表（Racket `app/core/i18n.rkt`、macOS `Lang.swift`、Windows `I18n.h`），zh 为默认、en 为回退；键不要求跨端一致（各端只说自己要说的话）
- **更新密钥轮换**：先发一个信任新公钥的版本，再用新私钥签名，见 docs/UPDATE.md

## 诚实缺口（不要在文档里夸口）

- Windows 播放尚无波形/无缝衔接/章节标记（播放/seek/倍速 1.0–2.0 已有，M1 已追平 macOS）
- 发现页全目录搜索（`catalog-search` RPC）目前只在 macOS 宿主与 CLI 落地；Windows 发现面板还是纯精选目录，搜索 UI 待跟进
- Windows「检查更新」只报告结果，不自动安装；升级走 MSI 覆盖安装
- macOS 构建 ad-hoc 签名（公证证书未配置），首次启动需右键打开
- 翻译按句计费，成本由用户的 API key 承担
