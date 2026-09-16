# 第三方图标出处与授权说明

## assets/icons/blocks 里的方块图标

这些 PNG 来自 PCL2（Plain Craft Launcher 2）仓库的 Plain Craft Launcher 2/Images/Blocks/
（Anvil / CobbleStone / CommandBlock / Egg / Fabric / GoldBlock / Grass / GrassPath /
NeoForge / OptiFabric / RedstoneBlock / RedstoneLampOff / RedstoneLampOn，原图 64x64 或 48x48）。

依据 PCL 的《PCL 存储库合理使用指南》，这属于它定义的"轻度使用"（参考存储库中极小部分的内容），
我们履行对应的两项义务：

1. **署名**：在此声明这些图标来自 PCL2 项目。
2. **不混淆**：本项目的名称、图标与界面风格均不以 PCL / Plain Craft Launcher 开头，
   也不自称"PCL 手机版"等任何会造成混淆或暗示与 PCL 有关的名称。
   SXCL 与 PCL 是两个独立项目，彼此没有隶属、赞助或背书关系。

## 需要知道的进一步事实（这部分 PCL 无权授权）

这些图标并非 PCL 原创，PCL 的许可无法授予它们的权利：

* 草方块 / 铁砧 / 圆石 / 命令方块 / 金块 / 红石块 / 红石灯 / 土径 / 鸡蛋 ——
  是 Mojang 的 Minecraft 素材（PCL 自己的 Resources/Custom.xaml 也把它们统称为
  "Minecraft 方块与物品图片"），受 Minecraft 使用指南约束；
* 布料（Fabric）是 FabricMC 项目的标识，狐狸（NeoForge）是 NeoForged 项目的标识，
  OptiFabric 同理 —— 各自受其项目商标政策约束。

因此本项目对它们的使用仅限"在启动器界面里标示版本/加载器类型"这一用途，
不得用于暗示与 Mojang / FabricMC / NeoForged 存在官方关联。

## 其它图标

导航栏与其余界面图标由本项目用 QPainter 现画（见 src/app/icons.py），不依赖外部图片资源。
