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
"""联机页（占位 + 方案说明）。

首页直接给联机留出入口，是因为这块的产品形态已经定了，先把位置占住：

    * Minecraft Java 版是**星型拓扑**：所有包都过服务端，客户端之间不直连，
      所以联机 = 有一个人（或一台服务器）当主机，其他人连他；
    * 打洞（P2P）解决的是"能不能连上"，不是"谁当主机"；
    * 打洞失败（对称 NAT / CGNAT）必须有中继兜底，否则总有几个人连不上。

这一页现在只把入口和说明摆好，功能随后按 房间码 -> 打洞 -> 中继 的顺序上。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    InfoBar,
    InfoBarPosition,
    LineEdit,
    PrimaryPushButton,
    PushButton,
    StrongBodyLabel,
)

from src.app.common.base_page import BasePage
from src.app.icons import grass_block_icon  # noqa: F401  (保持图标入口统一)
from src.app.theme import token


class MultiplayerPage(BasePage):
    """联机占位页：房间码 + 说明（按钮先禁用，功能没上不骗人）。"""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(title="联机",
                         subtitle="和朋友一起玩：房间码加入 / P2P 打洞 / 中继兜底（开发中）",
                         parent=parent)
        self._build_content()

    # ── 界面 ───────────────────────────────────────────────────

    def _build_content(self) -> None:
        self.add_content(self._status_card())
        self.add_content(self._room_card())
        self.add_content(self._notes_card())
        self.add_stretch()

    def _status_card(self) -> CardWidget:
        card = CardWidget(self.view)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(6)

        title = StrongBodyLabel("🚧 联机功能正在开发", card)
        title.setStyleSheet(f"color: {token('warning')}; font-size: 15px;")
        layout.addWidget(title)

        body = BodyLabel(
            "第一版会做的四件事：\n"
            "1. 一键开房：启动器自己拉起服务端并把存档挂上，不用进游戏手动点「对局域网开放」；\n"
            "2. 房间码：房主把 6 位码发给朋友，对方输码即进；\n"
            "3. P2P 打洞：同一房间的成员先尝试直连，延迟最低；\n"
            "4. 中继兜底：打洞失败（运营商 NAT）自动走服务器转发，不让任何人卡在门外。",
            card)
        body.setWordWrap(True)
        body.setTextColor(token("text_secondary"), token("text_secondary"))
        layout.addWidget(body)
        return card

    def _room_card(self) -> CardWidget:
        """先把交互位置留出来：房间码输入 + 加入/创建（暂禁用）。"""
        card = CardWidget(self.view)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(10)

        layout.addWidget(StrongBodyLabel("加入房间", card))

        row = QWidget(card)
        row_layout = QHBoxLayout(row)
        row_layout.setContentsMargins(0, 0, 0, 0)
        row_layout.setSpacing(8)

        self.room_input = LineEdit(row)
        self.room_input.setPlaceholderText("输入 6 位房间码，例如 A1B2C3")
        self.room_input.setMaxLength(6)
        self.room_input.setEnabled(False)
        row_layout.addWidget(self.room_input, 1)

        self.join_btn = PrimaryPushButton("加入房间", row)
        self.join_btn.setEnabled(False)
        self.join_btn.clicked.connect(self._not_ready)
        row_layout.addWidget(self.join_btn)

        self.host_btn = PushButton("创建房间（我来当主机）", row)
        self.host_btn.setEnabled(False)
        self.host_btn.clicked.connect(self._not_ready)
        row_layout.addWidget(self.host_btn)

        layout.addWidget(row)

        hint = BodyLabel("当主机的机器要自己跑服务端，对单核性能和上行带宽有要求（开房前会先做一次体检）",
                         card)
        hint.setWordWrap(True)
        hint.setTextColor(token("text_tertiary"), token("text_tertiary"))
        layout.addWidget(hint)
        return card

    def _notes_card(self) -> CardWidget:
        card = CardWidget(self.view)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(6)

        layout.addWidget(StrongBodyLabel("几个已经定下来的设计", card))
        notes = [
            "星型拓扑：Minecraft 的客户端之间不直连，所有数据都过服务端 —— 所以联机一定有一个人当主机。",
            "主机可以不是玩家的电脑：后续会支持把存档推到服务器上开房（云端主机），掉线也不会散伙。",
            "手机端只做加入方：安卓跑服务端会发热降频、被后台杀掉，体验很差，所以不提供「手机当主机」。",
            "兼容局域网：同一个 WiFi 下直接连，不打洞、不走中继，最稳也最快。",
        ]
        for note in notes:
            item = BodyLabel("· " + note, card)
            item.setWordWrap(True)
            item.setTextColor(token("text_secondary"), token("text_secondary"))
            layout.addWidget(item)
        return card

    def _not_ready(self) -> None:
        InfoBar.info(title="还没上线", content="联机功能在开发中，先把位置占住 😄",
                     orient=InfoBarPosition.TOP, isClosable=True, duration=2500, parent=self)
