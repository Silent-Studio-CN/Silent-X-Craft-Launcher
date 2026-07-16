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

import threading
import webbrowser
import sys
import platform
import re
import subprocess
from pathlib import Path

from PySide6.QtCore import Qt, QThread, Signal, QMetaObject, QEvent
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget, QPushButton
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    ComboBox,
    FlowLayout,
    InfoBar,
    InfoBarPosition,
    PrimaryPushButton,
    PushButton,
    SearchLineEdit,
    StrongBodyLabel,
)

from src.app.common.base_page import BasePage
from src.app.common.launcher_config import cfg
from src.app.services.version_manifest import (
    fetch_version_manifest,
    filter_versions,
    VersionType,
    GameVersion,
)
from src.app.services.download_service import VersionInstaller, get_installed_versions, get_version_info


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

        self._build_content()
        self._load_versions()

        # ── 定时检测文件变化 ──
        from PySide6.QtCore import QTimer
        self._fs_watch_timer = QTimer(self)
        self._fs_watch_timer.setInterval(8000)  # 每 8 秒
        self._fs_watch_timer.timeout.connect(self._check_fs_changes)
        self._fs_watch_timer.start()

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
        self.version_container = QWidget(self.view)
        self.version_layout = QVBoxLayout(self.version_container)
        self.version_layout.setContentsMargins(0, 0, 0, 0)
        self.version_layout.setSpacing(8)

        self.status_label = BodyLabel("正在加载版本清单…", self.view)

        self.add_content(toolbar)
        self.add_content(self.status_label)
        self.add_content(self.version_container)
        self.add_stretch()

    def _load_versions(self) -> None:
        self.status_label.setText("正在加载版本清单…")
        self._clear_version_cards()
        self.refresh_btn.setEnabled(False)

        source = cfg.downloadSource.value
        self._worker = FetchWorker(source)
        self._worker.finished.connect(self._on_versions_loaded)
        self._worker.error.connect(self._on_load_error)
        self._worker.start()

    def _on_versions_loaded(self, versions: list[GameVersion]) -> None:
        self._versions = versions
        self._installed = get_installed_versions()
        self._last_fs_snapshot = set(self._installed)
        self.refresh_btn.setEnabled(True)
        self._apply_filters()
        self.status_label.setText(f"共 {len(self._versions)} 个版本，已安装 {len(self._installed)} 个")
        self._show_versions(self._filtered)

    def _check_fs_changes(self):
        """每 8 秒检测文件系统变化（用户手动删/增版本后自动刷新）"""
        if not self._versions:
            return
        current = set(get_installed_versions())
        last = getattr(self, "_last_fs_snapshot", None)
        if last is not None and current != last:
            self._last_fs_snapshot = current
            self._installed = list(current)
            self._show_versions(self._filtered)
            self.status_label.setText(f"共 {len(self._versions)} 个版本，已安装 {len(self._installed)} 个")

    def _on_load_error(self, error: str) -> None:
        self.refresh_btn.setEnabled(True)
        self.status_label.setText("加载失败")
        InfoBar.error(
            title="加载失败",
            content=f"无法获取版本清单：{error}",
            orient=InfoBarPosition.TOP,
            isClosable=True,
            duration=5000,
            parent=self,
        )

    def _clear_version_cards(self) -> None:
        """安全清理所有版本卡片"""
        for child in self.version_container.children():
            if isinstance(child, CardWidget):
                child.deleteLater()
        
        while self.version_layout.count() > 0:
            item = self.version_layout.takeAt(0)
            if item and hasattr(item, 'widget'):
                widget = item.widget()
                if widget:
                    widget.deleteLater()

    def _apply_filters(self) -> None:
        category_map = {
            0: VersionType.ALL,
            1: VersionType.RELEASE,
            2: VersionType.SNAPSHOT,
            3: VersionType.OLD,
        }
        category = category_map.get(self.category_combo.currentIndex(), VersionType.ALL)
        query = self.search_box.text()

        self._filtered = filter_versions(
            self._versions,
            query=query,
            category=category,
        )

    def _show_versions(self, versions: list[GameVersion]) -> None:
        """显示版本列表 - 一行一个"""
        self._clear_version_cards()

        if not versions:
            empty_label = BodyLabel("没有匹配的版本", self.version_container)
            self.version_layout.addWidget(empty_label)
            return

        # 更新已安装列表
        self._installed = get_installed_versions()

        for version in versions:
            card = self._create_version_card(version)
            self.version_layout.addWidget(card)

    def _create_version_card(self, version: GameVersion) -> CardWidget:
        """创建单个版本卡片 - 整个卡片可点击进入配置页"""
        card = CardWidget(self.version_container)
        card.setFixedHeight(52)
        card.setAttribute(Qt.WA_Hover, True)
        card.setCursor(Qt.PointingHandCursor)
        card.version_id = version.id

        layout = QHBoxLayout(card)
        layout.setContentsMargins(16, 6, 16, 6)
        layout.setSpacing(12)

        # ---- 点击事件由 eventFilter 处理 ----

        # 版本号
        name_label = StrongBodyLabel(version.id, card)
        name_label.setFixedWidth(140)
        layout.addWidget(name_label)

        # 版本类型
        type_map = {"release": "正式版", "snapshot": "快照", "old": "旧版"}
        type_label = BodyLabel(type_map.get(version.version_type, version.version_type), card)
        type_label.setTextColor("#666666", "#999999")
        type_label.setFixedWidth(60)
        layout.addWidget(type_label)

        # 发布日期
        date_label = BodyLabel(version.release_label, card)
        date_label.setTextColor("#888888", "#888888")
        date_label.setFixedWidth(150)
        layout.addWidget(date_label)

        # 弹簧撑开
        layout.addStretch(1)

        # --- 操作按钮（"版本日志"和"获取服务端"） ---
        # 单击卡片任意位置进入配置页（card_click_btn 处理）
        button_widget = QWidget(card)
        button_widget.setVisible(False)
        button_layout = QHBoxLayout(button_widget)
        button_layout.setContentsMargins(0, 0, 0, 0)
        button_layout.setSpacing(8)

        # "版本日志"按钮
        log_btn = QPushButton("📜 版本日志")
        log_btn.setFixedSize(80, 28)
        log_btn.clicked.connect(lambda checked, v=version: self._open_version_wiki(v))
        button_layout.addWidget(log_btn)

        # "获取服务端"按钮
        server_btn = QPushButton("🖥 获取服务端")
        server_btn.setFixedSize(90, 28)
        server_btn.clicked.connect(lambda checked, v=version: self._show_server_placeholder(v))
        button_layout.addWidget(server_btn)

        # 把按钮容器放在透明按钮上面
        button_widget.raise_()

        layout.addWidget(button_widget)

        card.button_widget = button_widget

        card.installEventFilter(self)

        return card

    def eventFilter(self, obj, event):
        """事件过滤器：处理卡片点击和悬停显示按钮"""
        if isinstance(obj, CardWidget):
            if event.type() == QEvent.MouseButtonRelease and event.button() == Qt.MouseButton.LeftButton:
                # 点击卡片进入配置页（排除按钮点击）
                if hasattr(obj, 'version_id'):
                    v = next((v for v in self._versions if v.id == obj.version_id), None)
                    if v and not self._on_version_card_clicked(v):
                        pass
                return True
            if hasattr(obj, 'button_widget'):
                if event.type() == QEvent.Enter:
                    obj.button_widget.setVisible(True)
                elif event.type() == QEvent.Leave:
                    obj.button_widget.setVisible(False)
        return super().eventFilter(obj, event)

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

    # ==================== Java 版本检测服务 ====================
    
    def _detect_java_version(self, java_path: str) -> tuple[int, str]:
        """检测 Java 版本，返回 (主版本号, 完整版本字符串)"""
        try:
            result = subprocess.run(
                [java_path, '-version'],
                capture_output=True,
                text=True,
                timeout=5,
            )
            output = result.stderr + result.stdout
            
            # 匹配版本号
            match = re.search(r'version "(\d+)', output)
            if match:
                major = int(match.group(1))
                return major, output
            return 0, output
        except Exception as e:
            print(f"[JavaVersion] 检测失败: {e}")
            return 0, ""
    
    def _get_supported_jvm_args(self, java_major: int) -> list:
        """根据 Java 版本返回支持的 JVM 参数"""
        args = [
            "--enable-native-access=ALL-UNNAMED",
            "-XX:+UnlockExperimentalVMOptions",
            "-XX:+UseG1GC",
            "-XX:G1NewSizePercent=20",
            "-XX:G1ReservePercent=20",
            "-XX:G1HeapRegionSize=32M",
            "-XX:MaxGCPauseMillis=50",
            "-XX:+PerfDisableSharedMem",
            "-XX:MinHeapFreeRatio=25",
            "-XX:MaxHeapFreeRatio=40",
            "-XX:-OmitStackTraceInFastThrow",
            "-Djdk.lang.Process.allowAmbiguousCommands=True",
            "-Dfml.ignoreInvalidMinecraftCertificates=True",
            "-Dfml.ignorePatchDiscrepancies=True",
        ]
        
        # Java 9+ 支持
        if java_major >= 9:
            args.append("--sun-misc-unsafe-memory-access=allow")
        
        # Java 23+ 支持 CompactObjectHeaders
        if java_major >= 23:
            args.append("-XX:+UseCompactObjectHeaders")
        
        return args
    
    def _is_java_compatible(self, java_major: int, mc_version: str) -> tuple[bool, str]:
        """检查 Java 版本是否与 Minecraft 版本兼容"""
        required = 17  # 默认
        try:
            parts = mc_version.split('.')
            if len(parts) >= 2:
                if int(parts[0]) >= 26:
                    required = 25
                elif int(parts[0]) >= 21:
                    required = 21
                elif int(parts[0]) == 1 and int(parts[1]) >= 21:
                    required = 21
                elif int(parts[0]) == 1 and int(parts[1]) >= 17:
                    required = 17
        except:
            pass
        
        if java_major >= required:
            return True, f"Java {java_major} 兼容 (需要 Java {required}+)"
        else:
            return False, f"Java {java_major} 版本过低 (需要 Java {required}+)"

    # ==================== 平台检测和库兼容性 ====================
    
    def _detect_platform(self):
        """检测当前平台信息"""
        system = sys.platform
        is_windows = system.startswith('win')
        is_macos = system.startswith('darwin')
        is_linux = system.startswith('linux')
        arch = platform.machine().lower()
        is_arm64 = arch in ['arm64', 'aarch64']
        return is_windows, is_macos, is_linux, is_arm64

    def _is_lib_compatible(self, lib: dict, is_windows: bool, is_macos: bool, is_linux: bool, is_arm64: bool) -> bool:
        """完整库兼容性检测 - 支持 os 和 arch 规则"""
        rules = lib.get('rules', [])
        if not rules:
            return True
        
        current_os = None
        if is_windows:
            current_os = 'windows'
        elif is_macos:
            current_os = 'osx'
        elif is_linux:
            current_os = 'linux'
        
        allow = False
        for rule in rules:
            action = rule.get('action', 'allow')
            os_info = rule.get('os', {})
            
            # 检查操作系统
            rule_os = os_info.get('name')
            if rule_os and rule_os != current_os:
                continue
            
            # 检查架构
            rule_arch = os_info.get('arch')
            if rule_arch:
                if rule_arch == 'arm64' and not is_arm64:
                    continue
                if rule_arch == 'x86' and is_arm64:
                    continue
            
            allow = action == 'allow'
        
        return allow

    def _get_native_key(self, natives: dict, is_windows: bool, is_macos: bool, is_linux: bool, is_arm64: bool) -> str | None:
        """根据平台获取 natives 键名"""
        if is_windows:
            if is_arm64 and 'natives-windows-arm64' in natives:
                return 'natives-windows-arm64'
            if 'natives-windows' in natives:
                return 'natives-windows'
        elif is_macos:
            if is_arm64 and 'natives-osx-arm64' in natives:
                return 'natives-osx-arm64'
            if 'natives-osx' in natives:
                return 'natives-osx'
        elif is_linux:
            if is_arm64 and 'natives-linux-arm64' in natives:
                return 'natives-linux-arm64'
            if 'natives-linux' in natives:
                return 'natives-linux'
        return None

    def _build_classpath(self, version_info: dict, game_dir: Path, is_windows: bool, is_macos: bool, is_linux: bool, is_arm64: bool) -> tuple[list[str], int]:
        """构建 classpath 列表和分隔符"""
        libraries_dir = game_dir / "libraries"
        classpath_entries = []
        lib_count = 0

        for lib in version_info.get('libraries', []):
            if not self._is_lib_compatible(lib, is_windows, is_macos, is_linux, is_arm64):
                continue
            downloads = lib.get('downloads', {})
            artifact = downloads.get('artifact')
            natives = downloads.get('classifiers') or {}
            native_key = self._get_native_key(natives, is_windows, is_macos, is_linux, is_arm64)

            if native_key and native_key in natives:
                path = natives[native_key].get('path')
                if path:
                    lib_file = libraries_dir / path
                    if lib_file.exists():
                        classpath_entries.append(str(lib_file))
                        lib_count += 1
            elif artifact:
                path = artifact.get('path')
                if path:
                    lib_file = libraries_dir / path
                    if lib_file.exists():
                        classpath_entries.append(str(lib_file))
                        lib_count += 1

        # 游戏 jar 放在最后
        return classpath_entries, lib_count

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
        self._apply_filters()
        self._show_versions(self._filtered)

    def _on_category_changed(self, index: int) -> None:
        self._apply_filters()
        self._show_versions(self._filtered)