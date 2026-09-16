# Silent X Craft Launcher (静音 X 飞船发射器)

一款跨平台、开源的 Minecraft 启动器，采用 PCL2 风格界面，支持 BMCLAPI 镜像加速。

---

## ✨ 特性

- **跨平台** — 支持 Windows / macOS / Linux，x64 与 ARM64 架构
- **PCL2 风格界面** — 基于 PySide6 + QFluentWidgets，扁平卡片式设计，明暗主题跟随系统
- **BMCLAPI 镜像加速** — 自动切换国内镜像源，解决 Minecraft 资源下载慢问题
- **多线程分片下载** — 大文件自动分片并行下载，支持断点续传和 SHA1 校验
- **模组加载器支持** — 一键安装 Forge / Fabric / NeoForge，自动处理版本兼容
- **智能 Java 管理** — 自动扫描系统 Java 运行时，支持从 Oracle CDN 一键下载 JDK 21 LTS
- **启动进度可视化** — 启动游戏时实时显示各阶段状态，自动检测游戏窗口
- **崩溃日志收集** — 游戏崩溃后自动收集 crash-reports / hs_err 日志

---

## 下载引擎（自研异步）

引擎在 src/core/download/，**纯标准库实现（asyncio + ssl）**，不依赖 aiohttp / httpx：

- **多连接并发**：单线程事件循环 + 最多 128 条连接（默认 32），大文件自动 HTTP Range 分片，
  小文件跳过探测直接下；实测 25.6MB 客户端 jar 在官方源上 3.5 秒完成。
- **两条路自动回退**：每个资源都同时算出「Mojang 官方」与「BMCLAPI 镜像」两个 URL
  （见 src/core/mirror.py），一条失败自动换另一条；某条路持续低于 512 KB/s 也会自动换源。
- **智能选源（默认）**：实测速度与失败记录会写进 cache/sources.json（见 src/core/source_stats.py），
  下次启动直接首选更快的那条路；硬失败（连不上 / 超时）会立刻熔断该源 90 秒（src/core/health.py），
  所以镜像抽风时启动不会被拖住，镜像恢复后速度数据会自动把它拉回来。删掉该 json 即重置偏好。
- **元数据同样有硬截止**：src/core/net.py 把请求放进 worker 线程并 join(deadline)，
  避免某个源对多个 IP 逐个超时导致"卡死 80 秒"。
- **断点续传**：.part 与 .part.json 记录已完成分片，中断后只补缺失部分（删掉这两个文件即可重来）。
- **全局限速**：设置页可设 KB/s（0 = 不限速），所有连接共用一个令牌桶，改动即时生效。
- **全量 SHA1 校验**：**凡是 Mojang 提供哈希的资源一律强制校验** —— 版本清单、版本 JSON、
  客户端 / 服务端 jar、依赖库（含 natives 分类器）、资源索引、资源对象、官方 JRE 文件。
  校验失败即删档、换源、重下；结果有磁盘缓存（cache/hashes.json），不会每次启动重算几千个资源。

### 下载源对照（实测，2026-09）

| 资源 | 官方（第一条路） | BMCLAPI（第二条路） |
|:--|:--|:--|
| 版本清单 | piston-meta.mojang.com/mc/game/version_manifest_v2.json | bmclapi2.bangbang93.com/...（同路径） |
| 版本 JSON / 资源索引 | piston-meta.mojang.com/v1/packages/SHA1/id.json | 同路径透传 |
| 客户端 jar | piston-data.mojang.com/v1/objects/SHA1/client.jar | 同路径透传，或 /version/ID/client |
| 依赖库 | libraries.minecraft.net/PATH | /maven/PATH |
| 资源对象 | resources.download.minecraft.net/H2/HASH | /assets/H2/HASH |
| 模组加载器 | Forge / NeoForge / Fabric maven | /maven、/forge、/neoforge、/fabric-meta |
| Fabric 安装器 | maven.fabricmc.net | 镜像没有，固定走官方 |
| Quilt | maven.quiltmc.org / meta.quiltmc.org | 镜像不可用，固定走官方 |
| 官方 JRE | launchermeta.mojang.com/v1/products/java-runtime/... | 清单与文件都支持路径透传 |

> 注意：BMCLAPI 的 /version/ID/json **不会**改写内部 URL；/version/ID/CATEGORY
> 只对 client / server / json 有效，其它 category 会静默返回客户端 jar。
> 这些坑在 src/core/mirror.py 顶部有完整记录。

### 维护脚本

    python scripts/probe_sources.py                                 # 体检两条路是否都通
    python scripts/bench_download.py --mode all --source official    # 并发 / 限速基准
    python scripts/e2e_install.py --version 1.21.1 --assets 200      # 真实安装 + 校验端到端自测
    python scripts/check_keymap_ui.py                               # 按键映射页像素级自检（深/浅色）
    python scripts/export_keymap_assets.py                          # 把预设+教学导出给安卓端
    python scripts/build_android_apk.py --check                     # 安卓打包工具链体检

想重置"哪条路更快"的记忆，删掉 {配置目录}/cache/sources.json 即可。

## 📦 安装

### 前提条件

- Python 3.11+
- Git

### 从源码运行

```bash
git clone https://github.com/Silent-Studio-CN/Silent-X-Craft-Launcher.git
cd Silent-X-Craft-Launcher
pip install -r requirements.txt
python main.py
```

### 打包为可执行文件（一份代码 → 各平台安装包）

所有平台的产物都由同一套入口驱动，先看这台机器能打什么：

```bash
python scripts/build_installers.py --list      # 目标 + 工具链体检（缺什么会直说）
python scripts/build_installers.py --prepare   # 只生成 Inno/.desktop/AppRun 模板
python scripts/build_installers.py --target windows-portable
python scripts/build_installers.py --all       # 打本机支持的全部目标
```

| 目标 | 产物 | 需要的工具 |
|:--|:--|:--|
| `windows-portable` | `SXCL-<版本>-windows-x64-portable.zip` | Nuitka（首选）或 PyInstaller |
| `windows-setup` | `SXCL-<版本>-windows-x64-setup.exe` | 上面的 + Inno Setup（`winget install JRSoftware.InnoSetup`）|
| `macos` | `SXCL-<版本>-macos-<架构>-app.zip`（有 hdiutil 再出 `.dmg`）| 在 macOS 主机上跑 |
| `linux` | `SXCL-<版本>-linux-x64.AppImage`（没有 appimagetool 就出 tar.gz）| 在 Linux 主机上跑 |
| `android` | `build/android/SXCL-Keymap-<版本>.apk` | Android SDK build-tools + JDK 17（**不需要 Gradle**）|

安卓包走的是免 Gradle 的 `scripts/build_android_apk.py`（aapt2 → javac → d8 → zipalign → apksigner），
在这台开发机上实测可出签名 APK；`android/` 里同时也有一份完整的 Gradle 工程，
两条路的产物是同一个 App（包名 `com.silentstudio.sxcl`）。

单独打包（旧写法，仍然可用）：

```bash
pip install nuitka
python build.py                    # 自动识别 Windows / macOS / Linux
```

## 🏗 项目架构

```
main.py                     # 应用入口
src/
├── core/                   # 跨平台核心层（无 UI 依赖，可单独跑）
│   ├── platform.py         # 系统 / 架构 / 路径（所有 os.name 判断都在这里）
│   ├── settings.py         # 设置门面：唯一允许碰 UI 配置的地方
│   ├── constants.py exceptions.py logger.py lang.py
│   ├── mirror.py           # 官方 + BMCLAPI 双路资源定位（含实测结论）
│   ├── net.py              # 元数据请求：硬截止 + 双路 + 熔断
│   ├── health.py           # 下载源熔断表
│   ├── source_stats.py     # 源偏好记忆（谁快记谁）
│   ├── download/           # 自研异步下载引擎（asyncio，纯标准库）
│   │   ├── engine.py limiter.py verify.py spec.py
│   └── keymap/             # 按键布局数据契约（桌面 + 安卓共用，见下节）
│       ├── model.py        # sxcl.keymap.v1：控件 / 绑定 / 校验 / 冲突检测
│       ├── presets.py      # 极简 / 生存 / 建造 / 对战 / 单手 五套预设
│       ├── guide.py        # 教学步骤生成（手机上的"按键帮助"文案源头）
│       ├── fcl.py          # FCL 布局导入导出（迁移用）
│       └── store.py        # 布局存取（配置目录 keymaps/）
├── services/               # 业务逻辑（只依赖 core）
│   ├── java/               # finder / compatibility / mojang_runtime（官方 JRE）
│   ├── minecraft/          # manifest / launcher / installed
│   └── mod_loader/         # api（Forge/Fabric/NeoForge/OptiFine）
│       ├── installer.py    # 加载器安装编排
│       └── analyzers/      # 安装器分析（forge / fabric）
└── app/                    # UI 层（PySide6 + QFluentWidgets）
    ├── common/             # launcher_config（QConfig）/ base_page
    ├── pages/              # home / versions / download_config / download_progress / launch
    │                       # / tasks / settings / keymap（按键映射编辑器）
    ├── widgets/            # java_setting_card 等自定义控件
    └── main_window.py

android/                    # 安卓端（纯 Java，无第三方依赖；共享 src/core/keymap 的数据契约）
├── app/src/main/java/com/silentstudio/sxcl/
│   ├── keymap/             # 与 Python 侧同构的模型/校验/冲突/教学读取
│   ├── overlay/            # 悬浮按键层 + 按键注入接口（InputSink）
│   ├── data/               # assets 布局读取 + 用户改过的布局存储
│   └── ui/                 # 布局列表 / 预览 / 教学界面（GuideActivity）
└── app/src/main/assets/keymaps/   # 由 scripts/export_keymap_assets.py 从 Python 预设生成
```

依赖方向单向：**core ← services ← app**；core 里只有 settings.py 会接触 UI 配置。

## 🎨 主题

- 主题状态**只有一个来源**：QFluentWidgets 内置的 qconfig.themeMode / qconfig.themeColor
  （不要在 LauncherConfig 里重定义，否则会出现两套状态，切换时半明半暗）。
- 所有自定义颜色都走 src/app/theme.py 的令牌表（tokens()），页面用
  on_theme_changed(回调) 注册"主题变了重刷样式"，切换时无需重建窗口
  （SXCL 的下载/安装任务是 QThread，重建窗口会打断它们）。
- apply_theme() 会一次性设置：库主题 + 应用 QPalette（原生控件）+ 全局 QSS + 刷新所有控件。
- 设置页提供"主题模式"（浅色/深色/跟随系统）与"主题色"两张卡片。
- 历史遗留的 #ff009faa（带 alpha 的青绿）已在启动时自动纠正为 Fluent 蓝 #0067c0。

## 🌍 跨平台

所有系统判断都集中在 src/core/platform.py（不要在新代码里直接写 sys.platform / os.name），
下载、安装、启动三条链路都已经做到"同一份代码三平台跑"：

| 能力 | Windows | macOS | Linux |
|:--|:--|:--|:--|
| 打包产物 | onefile exe（x64 / ARM64） | .app bundle（x64 / ARM64） | onefile 二进制（x64） |
| 官方 JRE 平台键 | windows-x64 / windows-arm64 | mac-os / mac-os-arm64 | linux |
| natives 选择 | natives-windows(-arm64) | natives-macos(-arm64) + patch 叠加 | natives-linux |
| 打开游戏目录 | os.startfile | open | xdg-open |
| 游戏窗口检测 | user32.FindWindow | 不支持，退化为"等待进程" | 同左 |
| 进程优先级调整 | 低于正常 | — | — |
| 界面字体 | Segoe UI / 微软雅黑 | PingFang SC | Noto Sans CJK SC / 文泉驿 |

已知限制（都会在 UI 上如实体现，不会假装成功）：

- **窗口检测**：macOS 需要 pyobjc / 辅助功能权限、Linux 需要 xdotool，因此这两平台不做枚举，
  启动页显示"等待游戏启动"，进程存活 8 秒即视为成功（Windows 仍然是真检测）。
- **macOS 分发**：未签名未公证的 .app 首次运行需 右键→打开，或执行
  xattr -dr com.apple.quarantine SXCL.app；正式发布请自行 codesign --deep 加 notarytool。
- **Linux 运行期依赖**：libxcb-cursor0、libxkbcommon-x11-0、libegl1、libgl1 以及中文字体
  （缺 xcb-cursor 会直接起不来）。
- **natives 架构隔离**：同一原生库只保留最匹配当前架构的那一个变体（避免 Apple Silicon /
  Windows ARM64 上 x86_64 与 arm64 的 dylib/dll 互相覆盖），macOS 的 -patch 补丁包单独保留。

在任意系统上排查问题，先跑：

    python scripts/check_platform.py                    # 平台能力 / Java / 目录 / JRE 平台键
    python scripts/check_platform.py --version 1.21.1   # 顺带列出该版本会下载哪些 natives

## 📱 安卓端与按键帮助

安卓端不是"把桌面界面塞进手机"，而是一个**共享受众数据的按键助手**：

* 数据契约只有一份：`sxcl.keymap.v1`（`src/core/keymap/`）。桌面启动器里编好的布局，
  经 `scripts/export_keymap_assets.py` 导出成 `assets/keymaps/*.json` 直接进 APK，
  预设和教学文案不会出现"两端各写一遍"的漂移。
* 手机上的按键层是悬浮窗（`KeymapOverlayService`）+ 可拖拽编辑，坐标全部归一化，
  换机型/横竖屏都不会错位（FCL 用像素坐标，换机就要重排）。
* 注入接口是 `InputSink`（`keyDown/keyUp/keyTap`），接哪种注入器都行，
  默认实现只打日志，所以界面和教学可以脱离游戏先跑通。

### 比 FCL 多出来的"按键帮助"

| 能力 | FCL | SXCL |
|:--|:--|:--|
| 冲突检测 | 无（键抢了只能自己踩坑）| 抢键（不同动作绑同一个键）、控件重叠、缺关键动作都会直接报出来 |
| 教学 | 只有可拖的按钮 | 每个键带 `hint`，`build_guide()` 生成分步教学（跳跃→移动→背包…），可导出 Markdown |
| 找键 | 肉眼在满屏按钮里找 | 输入中文/英文/按键名（"潜行"、"jump"、"KEY_SPACE"）就高亮对应按键 |
| 跨端编辑 | 只能在手机上改 | 桌面端 `按键映射` 页编排 + 预览 + 冲突面板，再推给手机 |
| 手势说明 | 长按/双击靠猜 | 界面上直接标出"长按另有功能"，四种事件（按下/长按/单击/双击）与 FCL 一致 |
| 迁移 | — | 支持导入 FCL 导出布局（像素坐标自动归一化），也能导出回 FCL |

真机安装（USB 调试打开后）：

```bash
python scripts/build_android_apk.py --install     # 打完直接 adb install -r
```

APK 需要"显示在其他应用上层"权限；悬浮层退出/切后台一定会 `releaseAll()`，
不会出现"手放开了人还在往前走"的经典问题。

## ⚙ 配置

| 设置项 | 说明 |
|--------|------|
| Java 路径 | 手动选择或自动检测 |
| 最大内存 | 滑块调节，范围 20%–75% 系统内存 |
| 下载源 | 智能（自动选更快的源，默认）/ Mojang 官方源 / BMCLAPI 镜像源 |
| 并发连接数 | 4-128，默认 32；带宽跑不满可调高 |
| 下载限速 | KB/s，0 = 不限速，全局生效 |
| SHA1 校验 | 默认开启，Mojang 给了哈希的资源全部强制校验 |
| 版本隔离 | 每个版本独立运行目录 |
| 主题 | 浅色 / 深色 / 跟随系统 |

## 🔗 相关资源

- [BMCLAPI 文档](https://bmclapidoc.bangbang93.com/) — 镜像 API 说明
- [PCL2 源码](https://github.com/Meloong-Git/PCL) — 界面设计参考
- [QFluentWidgets](https://qfluentwidgets.com/) — UI 组件库

## 📄 许可证

本项目采用 **GNU Affero General Public License v3.0 (AGPL-3.0)**，
并附加 SXCL 特别条款（禁止收费、轻度/重度使用划分、禁止碰瓷命名）。

详见 [LICENSE](LICENSE) 文件。

> 本项目使用 PySide6（LGPL-3.0 许可）作为 Qt 绑定库，
> LGPL 与 AGPL 兼容。
>
> **中文版许可优先。如中英文版本存在歧义，以中文版本为准。**
>
> 版权所有 © SilentStudio
> 开发所属：SilentStudio → SilentCodeTeams → Silent X Craft Launcher Dev

---

**Silent X Craft Launcher** — 让启动更快，让游戏更静。
