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
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Download configuration page — PCL2-style accordion menu."""

from __future__ import annotations

import threading
from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton, QLineEdit, QFrame,
)
from qfluentwidgets import (
    BodyLabel, CardWidget, InfoBar, InfoBarPosition,
    PrimaryPushButton, ComboBox,
)

from src.app.common.base_page import BasePage
from src.app.services.version_manifest import GameVersion
from src.app.services.mod_loader_service import (
    fetch_forge_versions, fetch_fabric_versions,
    filter_neoforge_by_mc_version, check_optifine,
)


# ── Loader fetch worker ──────────────────────────────────────────


class LoaderFetchWorker(QThread):
    finished = Signal(dict)
    error = Signal(str)

    def __init__(self, mc_version: str):
        super().__init__()
        self.mc_version = mc_version

    def run(self):
        try:
            result = {
                'forge': fetch_forge_versions(self.mc_version),
                'fabric': fetch_fabric_versions(self.mc_version, only_stable=False),
                'neoforge': filter_neoforge_by_mc_version(self.mc_version),
                'optifine': check_optifine(self.mc_version),
            }
            self.finished.emit(result)
        except Exception as e:
            self.error.emit(str(e))


# ── Accordion Section ────────────────────────────────────────────


class AccordionSection(QWidget):
    """A single collapsible section in the accordion."""

    def __init__(self, title: str, icon: str = "", parent=None):
        super().__init__(parent)
        self._expanded = False
        self._content: QWidget | None = None

        self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet("""
            AccordionSection {
                background: transparent;
                border-radius: 8px;
            }
        """)

        vbox = QVBoxLayout(self)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        # ── Toggle button ──
        self._header = QPushButton(self)
        self._header.setFixedHeight(44)
        self._header.setCursor(Qt.PointingHandCursor)
        self._header.setStyleSheet("""
            QPushButton {
                text-align: left;
                border: none;
                border-radius: 8px;
                padding: 0 16px;
                font-size: 14px;
                font-weight: 600;
                background: rgba(128, 128, 128, 0.06);
            }
            QPushButton:hover {
                background: rgba(128, 128, 128, 0.12);
            }
        """)
        self._header.setText(f"{icon}  {title}")

        # Arrow indicator
        self._arrow = BodyLabel("▼", self._header)
        self._arrow.setStyleSheet("color: #888; font-size: 10px;")
        header_layout = QHBoxLayout(self._header)
        header_layout.setContentsMargins(16, 0, 16, 0)
        header_layout.addStretch()
        header_layout.addWidget(self._arrow)

        vbox.addWidget(self._header)

        # ── Content container ──
        self._content_container = QWidget(self)
        self._content_layout = QVBoxLayout(self._content_container)
        self._content_layout.setContentsMargins(16, 8, 16, 12)
        self._content_layout.setSpacing(8)
        self._content_container.setVisible(False)
        vbox.addWidget(self._content_container)

        self._header.clicked.connect(self._toggle)

    def set_content_widget(self, widget: QWidget):
        """Replace the content area with a custom widget."""
        self._content_layout.addWidget(widget)

    def add_content(self, widget: QWidget):
        self._content_layout.addWidget(widget)

    def add_layout(self, layout):
        self._content_layout.addLayout(layout)

    def _toggle(self):
        self.set_expanded(not self._expanded)

    def set_expanded(self, expanded: bool, animate: bool = True):
        self._expanded = expanded
        self._arrow.setText("▲" if expanded else "▼")
        self._content_container.setVisible(expanded)
        # Update header style
        bg = "rgba(128, 128, 128, 0.12)" if expanded else "rgba(128, 128, 128, 0.06)"
        self._header.setStyleSheet(f"""
            QPushButton {{
                text-align: left; border: none; border-radius: 8px;
                padding: 0 16px; font-size: 14px; font-weight: 600;
                background: {bg};
            }}
            QPushButton:hover {{
                background: rgba(128, 128, 128, 0.12);
            }}
        """)

    def is_expanded(self) -> bool:
        return self._expanded


# ── Loader Card (version dropdown row) ───────────────────────────


class LoaderRow(QWidget):
    """Single loader row with a version combo box."""

    loader_selected = Signal(str, str)
    loader_cleared = Signal(str)

    def __init__(self, loader_type: str, display_name: str, parent=None):
        super().__init__(parent)
        self.loader_type = loader_type
        self.display_name = display_name
        self._selected_version = None
        self._versions = []
        self.is_loading = True

        self.setFixedHeight(36)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 0, 8, 0)
        layout.setSpacing(12)

        self.name_label = BodyLabel(display_name, self)
        self.name_label.setFixedWidth(64)
        layout.addWidget(self.name_label)

        self.combo = ComboBox(self)
        self.combo.setMinimumWidth(200)
        self.combo.setFixedHeight(28)
        self.combo.addItem("加载中...")
        self.combo.setEnabled(False)
        self.combo.currentTextChanged.connect(self._on_combo_changed)
        layout.addWidget(self.combo)

        layout.addStretch()

        self.check_label = BodyLabel("", self)
        self.check_label.setVisible(False)
        self.check_label.setStyleSheet("color: #52c41a; font-weight: 600;")
        layout.addWidget(self.check_label)

        self.clear_btn = QPushButton("✕", self)
        self.clear_btn.setFixedSize(20, 20)
        self.clear_btn.setStyleSheet("""
            QPushButton { border: none; color: #999; font-size: 14px; font-weight: bold; }
            QPushButton:hover { color: #ff4d4f; }
        """)
        self.clear_btn.setVisible(False)
        self.clear_btn.clicked.connect(self._clear)
        layout.addWidget(self.clear_btn)

    def set_loading(self, loading: bool):
        self.is_loading = loading
        if loading:
            self.combo.clear(); self.combo.addItem("加载中...")
            self.combo.setEnabled(False)
            self.check_label.setVisible(False)
            self.clear_btn.setVisible(False)

    def set_versions(self, versions: list, version_key: str = 'version'):
        self._versions = versions
        self.combo.clear()
        self.combo.setEnabled(True)
        self.is_loading = False
        if not versions:
            self.combo.addItem("无可用版本"); self.combo.setEnabled(False)
            return
        for v in versions:
            ver = v.get(version_key, v.get('version', ''))
            if isinstance(ver, dict):
                ver = ver.get('version', '')
            label = str(ver)
            is_beta = 'beta' in label.lower()
            self.combo.addItem(f"{label} {'(Beta)' if is_beta else ''}", userData=ver)
        if self.combo.count() > 0:
            self.combo.setCurrentIndex(0)
            self._select(self.combo.itemData(0))

    def _on_combo_changed(self, text: str):
        if not text or text in ("加载中...", "无可用版本"):
            return
        idx = self.combo.currentIndex()
        if idx >= 0:
            ver = self.combo.itemData(idx)
            if ver:
                self._select(ver)

    def _select(self, version: str):
        self._selected_version = version
        self.check_label.setText("✓ 已选"); self.check_label.setVisible(True)
        self.clear_btn.setVisible(True)
        self.loader_selected.emit(self.loader_type, version)

    def _clear(self):
        self._selected_version = None
        self.check_label.setVisible(False); self.clear_btn.setVisible(False)
        self.loader_cleared.emit(self.loader_type)

    def get_selected_version(self) -> str | None:
        return self._selected_version

    def is_selected(self) -> bool:
        return self._selected_version is not None


# ── Download Config Page ─────────────────────────────────────────


class DownloadConfigPage(BasePage):
    """版本配置页 — PCL2 风格手风琴菜单."""

    def __init__(self, version: GameVersion, parent=None):
        super().__init__(title=f"安装 {version.id}", subtitle="", parent=parent)
        self.version = version
        self._forge_versions = []
        self._fabric_versions = []
        self._neoforge_versions = []
        self._worker = None
        self._selected_loader = None
        self._selected_loader_version = None

        self._build_content()
        self._load_loader_versions_async()

    def _build_content(self):
        # ── Back button ──
        back = QPushButton("←  返回版本列表", self.view)
        back.setCursor(Qt.PointingHandCursor)
        back.setStyleSheet("""
            QPushButton { border: none; color: #0078d4; font-size: 13px; padding: 8px 0; text-align: left; }
            QPushButton:hover { color: #005a9e; }
        """)
        back.clicked.connect(self._go_back)
        self.vBoxLayout.insertWidget(0, back)

        # ── Section 1: 版本名称 ──
        self._name_section = AccordionSection("版本名称", "📝", self.view)
        self.name_input = QLineEdit(self.view)
        self.name_input.setPlaceholderText("输入自定义版本名称…")
        self.name_input.setText(self.version.id)
        self.name_input.textChanged.connect(self._on_name_manual_edit)
        self.name_input.setStyleSheet("""
            QLineEdit { border: 1px solid #d0d0d0; border-radius: 6px; padding: 8px 12px;
                        font-size: 13px; background: transparent; }
            QLineEdit:focus { border-color: #0078d4; }
        """)
        self.warning_label = BodyLabel("⚠ 不能与现有版本名相同", self.view)
        self.warning_label.setTextColor("#ff4d4f", "#ff7875")
        self.warning_label.setVisible(False)
        self._name_section.add_content(self.name_input)
        self._name_section.add_content(self.warning_label)
        self._name_section.set_expanded(True)
        self.add_content(self._name_section)

        # ── Section 2: 模组加载器 ──
        self._loader_section = AccordionSection("模组加载器", "🔧", self.view)

        # Forge
        self.forge_row = LoaderRow("forge", "Forge", self.view)
        self.forge_row.loader_selected.connect(self._on_loader_selected)
        self.forge_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.forge_row)

        self._add_separator()

        # NeoForge
        self.neoforge_row = LoaderRow("neoforge", "NeoForge", self.view)
        self.neoforge_row.loader_selected.connect(self._on_loader_selected)
        self.neoforge_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.neoforge_row)

        self._add_separator()

        # Fabric
        self.fabric_row = LoaderRow("fabric", "Fabric", self.view)
        self.fabric_row.loader_selected.connect(self._on_loader_selected)
        self.fabric_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.fabric_row)

        self._add_separator()

        # OptiFine
        of_layout = QHBoxLayout()
        of_layout.setContentsMargins(8, 4, 8, 4)
        of_label = BodyLabel("OptiFine", self.view)
        of_label.setFixedWidth(64)
        of_layout.addWidget(of_label)
        self.optifine_status = BodyLabel("检测中…", self.view)
        of_layout.addWidget(self.optifine_status)
        of_layout.addStretch()
        self._loader_section.add_layout(of_layout)

        self._loader_section.set_expanded(False)
        self.add_content(self._loader_section)

        # ── Section 3: 下载按钮 ──
        download_card = CardWidget(self.view)
        dl_layout = QHBoxLayout(download_card)
        dl_layout.setContentsMargins(0, 0, 0, 0)
        self.download_btn = PrimaryPushButton("开始下载", download_card)
        self.download_btn.setFixedHeight(44)
        self.download_btn.clicked.connect(self._on_download)
        dl_layout.addWidget(self.download_btn)
        self.add_content(download_card)

        self.add_stretch()

    def _add_separator(self):
        line = QFrame(self.view)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128,128,128,0.15); max-height: 1px; margin: 0 8px;")
        self._loader_section.add_content(line)

    # ── Loader data ─────────────────────────────────────────────

    def _load_loader_versions_async(self):
        self._worker = LoaderFetchWorker(self.version.id)
        self._worker.finished.connect(self._on_loader_loaded)
        self._worker.error.connect(self._on_loader_error)
        self._worker.start()

    def _on_loader_loaded(self, result):
        self._forge_versions = result.get('forge', [])
        self._fabric_versions = result.get('fabric', [])
        self._neoforge_versions = result.get('neoforge', [])
        has_opti = result.get('optifine', False)

        self.forge_row.set_versions(self._forge_versions, 'version')
        self.fabric_row.set_versions(self._fabric_versions, 'loader.version')
        self.neoforge_row.set_versions(self._neoforge_versions, 'version')

        self.optifine_status.setText("✅ 支持" if has_opti else "—")
        self.optifine_status.setStyleSheet(
            "color: #52c41a;" if has_opti else "color: #999;"
        )

    def _on_loader_error(self, error):
        for r in [self.forge_row, self.neoforge_row, self.fabric_row]:
            r.combo.clear(); r.combo.addItem("加载失败"); r.combo.setEnabled(False)

    # ── Loader selection ────────────────────────────────────────

    def _on_loader_selected(self, ltype: str, version: str):
        for r in [self.forge_row, self.neoforge_row, self.fabric_row]:
            if r.loader_type != ltype and r.is_selected():
                r._clear()
        self._selected_loader = ltype
        self._selected_loader_version = version
        self._update_version_name()

    def _on_loader_cleared(self, ltype: str):
        self._selected_loader = None
        self._selected_loader_version = None
        self._update_version_name()

    def _update_version_name(self):
        base = self.version.id
        if self._selected_loader and self._selected_loader_version:
            vn = f"{base}-{self._selected_loader}-{self._selected_loader_version}"
        else:
            vn = base
        if not getattr(self, '_user_edited_name', False):
            self.name_input.blockSignals(True)
            self.name_input.setText(vn)
            self.name_input.blockSignals(False)
        self.name_input.setPlaceholderText(f"自动生成: {vn}")
        self._check_version_exists(vn)

    def _check_version_exists(self, version_name: str):
        from pathlib import Path
        from src.app.common.launcher_config import cfg
        d = Path(cfg.gameDirectory.value) / "versions" / version_name
        if d.exists() and (d / f"{version_name}.jar").exists() and (d / f"{version_name}.json").exists():
            self.name_input.setStyleSheet("""
                QLineEdit { border: 2px solid #ff4d4f; border-radius: 6px; padding: 8px 12px; font-size: 13px; }
            """)
            self.warning_label.setVisible(True)
            self.download_btn.setEnabled(False)
            self.download_btn.setText("⚠️ 版本已存在")
        else:
            self.name_input.setStyleSheet("""
                QLineEdit { border: 1px solid #d0d0d0; border-radius: 6px; padding: 8px 12px;
                            font-size: 13px; background: transparent; }
                QLineEdit:focus { border-color: #0078d4; }
            """)
            self.warning_label.setVisible(False)
            self.download_btn.setEnabled(True)
            self.download_btn.setText("开始下载")

    def _on_name_manual_edit(self, text: str):
        ph = self.name_input.placeholderText()
        self._user_edited_name = bool(text and text != ph and text != self.version.id)
        # 每次用户打字都检测重名
        self._check_version_exists(text.strip())

    def _go_back(self):
        mw = self.window()
        if hasattr(mw, 'go_back_to_versions'):
            mw.go_back_to_versions()
        else:
            self.parent().switchTo(self.parent().versions_page)

    def _on_download(self):
        vn = self.name_input.text().strip()
        if not vn:
            InfoBar.warning(title="请输入版本名称", content="版本名称不能为空",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)
            return

        from pathlib import Path
        from src.app.common.launcher_config import cfg
        d = Path(cfg.gameDirectory.value) / "versions" / vn
        if d.exists() and (d / f"{vn}.jar").exists() and (d / f"{vn}.json").exists():
            InfoBar.warning(title="版本已存在", content=f"版本 '{vn}' 已经安装，请使用不同的版本名称",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=5000, parent=self)
            self.name_input.setStyleSheet("""
                QLineEdit { border: 2px solid #ff4d4f; border-radius: 6px; padding: 8px 12px; font-size: 13px; }
            """)
            return

        self.name_input.setStyleSheet("""
            QLineEdit { border: 1px solid #d0d0d0; border-radius: 6px; padding: 8px 12px;
                        font-size: 13px; background: transparent; }
            QLineEdit:focus { border-color: #0078d4; }
        """)

        lt, lv = "none", None
        if self._selected_loader == 'forge':
            lv = self.forge_row.get_selected_version()
            if lv: lt = 'forge'
        elif self._selected_loader == 'neoforge':
            lv = self.neoforge_row.get_selected_version()
            if lv: lt = 'neoforge'
        elif self._selected_loader == 'fabric':
            lv = self.fabric_row.get_selected_version()
            if lv: lt = 'fabric'

        mw = self.window()
        if hasattr(mw, 'switch_to_download_progress'):
            mw.switch_to_download_progress(version=self.version, version_name=vn,
                                           loader_type=lt, loader_version=lv)
        else:
            self._do_download_fallback(vn, lt, lv)

    def _do_download_fallback(self, vn: str, lt: str, lv: str):
        self.download_btn.setEnabled(False)
        self.download_btn.setText("安装中…")
        InfoBar.info(title="开始安装", content=f"正在安装 {vn}，请稍候…",
                     orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)

        def work():
            from src.app.services.download_service import VersionInstaller
            from PySide6.QtCore import QTimer
            inst = VersionInstaller()
            inst.set_progress_callback(lambda c, t, s: QTimer.singleShot(0, lambda: self._update_progress(c, t, s)))
            ok = inst.install_version(self.version, loader_type=lt, loader_version=lv, version_name=vn)
            QTimer.singleShot(0, lambda: self._on_install_finished(ok, vn))
        threading.Thread(target=work, daemon=True).start()

    def _update_progress(self, cur: int, total: int, status: str):
        if total > 0:
            self.download_btn.setText(f"{status} ({cur * 100 // total}%)")

    def _on_install_finished(self, ok: bool, vn: str):
        self.download_btn.setEnabled(True)
        if ok:
            self.download_btn.setText("✅ 安装完成")
            InfoBar.success(title="安装成功", content=f"{vn} 安装完成！",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)
        else:
            self.download_btn.setText("❌ 安装失败")
            InfoBar.error(title="安装失败", content=f"{vn} 安装失败，请查看控制台日志",
                          orient=InfoBarPosition.TOP, isClosable=True, duration=5000, parent=self)
