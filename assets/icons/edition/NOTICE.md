# assets/icons/edition/* —— 下载页"版本形态"那两枚图标

下载页侧2 页脚是 [Java 咖啡杯] | [基岩版 LOGO] 两个可点图标(中间一条 1 设备像素竖线,见
`src/ui/pages/download_page.cpp` 的 EditionPickRow)。两枚都是**原版/官方素材**,
没有一枚是自绘替代品;逐枚出处如下(将来谁问都能查到从哪来)。

## java_logo.svg —— Java 版(咖啡杯)

- 字节数 1540,sha256 `7582E518A9C02425F97155E5A3BD39D1A3A7D421B78CAF9C8DF7443DAD3EDC5D`。
- 来源(2026-09-26 取用;用本仓库自己的下载器 `build/Release/sxcl-dl.exe get <url> <dest>` 实测):
  * `https://raw.githubusercontent.com/devicons/devicon/master/icons/java/java-original.svg`
  * `https://unpkg.com/devicon@latest/icons/java/java-original.svg`
  这两条 URL **都**在本机下载成功,且下载到的字节与仓库里这份 **sha256 完全相同**(实测,不是推断)。
- 内容:128x128 viewBox 的矢量咖啡杯,填充色 `#0074BD`(Oracle Java 蓝)。
- 授权/商标:devicon 仓库整体为 MIT(逐枚图标在它自己的 NOTICE 里标来源);
  **Java 与咖啡杯图形是 Oracle 的商标**,本目录只作为"这是 Java 版"的指认标识,
  不宣称任何关联、不用于再分发。
- 同批还有一枚带 "Java" 字样的 `java_cup2.svg`,**故意没有收进来**:界面上不出现文字
  (docs/27 §11.5 文字纪律;用户口径是"图标点选",不是"图标 + 字样")。

## bedrock_logo.png —— 基岩版(官方 MINECRAFT 标题 LOGO)【界面用的就是这一枚】

- 字节数 86796,1937x333,PNG / Format32bppArgb(RGBA,透明底边),
  sha256 `1AB368C3719A0FA0C273A0040BD5D3E8C47A9678B8DFF22A09AA1BF570781662` ——
  与下面 APK 里那个条目**逐字节相同**(复制,未做任何像素改动、未缩放、未裁剪)。
- 来源(2026-09-26 取用):用户本机的基岩版安装包
  `D:\SilentStudio\AdbGUI\APK\Minecraft_1.26.40.5.apk` 里的条目
  `assets/assets/resource_packs/vanilla/textures/ui/title.png`
  (1937x333,86796 字节,同一个 sha256)。
- 实测内部结构(用来定界面尺寸的,不是估的):
  * 不透明像素(A>0)**602849 / 645021 = 93.5%**;不透明且偏亮的"字母墨迹"(均值>100)
    **286915** 个,其包围盒 **(19,17)..(1916,289) = 1898x273**;
  * 字母墨迹高 = 画布高的 **82.0%**;画布横纵比 1937/333 = **5.82:1**;字母包围盒横纵比 6.95:1。
  * 界面里按"字母墨迹与咖啡杯墨迹同高(20 逻辑像素)"摆:绘制盒高给 **24 逻辑像素**,
    宽由控件按横纵比算成 **140 逻辑像素**(24 x 5.8168),字母墨迹落到 24 x 0.82 = **19.7 逻辑像素**。
- 授权/商标:Minecraft 的图形素材归 Mojang/Microsoft,**随 Minecraft Bedrock 客户端分发**;
  本目录只作为"这是基岩版"的指认标识,不宣称任何关联、不用于再分发
  (与 `assets/icons/bedrock/NOTICE.md` 同一条纪律)。

## bedrock_block.png —— 基岩方块(**备用文件,界面里不用**)

- 字节数 683,64x64,PNG / Format32bppArgb,
  sha256 `B8E4CB0BCB140810477DFF00A312BB4F56A3C2AB594D5C2C103E95BA80738D92`。
- 由同一 APK 里的 `assets/assets/resource_packs/vanilla/textures/blocks/bedrock.png`
  (16x16,278 字节,sha256 `20CED86BA8CB89E29E2115F76C758278893E145575A73CA311BCC4305C140D04`)
  按 **4 倍最近邻**放大而成(像素原样、未重绘)。
- 留它的原因:2026-09-26 用户先要过"基岩方块",随后明确改成"扒 APK 里的官方 LOGO",
  所以方块这枚只作为备用素材留在目录里(界面不引用),要换回去改一行 `setIconFile` 即可。
- 为什么不从网上拿基岩素材:同日实测这台机器上 minecraft.wiki 返回 403、Fandom 连接拿不到响应、
  upload.wikimedia.org 超时、jsdelivr 连接失败 —— 可达的只有 raw.githubusercontent.com 与
  unpkg.com;而 APK 里这两份就是 Mojang 官方素材本身,出处比任何镜像都硬。
