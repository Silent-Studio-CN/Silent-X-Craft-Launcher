# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# Silent X Craft Launcher (SXCL) is a third-party Minecraft launcher developed
# by the Silent X Craft Launcher Dev team, operating under the management of
# SilentCodeTeams, and overseen by SilentStudio.
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
"""Versions page — fetch and display Minecraft versions."""

from __future__ import annotations

import webbrowser
from pathlib import Path

from PySide6.QtCore import (
    Qt, QThread, Signal, QEvent, QTimer, QModelIndex, QRect, QSize, QAbstractListModel,
)
from PySide6.QtGui import QBrush, QColor, QPainter, QPen
from PySide6.QtWidgets import (
    QAbstractItemView, QHBoxLayout, QListView, QPushButton, QStyle, QStyledItemDelegate,
    QVBoxLayout, QWidget,
)
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    ComboBox,
    FlowLayout,
    IndeterminateProgressRing,
    InfoBar,
    InfoBarPosition,
    PrimaryPushButton,
    PushButton,
    SearchLineEdit,
    StrongBodyLabel,
    isDarkTheme,
)

from src.app.common.base_page import BasePage
from src.app.theme import token as _token
from src.app.common.launcher_config import cfg
from src.services.minecraft.manifest import (
    fetch_version_manifest,
    filter_versions,
    VersionType,
    GameVersion,
)
from src.app.icons import grass_block_pixmap, loader_chip_text, loader_color
from src.services.minecraft.installed import get_installed_versions, get_version_info
from src.services.minecraft.loaders import scan_installed


class FetchWorker(QThread):
    """Background thread for fetching version manifest."""
    finished = Signal(object)  # list[GameVersion]
    error = Signal(str)

    def __init__(self, source):
        super().__init__()
        self.source = source

    def run(self):
        try:
            _, versions = fetch_version_manifest(self.source)
            self.finished.emit(versions)
        except Exception as e:
            self.error.emit(str(e))


class VersionListModel(QAbstractListModel):
    """版本列表模型 —— 只存数据不建控件，这是列表不卡的关键。"""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._items: list[GameVersion] = []
        self._installed: set[str] = set()
        self._loaders: dict = {}      # 版本 id -> [(kind, version)]
        self._problems: dict = {}     # 版本 id -> 不能启动的原因

    def set_versions(self, versions, installed=None) -> None:
        self.beginResetModel()
        self._items = list(versions)
        if installed is not None:
            self._installed = set(installed)
        self.endResetModel()

    def set_loaders(self, mapping: dict) -> None:
        """版本 id -> [(加载器种类, 版本号)]；只放已安装且带加载器的版本。"""
        self._loaders = dict(mapping or {})
        if self._items:
            self.dataChanged.emit(self.index(0, 0), self.index(len(self._items) - 1, 0),
                                  [self.LoadersRole, self.ProblemRole])

    def set_problems(self, mapping: dict) -> None:
        """版本 id -> 不能启动的原因（挂在原版那一行上，用户才看得到）。"""
        self._problems = dict(mapping or {})
        if self._items:
            self.dataChanged.emit(self.index(0, 0), self.index(len(self._items) - 1, 0),
                                  [self.ProblemRole])

    def rowCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._items)

    RowRole = Qt.UserRole                 # 该行的 GameVersion
    InstalledRole = Qt.UserRole + 1
    LoadersRole = Qt.UserRole + 2
    ProblemRole = Qt.UserRole + 3      # 不能启动的原因（缺前置/JSON 坏）

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or not (0 <= index.row() < len(self._items)):
            return None
        version = self._items[index.row()]
        if role == Qt.DisplayRole:
            return version.id
        if role == Qt.UserRole:
            return version
        if role == self.InstalledRole:
            return version.id in self._installed
        if role == self.LoadersRole:
            return self._loaders.get(version.id, [])
        if role == self.ProblemRole:
            return self._problems.get(version.id, "")
        if role == Qt.ToolTipRole:
            state = "已安装" if version.id in self._installed else "未安装"
            return f"{version.id}\n{version.version_type} | {version.release_label} | {state}"
        return None


class VersionRowDelegate(QStyledItemDelegate):
    """一行一个版本：名称 / 类型 / 日期；悬停时右侧出现「版本日志 / 获取服务端」。"""

    action_triggered = Signal(object, str)

    ROW_HEIGHT = 52
    BTN_H = 28
    LOG_W = 84
    SRV_W = 96
    GAP = 8
    MARGIN = 16

    def sizeHint(self, option, index) -> QSize:
        return QSize(0, self.ROW_HEIGHT)

    def _button_rects(self, rect: QRect):
        y = rect.top() + (rect.height() - self.BTN_H) // 2
        srv = QRect(rect.right() - self.MARGIN - self.SRV_W, y, self.SRV_W, self.BTN_H)
        log = QRect(srv.left() - self.GAP - self.LOG_W, y, self.LOG_W, self.BTN_H)
        return log, srv

    def paint(self, painter: QPainter, option, index) -> None:
        version = index.data(Qt.UserRole)
        if version is None:
            return
        painter.save()
        painter.setRenderHint(QPainter.Antialiasing, True)
        rect = option.rect.adjusted(0, 2, 0, -2)
        hovered = bool(option.state & QStyle.State_MouseOver)
        dark = isDarkTheme()

        painter.setPen(Qt.NoPen)
        from src.app.theme import qcolor as _tc
        painter.setBrush(QBrush(_tc("hover_strong") if hovered else _tc("hover")))
        painter.drawRoundedRect(rect, 6, 6)

        from src.app.theme import qcolor as theme_color
        text_color = theme_color("text")
        sub_color = theme_color("text_tertiary")
        type_map = {"release": "正式版", "snapshot": "快照", "old": "旧版"}

        left = rect.left() + 16
        font = painter.font()
        font.setBold(True)
        painter.setFont(font)
        painter.setPen(QPen(text_color))
        painter.drawText(QRect(left, rect.top(), 144, rect.height()),
                         Qt.AlignVCenter | Qt.AlignLeft, version.id)

        font.setBold(False)
        painter.setFont(font)
        painter.setPen(QPen(sub_color))
        painter.drawText(QRect(left + 152, rect.top(), 64, rect.height()),
                         Qt.AlignVCenter | Qt.AlignLeft,
                         type_map.get(version.version_type, version.version_type))
        painter.drawText(QRect(left + 226, rect.top(), 110, rect.height()),
                         Qt.AlignVCenter | Qt.AlignLeft, version.release_label)

        if index.data(VersionListModel.InstalledRole):
            from PySide6.QtGui import QColor as _QColor
            from src.app.theme import qcolor as _ok
            # 草方块 = Minecraft 版本的通用符号，一眼看出这条是装好的
            from src.app.icons import block_pixmap, state_icon_kind
            icon_kind = state_icon_kind(version.version_type,
                                        index.data(VersionListModel.LoadersRole),
                                        bool(index.data(VersionListModel.ProblemRole)))
            painter.drawPixmap(QRect(left + 340, rect.top() + (rect.height() - 14) // 2, 14, 14),
                               block_pixmap(icon_kind, 28))
            painter.setPen(QPen(_ok("success")))
            painter.drawText(QRect(left + 360, rect.top(), 60, rect.height()),
                             Qt.AlignVCenter | Qt.AlignLeft, "已安装")
            # 同一个原版下装了哪些模组加载器（用户经常一个版本装好几套）
            chip_x = left + 424
            metrics = painter.fontMetrics()
            for kind, version_text in (index.data(VersionListModel.LoadersRole) or [])[:3]:
                text = loader_chip_text(kind, version_text)
                width = metrics.horizontalAdvance(text) + 16
                if chip_x + width > rect.right() - 8:
                    break
                chip = QRect(chip_x, rect.top() + (rect.height() - 20) // 2, width, 20)
                color = _QColor(loader_color(kind))
                fill = _QColor(color)
                fill.setAlpha(48)
                painter.setPen(QPen(color, 1))
                painter.setBrush(QBrush(fill))
                painter.drawRoundedRect(chip, 4, 4)
                painter.setPen(QPen(color))
                painter.drawText(chip, Qt.AlignCenter, text)
                chip_x += width + 6

            # 不能启动的原因直接贴在行里（缺前置/JSON 坏），别等用户点启动才报错
            problem = index.data(VersionListModel.ProblemRole) or ""
            if problem:
                text = "⚠ " + problem
                width = metrics.horizontalAdvance(text) + 16
                if chip_x + width <= rect.right() - 8:
                    chip = QRect(chip_x, rect.top() + (rect.height() - 20) // 2, width, 20)
                    color = _QColor(_ok("danger"))
                    fill = _QColor(color)
                    fill.setAlpha(48)
                    painter.setPen(QPen(color, 1))
                    painter.setBrush(QBrush(fill))
                    painter.drawRoundedRect(chip, 4, 4)
                    painter.setPen(QPen(color))
                    painter.drawText(chip, Qt.AlignCenter, text)
            painter.setBrush(Qt.NoBrush)

        if hovered:
            log_rect, srv_rect = self._button_rects(rect)
            from src.app.theme import qcolor as _ac
            painter.setPen(QPen(_ac("accent")))
            painter.setBrush(Qt.NoBrush)
            for btn_rect, label in ((log_rect, "版本日志"), (srv_rect, "获取服务端")):
                painter.drawRoundedRect(btn_rect, 4, 4)
                painter.drawText(btn_rect, Qt.AlignCenter, label)
        painter.restore()

    def button_rects(self, index, view) -> tuple:
        """给视图做命中测试用：这一行两个按钮的屏幕矩形。

        以前是在 editorEvent 里判的，而那里依赖 option.state 的 MouseOver —— 实测
        **松开鼠标时 Qt 并不给 MouseOver**，于是「版本日志」永远点不动（事件被当成整行点击，
        结果打开了下载配置页）。现在把命中测试交给视图自己（见 VersionListView）。
        """
        rect = view.visualRect(index).adjusted(0, 2, 0, -2)
        return self._button_rects(rect)


class VersionListView(QListView):
    """版本列表：把行内那两个按钮（版本日志 / 获取服务端）的点击接住。

    为什么不放在 delegate 的 editorEvent 里：那条路要求 option.state 带 MouseOver，
    而松手时 Qt 不给这个状态 —— 实测点「版本日志」会落空并退化成整行点击。
    在 mouseReleaseEvent 里直接算矩形最稳，而且能在按钮命中时**不调用 super()**，
    从而保证"点按钮就只是点按钮"，不会再触发整行的下载配置页。
    """

    action_triggered = Signal(object, str)

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.LeftButton:
            index = self.indexAt(event.position().toPoint())
            delegate = self.itemDelegate()
            if index.isValid() and hasattr(delegate, "button_rects"):
                version = index.data(VersionListModel.RowRole)
                if version is not None:
                    log_rect, srv_rect = delegate.button_rects(index, self)
                    pos = event.position().toPoint()
                    if log_rect.contains(pos):
                        self.action_triggered.emit(version, "wiki")
                        event.accept()
                        return
                    if srv_rect.contains(pos):
                        self.action_triggered.emit(version, "server")
                        event.accept()
                        return
        super().mouseReleaseEvent(event)


class VersionsPage(BasePage):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(
            title="游戏版本",
            subtitle="选择要启动的 Minecraft 版本",
            parent=parent,
        )
        self._versions: list[GameVersion] = []
        self._filtered: list[GameVersion] = []
        self._worker: FetchWorker | None = None
        self._installed: list[str] = []
        self._installed_versions: list = []

        self._build_content()
        self._load_versions()

    def _build_content(self) -> None:
        # ---- 工具栏 ----
        toolbar = QWidget(self.view)
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        toolbar_layout.setSpacing(12)

        self.search_box = SearchLineEdit(toolbar)
        self.search_box.setPlaceholderText("搜索版本号…")
        self.search_box.setClearButtonEnabled(True)
        self.search_box.textChanged.connect(self._on_search)

        # 搜索防抖：以前每敲一个字符就重建全部卡片（实测 0.6~0.8 秒/字符）
        self._reload_timer = QTimer(self)
        self._reload_timer.setSingleShot(True)
        self._reload_timer.setInterval(260)
        self._reload_timer.timeout.connect(self._reload_list)

        self.category_combo = ComboBox(toolbar)
        self.category_combo.addItems(["全部", "正式版", "快照", "旧版"])
        self.category_combo.setCurrentIndex(1)  # 默认"正式版"
        self.category_combo.currentIndexChanged.connect(self._on_category_changed)

        self.refresh_btn = PushButton("刷新", toolbar)
        self.refresh_btn.clicked.connect(self._load_versions)

        toolbar_layout.addWidget(self.search_box, 1)
        toolbar_layout.addWidget(self.category_combo)
        toolbar_layout.addWidget(self.refresh_btn)

        # ---- 版本列表 ----
        # ── 加载中（居中旋转圈 + 文字）──
        self.loading_widget = QWidget(self.view)
        lw = QVBoxLayout(self.loading_widget)
        lw.setAlignment(Qt.AlignCenter)
        self.loading_spinner = IndeterminateProgressRing(self.loading_widget)
        self.loading_spinner.setFixedSize(48, 48)
        self.loading_label = BodyLabel("正在加载版本清单…", self.loading_widget)
        self.loading_label.setAlignment(Qt.AlignCenter)
        lw.addStretch(2)
        lw.addWidget(self.loading_spinner, 0, Qt.AlignCenter)
        lw.addSpacing(12)
        lw.addWidget(self.loading_label, 0, Qt.AlignCenter)
        lw.addStretch(3)

        # ── 统计文字 ──
        self.status_label = BodyLabel("", self.view)
        self.status_label.setVisible(False)

        # 虚拟化列表：只渲染可见行，几千个版本也是瞬间完成
        self.version_list = VersionListView(self.view)
        self.version_list.setObjectName("versionList")
        self.version_list.setUniformItemSizes(True)
        self.version_list.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel)
        self.version_list.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.version_list.setSelectionMode(QAbstractItemView.NoSelection)
        self.version_list.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.version_list.setMouseTracking(True)
        self.version_list.setAttribute(Qt.WA_Hover, True)
        self.version_list.viewport().setAttribute(Qt.WA_Hover, True)
        self.version_list.setMinimumHeight(260)
        self.version_list.setStyleSheet("QListView { background: transparent; border: none; }")

        self._model = VersionListModel(self.version_list)
        self._delegate = VersionRowDelegate(self.version_list)
        self.version_list.setModel(self._model)
        self.version_list.setItemDelegate(self._delegate)
        self.version_list.clicked.connect(self._on_row_clicked)
        # 行内按钮的点击由视图接（delegate 的 editorEvent 那条路点不动，见 VersionListView）
        self.version_list.action_triggered.connect(self._on_row_action)
        from src.app.theme import on_theme_changed
        on_theme_changed(lambda: (self.version_list.viewport().update(),
                                  self.status_label.update()))

        self.add_content(toolbar)
        self.add_content(self.loading_widget)
        self.add_content(self.status_label)
        self.vBoxLayout.addWidget(self.version_list, 1)

    def _load_versions(self) -> None:
        self.loading_widget.setVisible(True)
        self.loading_label.setText("正在加载版本清单…")
        self.status_label.setVisible(False)
        self._model.set_versions([])
        self.refresh_btn.setEnabled(False)

        source = cfg.downloadSource.value
        self._worker = FetchWorker(source)
        self._worker.finished.connect(self._on_versions_loaded)
        self._worker.error.connect(self._on_load_error)
        self._worker.start()

    def _refresh_installed(self) -> None:
        """扫描已安装版本（含模组加载器），并把结果同步给列表模型。

        只要求 JSON：Forge 1.13+ / Fabric 装的版本没有自己的 jar（继承原版），
        以前按"jar + json 都在"判断会把它们全漏掉。
        """
        try:
            self._installed_versions = scan_installed()
        except Exception:
            self._installed_versions = []

        # "已安装"只认精确匹配（原版目录在不在是事实，不能糊弄）
        self._installed = [item.id for item in self._installed_versions]

        # 加载器标签要挂到**原版那一行**上：列表里显示的是原版版本号，
        # 用户关心的是"1.21.11 这个版本下我已经装了哪些加载器"
        loaders: dict[str, list] = {}
        for item in self._installed_versions:
            if not item.loaders:
                continue
            keys = {item.id}
            if item.base_version:
                keys.add(item.base_version)
            for key in keys:
                bucket = loaders.setdefault(key, [])
                for loader in item.loaders:
                    pair = (loader.kind, loader.version)
                    if pair not in bucket:
                        bucket.append(pair)
        self._model.set_loaders(loaders)

        # 不能启动的原因（缺前置版本等）也挂到原版那一行
        problems: dict = {}
        for item in self._installed_versions:
            reason = item.problem()
            if not reason:
                continue
            for key in {item.id, item.base_version or item.id}:
                problems.setdefault(key, reason)
        self._model.set_problems(problems)

    def _loader_summary_text(self) -> str:
        """状态栏那行：已安装几个 + 各加载器各几个。"""
        counts: dict = {}
        for item in getattr(self, "_installed_versions", []):
            for loader in item.loaders:
                counts[loader.name] = counts.get(loader.name, 0) + 1
        if not counts:
            return ""
        parts = "、".join(f"{name} {count}" for name, count in sorted(counts.items()))
        multiple = sum(1 for count in counts.values() if count > 1)
        tail = "（同一原版装了多个加载器）" if multiple else ""
        return "｜加载器：" + parts + tail

    def _on_versions_loaded(self, versions: list[GameVersion]) -> None:

        self._versions = versions
        self._refresh_installed()
        self._last_fs_snapshot = set(self._installed)
        self.refresh_btn.setEnabled(True)
        self._apply_filters()
        self.loading_widget.setVisible(False)
        self.status_label.setVisible(True)
        self.status_label.setText(
            f"共 {len(self._versions)} 个版本，已安装 {len(self._installed)} 个"
            f"{self._loader_summary_text()}")
        self._show_versions(self._filtered)

    def _on_load_error(self, error: str) -> None:
        self.refresh_btn.setEnabled(True)
        self.loading_widget.setVisible(False)
        self.status_label.setVisible(True)
        self.status_label.setText("加载失败")
        InfoBar.error(
            title="加载失败",
            content=f"无法获取版本清单：{error}",
            orient=InfoBarPosition.TOP,
            isClosable=True,
            duration=5000,
            parent=self,
        )

    def _show_versions(self, versions: list) -> None:
        """把过滤后的结果塞进模型（不再创建任何控件）。"""
        self._refresh_installed()
        self._model.set_versions(versions, self._installed)
        if versions:
            self.status_label.setText(
                f"共 {len(self._versions)} 个版本，已安装 {len(self._installed)} 个 | "
                f"当前显示 {len(versions)} 个{self._loader_summary_text()}")
        else:
            self.status_label.setText("没有匹配的版本")

    def _on_row_clicked(self, index) -> None:
        version = index.data(Qt.UserRole)
        if version is not None:
            self._on_version_card_clicked(version)

    def _on_row_action(self, version, action: str) -> None:
        if action == "wiki":
            self._open_version_wiki(version)
        elif action == "server":
            self._show_server_placeholder(version)

    def _on_version_card_clicked(self, version: GameVersion):
        """单击版本卡片进入下载配置页"""
        main_window = self.window()
        if hasattr(main_window, 'switch_to_download_config'):
            main_window.switch_to_download_config(version)
        else:
            InfoBar.info(
                title="下载配置",
                content=f"准备配置 {version.id}",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )

    def _open_version_wiki(self, version: GameVersion):
        """根据地区打开对应的 Minecraft Wiki"""
        from urllib.parse import quote
        country_code = cfg.countryCode.value
        version_number = version.id
        if country_code and country_code.upper() != "CN":
            url = f"https://minecraft.wiki/w/Java_Edition_{version_number}"
        else:
            # 注意: 中文"版"字后面直接跟版本号，无下划线
            url = f"https://zh.minecraft.wiki/w/Java版{quote(version_number, safe='')}"
        webbrowser.open(url)

    def _show_server_placeholder(self, version: GameVersion):
        """显示获取服务端的占位信息"""
        InfoBar.info(
            title="获取服务端",
            content=f"即将进入 {version.id} 服务端下载页 (功能开发中)",
            orient=InfoBarPosition.TOP,
            isClosable=True,
            duration=3000,
            parent=self,
        )

    # ==================== 启动逻辑 ====================

    def _on_launch(self, version: GameVersion) -> None:
        """启动游戏 - 跳转到启动进度页"""
        # 检查 Java 是否设置
        if not cfg.javaPath.value:
            InfoBar.warning(
                title="未选择 Java",
                content="请先在设置中选择 Java 运行时",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=4000,
                parent=self,
            )
            return

        # 检查版本是否已安装
        if version.id not in get_installed_versions():
            InfoBar.error(
                title="版本未安装",
                content=f"{version.id} 未安装，请先下载",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=4000,
                parent=self,
            )
            return

        main_window = self.window()
        if hasattr(main_window, "switch_to_launch"):
            main_window.switch_to_launch(version)
        else:
            InfoBar.error(
                title="错误",
                content="无法启动：主窗口引用错误",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )

    def _on_download(self, version: GameVersion) -> None:
        """下载版本（旧方法，保留兼容）"""
        self._on_version_card_clicked(version)

    def _on_download_finished(self, version_id: str, success: bool) -> None:
        """下载完成（旧方法，保留兼容）"""
        self._installed = get_installed_versions()
        if success:
            InfoBar.success(
                title="下载完成",
                content=f"{version_id} 安装成功！",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
        else:
            InfoBar.error(
                title="下载失败",
                content=f"{version_id} 安装失败，请重试",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
        self._load_versions()

    def _on_search(self, text: str) -> None:
        self._reload_timer.start()

    def _apply_filters(self) -> None:
        """按分类 + 关键字过滤（结果交给 _show_versions 塞进模型）。"""
        category_map = {
            0: VersionType.ALL,
            1: VersionType.RELEASE,
            2: VersionType.SNAPSHOT,
            3: VersionType.OLD,
        }
        category = category_map.get(self.category_combo.currentIndex(), VersionType.ALL)
        self._filtered = filter_versions(
            self._versions,
            query=self.search_box.text(),
            category=category,
        )

    def _reload_list(self) -> None:
        self._apply_filters()
        self._show_versions(self._filtered)

    def _on_category_changed(self, index: int) -> None:
        self._reload_list()