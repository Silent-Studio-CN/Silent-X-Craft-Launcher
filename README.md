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
| 1 下载引擎 | **完成**：工作线程池 + 优先级队列 + `.part` 续传（含残片/416 自愈）+ 双路换源 + 全局限速 + 强校验 + 哈希缓存（多连接分片尚未实现） |
| 2 双路线路与熔断 | 未开始 |
| 3 元数据层 | **完成**：版本清单、`rules` 平台过滤、`natives` 分类器、资源对象展开（5,147 个按哈希去重） |
| 4 安装层（Forge/NeoForge/OptiFine 静默安装） | 未开始 |
| 5 启动层 | 未开始 |
| 6 UI（Qt6 + libqf） | 未开始 |
| 7 基岩与联机（Rust） | 未开始 |
| 8 打包（Inno / DMG / AppImage / APK） | 未开始 |

## 已验收的行为（真跑，不是"应该能行"）

| 用例 | 证据 |
| --- | --- |
| 正常下载 + 限速 | 276,542 B 的版本清单，`--rate 400kb` 下 0.75s 完成，退出码 0 |
| 摘要故意写错 | 退出码 1，**不留正式文件、`.part` 也删除**（坏文件不会冒充成功） |
| 首路 404 → 次路 | 自动换到第二候选并成功，退出码 0 |
| **断点续传** | 限速 50KB/s 下到 52,035 B 时强杀进程；重跑续传完成，产物 SHA-256 与完整下载**逐字节一致** |
| Range 兑现检查 | 实测 Mojang CDN 在带 `Accept-Encoding: gzip` 时**放弃 Range**（返回 200 全量）。传输层因此对 Range 请求强制 `identity`，引擎另有一层"服务端没兑现就全量重下"的保护 |

## 端到端验收：拉全一个真实的游戏目录

`sxcl-dl version latest <目录>` 一条命令产出**可启动级别**的完整游戏目录，全部按 Mojang 官方哈希强校验：

| 项 | 实测 |
| --- | --- |
| 资源对象（assets/objects） | **5,147 个 / 461 MB**，逐个重算 SHA-1 与文件名比对 → **0 不符** |
| 客户端 jar | 41,483,720 B，与 `downloads.client.sha1` 逐字节一致 |
| 资源索引 | 与 `assetIndex.sha1` 一致 |
| 依赖库 | 114 条按规则筛出 74 个，**74/74 哈希通过** |
| 游戏目录总体积 | **586.8 MB** |
| 重复运行（哈希缓存生效） | 5,225 个任务全部命中，**0 下载 0 失败，墙钟 3.36 秒** |

## 四平台编译验证（CI 全绿）

| 平台 | 作业 | 状态 |
| --- | --- | --- |
| Windows | `windows-msvc`（VS2026，不硬写生成器） | 通过 |
| Linux | `linux-gcc`（gcc + Ninja） | 通过 |
| macOS | `macos-universal`（`arm64;x86_64` 通用二进制） | 通过 |
| Android | `android-arm64`（NDK 交叉编译） | 通过 |
| Qt 传输后端 | `qt-transport-linux`（install-qt-action） | 通过 |

**CI 一次就抓出 5 个本机 MSVC 完全掩盖的真实缺陷**（值得记住，别删）：

1. `include/sxcl/fs.h` 用了 `size_t` 却没 include `<stddef.h>`（本机因为包含顺序恰好先引入了 stdio.h 而侥幸通过）。
2. `src/platform/platform_posix.c` 缺 `<fcntl.h>`：`open`/`O_WRONLY`/`O_CREAT` 未声明。
3. macOS 的 `struct stat` 与 Linux 不同：顶部定义 `_POSIX_C_SOURCE` 会让 macOS SDK 把 `st_mtim` 与 `st_mtimespec` **一起收窄掉**，去掉该宏即可（CMake 用 `-std=gnu11`，不需要它）。
4. Android 的 pthread 在 libc 里，`find_package(Threads REQUIRED)` 必然失败，要单独分支。
5. 不带 Qt 构建时，CLI 里 `#else` 分支的 `return` 让后续代码变成 unreachable，MSVC `C4702` 在 `/WX` 下直接打挂（本机装了 Qt，走的是另一个分支，看不出来）。

另有一条是测试自身的跨平台缺陷：元数据层测试的"期望总字节"写死成 windows/linux 两种，漏了 macOS 的 `natives-osx` 大小不同，现已改成按平台分别计算。

## 许可

AGPL-3.0 + SXCL 附加条款，与 Python 版一致（`LICENSE` 从 Python 版复制，勿改动条款）。
PCL 素材的署名与不混淆规则见 Python 版 `assets/icons/NOTICE.md`；基岩 APK 不参与分发。
