# 06 · libqf 对齐清单（SXCL 1:1 过程中实测出的库层偏差）

> libqf = `D:\SilentStudio\PyQf to C`（我们自有项目，可改）。
> 本清单只记**有实测证据**的偏差：现象 → qf 依据 → libqf 现状 → 修法 → 影响面。

## 1. PushButton / PrimaryPushButton 高度 30，qf 是 32
- 证据：页面代理实测按钮 30 高、"保存为我的布局" 110 宽；qf `button.py:18,96` 的 PushButton 为 32 高 / sizeHint 124。
- 影响：keymap 页按钮行 ink 209-708 vs 设计 224-785；画布高度 429 vs 425。

## 2. ComboBox 宽度 150，qf 是 156
- 证据：keymap toolbar 预设框窄 6px，连带搜索框宽 8px。
- 依据：qf `components/widgets/combo_box.py` 的 `sizeHint/minimumSizeHint`。

## 3. ScrollArea 缺 qf 的 1px frame
- libqf `fluent_scroll.cpp:205 setFrameShape(NoFrame)`；Python 的 QScrollArea 保留默认 1px StyledPanel。
- 证据：Python 内容 QWidget 在 (50,50) 1049x699、ScrollArea (49,49) 1051x701；C 版两者都是 (49,49) → 整页偏 1px。
- 量化：multiplayer 按 (dx=2,dy=1) 对齐后 DIFF 6.29% → 4.57%（能捞回 1.7 个点）；
  噪声地板（设计图自比平移 1px）= 5.0%(keymap) / 5.8%(multiplayer)。

## 4. 按钮四态与画法需逐条对齐 qf
- 依据 qf `button.py:96` 的 `paintEvent`（含 disabled）。质感原则见 `D:\JAVA-Qt\QraftLab-v0.5.1\QSS_ART.md`
  （立体边三要素、pressed 底边压平 + 下沉 1px、四态是底线）；**与 qf 冲突时以 qf 为准**。

## 5. 【影响最大】ScrollArea 滚动条必须是覆盖式浮层，不能占视口
- 现状：`ScrollArea::initArea()` 用 `setVerticalScrollBar()/setHorizontalScrollBar()` → 视口窄 12px。
- qf：`components/widgets/scroll_area.py` 的 SmoothScrollDelegate 用**裸子控件**覆盖式滚动条，
  `_adjustPos` 里 `resize(12, h-2)` + `move(w-13, 1)`。
- 实测代价（主页，1100x750）：滚动条把手列缺失 3679px + 7 个"启动"按钮左移 12px 计 14238px +
  按钮文字 4431px + 其它按钮文字 2900px ≈ **1.45%（主页全部剩余差异）**；改完预计降到 ~0.2%。
- 影响面：6 个页面全走 ScrollArea 壳。

## 6. CardWidget 描边顺序与 qf 不同
- qf：先描边（只上下两条路径）、后被 1px 内缩的填充盖住。
- libqf：先填充、再用 `adjusted(0.5,0.5,-0.5,-0.5)` 画整圈 → 卡片四周多一条 1px 深线
  （左缘实测 #1d1d1d，参考图同位置 #232323）。色差 6 级不进 DIFF，但每页每张卡都有。

## 7. 页面层临时兜底（libqf 修好后应删除）
| 兜底 | 位置 | 对应 libqf 项 |
|---|---|---|
| `applyButtonFont()` 钉 14px/高 32 | home_page.cpp | 第 1 项 |
| `setFrameShape(StyledPanel)+setLineWidth(1)` | home_page.cpp | 第 3 项 |
| QListWidget 就地下发 global_qss 规则 | keymap_page.cpp | 第 4 项(实为核心层缺 API) |

## 8.（可选，本工程已绕开）命中区/阴影带没有库级开关

- libqf `fluent_window.cpp:150-159`：`border = compositingAvailable() ? 30 : kResizeBorder(7)`，
  假定窗口四周有 30px 阴影带（`:151-153` 注释自述，数值来自 `fluent_window.h:221 kWinMargin=30`）。
- qf 的真实值：`qframelesswindow/windows/__init__.py:22 BORDER_WIDTH = 5`（物理像素；最大化时 0；
  判定用 `ScreenToClient + GetClientRect`，角优先）。
- **本工程已在 SXCL 侧绕开**：`MainWindow::nativeEvent` 覆写 + `kResizeBandPx = 5`，带外不回退基类
  （否则基类 30px 带会命中）。实测 WM_NCHITTEST：x=2/4 → HTLEFT、x=5 起 → HTCLIENT、
  汉堡键 (26,90) 与导航左缘 (10,300) 恢复 HTCLIENT、四角正确。
- 库级方案（`FluentWindowBase::setResizeBorder(int)`，默认保持现状，消费方传 5）**不是必须的**，
  仅对其它消费方有价值；若要做，归入本清单第 8 项。

## 9. 已落地的 4 项（旧 libqf 代理交付，2026-xx 复核通过）

| 项 | qf 依据 | 改法 | 实测 |
|---|---|---|---|
| 第 1 项 按钮度量 | `button.py:36 setFont(self)` → `common/font.py:45-60 getFont(14)`；qf 两个按钮类**没有 sizeHint**，尺寸 = button.qss 盒(padding 5/6 + 1px 边框) + 字体度量：高=`fm.height()+13`、宽=文本宽+26 | `PushButton::init()` 加 `setFont(fluent::appFont(14))`；`ToolButton` 补 `setIconSize(16,16)` | keymap 主色按钮包围盒与参考图**完全一致**（124x32 逻辑 @79.33,694） |
| 第 2 项 ComboBox | `combo_box.py:370-382`（`ComboBox(QPushButton, ComboBoxBase)`，无 sizeHint）+ `combo_box.qss:1-11` padding 5/31/6/11 → **宽 = 文本宽 + 44**、高 = `fm.height()+13` | 删掉写死的 `setMinimumWidth(140)`，加 `setFont(appFont(14))` 与 `sizeHint/minimumSizeHint` | 「极简（推荐新手）」**156x32**、「横屏」**72x32**，与 qf 逐值相等（测试断言） |
| 第 3 项 ScrollArea frame | `scroll_area.py` 三个类**一句 setFrameShape 都没有**；透明是显式 API `enableTransparentBackground()` | 删掉 `setFrameShape(NoFrame)` 与硬塞的 `border:none`，补同名 API | frameWidth=1、frameShape=6、内容内缩 1px；按钮随之与参考图逐像素对齐 |
| 第 4 项 四态 | libqf vendored 的 `button.qss` 与 qf **逐字节相同**（md5 dark BE54EFBC / light 577ECE1B），缺的只是 14px 字体 | 补字体即可 | 四态色值与 qf 全等：normal #2d2d2d / hover #323232 / pressed #272727 / disabled #292929 |

**反证（第 3 项）**：参考图上那条 1px 线部分是**截图脚本**造成的 ——
`tools/grab_reference_ui.py` 里 `page.setStyleSheet("QWidget { background: %s }" % token("bg"))`（widget 级）。
qf 5 组实验：应用级 `QScrollArea{background:transparent}` → 不画线；再加 widget 级 `QWidget{background:...}` → 画线；
widget 级写 `QScrollArea{background:...}` → 又不画。libqf 各情形与 qf 表现一致 → 不改。

## 10. 已登记偏差（不静默）

**深色下 PRIMARY 强调色不做 HSV 变换**：qf `style_sheet.py:449-509` 对 `--ThemeColorPrimary` 也做
`s*=0.84, v=1`；libqf 对 PRIMARY 用原始值（Light1/Light2/Dark1/Dark2 都换）。
- 固定验收环境 `SXCL_UI_ACCENT='#ff75df'` 下，现状与参考图主色底**完全一致**；若改成 qf 换算会变 `#ff8be4`，把 DIFF 推高。
- 反推：参考图原始强调色 ≈ **`#ff5ad9`**（其深色 PRIMARY 变换后正好 = `#ff75df`）。
- 因此在 C 版里 `SXCL_UI_ACCENT` 是**变换后的有效色**，而 Python 的 `qconfig.themeColor` 是**原始色**。
  ⚠️ 设置页实现强调色选择器时要注意这个语义差（要么改 libqf 的 PRIMARY 变换、要么在设置层做逆变换）。
- 已在 libqf 黄金文件 KNOWN_DEVIATIONS 登记，测试以「[登记偏差]」打印。

## 11. 构建坑：改 libqf 头文件后陈旧 obj 不刷新

现象：`build-libqf/_libqf/fluent.dir` 里留有陈旧 obj，MSBuild 不重编 → `fluent_material.obj: LNK2001 ComboBox::sizeHint`。
处置：**删掉该目录重新 build**（不是代码问题）。

## 12. 两个待收口的项（新 libqf 代理在做）

### 12.1 第 5 项（覆盖式滚动条）—— **旧代理从未实施**
- 事实核实：`fluent_scroll.cpp` 停在 18:18，`initArea()` 仍是 `setVerticalScrollBar(new SmoothScrollBar(...))`（占视口装法），
  全文搜 `attachToArea|overlay|adjustPos` 零命中 → 只有第 3 项（去 NoFrame）落地。
- 代价仍在：主页 1.45% 里的 3679px（把手列缺失）+ 14238px（7 个"启动"按钮左移 12px）+ 4431px（按钮文字）+ 2900px（其它按钮文字）。
- Python 真机取证（可直接当验收判据）：
  ```
  home ScrollArea: frameShape 6 / frameWidth 1 / viewport (1,1) 1049x699
  竖直覆盖式滚动条(裸子控件): geom 1038 1 12 699   ← 父宽 1051 - 13, y=1
  groove 0,0 12x699 ; handle 6 14 3 585
  横向: geom 1 688 1049 12
  手柄高 = int((699-28) * 701 / (102+701)) = 585  ← 对应 qf _adjustHandleSize(最小 30)
  ```
  与 qf `scroll_area.py` 的 `_adjustPos`(resize(12,h-2)+move(w-13,1)) 完全吻合。

### 12.2 PRIMARY 强调色变换（§10 的修正）
- 根因确认：libqf `fluent_style.cpp:57` 对 PRIMARY **原样返回** `m_themeColor`，而 qf `style_sheet.py:463-504`
  暗色要 `s*=0.84; v=1`。真机实测 qf 暗色 accent `#ff75df` → PRIMARY `#ff8be4`、LIGHT_1 `#ff94e6`、
  LIGHT_2 `#ffa5ea`、DARK_1 `#e67dcd`、DARK_2 `#d174bc`；libqf 主色按钮填充仍是 `#ff75df`（5 个采样点全错）。
- 改法：PRIMARY 也过 `derive()`；新增 `FluentStyle::accent()` 保留原始值。
- ⚠️ **验收口径随之改变**：参考图主色底 `#ff75df` = qf 对**原始色 `#ff5ad9`** 变换后的结果，
  所以修正后抓图要用 `SXCL_UI_ACCENT='#ff5ad9'`；若仍传 `#ff75df`，PRIMARY 会渲染成 `#ff8be4`
  —— 那是**正确行为**，不是回归。
