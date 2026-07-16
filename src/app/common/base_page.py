# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher
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
"""Reusable page scaffold for Fluent navigation interfaces."""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QVBoxLayout, QWidget
from qfluentwidgets import ScrollArea, SubtitleLabel, TitleLabel


class BasePage(ScrollArea):
    """Scrollable page with a title and subtitle header."""

    def __init__(
        self,
        title: str,
        subtitle: str,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent=parent)
        self.setObjectName(self.__class__.__name__)
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        self.view = QWidget(self)
        self.view.setStyleSheet("background: transparent;")
        self.setWidget(self.view)

        self.vBoxLayout = QVBoxLayout(self.view)
        self.vBoxLayout.setContentsMargins(28, 24, 28, 24)
        self.vBoxLayout.setSpacing(16)
        self.vBoxLayout.setAlignment(Qt.AlignmentFlag.AlignTop)

        self.titleLabel = TitleLabel(title, self.view)
        self.subtitleLabel = SubtitleLabel(subtitle, self.view)
        self.subtitleLabel.setTextColor("#606060", "#AAAAAA")

        self.vBoxLayout.addWidget(self.titleLabel)
        self.vBoxLayout.addWidget(self.subtitleLabel)

    def add_content(self, widget: QWidget) -> None:
        self.vBoxLayout.addWidget(widget)

    def add_stretch(self) -> None:
        self.vBoxLayout.addStretch(1)
