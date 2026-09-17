# 图标来源与许可

## assets/icons/blocks/*.png（13 个方块图）

- 来源：PCL（Plain Craft Launcher 2）的方块图标，经 Python 版 SXCL \
  （\`D:\\SilentStudio\\prog\\Silent-X-Craft-Launcher\\assets\\icons\\blocks\`）原样带入 C 版。
- 依据：PCL《分发有限许可》允许"极小部分内容 = 轻度使用"，硬性义务为**署名**与**不得暗示与 PCL 有关**。
  本目录仅用于版本状态标识，不宣称任何关联。
- 注意：草方块/铁砧/鸡蛋/圆石/命令方块等图形素材本身属于 Mojang 的 Minecraft 素材，
  布料为 FabricMC、狐狸为 NeoForged 的标识；上游 Python 版已作此处理，C 版保持一致。
- 对应关系（借自 PCL \`ModMinecraft.vb:785-809\`）：
  原版=草方块 / 快照=命令方块 / 旧版=圆石 / Forge=铁砧 / Fabric=布料 /
  NeoForge=狐狸 / OptiFine=土径 / LiteLoader=鸡蛋 / 出错=红石块。

## assets/theme/qf_exact/images/icons/*.svg（348 个）

- 来源：qfluentwidgets（PySide6-Fluent-Widgets，GPLv3）的 Qt 资源。
- 提取方式：\`tools/extract_qf_images.py\`（从安装包 \`_rc/resource.py\` 注册的资源里逐字节导出，
  与 Python 版正在渲染的那份完全一致）。
- 用法：深色主题取 \`*_white.svg\`，浅色主题取 \`*_black.svg\`（对应 qf \`FluentIconBase.path()\`）。

## assets/theme/qf_exact/{dark,light}/*.qss（68 个）

- 同上来源与提取方式；占位符（\`--FontFamilies\` / \`--ThemeColor*\`）由 \`FluentTheme\` 替换。
