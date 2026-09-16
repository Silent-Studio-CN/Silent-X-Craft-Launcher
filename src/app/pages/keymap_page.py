# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""按键映射页 —— 在电脑上编排手机用的虚拟按键（跨端编辑，FCL 只能在手机上改）。

页面结构：
    左侧：手机形状画布，可直接拖动按钮改位置（坐标是 0~1 归一化，适配任何分辨率）
    右侧：冲突检查 + 教学步骤（"这个键在哪"搜索会高亮画布上的控件）
    顶部：预设切换、横竖屏、导入/导出（含 FCL 布局导入）、设为当前布局
"""

from __future__ import annotations

import json
from pathlib import Path

from PySide6.QtCore import QPoint, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QBrush, QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QVBoxLayout,
    QWidget,
)
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    ComboBox,
    InfoBar,
    InfoBarPosition,
    PrimaryPushButton,
    PushButton,
    SearchLineEdit,
    StrongBodyLabel,
)

from src.app.common.base_page import BasePage
from src.app.theme import on_theme_changed, qcolor
from src.core.keymap import (
    DirectionControl,
    KeymapLayout,
    build_guide,
    build_preset,
    ensure_presets,
    guide_to_markdown,
    import_fcl_layout,
    list_layouts,
    load as load_layout_by_key,
    preset_names,
    PRESET_LABELS,
    save as save_layout_by_key,
    set_active,
)
from src.core.logger import log

SCREEN_RATIO = {"landscape": (16, 9), "portrait": (9, 16)}


class KeymapCanvas(QWidget):
    """手机形状的画布：按归一化坐标绘制控件，支持拖动。"""

    changed = Signal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.layout_data: KeymapLayout = build_preset("minimal")
        self.highlight_id = ""
        self._dragging = None
        self._drag_offset = (0.0, 0.0)
        self.setMinimumSize(520, 320)
        self.setMouseTracking(True)
        self._blink = False
        self._blink_timer = QTimer(self)
        self._blink_timer.setInterval(450)
        self._blink_timer.timeout.connect(self._on_blink)
        on_theme_changed(self.update)

    def set_layout(self, layout: KeymapLayout, highlight: str = "") -> None:
        self.layout_data = layout
        self.highlight_id = highlight
        self.update()

    def highlight(self, control_id: str) -> None:
        self.highlight_id = control_id
        self._blink = bool(control_id)      # 立刻亮起，用户点了就有反馈
        if control_id:
            self._blink_timer.start()
        else:
            self._blink_timer.stop()
        self.update()

    def _on_blink(self) -> None:
        self._blink = not self._blink
        self.update()

    # ── 坐标换算 ──────────────────────────────────────────────

    def _phone_rect(self) -> QRectF:
        width_ratio, height_ratio = SCREEN_RATIO.get(self.layout_data.screen, (16, 9))
        margin = 18
        available_w = max(80, self.width() - margin * 2)
        available_h = max(80, self.height() - margin * 2)
        scale = min(available_w / width_ratio, available_h / height_ratio)
        w, h = width_ratio * scale, height_ratio * scale
        return QRectF((self.width() - w) / 2, (self.height() - h) / 2, w, h)

    def _to_screen(self, x: float, y: float, w: float, h: float) -> QRectF:
        phone = self._phone_rect()
        return QRectF(phone.left() + x * phone.width(), phone.top() + y * phone.height(),
                      w * phone.width(), h * phone.height())

    # ── 交互 ─────────────────────────────────────────────────

    def _control_at(self, pos: QPoint):
        for control in self.layout_data.controls:
            if self._to_screen(control.x, control.y, control.w, control.h).contains(pos):
                return control
        return None

    def mousePressEvent(self, event) -> None:
        control = self._control_at(event.position())
        if control is None:
            return
        self._dragging = control
        self._drag_offset = (event.position().x() / self._phone_rect().width() - control.x,
                             event.position().y() / self._phone_rect().height() - control.y)
        self.update()

    def mouseMoveEvent(self, event) -> None:
        if self._dragging is None:
            return
        phone = self._phone_rect()
        if phone.width() <= 0 or phone.height() <= 0:
            return
        x = event.position().x() / phone.width() - self._drag_offset[0]
        y = event.position().y() / phone.height() - self._drag_offset[1]
        self._dragging.x = max(0.0, min(1.0 - self._dragging.w, x))
        self._dragging.y = max(0.0, min(1.0 - self._dragging.h, y))
        self.update()

    def mouseReleaseEvent(self, event) -> None:
        if self._dragging is not None:
            self._dragging = None
            self.changed.emit()

    # ── 绘制 ─────────────────────────────────────────────────

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        # 画布底色用页面背景色，手机区域用卡片色 —— 这样设备框一眼能分清
        painter.fillRect(self.rect(), qcolor("bg"))

        phone = self._phone_rect()
        painter.setPen(QPen(qcolor("border_strong"), 2))
        painter.setBrush(QBrush(qcolor("card")))
        painter.drawRoundedRect(phone, 14, 14)
        painter.setPen(QPen(qcolor("text_tertiary")))
        painter.setFont(QFont(painter.font().family(), 8))
        painter.drawText(QRectF(phone.left(), phone.top() - 16, phone.width(), 14),
                         Qt.AlignCenter, f"{self.layout_data.screen}  {SCREEN_RATIO.get(self.layout_data.screen)}")

        for control in self.layout_data.controls:
            rect = self._to_screen(control.x, control.y, control.w, control.h)
            highlighted = control.id == self.highlight_id and self._blink
            accent = qcolor("accent")
            border = accent if highlighted else qcolor("border_strong")
            fill = QColor(accent)
            fill.setAlpha(90 if highlighted else int(255 * min(0.75, control.opacity)))
            painter.setPen(QPen(border, 3 if highlighted else 1.4))
            painter.setBrush(QBrush(fill))
            if getattr(control, "shape", "round") == "round":
                painter.drawEllipse(rect)
            else:
                painter.drawRoundedRect(rect, 8, 8)

            painter.setPen(QPen(qcolor("text")))
            font = painter.font()
            font.setPointSizeF(max(7.5, min(11.0, rect.height() / 3.2)))
            font.setBold(highlighted)
            painter.setFont(font)
            label = getattr(control, "label", "") or control.id
            if isinstance(control, DirectionControl):
                painter.drawText(QRectF(rect.left(), rect.top(), rect.width(), rect.height() / 2),
                                 Qt.AlignCenter, label)
                # 摇杆：中心圆 + 摇杆帽，直观看出死区位置
                center = rect.center()
                knob = min(rect.width(), rect.height() * 0.5) * 0.22
                painter.setBrush(QBrush(qcolor("bg")))
                painter.setPen(QPen(qcolor("border_strong"), 1.2))
                painter.drawEllipse(center, knob * 1.6, knob * 1.6)
                painter.setBrush(QBrush(qcolor("accent")))
                painter.setPen(Qt.NoPen)
                painter.drawEllipse(center, knob, knob)
            else:
                painter.drawText(rect, Qt.AlignCenter, label)
        painter.end()


class KeymapPage(BasePage):
    """按键映射页。"""

    def __init__(self, parent=None) -> None:
        super().__init__(title="按键映射", subtitle="给手机端用的虚拟按键布局；可在此编排、检查冲突、导出教学", parent=parent)
        ensure_presets()
        self.layout_data: KeymapLayout = build_preset("minimal")
        self._current_key = "preset-minimal"
        self._build_content()
        self._reload_list()
        self._apply_preset(self._preset_combo.currentText())

    # ── UI ───────────────────────────────────────────────────

    def _build_content(self) -> None:
        toolbar = QWidget(self.view)
        row = QHBoxLayout(toolbar)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(10)

        self._preset_combo = ComboBox(toolbar)
        for name in preset_names():
            self._preset_combo.addItem(PRESET_LABELS.get(name, name), userData=name)
        self._preset_combo.currentIndexChanged.connect(
            lambda: self._apply_preset(self._preset_combo.currentData()))
        self._screen_combo = ComboBox(toolbar)
        self._screen_combo.addItems(["横屏", "竖屏"])
        self._screen_combo.setFixedWidth(90)
        self._screen_combo.currentIndexChanged.connect(
            lambda: self._apply_preset(self._preset_combo.currentData()))
        self._search = SearchLineEdit(toolbar)
        self._search.setPlaceholderText("这个键在哪？输入 潜行 / jump / E …")
        self._search.textChanged.connect(self._on_search)

        row.addWidget(BodyLabel("预设", toolbar))
        row.addWidget(self._preset_combo)
        row.addWidget(self._screen_combo)
        row.addWidget(self._search, 1)

        self._canvas = KeymapCanvas(self.view)
        self._canvas.changed.connect(self._refresh_analysis)

        self._conflict_list = QListWidget(self.view)
        self._conflict_list.setMinimumHeight(110)
        self._conflict_list.itemClicked.connect(
            lambda item: self._canvas.highlight(str(item.data(Qt.UserRole) or "")))
        self._guide_list = QListWidget(self.view)
        self._guide_list.itemClicked.connect(
            lambda item: self._canvas.highlight(str(item.data(Qt.UserRole) or "")))

        side = QVBoxLayout()
        side.setSpacing(8)
        side.addWidget(StrongBodyLabel("冲突检查", self.view))
        side.addWidget(self._conflict_list)
        side.addWidget(StrongBodyLabel("新手教学（可导出给手机端）", self.view))
        side.addWidget(self._guide_list)

        body = QHBoxLayout()
        body.setSpacing(16)
        body.addWidget(self._canvas, 3)
        body.addLayout(side, 2)

        buttons = QHBoxLayout()
        buttons.setSpacing(8)
        self._save_btn = PrimaryPushButton("保存为我的布局", self.view)
        self._save_btn.clicked.connect(self._on_save)
        self._active_btn = PushButton("设为当前布局", self.view)
        self._active_btn.clicked.connect(self._on_set_active)
        self._export_btn = PushButton("导出 JSON", self.view)
        self._export_btn.clicked.connect(self._on_export)
        self._import_btn = PushButton("导入 JSON", self.view)
        self._import_btn.clicked.connect(self._on_import)
        self._fcl_btn = PushButton("导入 FCL 布局", self.view)
        self._fcl_btn.clicked.connect(self._on_import_fcl)
        self._guide_btn = PushButton("导出教学 Markdown", self.view)
        self._guide_btn.clicked.connect(self._on_export_guide)
        for widget in (self._save_btn, self._active_btn, self._export_btn,
                       self._import_btn, self._fcl_btn, self._guide_btn):
            buttons.addWidget(widget)
        buttons.addStretch(1)

        self.add_content(toolbar)
        self.add_content(self._canvas_card())
        self.vBoxLayout.addLayout(body)
        self.vBoxLayout.addLayout(buttons)
        self.add_stretch()

    def _canvas_card(self) -> QWidget:
        card = CardWidget(self.view)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(self._canvas)
        self._canvas_card_widget = card
        return card

    # ── 数据流 ───────────────────────────────────────────────

    def _apply_preset(self, key: str) -> None:
        if not hasattr(self, "_canvas"):
            return
        key = key or "minimal"
        screen = "portrait" if self._screen_combo.currentIndex() == 1 else "landscape"
        layout = load_layout_by_key(key) or build_preset(key, screen)
        if layout.screen != screen:
            layout = build_preset(key, screen)
        if layout.screen != screen:
            # 有些预设自带屏幕方向（单手模式固定竖屏），下拉框跟着它走，别让界面自相矛盾
            self._screen_combo.blockSignals(True)
            self._screen_combo.setCurrentIndex(1 if layout.screen == "portrait" else 0)
            self._screen_combo.blockSignals(False)
            InfoBar.info(title="这个预设固定屏幕方向",
                         content=f"「{layout.name}」按{SCREEN_RATIO.get(layout.screen)}比例设计，已自动切过去",
                         orient=InfoBarPosition.TOP, isClosable=True, duration=2500, parent=self)
        self.layout_data = layout
        self._current_key = f"preset-{key}"
        self._canvas.set_layout(layout)
        self._refresh_analysis()

    def _reload_list(self) -> None:
        for item in list_layouts():
            if item["key"] in preset_names():
                continue
            self._preset_combo.addItem(item["label"], userData=item["key"])

    def _refresh_analysis(self) -> None:
        layout = self.layout_data
        self._conflict_list.clear()
        errors = layout.validate()
        conflicts = layout.conflicts()
        for message in errors:
            item = QListWidgetItem(f"✗ {message}")
            item.setForeground(qcolor("danger"))
            self._conflict_list.addItem(item)
        for conflict in conflicts:
            item = QListWidgetItem(f"⚠ {conflict.message}")
            item.setForeground(qcolor("warning"))
            item.setData(Qt.UserRole, conflict.controls[0] if conflict.controls else "")
            self._conflict_list.addItem(item)
        if not errors and not conflicts:
            self._conflict_list.addItem(QListWidgetItem("✓ 没有发现问题，可以直接用"))

        self._guide_list.clear()
        for step in build_guide(layout):
            flag = "（可选）" if step.optional else ""
            item = QListWidgetItem(f"{step.order}. {step.title}{flag} — {step.instruction}")
            item.setData(Qt.UserRole, step.control_id)
            self._guide_list.addItem(item)

    def _on_search(self, text: str) -> None:
        hits = self.layout_data.find(text)
        self._canvas.highlight(hits[0] if hits else "")
        if text and not hits:
            InfoBar.info(title="没有找到", content=f"没有控件匹配「{text}」",
                         orient=InfoBarPosition.TOP, isClosable=True, duration=2500, parent=self)

    # ── 动作 ─────────────────────────────────────────────────

    def _on_save(self) -> None:
        key = save_layout_by_key(self.layout_data)
        InfoBar.success(title="已保存", content=f"布局已存到 {key.name}",
                        orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)

    def _on_set_active(self) -> None:
        set_active(self._current_key)
        InfoBar.success(title="已设为当前", content="手机端启动时会读取这个布局",
                        orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)

    def _on_export(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "导出按键布局", f"{self.layout_data.name}.keymap.json", "按键布局 (*.json)")
        if not path:
            return
        Path(path).write_text(self.layout_data.to_json(), encoding="utf-8")
        InfoBar.success(title="已导出", content=path, orient=InfoBarPosition.TOP,
                        isClosable=True, duration=3000, parent=self)

    def _on_import(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "导入按键布局", "", "按键布局 (*.json)")
        if not path:
            return
        try:
            self.layout_data = KeymapLayout.from_dict(json.loads(Path(path).read_text(encoding="utf-8")))
            self._canvas.set_layout(self.layout_data)
            self._refresh_analysis()
            InfoBar.success(title="已导入", content=self.layout_data.name,
                            orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)
        except Exception as exc:      # noqa: BLE001
            log.warning("按键布局导入失败: %s", exc)
            InfoBar.error(title="导入失败", content=str(exc), orient=InfoBarPosition.TOP,
                          isClosable=True, duration=5000, parent=self)

    def _on_import_fcl(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择 FCL 布局文件", "", "JSON (*.json)")
        if not path:
            return
        try:
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            self.layout_data = import_fcl_layout(data)
            self._canvas.set_layout(self.layout_data)
            self._refresh_analysis()
            InfoBar.success(title="已从 FCL 导入",
                            content=f"{len(self.layout_data.buttons)} 个按钮，坐标已按屏幕归一化",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=4000, parent=self)
        except Exception as exc:      # noqa: BLE001
            log.warning("FCL 布局导入失败: %s", exc)
            InfoBar.error(title="导入失败", content=str(exc), orient=InfoBarPosition.TOP,
                          isClosable=True, duration=5000, parent=self)

    def _on_export_guide(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "导出教学", f"{self.layout_data.name}-教学.md", "Markdown (*.md)")
        if not path:
            return
        Path(path).write_text(guide_to_markdown(self.layout_data), encoding="utf-8")
        InfoBar.success(title="已导出教学", content=path, orient=InfoBarPosition.TOP,
                        isClosable=True, duration=3000, parent=self)
