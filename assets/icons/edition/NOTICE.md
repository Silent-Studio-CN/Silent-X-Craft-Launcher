# assets/icons/edition/* —— 下载页"版本形态"那两枚图标

下载页侧2 页脚是 [Java 咖啡杯] | [基岩方块] 两个可点图标(中间一条 1 设备像素竖线,见
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

## bedrock_logo.png —— 基岩版(基岩方块)

- 字节数 683,64x64,PNG / Format32bppArgb;由 16x16 原图按 **4 倍最近邻**放大 —— 与
  `assets/icons/blocks/` 那批方块图同一个放大口径(整数倍 = 最近邻,像素不糊)。
- 来源(2026-09-26 取用):用户本机的基岩版安装包
  `D:\SilentStudio\AdbGUI\APK\Minecraft_1.26.40.5.apk` 里的条目
  `assets/assets/resource_packs/vanilla/textures/blocks/bedrock.png`
  (16x16,278 字节,sha256 `20CED86BA8CB89E29E2115F76C758278893E145575A73CA311BCC4305C140D04`);
  **像素原样、未做任何重绘**,只做了整数倍放大。
- 为什么不从网上拿:同日实测这台机器上 minecraft.wiki 返回 403、Fandom 连接拿不到响应、
  upload.wikimedia.org 超时、jsdelivr 连接失败 —— 可达的只有 raw.githubusercontent.com 与
  unpkg.com;而 APK 里这份就是 Mojang 官方素材本身(下载页原来那枚基岩版 title.png 也是从
  同一个 APK 里取的,见 `assets/icons/bedrock/NOTICE.md`),出处比任何镜像都硬。
- 授权/商标:Minecraft 的图形素材归 Mojang/Microsoft;本目录只作为"这是基岩版"的指认标识,
  不宣称任何关联、不用于再分发(与 `assets/icons/blocks/NOTICE.md` 同一条纪律)。
