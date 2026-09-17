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
