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

- **原始素材(APK 里那一份,未做任何处理)**:
  * 来源(2026-09-26 取用):用户本机的基岩版安装包
    `D:\SilentStudio\AdbGUI\APK\Minecraft_1.26.40.5.apk` 里的条目
    `assets/assets/resource_packs/vanilla/textures/ui/title.png`;
  * 尺寸 1937x333,PNG/Format32bppArgb,86796 字节,
    sha256 `1AB368C3719A0FA0C273A0040BD5D3E8C47A9678B8DFF22A09AA1BF570781662`;
  * 实测:不透明(A=255)**92.9%**、非透明(A>0)**93.46%**、全透明(A=0)**6.54%** ——
    这枚官方 LOGO **自带一块黑色底板**(深色卡片上就是"一块更黑的方板 + 浅色字母")。
- **本仓库这一份 = 上一份"仅裁剪画布"**:
  * 裁到**字母墨迹包围盒** `(19,17)..(1916,289)` -> 尺寸 **1898x273**,151064 字节,
    sha256 `C9102616002FEEA5890406F9BB2054D3E2DCED3CB5C8104AA954D28A1B7D3FC7`;
  * **只裁画布:像素逐字节未改**,没有抠底、没有改色、没有缩放 ——
    实测裁剪结果与原图对应区域逐像素比对 **0 处不同**(1898x273 = 518154 个像素全等);
    裁完字母墨迹包围盒 = 整张画布 (0,0)..(1897,272),即"画布高 = 字母墨迹高";
  * 裁完不透明像素 501853 / 518154 = **96.85%**;
  * 裁的理由:原画布上下左右的黑边占高度 18%,按"外框等高"摆会让字母比咖啡杯小一圈;
    裁完两边**墨迹高度都是 20 逻辑像素**(验收脚本按墨迹高度断言,误差 <= 1 逻辑像素)。
- 界面里的用法:绘制盒高 20 逻辑像素(控件按素材横纵比算宽:1898/273 x 20 -> **139 逻辑像素**),
  贴 dpr 渲染;选中态是图标下方那条 2 逻辑像素的 accent 指示条(不是方框)。
- 授权/商标:Minecraft 的图形素材归 Mojang/Microsoft,**随 Minecraft Bedrock 客户端分发**;
  本目录只作为"这是基岩版"的指认标识,不宣称任何关联、不用于再分发
  (与 `assets/icons/bedrock/NOTICE.md` 同一条纪律)。
- 同 APK 里的另外两枚标题图(本次**没有**采用,仅记录实测,免得以后重复找):
  `resource_packs/beta/textures/ui/title.png` 1936x406,不透明 79.0% / 全透明 20.3%;
  `textures/ui/minecraft_marketplace_title.png` 1436x548,不透明 36.3% / 全透明 63.3%
  (透明底最干净,但它是"市场"标题,不是基岩版标题)。

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
