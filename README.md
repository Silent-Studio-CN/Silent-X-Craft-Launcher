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

### 打包为可执行文件

```bash
pip install pyinstaller
pyinstaller --name "SXCL" --onefile --windowed --add-data "config:config" main.py
```

## 🏗 项目架构

```
src/
├── main.py                 # 应用入口
├── core/                   # 跨平台核心层（纯 Python，无 UI 依赖）
│   ├── platform.py         # 系统检测 / 路径 / 架构抽象
│   ├── constants.py        # 常量与枚举
│   ├── exceptions.py       # 异常体系
│   ├── logger.py           # 日志系统
│   └── mirror.py           # BMCLAPI URL 镜像映射
├── services/               # 业务逻辑层
│   ├── java/               # Java 发现 / 兼容性 / 自动下载
│   ├── minecraft/          # 版本清单 / 启动命令构建
│   ├── mod_loader/         # 模组加载器 API 客户端
│   └── download/           # 多线程分片下载引擎
├── app/                    # UI 层
│   ├── common/             # 基础组件
│   ├── pages/              # 页面：主页 / 版本 / 下载 / 设置 / 启动
│   └── widgets/            # 自定义控件
└── config/                 # 配置文件
```

## ⚙ 配置

所有设置通过图形界面完成：

| 设置项 | 说明 |
|--------|------|
| Java 路径 | 手动选择或自动检测 |
| 最大内存 | 滑块调节，范围 20%–75% 系统内存 |
| 下载源 | Mojang 官方源 / BMCLAPI 镜像源 |
| 版本隔离 | 每个版本独立运行目录 |
| 主题 | 浅色 / 深色 / 跟随系统 |

## 🔗 相关资源

- [BMCLAPI 文档](https://bmclapidoc.bangbang93.com/) — 镜像 API 说明
- [PCL2 源码](https://github.com/Meloong-Git/PCL) — 界面设计参考
- [QFluentWidgets](https://qfluentwidgets.com/) — UI 组件库

## 📄 许可证

本项目采用 **GNU Affero General Public License v3.0 (AGPL-3.0)**。

> 本项目使用 PySide6（LGPL-3.0 许可）作为 Qt 绑定库。
> LGPL 明确允许 LGPL 库被 AGPL 程序使用，二者兼容。
>
> 中文版许可优先。如中英文版本存在歧义，以中文版本为准。

---

**Silent X Craft Launcher** — 让启动更快，让游戏更静。
