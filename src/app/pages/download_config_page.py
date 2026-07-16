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
"""Download configuration page with PCL2-style loader cards."""

from __future__ import annotations

import threading
from PySide6.QtCore import Qt, QThread, Signal, QEvent
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton, 
    QLineEdit, QLabel, QFrame
)
from qfluentwidgets import (
    BodyLabel, CardWidget, InfoBar, InfoBarPosition,
    PrimaryPushButton, PushButton, StrongBodyLabel, ComboBox
)

from src.app.common.base_page import BasePage
from src.app.services.version_manifest import GameVersion
from src.app.services.mod_loader_service import (
    fetch_forge_versions,
    fetch_fabric_versions,
    filter_neoforge_by_mc_version,
    check_optifine,
)


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


class LoaderCard(QWidget):
    """单个加载器卡片 - 带下拉列表选择版本"""
    
    loader_selected = Signal(str, str)   # loader_type, version
    loader_cleared = Signal(str)         # loader_type
    
    def __init__(self, loader_type: str, display_name: str, parent=None):
        super().__init__(parent)
        self.loader_type = loader_type
        self.display_name = display_name
        self.versions = []
        self.selected_version = None
        self.is_selected = False
        self.is_loading = True
        
        self.setFixedHeight(44)
        self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet("""
            LoaderCard {
                background-color: transparent;
                border-radius: 4px;
            }
            LoaderCard:hover {
                background-color: rgba(0, 0, 0, 0.05);
            }
        """)
        
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 0, 8, 0)
        layout.setSpacing(12)
        
        # 名称
        self.name_label = BodyLabel(display_name, self)
        self.name_label.setFixedWidth(64)
        self.name_label.setStyleSheet("font-weight: 500;")
        layout.addWidget(self.name_label)
        
        # 版本下拉列表
        self.version_combo = ComboBox(self)
        self.version_combo.setMinimumWidth(200)
        self.version_combo.setFixedHeight(28)
        self.version_combo.setStyleSheet("""
            QComboBox {
                border: 1px solid #d0d0d0;
                border-radius: 4px;
                padding: 2px 8px;
                font-size: 12px;
            }
            QComboBox:hover {
                border-color: #0078d4;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
        """)
        self.version_combo.addItem("加载中...")
        self.version_combo.setEnabled(False)
        self.version_combo.currentTextChanged.connect(self._on_combo_changed)
        layout.addWidget(self.version_combo)
        
        layout.addStretch(1)
        
        # 选中状态标签
        self.status_label = BodyLabel("", self)
        self.status_label.setFixedWidth(60)
        self.status_label.setVisible(False)
        self.status_label.setStyleSheet("color: #52c41a;")
        layout.addWidget(self.status_label)
        
        # 取消选择按钮
        self.clear_btn = QPushButton("✕", self)
        self.clear_btn.setFixedSize(20, 20)
        self.clear_btn.setStyleSheet("""
            QPushButton {
                border: none;
                color: #999999;
                font-size: 14px;
                font-weight: bold;
            }
            QPushButton:hover {
                color: #ff4d4f;
            }
        """)
        self.clear_btn.setVisible(False)
        self.clear_btn.clicked.connect(self._on_clear_clicked)
        layout.addWidget(self.clear_btn)
    
    def set_loading(self, loading: bool):
        self.is_loading = loading
        if loading:
            self.version_combo.clear()
            self.version_combo.addItem("加载中...")
            self.version_combo.setEnabled(False)
            self.status_label.setVisible(False)
            self.clear_btn.setVisible(False)
    
    def set_versions(self, versions: list, version_key: str = 'version'):
        self.versions = versions
        
        self.version_combo.clear()
        self.version_combo.setEnabled(True)
        self.is_loading = False
        
        if not versions:
            self.version_combo.addItem("无可用版本")
            self.version_combo.setEnabled(False)
            return
        
        for v in versions:
            display = self._extract_version(v, version_key)
            if display:
                version_str = v.get('version', '')
                is_beta = '-beta' in version_str or 'beta' in version_str.lower()
                label = f"{display} {'(Beta)' if is_beta else ''}"
                self.version_combo.addItem(label, userData=display)
        
        if self.version_combo.count() > 0:
            self.version_combo.setCurrentIndex(0)
            first_version = self._extract_version(versions[0], version_key)
            if first_version:
                self._select_version(first_version)
    
    def _extract_version(self, v: dict, version_key: str) -> str:
        if version_key == 'version':
            return v.get('version', '')
        elif version_key == 'loader.version':
            return v.get('loader', {}).get('version', '')
        else:
            parts = version_key.split('.')
            value = v
            for p in parts:
                if isinstance(value, dict):
                    value = value.get(p, '')
                else:
                    return ''
            return str(value) if value else ''
    
    def _on_combo_changed(self, text: str):
        if not text or text == "加载中..." or text == "无可用版本":
            return
        index = self.version_combo.currentIndex()
        if index < 0:
            return
        version = self.version_combo.itemData(index)
        if version:
            self._select_version(version)
    
    def _select_version(self, version: str):
        self.selected_version = version
        self.is_selected = True
        self.status_label.setText("✓ 已选")
        self.status_label.setVisible(True)
        self.clear_btn.setVisible(True)
        self.loader_selected.emit(self.loader_type, version)
    
    def _on_clear_clicked(self):
        self.clear_selection()
        self.loader_cleared.emit(self.loader_type)
    
    def clear_selection(self):
        self.is_selected = False
        self.selected_version = None
        self.status_label.setVisible(False)
        self.clear_btn.setVisible(False)
        if self.version_combo.count() > 0:
            self.version_combo.setCurrentIndex(0)
    
    def get_selected_version(self) -> str:
        return self.selected_version
    
    def set_enabled(self, enabled: bool):
        self.setEnabled(enabled)
        if not enabled:
            self.setStyleSheet("""
                LoaderCard {
                    background-color: transparent;
                    border-radius: 4px;
                }
            """)
            self.name_label.setStyleSheet("font-weight: 500;")
            self.version_combo.setEnabled(False)
        else:
            self.setStyleSheet("""
                LoaderCard {
                    background-color: transparent;
                    border-radius: 4px;
                }
                LoaderCard:hover {
                    background-color: rgba(0, 0, 0, 0.05);
                }
            """)
            self.name_label.setStyleSheet("font-weight: 500;")
            if not self.is_loading and self.versions:
                self.version_combo.setEnabled(True)


class DownloadConfigPage(BasePage):
    """下载配置页 - PCL2 扁平卡片风格"""
    
    def __init__(self, version: GameVersion, parent=None):
        super().__init__(
            title=f"安装 {version.id}",
            subtitle="配置版本名称和模组加载器",
            parent=parent,
        )
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
        # ---- 返回按钮 ----
        back_btn = PushButton("← 返回版本列表", self.view)
        back_btn.clicked.connect(self._go_back)
        self.vBoxLayout.insertWidget(0, back_btn)
        
        # ---- 版本名称 ----
        name_card = CardWidget(self.view)
        name_layout = QVBoxLayout(name_card)
        name_layout.setContentsMargins(16, 12, 16, 12)
        name_layout.setSpacing(8)
        
        name_label = BodyLabel("版本名称", name_card)
        name_layout.addWidget(name_label)
        
        self.name_input = QLineEdit(name_card)
        self.name_input.setPlaceholderText("输入自定义版本名称...")
        self.name_input.setText(self.version.id)
        self.name_input.textChanged.connect(self._on_name_manual_edit)
        name_layout.addWidget(self.name_input)
        
        # 版本名重复警告（默认隐藏）
        self.warning_label = BodyLabel("⚠ 不能与现有版本名相同", name_card)
        self.warning_label.setTextColor("#ff4d4f", "#ff7875")
        self.warning_label.setVisible(False)
        name_layout.addWidget(self.warning_label)
        
        self.add_content(name_card)
        
        # ---- 模组加载器 ----
        loader_card = CardWidget(self.view)
        loader_card.setStyleSheet("CardWidget { background: transparent; border: none; }")
        loader_layout = QVBoxLayout(loader_card)
        loader_layout.setContentsMargins(0, 4, 0, 4)
        loader_layout.setSpacing(0)
        
        loader_title = StrongBodyLabel("模组加载器 (三选一)", loader_card)
        loader_layout.addWidget(loader_title)
        loader_layout.addSpacing(8)
        
        line = QFrame(loader_card)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128, 128, 128, 0.2); max-height: 1px;")
        loader_layout.addWidget(line)
        
        # Forge
        self.forge_widget = LoaderCard("forge", "Forge", loader_card)
        self.forge_widget.loader_selected.connect(self._on_loader_selected)
        self.forge_widget.loader_cleared.connect(self._on_loader_cleared)
        loader_layout.addWidget(self.forge_widget)
        
        line = QFrame(loader_card)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128, 128, 128, 0.2); max-height: 1px; margin-left: 72px;")
        loader_layout.addWidget(line)
        
        # NeoForge
        self.neoforge_widget = LoaderCard("neoforge", "NeoForge", loader_card)
        self.neoforge_widget.loader_selected.connect(self._on_loader_selected)
        self.neoforge_widget.loader_cleared.connect(self._on_loader_cleared)
        loader_layout.addWidget(self.neoforge_widget)
        
        line = QFrame(loader_card)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128, 128, 128, 0.2); max-height: 1px; margin-left: 72px;")
        loader_layout.addWidget(line)
        
        # Fabric
        self.fabric_widget = LoaderCard("fabric", "Fabric", loader_card)
        self.fabric_widget.loader_selected.connect(self._on_loader_selected)
        self.fabric_widget.loader_cleared.connect(self._on_loader_cleared)
        loader_layout.addWidget(self.fabric_widget)
        
        line = QFrame(loader_card)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128, 128, 128, 0.2); max-height: 1px;")
        loader_layout.addWidget(line)
        
        # OptiFine
        optifine_layout = QHBoxLayout()
        optifine_layout.setContentsMargins(8, 8, 8, 4)
        optifine_label = BodyLabel("OptiFine:", loader_card)
        optifine_layout.addWidget(optifine_label)
        
        self.optifine_status = BodyLabel("检测中...", loader_card)
        optifine_layout.addWidget(self.optifine_status)
        optifine_layout.addStretch(1)
        loader_layout.addLayout(optifine_layout)
        
        self.add_content(loader_card)
        
        # ---- 下载按钮 ----
        self.download_btn = PrimaryPushButton("开始下载", self.view)
        self.download_btn.clicked.connect(self._on_download)
        self.add_content(self.download_btn)
        
        self.add_stretch()
    
    def _load_loader_versions_async(self):
        self._worker = LoaderFetchWorker(self.version.id)
        self._worker.finished.connect(self._on_loader_loaded)
        self._worker.error.connect(self._on_loader_error)
        self._worker.start()
    
    def _on_loader_loaded(self, result):
        self._forge_versions = result.get('forge', [])
        self._fabric_versions = result.get('fabric', [])
        self._neoforge_versions = result.get('neoforge', [])
        has_optifine = result.get('optifine', False)
        
        self.forge_widget.set_versions(self._forge_versions, 'version')
        self.fabric_widget.set_versions(self._fabric_versions, 'loader.version')
        self.neoforge_widget.set_versions(self._neoforge_versions, 'version')
        
        if has_optifine:
            self.optifine_status.setText("有")
            self.optifine_status.setStyleSheet("color: #52c41a;")
        else:
            self.optifine_status.setText("无")
            self.optifine_status.setStyleSheet("color: #cccccc;")
        
        for w in [self.forge_widget, self.neoforge_widget, self.fabric_widget]:
            w.set_enabled(True)
            w.is_loading = False
    
    def _on_loader_error(self, error):
        for w in [self.forge_widget, self.neoforge_widget, self.fabric_widget]:
            w.set_loading(False)
            w.version_combo.clear()
            w.version_combo.addItem("加载失败")
    
    def _on_loader_selected(self, loader_type: str, version: str):
        """某个加载器被选中"""
        for w in [self.forge_widget, self.neoforge_widget, self.fabric_widget]:
            if w.loader_type == loader_type:
                w.set_enabled(True)
            else:
                w.set_enabled(False)
                if w.is_selected:
                    w.clear_selection()
        
        self._selected_loader = loader_type
        self._selected_loader_version = version
        
        # 自动更新版本名称
        self._update_version_name()
        print(f"[DownloadConfig] 选择 {loader_type} 版本 {version}")
    
    def _on_loader_cleared(self, loader_type: str):
        """取消选择某个加载器"""
        for w in [self.forge_widget, self.neoforge_widget, self.fabric_widget]:
            w.set_enabled(True)
        
        self._selected_loader = None
        self._selected_loader_version = None
        
        # 自动更新版本名称
        self._update_version_name()
        print(f"[DownloadConfig] 取消选择 {loader_type}")
    
    def _update_version_name(self):
        """根据选择的加载器自动生成版本名称"""
        base_version = self.version.id
        
        if self._selected_loader and self._selected_loader_version:
            loader_name_map = {
                'forge': 'forge',
                'neoforge': 'neoforge',
                'fabric': 'fabric'
            }
            loader_name = loader_name_map.get(self._selected_loader, self._selected_loader)
            version_name = f"{base_version}-{loader_name}-{self._selected_loader_version}"
        else:
            version_name = base_version
        
        # 更新输入框（如果用户没有手动编辑）
        if not hasattr(self, '_user_edited_name') or not self._user_edited_name:
            self.name_input.blockSignals(True)
            self.name_input.setText(version_name)
            self.name_input.blockSignals(False)
        
        # 更新占位提示
        self.name_input.setPlaceholderText(f"自动生成: {version_name}")

        self._check_version_exists(version_name)

    def _check_version_exists(self, version_name: str):
        """检查版本是否已存在，显示文字提示"""
        from pathlib import Path
        from src.app.common.launcher_config import cfg
    
        game_dir = Path(cfg.gameDirectory.value)
        version_dir = game_dir / "versions" / version_name
        jar_path = version_dir / f"{version_name}.jar"
        json_path = version_dir / f"{version_name}.json"
    
        if version_dir.exists() and jar_path.exists() and json_path.exists():
            self.name_input.setStyleSheet("""
                QLineEdit {
                    border: 2px solid #ff4d4f;
                    border-radius: 4px;
                    padding: 4px 8px;
                }
            """)
            self.name_input.setToolTip(f"版本 '{version_name}' 已存在")
            self.warning_label.setVisible(True)
            self.download_btn.setEnabled(False)
            self.download_btn.setText("⚠️ 版本已存在")
        else:
            self.name_input.setStyleSheet("")
            self.name_input.setToolTip("")
            self.warning_label.setVisible(False)
            self.download_btn.setEnabled(True)
            self.download_btn.setText("开始下载")
    
    def _on_name_manual_edit(self, text: str):
        """用户手动编辑版本名称"""
        # 如果用户手动修改了，标记为手动编辑
        placeholder = self.name_input.placeholderText()
        if text and text != placeholder and text != self.version.id:
            self._user_edited_name = True
        else:
            self._user_edited_name = False
    
    def _go_back(self):
        main_window = self.window()
        if hasattr(main_window, 'go_back_to_versions'):
            main_window.go_back_to_versions()
        else:
            self.parent().switchTo(self.parent().versions_page)
    
    def _on_download(self):
        """点击下载按钮 - 跳转到进度页"""
        version_name = self.name_input.text().strip()
        if not version_name:
            InfoBar.warning(
                title="请输入版本名称",
                content="版本名称不能为空",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
            return
        
        from pathlib import Path
        from src.app.common.launcher_config import cfg
    
        game_dir = Path(cfg.gameDirectory.value)
        version_dir = game_dir / "versions" / version_name
        jar_path = version_dir / f"{version_name}.jar"
        json_path = version_dir / f"{version_name}.json"
    
        if version_dir.exists() and jar_path.exists() and json_path.exists():
            InfoBar.warning(
                title="版本已存在",
                content=f"版本 '{version_name}' 已经安装，请使用不同的版本名称",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=5000,
                parent=self,
            )
            # 高亮输入框
            self.name_input.setStyleSheet("""
                QLineEdit {
                    border: 2px solid #ff4d4f;
                    border-radius: 4px;
                    padding: 4px 8px;
                }
            """)
            return
    
        self.name_input.setStyleSheet("")
        
        loader_type = "none"
        loader_version = None
        
        if self._selected_loader == 'forge':
            loader_version = self.forge_widget.get_selected_version()
            if loader_version:
                loader_type = 'forge'
        elif self._selected_loader == 'neoforge':
            loader_version = self.neoforge_widget.get_selected_version()
            if loader_version:
                loader_type = 'neoforge'
        elif self._selected_loader == 'fabric':
            loader_version = self.fabric_widget.get_selected_version()
            if loader_version:
                loader_type = 'fabric'
        
        print(f"[DownloadConfig] 开始下载: {version_name}, 加载器: {loader_type}, 版本: {loader_version}")
        
        # 跳转到进度页
        main_window = self.window()
        if hasattr(main_window, 'switch_to_download_progress'):
            main_window.switch_to_download_progress(
                version=self.version,
                version_name=version_name,
                loader_type=loader_type,
                loader_version=loader_version,
            )
        else:
            # 降级方案：直接下载（不跳转）
            self._do_download_fallback(version_name, loader_type, loader_version)
    
    def _do_download_fallback(self, version_name: str, loader_type: str, loader_version: str):
        """降级方案：直接下载（不跳转进度页）"""
        self.download_btn.setEnabled(False)
        self.download_btn.setText("安装中...")
        
        InfoBar.info(
            title="开始安装",
            content=f"正在安装 {version_name}，请稍候...",
            orient=InfoBarPosition.TOP,
            isClosable=True,
            duration=3000,
            parent=self,
        )
        
        def install_thread():
            from src.app.services.download_service import VersionInstaller
            from PySide6.QtCore import QTimer
            
            installer = VersionInstaller()
            
            def on_progress(current, total, status):
                QTimer.singleShot(0, lambda: self._update_progress(current, total, status))
            
            installer.set_progress_callback(on_progress)
            success = installer.install_version(
                self.version,
                loader_type=loader_type,
                loader_version=loader_version,
                version_name=version_name
            )
            
            QTimer.singleShot(0, lambda: self._on_install_finished(success, version_name))
        
        threading.Thread(target=install_thread, daemon=True).start()
    
    def _update_progress(self, current: int, total: int, status: str):
        if total > 0:
            progress = int(current / total * 100)
            self.download_btn.setText(f"{status} ({progress}%)")
    
    def _on_install_finished(self, success: bool, version_name: str):
        self.download_btn.setEnabled(True)
        if success:
            self.download_btn.setText("✅ 安装完成")
            InfoBar.success(
                title="安装成功",
                content=f"{version_name} 安装完成！",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
        else:
            self.download_btn.setText("❌ 安装失败")
            InfoBar.error(
                title="安装失败",
                content=f"{version_name} 安装失败，请查看控制台日志",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=5000,
                parent=self,
            )