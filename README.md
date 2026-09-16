# Silent-X-Craft-Launcher C 版（SXCL-C）

SXCL 的 C/C++ 重写工程。本仓库 `main` 是 C 版；Python/PySide6 版保留在 `legacy-python` 分支，
两版功能对等、数据格式互通（共用 `PCL\Setup.ini`、`launcher_profiles.json`、`sxcl.keymap.v1` 等既有契约）。

## 分支与版本

| 分支 / 标签 | 内容 |
| --- | --- |
| `main` | C/C++ 重写版（本文件所述） |
| `legacy-python` | Python/PySide6 实现（重写前的最后一版，随时可取回） |
| `legacy-python-before-c` | 与 `legacy-python` 同一点的标签，防分支误删 |
| `v0.1.0` | Python 版的历史发布标签 |

取回 Python 版：`git switch legacy-python`（或在 GitHub 上切分支查看）。

## 为什么重写

- 启动器冷启动内存与耗时：Python 版实测常驻 >200 MB、启动数秒；C 版目标常驻 <40 MB、冷启动 <300 ms。
- 一份源码产出三端：Windows 安装包 / 便携包 / 安卓 APK（Python 版受 PySide6 无 android wheel 限制）。
- 下载引擎需要精确控制 socket、io_uring/IOCP 与限速节拍，语言层抽象越薄越好。

## 三条技术线

| 线 | 语言 | 内容 | 依赖 |
| --- | --- | --- | --- |
| `libsxcl` 核心 | C11 | 下载引擎（多连接 Range、`.part` 断点续传、令牌桶限速、流式 SHA-1/SHA-256 校验）、双路线路与熔断、优先级调度、设置与实例存档、版本清单解析、Java 运行时探测 | 仅 OS API + zlib |
| `sxcl-ui` 界面 | C++17 / Qt 6 Widgets | 直接消费 `D:\SilentStudio\PyQf to C`（qf 的 C/C++ 重写）作为 Fluent 控件层 | libsxcl、libqf |
| `sxcl-net` 基岩与联机 | Rust（C ABI 导出） | 基岩版本管理、微软 / 离线登录、联机（复用 WebRTC / NetherNet）、协议加解密与解析 | libsxcl |

分层规则与 Python 版一致：**core ← services ← ui**，core 不得反向依赖 UI；跨层只经 C ABI 或稳定头文件。

## 目录

```
include/sxcl/    公共 C 头（唯一对外契约）
src/core/        下载引擎、限速、校验、线路、设置、缓存
src/services/    版本清单、模组加载器静默安装、账户、实例管理
src/ui/          Qt 界面（消费 libqf）
src/platform/    win32 / linux / android 平台封装
crates/          Rust：基岩与联机（C ABI）
docs/            设计与迁移文档
tests/           单元测试与对拍（与 Python 版逐字段比对）
tools/           构建、打包（Inno Setup）、资源导出脚本
```

## 构建

本机（Windows，i5-11320H，无 C/C++ 编译器）不承担编译；C/C++ 编译在编译机 WS2025 上做：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSXCL_BUILD_UI=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

UI 需要 Qt 6.11.2（WS2025 已装，含 4 套安卓套件）与 `libqf`；纯核心库构建可 `-DSXCL_BUILD_UI=OFF`。

## 不变量（改代码前先读）

- **本工程不含 Python**：没有 `.py` 文件、不依赖 Python 运行时、不调用 Python 脚本；
  构建、测试、打包一律 CMake + MSVC + `.bat`/PowerShell。Python 版只作为**语义与数据格式的参考**，
  阅读它可以，但它的代码、脚本、生成流程都不进这个仓库。
- 跨平台代码默认不写平台专有 API；必须写时收敛到 `src/platform/`，并在文件头注明支持的平台。
- 面向用户的行为改动，先在 Python 版确认语义，再在 C 版实现（避免两版分叉），但实现与验证都在 C 侧完成。

## 现状

| 阶段 | 状态 |
| --- | --- |
| 0 骨架与工具链 | **已完成**：MSVC 14.51 编过，`ctest` 全绿（限速器 27 项 / 哈希 79 项 / 校验层 / 传输层 14 项） |
| 1 下载引擎 | 进行中：限速器、校验层、文件系统层、Qt 传输后端已就位；引擎与 `sxcl-dl` CLI 待写 |
| 2 双路线路与熔断 | 未开始 |
| 3 元数据层 | 未开始 |
| 4 安装层（Forge/NeoForge/OptiFine 静默安装） | 未开始 |
| 5 启动层 | 未开始 |
| 6 UI（Qt6 + libqf） | 未开始 |
| 7 基岩与联机（Rust） | 未开始 |
| 8 打包（Inno / DMG / AppImage / APK） | 未开始 |

四平台编译验证见 `docs/02-迁移路线.md` 与 CI 配置。

## 许可

AGPL-3.0 + SXCL 附加条款，与 Python 版一致（`LICENSE` 从 Python 版复制，勿改动条款）。
PCL 素材的署名与不混淆规则见 Python 版 `assets/icons/NOTICE.md`；基岩 APK 不参与分发。
