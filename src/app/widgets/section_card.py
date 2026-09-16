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
"""可折叠分区卡片 —— 全项目统一的"一块一块"式布局单元。

替换掉之前每个页面各写一份的 AccordionSection，差别在于：

  * 不画分隔线：分区之间靠卡片间距区分（PCL 也是整块卡片，不划线）；
  * 展开/收起走统一动画系统（src/app/animations.py），时长缓动一致；
  * 颜色全部来自 theme 令牌，主题切换时跟着变。
"""

from __future__ import annotations

from typing import Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QHBoxLayout, QLabel, QVBoxLayout, QWidget
from qfluentwidgets import BodyLabel, StrongBodyLabel

from src.app.animations import NORMAL, collapse_height, expand_height
from src.app.icons import block_pixmap
from src.app.styles import section_card_qss
from src.app.theme import on_theme_changed, token

__all__ = ["SectionCard"]


class _ClickableHeader(QWidget):
    """标题行：整行可点（点一下展开/收起）。"""

    clicked = Signal()

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.LeftButton and self.rect().contains(event.position().toPoint()):
            self.clicked.emit()
        super().mouseReleaseEvent(event)


class SectionCard(QWidget):
    """一块可折叠的卡片。

        section = SectionCard("模组加载器", "forge")
        section.add_content(widget)
        section.set_expanded(False)
    """

    expanded = Signal(object)

    ICON_KINDS = ("forge", "fabric", "neoforge", "optifine", "vanilla", "snapshot")

    def __init__(self, title: str, icon: str = "", parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setObjectName("SectionCard")
        self.setAttribute(Qt.WA_StyledBackground, True)
        self._expanded = True
        self._icon_kind = icon if icon in self.ICON_KINDS else ""

        outer = QVBoxLayout(self)
        outer.setContentsMargins(4, 4, 4, 8)
        outer.setSpacing(0)

        self.header = _ClickableHeader(self)
        self.header.setFixedHeight(46)
        self.header.setCursor(Qt.PointingHandCursor)
        self.header.clicked.connect(self.toggle)
        head = QHBoxLayout(self.header)
        head.setContentsMargins(14, 0, 14, 0)
        head.setSpacing(10)

        self.logo = None
        if self._icon_kind:
            self.logo = QLabel(self.header)
            self.logo.setFixedSize(24, 24)
            self.logo.setPixmap(block_pixmap(self._icon_kind, 24))
            head.addWidget(self.logo)
        elif icon:
            head.addWidget(StrongBodyLabel(icon, self.header))

        self.title_label = StrongBodyLabel(title, self.header)
        head.addWidget(self.title_label)
        head.addStretch(1)

        self.summary_label = BodyLabel("", self.header)
        self.summary_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        head.addWidget(self.summary_label)

        self.arrow_label = BodyLabel("▾", self.header)
        self.arrow_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        head.addWidget(self.arrow_label)
        outer.addWidget(self.header)

        self.body = QWidget(self)
        self._body_layout = QVBoxLayout(self.body)
        self._body_layout.setContentsMargins(14, 0, 14, 8)
        self._body_layout.setSpacing(6)
        outer.addWidget(self.body)

        self._apply_style()
        on_theme_changed(self._apply_style)

    # ── 内容 ──────────────────────────────────────────────────

    def add_content(self, widget: QWidget) -> None:
        self._body_layout.addWidget(widget)

    def add_layout(self, layout) -> None:
        self._body_layout.addLayout(layout)

    def set_summary(self, text: str) -> None:
        self.summary_label.setText(text)

    # ── 折叠 ──────────────────────────────────────────────────

    def is_expanded(self) -> bool:
        return self._expanded

    def toggle(self) -> None:
        self.set_expanded(not self._expanded)

    def set_expanded(self, expanded: bool, animate: bool = True) -> None:
        expanded = bool(expanded)
        if expanded == self._expanded and self.body.isVisible() == expanded:
            return
        self._expanded = expanded
        if expanded:
            self.body.setVisible(True)
            if animate:
                self.body.setMaximumHeight(0)
                expand_height(self.body, NORMAL)
            else:
                self.body.setMaximumHeight(16777215)
        else:
            if animate:
                collapse_height(self.body, NORMAL)
            else:
                self.body.setVisible(False)
                self.body.setMaximumHeight(16777215)
        self.arrow_label.setText("▴" if expanded else "▾")
        if expanded:
            self.expanded.emit(self)

    def _apply_style(self) -> None:
        self.setStyleSheet(section_card_qss())
        if self.logo is not None and self._icon_kind:
            self.logo.setPixmap(block_pixmap(self._icon_kind, 24))
        self.summary_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
        self.arrow_label.setTextColor(token("text_tertiary"), token("text_tertiary"))
