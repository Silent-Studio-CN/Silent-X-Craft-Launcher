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
"""Download progress page - Visual Studio Installer style."""

from __future__ import annotations

import json
import time
import threading
from pathlib import Path
from typing import Optional, List

from PySide6.QtCore import Qt, QTimer, QThread, Signal
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget, QProgressBar, QFrame, QLabel
from PySide6.QtGui import QPainter, QColor, QPen, QBrush
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    InfoBar,
    InfoBarPosition,
    PrimaryPushButton,
    PushButton,
    StrongBodyLabel,
)

from src.app.common.base_page import BasePage
from src.app.common.launcher_config import cfg
from src.app.common.logger import log, log_exception
from src.app.services.version_manifest import GameVersion, fetch_version_manifest
from src.app.services.download.download_engine import DownloadEngine


class StageIndicator(QWidget):
    """阶段指示器 - 转圈/对号"""
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(20, 20)
        self._state = "idle"  # idle, spinning, done, failed
        self._angle = 0
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._update_angle)
        self._timer.setInterval(50)
    
    def set_state(self, state: str):
        """设置状态: idle, spinning, done, failed"""
        self._state = state
        if state == "spinning":
            self._timer.start()
        else:
            self._timer.stop()
        self.update()
    
    def _update_angle(self):
        self._angle = (self._angle + 12) % 360
        self.update()
    
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        rect = self.rect()
        center = rect.center()
        radius = min(rect.width(), rect.height()) // 2 - 2
        line_width = 2
        
        if self._state == "idle":
            # 空心圆
            painter.setPen(QPen(QColor(200, 200, 200), line_width))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(center, radius, radius)
            
        elif self._state == "spinning":
            # 旋转环
            painter.setPen(QPen(QColor(0, 120, 212), line_width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            span_angle = 270 * 16
            start_angle = self._angle * 16
            painter.drawArc(rect.adjusted(line_width, line_width, -line_width, -line_width), start_angle, span_angle)
            
        elif self._state == "done":
            # 对号
            painter.setPen(QPen(QColor(82, 196, 26), 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            # 画勾
            path = QPainterPath()
            path.moveTo(center.x() - 5, center.y())
            path.lineTo(center.x() - 1, center.y() + 5)
            path.lineTo(center.x() + 6, center.y() - 5)
            painter.drawPath(path)
            
        elif self._state == "failed":
            # X
            painter.setPen(QPen(QColor(255, 77, 79), 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            offset = 5
            painter.drawLine(center.x() - offset, center.y() - offset, center.x() + offset, center.y() + offset)
            painter.drawLine(center.x() + offset, center.y() - offset, center.x() - offset, center.y() + offset)


class ProgressStage:
    """安装阶段"""
    def __init__(self, name: str, weight: int = 1):
        self.name = name
        self.weight = weight
        self.indicator: Optional[StageIndicator] = None


class DownloadProgressPage(BasePage):
    """下载进度页 - VS Installer 风格"""
    
    def __init__(
        self,
        version: GameVersion,
        version_name: str,
        loader_type: str = "none",
        loader_version: str = None,
        parent=None,
    ):
        super().__init__(
            title=f"正在安装 {version_name}",
            subtitle="",
            parent=parent,
        )
        self.subtitleLabel.hide()
        
        self.version = version
        self.version_name = version_name
        self.loader_type = loader_type
        self.loader_version = loader_version
        
        self._worker: Optional[InstallWorker] = None
        self._is_finished = False
        self._stage_widgets: List[tuple] = []  # (label, indicator)
        
        self._build_content()
        self._start_installation()
    
    def _build_content(self):
        """构建 UI"""
        card = CardWidget(self.view)
        card.setStyleSheet("CardWidget { border-radius: 8px; }")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(32, 24, 32, 24)
        layout.setSpacing(12)
        
        # ---- 标题行 ----
        title_layout = QHBoxLayout()
        self.stage_label = StrongBodyLabel("准备安装", card)
        self.stage_label.setStyleSheet("font-size: 15px;")
        title_layout.addWidget(self.stage_label)
        title_layout.addStretch(1)
        
        self.status_badge = BodyLabel("● 进行中", card)
        self.status_badge.setStyleSheet("color: #0078d4; font-weight: 500;")
        title_layout.addWidget(self.status_badge)
        layout.addLayout(title_layout)
        
        # ---- 进度条 ----
        self.progress_bar = QProgressBar(card)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFixedHeight(6)
        self.progress_bar.setTextVisible(False)
        self.progress_bar.setStyleSheet("""
            QProgressBar { border: none; background: rgba(128, 128, 128, 0.2); border-radius: 3px; }
            QProgressBar::chunk { background: #0078d4; border-radius: 3px; }
        """)
        layout.addWidget(self.progress_bar)
        
        # ---- 进度百分比 ----
        self.percent_label = BodyLabel("0%", card)
        self.percent_label.setTextColor("#888888", "#888888")
        self.percent_label.setAlignment(Qt.AlignmentFlag.AlignRight)
        layout.addWidget(self.percent_label)
        
        # ---- 分隔线 ----
        line = QFrame(card)
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet("background: rgba(128, 128, 128, 0.2); max-height: 1px;")
        layout.addWidget(line)
        
        # ---- 阶段列表 ----
        self.stage_list_layout = QVBoxLayout()
        self.stage_list_layout.setSpacing(6)
        self.stage_list_layout.setContentsMargins(0, 8, 0, 0)
        
        # 定义阶段
        stages = [
            ("下载原版 json 文件", 1),
            ("下载原版 client.jar", 5),
            ("下载原版支持库文件", 20),
            ("下载原版资源文件", 30),
        ]
        
        if self.loader_type != "none":
            stages.extend([
                ("下载加载器", 5),
                ("分析加载器依赖", 5),
                ("下载加载器依赖库", 15),
                ("执行加载器安装", 10),
            ])
        
        stages.append(("整理文件", 5))
        stages.append(("安装完成", 1))
        
        self._stage_infos = stages
        
        for name, weight in stages:
            row = QHBoxLayout()
            row.setSpacing(10)
            
            indicator = StageIndicator(card)
            row.addWidget(indicator)
            
            label = BodyLabel(name, card)
            label.setTextColor("#999999", "#666666")
            label.setStyleSheet("font-size: 13px;")
            row.addWidget(label)
            
            row.addStretch(1)
            self.stage_list_layout.addLayout(row)
            self._stage_widgets.append((label, indicator))
        
        layout.addLayout(self.stage_list_layout)
        self.add_content(card)
        
        # ---- 按钮 ----
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(12)
        
        self.cancel_btn = PushButton("取消", self.view)
        self.cancel_btn.clicked.connect(self._on_cancel)
        btn_layout.addWidget(self.cancel_btn)
        
        btn_layout.addStretch(1)
        
        self.back_btn = PrimaryPushButton("返回", self.view)
        self.back_btn.setEnabled(False)
        self.back_btn.clicked.connect(self._on_back)
        btn_layout.addWidget(self.back_btn)
        
        self.vBoxLayout.addLayout(btn_layout)
        self.add_stretch()
    
    def _start_installation(self):
        """启动安装"""
        self._worker = InstallWorker(
            version=self.version,
            version_name=self.version_name,
            loader_type=self.loader_type,
            loader_version=self.loader_version,
            game_dir=Path(cfg.gameDirectory.value),
        )
        self._worker.stage_changed.connect(self._on_stage_changed)
        self._worker.progress.connect(self._on_progress)
        self._worker.detail.connect(self._on_detail)
        self._worker.finished.connect(self._on_finished)
        self._worker.start()
    
    def _on_stage_changed(self, index: int, total: int, name: str):
        """阶段切换"""
        # 标记前一个阶段完成
        if index > 0 and index - 1 < len(self._stage_widgets):
            prev_label, prev_indicator = self._stage_widgets[index - 1]
            prev_indicator.set_state("done")
            prev_label.setTextColor("#52c41a", "#73d13d")
        
        # 当前阶段开始
        if index < len(self._stage_widgets):
            label, indicator = self._stage_widgets[index]
            indicator.set_state("spinning")
            label.setTextColor("#0078d4", "#00bcf2")
            label.setStyleSheet("font-size: 13px; font-weight: 500;")
            self.stage_label.setText(name)
        
        # 重置进度条
        self.progress_bar.setValue(0)
        self.percent_label.setText("0%")
        self.status_badge.setText("● 进行中")
        self.status_badge.setStyleSheet("color: #0078d4; font-weight: 500;")
    
    def _on_progress(self, current: int, total: int):
        if total > 0:
            percent = int(current / total * 100)
            self.progress_bar.setValue(percent)
            self.percent_label.setText(f"{percent}%")
    
    def _on_detail(self, text: str):
        pass  # 暂时不需要详情
    
    def _on_finished(self, success: bool, message: str):
        self._is_finished = True
        self.cancel_btn.setEnabled(False)
        self.back_btn.setEnabled(True)
        
        if success:
            # 所有阶段标记完成
            for label, indicator in self._stage_widgets:
                indicator.set_state("done")
                label.setTextColor("#52c41a", "#73d13d")
            
            self.progress_bar.setValue(100)
            self.percent_label.setText("100%")
            self.status_badge.setText("✓ 完成")
            self.status_badge.setStyleSheet("color: #52c41a; font-weight: 500;")
            self.stage_label.setText("安装完成")
            
            InfoBar.success(
                title="安装成功",
                content=message,
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
        else:
            # 当前阶段标记失败
            for label, indicator in self._stage_widgets:
                if indicator._state == "spinning":
                    indicator.set_state("failed")
                    label.setTextColor("#ff4d4f", "#ff7875")
                    break
            
            self.progress_bar.setStyleSheet("""
                QProgressBar { border: none; background: rgba(128, 128, 128, 0.2); border-radius: 3px; }
                QProgressBar::chunk { background: #ff4d4f; border-radius: 3px; }
            """)
            self.status_badge.setText("✗ 失败")
            self.status_badge.setStyleSheet("color: #ff4d4f; font-weight: 500;")
            self.stage_label.setText("安装失败")
            
            InfoBar.error(
                title="安装失败",
                content=message,
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=5000,
                parent=self,
            )
    
    def _on_cancel(self):
        if self._worker:
            self._worker.cancel()
            self.cancel_btn.setEnabled(False)
            self.cancel_btn.setText("正在取消...")
    
    def _on_back(self):
        main_window = self.window()
        if hasattr(main_window, 'go_back_to_versions'):
            main_window.go_back_to_versions()


class InstallWorker(QThread):
    """安装工作线程"""
    
    stage_changed = Signal(int, int, str)
    progress = Signal(int, int)
    detail = Signal(str)
    finished = Signal(bool, str)
    
    def __init__(
        self,
        version: GameVersion,
        version_name: str,
        loader_type: str,
        loader_version: str,
        game_dir: Path,
    ):
        super().__init__()
        self.version = version
        self.version_name = version_name
        self.loader_type = loader_type
        self.loader_version = loader_version
        self.game_dir = game_dir
        self._cancel = False
        
        self._vanilla_json = None
        self._version_data = None
        self._installer_path = None
        self._loader_libraries = []
    
    def cancel(self):
        self._cancel = True

    def _cleanup_on_failure(self):
        """安装失败时清理版本目录（只删本次安装创建的，不碰 versions/ 和其他版本）"""
        import shutil
        version_dir = self.game_dir / "versions" / self.version_name
        if version_dir.exists():
            try:
                shutil.rmtree(version_dir)
                log.info("安装失败，已清理版本目录: %s", version_dir)
            except Exception as e:
                log.warning("安装失败后清理版本目录出错: %s", e)
    
    def run(self):
        try:
            # 定义阶段
            stages = [
                ("下载原版 json 文件", self._stage_vanilla_json),
                ("下载原版 client.jar", self._stage_vanilla_jar),
                ("下载原版支持库文件", self._stage_vanilla_libraries),
                ("下载原版资源文件", self._stage_vanilla_assets),
            ]
            
            if self.loader_type != "none":
                stages.extend([
                    ("下载加载器", self._stage_download_loader),
                    ("分析加载器依赖", self._stage_analyze_loader),
                    ("下载加载器依赖库", self._stage_download_loader_libs),
                    ("执行加载器安装", self._stage_run_loader),
                ])
            
            stages.append(("整理文件", self._stage_finish))
            
            total = len(stages)
            
            for i, (name, func) in enumerate(stages):
                if self._cancel:
                    self.finished.emit(False, "已取消安装")
                    return
                
                self.stage_changed.emit(i, total, name)
                success = func()
                
                if not success:
                    # 安装失败 → 清理本次创建的版本目录
                    self._cleanup_on_failure()
                    self.finished.emit(False, f"{name} 失败")
                    return
            
            self.finished.emit(True, f"{self.version_name} 安装完成")
            
        except Exception as e:
            log_exception(log, f"安装异常: {e}")
            self._cleanup_on_failure()
            self.finished.emit(False, f"安装异常: {str(e)}")
    
    # ================================================================
    # 各阶段实现
    # ================================================================
    
    def _stage_vanilla_json(self) -> bool:
        """下载原版 json"""
        try:
            self.detail.emit("获取版本信息...")
            
            source = cfg.downloadSource.value
            _, versions = fetch_version_manifest(source)
            
            for v in versions:
                if v.id == self.version.id:
                    self._version_data = v
                    break
            
            if not self._version_data:
                self.detail.emit("未找到版本信息")
                return False
            
            self.detail.emit("下载版本配置...")
            
            import requests
            resp = requests.get(self._version_data.url, timeout=30)
            resp.raise_for_status()
            self._vanilla_json = resp.json()
            
            version_dir = self.game_dir / "versions" / self.version_name
            version_dir.mkdir(parents=True, exist_ok=True)
            json_path = version_dir / f"{self.version_name}.json"
            with open(json_path, 'w', encoding='utf-8') as f:
                json.dump(self._vanilla_json, f, ensure_ascii=False, indent=2)
            
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"下载原版json失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_vanilla_jar(self) -> bool:
        """下载原版 client.jar（必须成功，模组加载器依赖它进行 patch）"""
        try:
            client_info = self._vanilla_json.get('downloads', {}).get('client')
            if not client_info:
                msg = f"{self.version_name} 版本清单中无 client.jar 信息"
                self.detail.emit(f"❌ {msg}")
                log.error("安装 | %s", msg)
                return False

            url = client_info.get('url')
            size = client_info.get('size', 0)
            if not url:
                msg = f"{self.version_name} 版本清单中 client.jar URL 为空"
                self.detail.emit(f"❌ {msg}")
                log.error("安装 | %s", msg)
                return False

            self.detail.emit(f"下载 client.jar ({size // 1024 // 1024} MB)...")
            log.info("安装 | 下载 client.jar (%d MB) | %s", size // 1024 // 1024, url[:80])

            jar_path = self.game_dir / "versions" / self.version_name / f"{self.version_name}.jar"

            engine = DownloadEngine(max_workers=4)
            engine.set_progress_callback(lambda c, t, s: self.progress.emit(c, t))

            success = engine.download(url, jar_path, size)

            if success:
                log.info("安装 | client.jar 下载完成: %s", self.version_name)
                self.progress.emit(100, 100)
                return True
            else:
                msg = f"{self.version_name} client.jar 下载失败，无法继续安装"
                self.detail.emit(f"❌ {msg}")
                log.error("安装 | %s | %s", msg, url[:80])
                return False
        except Exception as e:
            log_exception(log, f"下载client.jar失败: {e}")
            self.detail.emit(f"❌ 下载 client.jar 异常: {e}")
            return False
    
    def _stage_vanilla_libraries(self) -> bool:
        """下载原版支持库（并行批量下载）"""
        try:
            libraries = self._vanilla_json.get('libraries', [])
            total = len(libraries)

            if total == 0:
                self.progress.emit(100, 100)
                return True

            # 收集需要下载的条目
            batch_items: list[tuple[str, Path, int, str | None]] = []
            for lib in libraries:
                if self._cancel:
                    return False
                artifact = lib.get("downloads", {}).get("artifact")
                if not artifact:
                    continue
                lib_path = self.game_dir / "libraries" / artifact.get("path", "")
                lib_path.parent.mkdir(parents=True, exist_ok=True)
                size = artifact.get("size", 0)
                # 跳过已存在且大小匹配的
                if lib_path.exists() and size > 0 and lib_path.stat().st_size == size:
                    continue
                if lib_path.exists() and size == 0:
                    continue
                batch_items.append((artifact["url"], lib_path, size, None))

            if not batch_items:
                self.progress.emit(100, 100)
                return True

            self.detail.emit(f"并行下载 {len(batch_items)} 个依赖库...")
            self.progress.emit(0, len(batch_items))

            engine = DownloadEngine(max_workers=8, max_retries=2)
            results = engine.download_batch(batch_items, max_concurrent=8)

            failed = sum(1 for r in results if not r)
            if failed > 0:
                self.detail.emit(f"{failed} 个依赖库下载失败")
                # 不直接返回失败，让后续阶段处理缺失文件
                log.warning(f"原版库下载: {failed}/{len(results)} 失败")

            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"下载原版支持库失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_vanilla_assets(self) -> bool:
        """下载原版资源（并行批量下载）"""
        try:
            asset_index = self._vanilla_json.get('assetIndex', {})
            index_url = asset_index.get('url')
            if not index_url:
                self.progress.emit(100, 100)
                return True

            import requests
            resp = requests.get(index_url, timeout=30)
            resp.raise_for_status()
            index_data = resp.json()

            objects = index_data.get('objects', {})
            total = len(objects)

            if total == 0:
                self.progress.emit(100, 100)
                return True

            # 收集需要下载的条目
            batch_items: list[tuple[str, Path, int, str | None]] = []
            for key, info in objects.items():
                if self._cancel:
                    return False
                hash_val = info.get('hash')
                if not hash_val:
                    continue
                obj_path = self.game_dir / "assets" / "objects" / hash_val[:2] / hash_val
                size = info.get('size', 0)
                if obj_path.exists() and size > 0 and obj_path.stat().st_size == size:
                    continue
                from src.core.mirror import maybe_mirror_url
                asset_url = maybe_mirror_url(
                    f"https://resources.download.minecraft.net/{hash_val[:2]}/{hash_val}"
                )
                batch_items.append((asset_url, obj_path, size, None))

            if not batch_items:
                self.progress.emit(100, 100)
                return True

            self.detail.emit(f"并行下载 {len(batch_items)} 个资源文件...")
            self.progress.emit(0, len(batch_items))

            engine = DownloadEngine(max_workers=8, max_retries=2)
            results = engine.download_batch(batch_items, max_concurrent=8)

            failed = sum(1 for r in results if not r)
            if failed > 0:
                log.warning(f"资源文件下载: {failed}/{len(results)} 个失败")

            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"下载原版资源失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_download_loader(self) -> bool:
        """下载加载器"""
        try:
            from src.app.services.mod_loader_service import ForgeAPI, FabricAPI, NeoForgeAPI
            
            if self.loader_type == "forge":
                forge_id = f"{self.version.id}-{self.loader_version}"
                url = ForgeAPI.get_installer_url(forge_id)
            elif self.loader_type == "fabric":
                url = FabricAPI.get_installer_url()
            elif self.loader_type == "neoforge":
                url = NeoForgeAPI.get_installer_url(self.loader_version)
            else:
                return False
            
            self.detail.emit(f"下载 {self.loader_type} 安装器...")
            
            installer_dir = self.game_dir / "versions" / self.version_name
            installer_dir.mkdir(parents=True, exist_ok=True)
            installer_path = installer_dir / f"{self.loader_type}-installer.jar"
            self._installer_path = installer_path
            
            engine = DownloadEngine(max_workers=4)
            engine.set_progress_callback(lambda c, t, s: self.progress.emit(c, t))
            
            success = engine.download(url, installer_path)
            
            if success:
                self.progress.emit(100, 100)
            return success
        except Exception as e:
            log_exception(log, f"下载加载器失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_analyze_loader(self) -> bool:
        """分析加载器依赖"""
        try:
            self.detail.emit("分析加载器依赖库...")
            
            from src.app.services.installer.forge_analyzer import ForgeAnalyzer
            from src.app.services.installer.fabric_analyzer import FabricAnalyzer
            from src.app.services.installer.neoforge_analyzer import NeoForgeAnalyzer
            
            if self.loader_type == "forge":
                analyzer = ForgeAnalyzer(self._installer_path)
            elif self.loader_type == "fabric":
                analyzer = FabricAnalyzer(self._installer_path)
            elif self.loader_type == "neoforge":
                analyzer = NeoForgeAnalyzer(self._installer_path)
            else:
                return False
            
            self._loader_libraries = analyzer.get_libraries()
            self.detail.emit(f"发现 {len(self._loader_libraries)} 个依赖库")
            
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"分析加载器依赖失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_download_loader_libs(self) -> bool:
        """下载加载器依赖库（并行批量下载）"""
        try:
            libraries = self._loader_libraries
            total = len(libraries)

            if total == 0:
                self.progress.emit(100, 100)
                return True

            # 收集需要下载的条目，多源回退使用第一可用 URL
            batch_items: list[tuple[str, Path, int, str | None]] = []
            for lib in libraries:
                if self._cancel:
                    return False
                lib_path = self.game_dir / "libraries" / lib.get("path", "")
                lib_path.parent.mkdir(parents=True, exist_ok=True)
                if lib_path.exists():
                    continue
                # 选择第一个可用源的 URL
                url = lib.get("url", "") + lib.get("path", "")
                if not url or url == "":
                    # fallback to Maven Central
                    url = f"https://repo1.maven.org/maven2/{lib.get('path', '')}"
                batch_items.append((url, lib_path, 0, None))

            if not batch_items:
                self.progress.emit(100, 100)
                return True

            self.detail.emit(f"并行下载 {len(batch_items)} 个加载器依赖库...")
            self.progress.emit(0, len(batch_items))

            engine = DownloadEngine(max_workers=8, max_retries=2)
            results = engine.download_batch(batch_items, max_concurrent=8)

            failed = sum(1 for r in results if not r)
            if failed > 0:
                self.detail.emit(f"{failed} 个加载器库下载失败")
                log.warning(f"加载器库下载: {failed}/{len(results)} 失败")

            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"下载加载器依赖库失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_run_loader(self) -> bool:
        """执行加载器安装"""
        try:
            self.detail.emit("执行加载器安装...")
            
            from src.app.services.mod_loader_installer import ModLoaderInstaller
            
            installer = ModLoaderInstaller(self.game_dir)
            installer.set_progress_callback(lambda c, t, s: self.progress.emit(c, t))
            
            success = installer.install(
                mc_version=self.version.id,
                loader_type=self.loader_type,
                loader_version=self.loader_version,
                installer_path=self._installer_path,
                custom_name=self.version_name,
            )
            
            if success:
                self.progress.emit(100, 100)
            return success
        except Exception as e:
            log_exception(log, f"执行加载器安装失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
    
    def _stage_finish(self) -> bool:
        """整理文件"""
        try:
            self.detail.emit("清理临时文件...")
            
            if self._installer_path and self._installer_path.exists():
                self._installer_path.unlink()
            
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"整理文件失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False