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
"""加载器选择行 —— PCL 那种手风琴，不是"两米长的下拉菜单"。

以前这里是一个 ComboBox：Forge 一个版本动辄数百个构建，下拉一展开整屏都是它，
而且挡住下面的按钮。现在改成：

    标题行：加载器图标（PCL 原图）+ 名字 + 已选版本 + 展开箭头   ← 平时只有这一行
    展开后：搜索框 + **固定高度**（190px）的列表 + 清除按钮        ← 选完自动收起

同一时间只展开一个（由页面把所有行塞进 set_group 协调），点标题行就能开合。
"""

from __future__ import annotations

from typing import Dict, List, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QHBoxLayout, QLabel, QListWidget, QListWidgetItem, QVBoxLayout, QWidget
from qfluentwidgets import BodyLabel, PushButton, SearchLineEdit, StrongBodyLabel

from PySide6.QtCore import QRect, QSize
from PySide6.QtGui import QBrush, QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import QStyle, QStyledItemDelegate

from src.app.animations import NORMAL, collapse_height, expand_height, fade
from src.app.icons import block_pixmap
from src.app.theme import token

__all__ = ["LoaderRow"]


class LoaderVersionDelegate(QStyledItemDelegate):
    """版本列表的行：不是"纯文本清单"，而是一行里有主信息 + 次要信息 + 标签。

        主信息：版本号（加粗、大一号）
        次要信息：构建日期 / 类型（小字、灰色，能取到才显示）
        标签：最新 / 推荐 / Beta（彩色小胶囊）
        选中：右侧一个 ✓ + 行底色带强调色
    """

    ROW_HEIGHT = 40

    def sizeHint(self, option, index) -> QSize:
        return QSize(0, self.ROW_HEIGHT)

    def paint(self, painter: QPainter, option, index) -> None:
        from src.app.theme import qcolor

        painter.save()
        painter.setRenderHint(QPainter.Antialiasing, True)
        rect = option.rect.adjusted(6, 3, -6, -3)
        hovered = bool(option.state & QStyle.State_MouseOver)
        selected = bool(index.data(Qt.UserRole + 2))

        if selected:
            fill = QColor(qcolor("accent"))
            fill.setAlpha(38)
            painter.setPen(QPen(QColor(qcolor("accent")), 1))
            painter.setBrush(QBrush(fill))
        elif hovered:
            painter.setPen(Qt.NoPen)
            painter.setBrush(QBrush(qcolor("hover")))
        else:
            painter.setPen(Qt.NoPen)
            painter.setBrush(Qt.NoBrush)
        painter.drawRoundedRect(rect, 6, 6)

        label = str(index.data(Qt.DisplayRole) or "")
        detail = str(index.data(Qt.UserRole + 3) or "")
        tags = index.data(Qt.UserRole + 4) or []

        text_color = qcolor("text")
        sub_color = qcolor("text_tertiary")
        font = QFont(painter.font())
        font.setBold(True)
        font.setPointSizeF(font.pointSizeF() + 0.5)
        painter.setFont(font)
        painter.setPen(QPen(text_color))
        painter.drawText(QRect(rect.left() + 10, rect.top(), rect.width() - 120, rect.height()),
                         Qt.AlignVCenter | Qt.AlignLeft, label)

        if detail:
            font.setBold(False)
            font.setPointSizeF(max(7.5, font.pointSizeF() - 2.0))
            painter.setFont(font)
            painter.setPen(QPen(sub_color))
            painter.drawText(QRect(rect.left() + 10, rect.top(), rect.width() - 120, rect.height()),
                             Qt.AlignVCenter | Qt.AlignRight, detail)

        # 标签从右往左排
        x = rect.right() - 12
        metrics = painter.fontMetrics()
        for text, color_name in reversed(list(tags)):
            width = metrics.horizontalAdvance(text) + 14
            chip = QRect(x - width, rect.center().y() - 9, width, 18)
            color = QColor(qcolor(color_name))
            fill = QColor(color)
            fill.setAlpha(46)
            painter.setPen(QPen(color, 1))
            painter.setBrush(QBrush(fill))
            painter.drawRoundedRect(chip, 5, 5)
            painter.setPen(QPen(color))
            painter.drawText(chip, Qt.AlignCenter, text)
            x -= width + 6

        if selected:
            painter.setPen(QPen(QColor(qcolor("accent")), 2))
            painter.drawText(QRect(rect.right() - 6, rect.top(), 0, rect.height()),
                             Qt.AlignVCenter | Qt.AlignRight, "")
        painter.restore()


class _ClickableHeader(QWidget):
    """标题行：整行可点（点一下就是展开/收起）。"""

    clicked = Signal()

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.LeftButton and self.rect().contains(event.position().toPoint()):
            self.clicked.emit()
        super().mouseReleaseEvent(event)


class LoaderRow(QWidget):
    """一个加载器（Forge / NeoForge / Fabric / OptiFine…）的选择行。"""

    loader_selected = Signal(str, str)      # (loader_type, version)
    loader_cleared = Signal(str)
    expanded = Signal(object)               # 展开时通知页面把别人收起来

    LIST_HEIGHT = 190                       # 固定高度：再长的版本列表也在框里滚
    HEADER_HEIGHT = 42

    def __init__(self, loader_type: str, display_name: str, parent=None):
        super().__init__(parent)
        self.loader_type = loader_type
        self.display_name = display_name
        self._selected_version: Optional[str] = None
        self._versions: List[dict] = []
        self._version_key = "version"
        self._group: List["LoaderRow"] = []
        self.is_loading = True
        self._expanded = False

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        # ── 标题行 ──
        self.header = _ClickableHeader(self)
        self.header.setFixedHeight(self.HEADER_HEIGHT)
        self.header.setCursor(Qt.PointingHandCursor)
        self.header.clicked.connect(self.toggle)
        head = QHBoxLayout(self.header)
        head.setContentsMargins(10, 0, 10, 0)
        head.setSpacing(10)

        self.logo = QLabel(self.header)
        self.logo.setFixedSize(22, 22)
        self.logo.setPixmap(block_pixmap(loader_type, 22))
        self.logo.setScaledContents(True)
        head.addWidget(self.logo)

        self.name_label = StrongBodyLabel(display_name, self.header)
        head.addWidget(self.name_label)

        head.addStretch(1)

        self.summary_label = BodyLabel("加载中…", self.header)
        self.summary_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        head.addWidget(self.summary_label)

        self.arrow_label = BodyLabel("▾", self.header)
        self.arrow_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        head.addWidget(self.arrow_label)

        outer.addWidget(self.header)

        # ── 展开区（默认收起） ──
        self.body = QWidget(self)
        body_layout = QVBoxLayout(self.body)
        body_layout.setContentsMargins(12, 4, 12, 10)
        body_layout.setSpacing(8)

        self.search = SearchLineEdit(self.body)
        self.search.setPlaceholderText("筛选版本…")
        self.search.textChanged.connect(self._apply_filter)
        body_layout.addWidget(self.search)

        self.list = QListWidget(self.body)
        self.list.setFixedHeight(self.LIST_HEIGHT)
        self.list.setUniformItemSizes(True)
        self.list.setMouseTracking(True)                 # 悬停高亮靠它
        self.list.setItemDelegate(LoaderVersionDelegate(self.list))
        self.list.itemClicked.connect(self._on_item_clicked)
        body_layout.addWidget(self.list)

        bottom = QHBoxLayout()
        bottom.setContentsMargins(0, 0, 0, 0)
        self.hint_label = BodyLabel("", self.body)
        self.hint_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        bottom.addWidget(self.hint_label)
        bottom.addStretch(1)
        self.clear_btn = PushButton("清除选择", self.body)
        self.clear_btn.clicked.connect(self._clear)
        bottom.addWidget(self.clear_btn)
        body_layout.addLayout(bottom)

        self.body.setVisible(False)
        outer.addWidget(self.body)

        from src.app.theme import on_theme_changed
        on_theme_changed(self._refresh_summary_color)

    # ── 折叠逻辑 ───────────────────────────────────────────────

    def set_group(self, rows: List["LoaderRow"]) -> None:
        """同一组里只允许展开一个（手风琴行为）。"""
        self._group = [row for row in rows if row is not self]

    def toggle(self) -> None:
        self.set_expanded(not self._expanded)

    def set_expanded(self, expanded: bool) -> None:
        """展开/收起（带动画）。同一组里只留一个展开。"""
        expanded = bool(expanded)
        if expanded == self._expanded:
            return
        self._expanded = expanded
        if expanded:
            self.body.setMaximumHeight(0)        # 从 0 长出来，别闪一下
            self.body.setVisible(True)
            expand_height(self.body, NORMAL, None)
            fade(self.body, 0.0, 1.0, NORMAL)
        else:
            collapse_height(self.body, NORMAL, None)
        self.arrow_label.setText("▴" if expanded else "▾")
        if self._expanded:
            for row in self._group:
                if row._expanded:
                    row.set_expanded(False)
            self.search.setFocus()
            self.expanded.emit(self)

    # ── 数据 ──────────────────────────────────────────────────

    def set_loading(self, loading: bool) -> None:
        self.is_loading = loading
        if loading:
            self.summary_label.setText("加载中…")
            self.list.clear()
            self.hint_label.setText("")

    def set_error(self, message: str = "加载失败") -> None:
        self.is_loading = False
        self._versions = []
        self.summary_label.setText(message)
        self.list.clear()
        self.hint_label.setText(message)

    def set_versions(self, versions: list, version_key: str = "version") -> None:
        """把版本列表塞进来（默认选中第一个，和以前的下拉行为一致）。"""
        self.is_loading = False
        self._versions = list(versions or [])
        self._version_key = version_key or "version"
        self.list.clear()
        if not self._versions:
            self.summary_label.setText("无可用版本")
            self.hint_label.setText("这个游戏版本没有对应的加载器版本")
            self.clear_btn.setVisible(False)
            return

        # 先算好"最新"标记（列表本身按时间倒序，第一条就是最新）
        labels = []
        for index, item in enumerate(self._versions):
            label = self._extract_version(item, self._version_key)
            if not label:
                continue
            tags = []
            if index == 0:
                tags.append("最新")
            if isinstance(item, dict) and item.get("recommended"):
                tags.append("推荐")
            if "beta" in label.lower() or (isinstance(item, dict) and item.get("beta")):
                tags.append("Beta")
            suffix = f"    （{' / '.join(tags)}）" if tags else ""
            labels.append((label, f"{label}{suffix}"))

        self._labels = labels
        self.hint_label.setText(f"共 {len(labels)} 个版本，点一行选中")
        self._fill_list(labels)

        if labels:
            self._select(labels[0][0])

    @staticmethod
    def _extract_version(item, version_key: str) -> str:
        """取版本号，支持 loader.version 这种点号路径（Fabric 的结构就是这样）。"""
        if not version_key:
            return ""
        current = item
        for part in str(version_key).split("."):
            if isinstance(current, dict):
                current = current.get(part)
            else:
                return ""
        if isinstance(current, dict):
            current = current.get("version", "")
        return str(current or "")

    def _fill_list(self, labels) -> None:
        """填列表：每行带主文本 + 次要信息（日期）+ 标签，交给 delegate 画。"""
        self.list.clear()
        for value, text in labels:
            item = QListWidgetItem(text)
            item.setData(Qt.UserRole, value)
            item.setData(Qt.UserRole + 2, value == self._selected_version)
            item.setData(Qt.UserRole + 3, self._detail_of(value))
            item.setData(Qt.UserRole + 4, self._tags_of(value))
            self.list.addItem(item)

    def _detail_of(self, version: str) -> str:
        """次要信息：构建日期（加载器列表里叫 releaseTime / time / date 的都有）。"""
        for entry in self._versions:
            if not isinstance(entry, dict):
                continue
            if self._extract_version(entry, self._version_key) != version:
                continue
            for key in ("releaseTime", "time", "date", "timestamp"):
                raw = entry.get(key)
                if raw:
                    return str(raw).replace("T", " ")[:16]
            if entry.get("type"):
                return str(entry["type"])
        return ""

    def _tags_of(self, version: str) -> list:
        """标签：最新 / 推荐 / Beta（颜色名走主题令牌）。"""
        tags = []
        labels = getattr(self, "_labels", [])
        if labels and labels[0][0] == version:
            tags.append(("最新", "accent"))
        for entry in self._versions:
            if not isinstance(entry, dict):
                continue
            if self._extract_version(entry, self._version_key) != version:
                continue
            if entry.get("recommended"):
                tags.append(("推荐", "success"))
            if "beta" in version.lower() or entry.get("beta"):
                tags.append(("Beta", "warning"))
        return tags

    def _apply_filter(self, text: str) -> None:
        needle = (text or "").strip().lower()
        labels = getattr(self, "_labels", [])
        if not needle:
            self._fill_list(labels)
            return
        self._fill_list([pair for pair in labels if needle in pair[0].lower()])

    def _on_item_clicked(self, item: QListWidgetItem) -> None:
        value = item.data(Qt.UserRole)
        if value:
            self._select(str(value))
            self.set_expanded(False)          # 选完就收起来，别挡着下一步

    def _select(self, version: str) -> None:
        self._selected_version = version
        self._refresh_selection()
        self.summary_label.setText(f"已选 {version}")
        self._refresh_summary_color()
        self.clear_btn.setVisible(True)
        self.loader_selected.emit(self.loader_type, version)

    def _clear(self) -> None:
        self._selected_version = None
        self.summary_label.setText("未选择")
        self._refresh_summary_color()
        self.clear_btn.setVisible(False)
        self.loader_cleared.emit(self.loader_type)

    def _refresh_selection(self) -> None:
        """把"哪一行是选中的"同步给 delegate。"""
        for row in range(self.list.count()):
            item = self.list.item(row)
            item.setData(Qt.UserRole + 2, item.data(Qt.UserRole) == self._selected_version)

    def _refresh_summary_color(self) -> None:
        color = token("success") if self._selected_version else token("text_tertiary")
        self.summary_label.setTextColor(color, color)
        self.arrow_label.setTextColor(token("text_tertiary"), token("text_tertiary"))

    # ── 查询 ──────────────────────────────────────────────────

    def is_selected(self) -> bool:
        return bool(self._selected_version)

    def get_selected_version(self) -> Optional[str]:
        return self._selected_version

    def set_selected_version(self, version: Optional[str]) -> None:
        if version:
            self._select(str(version))
        else:
            self._clear()
