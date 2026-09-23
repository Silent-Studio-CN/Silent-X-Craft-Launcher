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

