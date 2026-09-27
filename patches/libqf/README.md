# libqf 补丁:滚动条淡出动画的悬空指针(真机崩了两次的根)

**目标文件**:`D:\SilentStudio\PyQf to C\src\fluent\fluent_scroll.cpp` 的 `ScrollBar::fadeTo`

**为什么放在我们仓库里**:那个目录**不是 git 仓库**(`git -C "D:\SilentStudio\PyQf to C" rev-parse`
报 not a git repository),补丁只躺在工作区里,重新拉一份 libqf 就丢了。这里留一份**可对照的原文**,
外加可复现的验收命令。

## 现象(崩溃取证给的,符号栈直接指到源码行)

| 时间 | 出错指令 | 对应源码 |
| --- | --- | --- |
| 2026-09-23 07:41:40 | `QAbstractAnimation::stop+0x4` | `m_fadeAni->stop()` |
| 2026-09-23 08:07:38 | `QObject::deleteLater+0x22` | `m_fadeAni->deleteLater()` |

两次报告都在 `%APPDATA%\SilentXCraftLauncher\logs\crashes\` 里(带 .dmp)。

## 根因

淡出动画自己把自己删了,而成员指针从没清过:

```cpp
    connect(ani, &QVariantAnimation::finished, ani, &QObject::deleteLater);  // 自杀
    m_fadeAni = ani;                                                        // 指针留着 -> 悬空
```

于是**下一次**鼠标划过/离开滚动条(或那个 200ms 的延迟回调)再调 `fadeTo()`,
就是在已释放的对象上 `stop()` / `deleteLater()` —— 用户看到的"点着点着窗口直接没了"。

## 修法(已应用)

```diff
 void ScrollBar::fadeTo(double target) {
     if (m_fadeAni) {
         m_fadeAni->stop();
         m_fadeAni->deleteLater();
+        m_fadeAni = nullptr;                 // 删旧动画立刻清指针
     }
     auto *ani = new QVariantAnimation(this);
     ...
-    connect(ani, &QVariantAnimation::finished, ani, &QObject::deleteLater);
+    connect(ani, &QVariantAnimation::finished, this, [this, ani]() {
+        if (m_fadeAni == ani) m_fadeAni = nullptr;   // 自杀前先清成员
+        ani->deleteLater();
+    });
+    connect(ani, &QObject::destroyed, this, [this, ani]() {   // 兜底:被别的路径删掉也清
+        if (m_fadeAni == ani) m_fadeAni = nullptr;
+    });
     m_fadeAni = ani;
     ani->start();
 }
```

## 验收(可复现,不靠手点鼠标)

给可见滚动条发 N 次 Enter/Leave(leave 就会 fadeTo),每 200ms 一次 ——
足够让上一个动画跑完并被 deleteLater 掉:

```powershell
$env:SXCL_UI_ROUTE='settings'; $env:SXCL_UI_SCROLLBAR_STRESS='30'
& 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build-ui\src\ui\Release\sxcl-ui.exe'
```

* **修之前**:第 12 次访问违例(`exit -1073741819`),logs/crashes 里多一份报告;
* **修之后**:30 次全部走完,打 `[sxcl-ui] STRESS: 30 次 Enter/Leave 之后**还活着**`,
  `rc=0`,不再产生崩溃报告。


---

# 补丁 2:顶部通知条做成**长条**(用户 2026-09-23)

**目标文件**:`D:\SilentStudio\PyQf to C\src\fluent\fluent_controls.cpp` 的 `InfoBar::push`

用户原话:「弹窗能不能做长条,就是从顶部下来的那个」—— 原来宽度被卡在 560 以内,
屏幕上就是中间/右上角一个小方块(真机日志 `ib: shown ... size=264x92`)。

```diff
-    // 浮层宽度:内容自适应,上限取页面可用宽与 560(内容自动换行,高度随之)
-    const int pageW = host->width();
-    const int cap = (pageW > 200) ? qMin(560, pageW - 48) : 560;
-    bar->setMaximumWidth(cap);
+    // 长条:铺满宿主宽度(左右各留 24) —— 管理器按 bar 实际宽度算 x,条一变宽自然横贯顶部
+    const int pageW = host->width();
+    const int stripW = (pageW > 200) ? (pageW - 48) : pageW;
+    bar->setFixedWidth(stripW);
```

**验收**:`SXCL_UI_ROUTE=versions` + `SXCL_UI_SHOT` -> 日志里
`ib: shown at ... size=1052x92`(窗口 1100,左右各留 24),截图 `build/shots/infobar_strip.png`。


---

# 补丁 3:下拉弹层宽度 = 宿主窗口宽度(用户 2026-09-27)

**补丁原文**:`0003-combo-popup-width.patch`(同一目录,5 个文件 / 13 个 hunk,可 `git apply -p1` 重放)

用户原话:「还有那个下拉菜单儿,以后你就给我他妈显示完整个窗口的宽度,放不下再换行,能听懂吗?
天天整那个长方形,谁看呀?」

## 改之前是什么样(私有构建实测,窗口 1100x750,深色)

| 弹层 | 面板宽(逻辑像素) | 占窗口 |
| --- | --- | --- |
| 设置页 · 主题模式 | 108 | 9.8% |
| 设置页 · 版本列表刷新频率 | 95 | 8.6% |
| 设置页 · Java 运行路径 | 647 | 58.8% |
| 下载页 · Minecraft 版本分类 | 94 | 8.5% |

旧算法有两套、还各说各话:qf 原版是**宽度 = 下拉框自身宽度**(combo_box.py:325-327);
后来在 `ComboBox::showPopup` 里补过一条**宽度 = 最宽条目 + 52**,所以只有 Java 那一条长得像样
(也仍然只有窗口的 6 成)。用户看到的"长方形"就是前一套。

## 改法(一处实现,全仓所有下拉共用)

规矩落在控件层 `ComboBoxMenu::applyHostWidthPolicy()`:`popup()` / `exec()` 内部**自动**套用,
页面代码一个字都不用写。

1. **铺满宿主窗口**的可用宽度(两侧各留 12);
2. 条目文字放不下就**换行** —— 用 `QTextLayout`(与绘制同一条排版路径)量行数,条目 `sizeHint`
   的高跟着长,不再靠省略号/硬裁;
3. 只有"一整段不可断开的文字"(比如一条没有空格的 Java 路径)比窗口还宽时,才允许弹层比窗口
   略宽、把这一整段装下 —— 上限仍是光标所在屏可用宽度 - 16;
4. 连屏幕都装不下才省略(验收 trace 里 `fit=0`,不会静默发生)。

弹层的 x 也从"以下拉框为圆心"改成**面板左缘 = 窗口左缘 + 12**(横贯窗口)。

| 文件 | 改动 |
| --- | --- |
| `src/fluent/fluent_menu.h/.cpp` | 新增 `applyHostWidthPolicy()` / `itemTextWidth()` / `reflowItemForPolicy()`;`popup()`、`exec()`、`addItem()` 接线;x 对齐窗口 |
| `src/fluent/fluent_setting_cards.cpp` | `qf::ComboBox::showPopup()` 里那段"最宽条目 + 52"的局部算法删掉,改调同一条规矩;`view()` 也铺同一套(不省略 / 不横向压缩 / 换行) |
| `src/fluent/fluent_input.h/.cpp` | 覆盖 `libqf::LineEdit::setCompleter()`:补全弹层(模组页"版本"输入框)走同一条规矩 |

## 验收(可复现,不靠"看着像")

`tools/ui_combo_width.ps1`(私有构建 `D:/SilentStudio/_build/combow`;build-ui 不碰)。
它按 `SXCL_UI_POPUP_KIND` + `SXCL_UI_POPUP=text:<条目子串>` 打开每个弹层,读
`[sxcl-ui] popup-trace / popup-item` 读数,并**逐像素量截图**(面板 bbox、墨迹 bbox、每条文字行)
交叉验证:

| 页面 · 弹层 | 窗口宽 | 面板宽(trace / 截图) | 占比 | 最宽条目 |
| --- | --- | --- | --- | --- |
| 设置页 · 主题模式 | 1100 | 1076 / 1074.7 | **97.8%** | "跟随系统" 56 ≤ 文字区 1042 |
| 设置页 · 版本列表刷新频率 | 1100 | 1076 / 1074.7 | **97.8%** | "10分钟" 43 ≤ 1042 |
| 设置页 · Java 运行路径(长条目) | 1100 | 1076 / 1074.7 | **97.8%** | Java 路径 595 ≤ 1042,截图里画出来 **594.0**(没截断) |
| 下载页 · Minecraft 版本分类 | 1100 | 1076 / 1074.7 | **97.8%** | "正式版" 42 ≤ 1042 |
| 下载页 · MOD 版本筛选(补全弹层) | 1100 | 1076 / 1068.0 | **97.8%** | "1.21.4" 34 ≤ 文字区 1066 |

* 长条目断言:Java 路径那条 `textw=595`,弹层文字区 `1042`,截图里最宽的一行墨迹 **594.0** 逻辑像素
  —— 与文字自然宽差 1px(`tt` 的右侧留白),即**整条都画出来了**;墨迹右缘距面板右缘 460.7px。
* 每条断言 `fit=1`(一行装得下,或换行后每一行都画得进自己的行高)。
