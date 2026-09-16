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

import hashlib
import json
import os
import shutil
import time
import threading
from pathlib import Path
from typing import Optional, List

from PySide6.QtCore import Qt, QTimer, QThread, Signal
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget, QProgressBar, QFrame, QLabel
from PySide6.QtGui import QPainter, QPainterPath, QColor, QPen, QBrush
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
from src.app.theme import token as _token
from src.app.common.launcher_config import cfg
from src.core.logger import log, log_exception
from src.services.minecraft.manifest import GameVersion, fetch_version_manifest
from src.core.mirror import candidates
from src.core.download import (
    ProgressInfo,
    default_engine,
    spec_for,
    specs_for_asset_objects,
    specs_for_libraries,
    specs_for_version_json,
)


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
        self._bar_state = "running"            # running / done / failed

        self._build_content()
        from src.app.theme import on_theme_changed
        on_theme_changed(self._apply_theme_styles)
        self._start_installation()
    
    def _style_progress(self, state: str = "normal") -> None:
        """进度条配色（主题切换时会重新调用；失败态用 danger 色）。"""
        from src.app.theme import token
        color = {"normal": token("accent"), "failed": token("danger"),
                 "done": token("success")}.get(state, token("accent"))
        self.progress_bar.setStyleSheet(
            f"QProgressBar {{ border: none; background: {token('track')}; border-radius: 3px; }}"
            f"QProgressBar::chunk {{ background: {color}; border-radius: 3px; }}")

    def _badge_color(self) -> str:
        from src.app.theme import token
        return {"running": token("accent"), "done": token("success"),
                "failed": token("danger")}.get(self._bar_state, token("text_secondary"))

    def _apply_theme_styles(self) -> None:
        """把页面里的内联样式按当前主题令牌重刷一遍。"""
        from src.app.theme import token
        self._style_progress(self._bar_state)
        self._divider.setStyleSheet(f"background: {token('separator')}; max-height: 1px;")
        self.status_badge.setStyleSheet(f"color: {self._badge_color()}; font-weight: 500;")
        self.detail_label.setStyleSheet(f"font-size: 12px; color: {token('text_tertiary')};")

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
        self.status_badge.setStyleSheet(f"color: {_token('accent')}; font-weight: 500;")
        title_layout.addWidget(self.status_badge)
        layout.addLayout(title_layout)
        
        # ---- 进度条 ----
        self.progress_bar = QProgressBar(card)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFixedHeight(6)
        self.progress_bar.setTextVisible(False)
        layout.addWidget(self.progress_bar)
        
        # ---- 进度百分比 ----
        self.percent_label = BodyLabel("0%", card)
        self.percent_label.setTextColor(_token("text_tertiary"), _token("text_tertiary"))
        self.percent_label.setAlignment(Qt.AlignmentFlag.AlignRight)
        layout.addWidget(self.percent_label)

        # 详情行：显示"正在下载哪个文件 / 速度 / 剩余时间 / 来自哪个源"
        self.detail_label = BodyLabel("", card)
        self.detail_label.setTextColor(_token("text_tertiary"), _token("text_tertiary"))
        self.detail_label.setWordWrap(True)
        self.detail_label.setStyleSheet("font-size: 12px;")
        layout.addWidget(self.detail_label)
        
        # ---- 分隔线 ----
        
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
            label.setTextColor(_token("text_tertiary"), _token("text_disabled"))
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

        # ── 进度节流：最多 10 次/秒，避免 UI 闪烁 ──
        self._throttle_progress = (0, 0)
        self._throttle_timer = QTimer(self)
        self._throttle_timer.setInterval(100)
        self._throttle_timer.timeout.connect(self._flush_progress)
        self._throttle_timer.start()

        self._worker.start()
    
    def _on_stage_changed(self, index: int, total: int, name: str):
        """阶段切换"""
        # 标记前一个阶段完成
        if index > 0 and index - 1 < len(self._stage_widgets):
            prev_label, prev_indicator = self._stage_widgets[index - 1]
            prev_indicator.set_state("done")
            prev_label.setTextColor(_token("success"), _token("success"))
        
        # 当前阶段开始
        if index < len(self._stage_widgets):
            label, indicator = self._stage_widgets[index]
            indicator.set_state("spinning")
            label.setTextColor(_token("accent"), _token("accent"))
            label.setStyleSheet("font-size: 13px; font-weight: 500;")
            self.stage_label.setText(name)
        
        # 重置进度条
        self.progress_bar.setValue(0)
        self.percent_label.setText("0%")
        self.status_badge.setText("● 进行中")
    
    def _on_progress(self, current: int, total: int):
        # 节流：只存值，由 QTimer 统一刷新
        self._throttle_progress = (current, total)

    def _flush_progress(self):
        """每 100ms 刷新一次进度，防止 UI 闪烁"""
        current, total = self._throttle_progress
        if total > 0:
            percent = int(current / total * 100)
            self.progress_bar.setValue(percent)
            self.percent_label.setText(f"{percent}%")
    
    def _on_detail(self, text: str):
        if hasattr(self, "detail_label"):
            self.detail_label.setText(text[:200])
    
    def _on_finished(self, success: bool, message: str):
        self._is_finished = True
        self.cancel_btn.setEnabled(False)
        self.back_btn.setEnabled(True)

        self._bar_state = "done" if success else "failed"
        self._apply_theme_styles()

        # 更新任务页状态
        task_key = f"download_progress_{self.version_name}"
        mw = self.window()
        if hasattr(mw, 'tasks_page'):
            if success:
                mw.tasks_page.set_task_done(task_key)
            else:
                mw.tasks_page.set_task_failed(task_key)
        
        if success:
            # 所有阶段标记完成
            for label, indicator in self._stage_widgets:
                indicator.set_state("done")
                label.setTextColor(_token("success"), _token("success"))
            
            self.progress_bar.setValue(100)
            self.percent_label.setText("100%")
            self.status_badge.setText("✓ 完成")
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
                    label.setTextColor(_token("danger"), _token("danger"))
                    break
            
            self.status_badge.setText("✗ 失败")
            self.status_badge.setStyleSheet(f"color: {_token('danger')}; font-weight: 500;")
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
    """安装工作线程 —— 全部走"自研异步下载引擎 + 双源回退 + SHA1 强制校验"。

    与旧实现的区别
    --------------
    * 下载统一交给 AsyncDownloadEngine（asyncio 并发 + 连接复用 + 全局限速）。
    * 每个资源都带官方/镜像两条候选 URL，失败自动换源。
    * 凡是 Mojang 提供了 SHA1 的资源（版本 JSON / jar / 资源索引 / 依赖库 /
      资源对象）下载后强制校验，校验失败删档重下。
    * 新增 natives 解压阶段：以前 natives 分类器既不下载也不解压，
      -Djava.library.path 指向空目录，游戏必然启动失败。
    """

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
        self._cancel_event = threading.Event()

        self._version_json = None
        self._installer_path = None
        self._loader_libraries = []
        self._last_summary = ""

    # ── 取消 ─────────────────────────────────────────────────────

    def cancel(self):
        self._cancel = True
        self._cancel_event.set()

    # ── 通用：元数据（小 JSON）下载，双源 + sha1 ─────────────────

    def _meta_bytes(self, url: str, sha1=None, timeout: int = 30):
        import requests as _rq
        for source, candidate_url in candidates(url):
            self._check_cancel()
            try:
                resp = _rq.get(candidate_url, timeout=timeout)
                resp.raise_for_status()
                data = resp.content
                if sha1:
                    actual = hashlib.sha1(data).hexdigest()
                    if actual.lower() != sha1.lower():
                        log.warning("元数据校验失败 | %s | 期望 %s 实际 %s",
                                    candidate_url[:90], sha1[:12], actual[:12])
                        continue
                log.info("元数据 | 源=%s | %s", source, candidate_url[:100])
                return data
            except Exception as exc:
                log.warning("元数据失败 | 源=%s | %s | %s", source, candidate_url[:80], exc)
        return None

    def _check_cancel(self) -> bool:
        if self._cancel or self._cancel_event.is_set():
            raise RuntimeError("已取消安装")
        return False

    # ── 通用：文件批次下载（新引擎）──────────────────────────────

    def _run_downloads(self, specs, stage_name: str):
        """用新引擎下载一批 FileSpec，返回 results。"""
        if not specs:
            self.progress.emit(100, 100)
            return []
        engine = default_engine()
        total_bytes = sum(max(0, s.size) for s in specs) or 1

        def on_progress(p: ProgressInfo) -> None:
            self.progress.emit(int(p.bytes_done), int(max(1, p.bytes_total)))
            speed = p.speed_bps / 1024 / 1024
            self.detail.emit(
                f"{stage_name}: {p.files_done}/{p.files_total} 个文件 "
                f"| {p.bytes_done / 1024 / 1024:.1f}/{total_bytes / 1024 / 1024:.1f} MB "
                f"| {speed:.1f} MB/s | 剩余 {p.eta_seconds:.0f}s | {p.current[:46]}")

        log.info("安装 | %s | %d 个文件 | 首选源=%s", stage_name, len(specs),
                 specs[0].candidates[0][0] if specs else "-")
        results = engine.download(specs, on_progress=on_progress, cancel=self._cancel_event)
        ok = sum(1 for r in results if r.ok)
        by_source = {}
        for r in results:
            by_source[r.source] = by_source.get(r.source, 0) + 1
        log.info("安装 | %s 完成: %d/%d | 来源统计=%s", stage_name, ok, len(results), by_source)
        self.progress.emit(100, 100)
        return results

    @staticmethod
    def _failures(results):
        return [r for r in results if not r.ok]

    # ── 清理 ─────────────────────────────────────────────────────

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

    # ── 主流程 ───────────────────────────────────────────────────

    def run(self):
        try:
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

            stages.append(("解压 natives", self._stage_extract_natives))
            stages.append(("整理文件", self._stage_finish))

            total = len(stages)
            for i, (name, func) in enumerate(stages):
                if self._cancel:
                    self.finished.emit(False, "已取消安装")
                    return
                self.stage_changed.emit(i, total, name)
                if not func():
                    self._cleanup_on_failure()
                    tail = f"：{self._last_summary}" if self._last_summary else ""
                    self.finished.emit(False, f"{name} 失败{tail}")
                    return

            self.finished.emit(True, f"{self.version_name} 安装完成")

        except Exception as e:
            if self._cancel:
                self.finished.emit(False, "已取消安装")
                return
            log_exception(log, f"安装异常: {e}")
            self._cleanup_on_failure()
            self.finished.emit(False, f"安装异常: {str(e)}")

    # ================================================================
    # 各阶段实现
    # ================================================================

    def _stage_vanilla_json(self) -> bool:
        """下载版本 JSON —— 用清单里的 sha1 校验原始字节。"""
        try:
            self.detail.emit("获取版本清单…")
            source = cfg.downloadSource.value
            _, versions = fetch_version_manifest(source)
            entry = None
            for v in versions:
                if v.id == self.version.id:
                    entry = v
                    break
            if not entry:
                self._last_summary = "清单中没有该版本"
                self.detail.emit("未找到版本信息")
                return False

            self.detail.emit("下载版本配置…")
            raw = self._meta_bytes(entry.url, getattr(entry, "sha1", None) or None)
            if raw is None:
                self._last_summary = "版本 JSON 所有源均失败"
                return False

            data = json.loads(raw.decode("utf-8"))
            self._version_json = data

            version_dir = self.game_dir / "versions" / self.version_name
            version_dir.mkdir(parents=True, exist_ok=True)
            data["id"] = self.version_name
            json_path = version_dir / f"{self.version_name}.json"
            with open(json_path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            log.info("安装 | 版本 JSON 已保存: %s (原版 %s)", json_path, entry.id)
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"下载原版json失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_vanilla_jar(self) -> bool:
        """下载客户端 jar（模组加载器要拿它做 patch，必须成功且校验通过）"""
        try:
            specs = specs_for_version_json(self._version_json, self.game_dir, self.version_name)
            jar_specs = [s for s in specs if s.kind == "client-jar"]
            if not jar_specs:
                self._last_summary = "版本清单中无 client.jar 信息"
                self.detail.emit("❌ " + self._last_summary)
                return False
            results = self._run_downloads(jar_specs, "客户端 jar")
            ok = all(r.ok for r in results)
            if not ok:
                self._last_summary = "client.jar 下载或校验失败"
                self.detail.emit("❌ " + self._last_summary)
            return ok
        except Exception as e:
            log_exception(log, f"下载client.jar失败: {e}")
            self.detail.emit(f"❌ 下载 client.jar 异常: {e}")
            self._last_summary = str(e)
            return False

    def _stage_vanilla_libraries(self) -> bool:
        """下载原版支持库（含 natives 分类器），全部带 sha1 校验。"""
        try:
            specs = specs_for_libraries(self._version_json, self.game_dir)
            if not specs:
                self.progress.emit(100, 100)
                return True
            results = self._run_downloads(specs, "支持库")
            bad = self._failures(results)
            if bad:
                names = ", ".join(r.name for r in bad[:3])
                self._last_summary = f"{len(bad)}/{len(results)} 个依赖库失败（{names}）"
                self.detail.emit("❌ " + self._last_summary)
                return False
            return True
        except Exception as e:
            log_exception(log, f"下载原版支持库失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_vanilla_assets(self) -> bool:
        """下载资源索引 + 资源对象（对象文件名即 sha1，天然全量校验）。"""
        try:
            all_specs = specs_for_version_json(self._version_json, self.game_dir, self.version_name)
            index_specs = [s for s in all_specs if s.kind == "asset-index"]
            if not index_specs:
                self.progress.emit(100, 100)
                return True
            results = self._run_downloads(index_specs, "资源索引")
            if not all(r.ok for r in results):
                self._last_summary = "资源索引下载或校验失败"
                return False

            index_path = index_specs[0].dest
            with open(index_path, "r", encoding="utf-8") as f:
                index_data = json.load(f)
            objects = specs_for_asset_objects(index_data, self.game_dir)
            if not objects:
                self.progress.emit(100, 100)
                return True
            self.detail.emit(f"资源文件共 {len(objects)} 个…")
            results = self._run_downloads(objects, "资源文件")
            bad = self._failures(results)
            if bad:
                # 资源缺失不阻断安装（不影响启动），但要如实汇报
                log.warning("安装 | 资源文件失败 %d/%d，例如 %s", len(bad), len(results),
                            ", ".join(r.name for r in bad[:3]))
                self.detail.emit(f"⚠ {len(bad)} 个资源文件下载失败（不影响启动）")
            return True
        except Exception as e:
            log_exception(log, f"下载原版资源失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_download_loader(self) -> bool:
        """下载加载器安装器（无 Mojang 哈希，仅做大小/可读检查）。"""
        try:
            from src.services.mod_loader.api import ForgeAPI, FabricAPI, NeoForgeAPI

            if self.loader_type == "forge":
                forge_id = f"{self.version.id}-{self.loader_version}"
                url = ForgeAPI.get_installer_url(forge_id)
                kind = "forge-installer"
            elif self.loader_type == "fabric":
                url = FabricAPI.get_installer_url()
                kind = "fabric-installer"        # 实测镜像没有 fabric-installer，只走官方
            elif self.loader_type == "neoforge":
                url = NeoForgeAPI.get_installer_url(self.loader_version)
                kind = "neoforge-installer"
            else:
                return False

            version_dir = self.game_dir / "versions" / self.version_name
            version_dir.mkdir(parents=True, exist_ok=True)
            dest = version_dir / f"{self.loader_type}-installer.jar"
            self._installer_path = dest

            spec = spec_for(dest, url, kind=kind, label=dest.name)
            results = self._run_downloads([spec], "加载器安装器")
            ok = all(r.ok for r in results) and dest.exists() and dest.stat().st_size > 0
            if not ok:
                self._last_summary = "加载器安装器下载失败"
            return ok
        except Exception as e:
            log_exception(log, f"下载加载器失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_analyze_loader(self) -> bool:
        """分析加载器依赖"""
        try:
            self.detail.emit("分析加载器依赖库…")

            from src.services.mod_loader.analyzers.forge_analyzer import ForgeAnalyzer
            from src.services.mod_loader.analyzers.fabric_analyzer import FabricAnalyzer
            from src.services.mod_loader.analyzers.forge_analyzer import NeoForgeAnalyzer

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
            self._last_summary = str(e)
            return False

    def _stage_download_loader_libs(self) -> bool:
        """下载加载器依赖库（两条路 + 大小校验）"""
        try:
            specs = []
            for lib in self._loader_libraries or []:
                path = lib.get("path") or ""
                if not path:
                    continue
                url = (lib.get("url") or "").rstrip("/") + "/" + path
                specs.append(spec_for(self.game_dir / "libraries" / path, url,
                                      kind="loader-library", label=path))
            if not specs:
                self.progress.emit(100, 100)
                return True
            results = self._run_downloads(specs, "加载器依赖库")
            bad = self._failures(results)
            if bad:
                self._last_summary = f"{len(bad)}/{len(results)} 个加载器依赖库失败"
                log.warning("安装 | %s", self._last_summary)
                return False
            return True
        except Exception as e:
            log_exception(log, f"下载加载器依赖库失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_run_loader(self) -> bool:
        """执行加载器安装"""
        try:
            self.detail.emit("执行加载器安装…")

            from src.services.mod_loader.installer import ModLoaderInstaller

            installer = ModLoaderInstaller(self.game_dir)

            def _on_loader_progress(current: int, total: int, status: str) -> None:
                # 安装器自己会打印进度标记（解压版本信息 / 下载支持库 / 执行安装处理器…），
                # 把这些文字直接给用户看，比一个不动的进度条强得多
                self.progress.emit(current, total)
                if status:
                    self.detail.emit(status)

            installer.set_progress_callback(_on_loader_progress)
            installer.set_cancel_check(self.isInterruptionRequested)

            success = installer.install(
                mc_version=self.version.id,
                loader_type=self.loader_type,
                loader_version=self.loader_version,
                installer_path=self._installer_path,
                custom_name=self.version_name,
            )
            if not success:
                self._last_summary = "加载器安装器执行失败（详见日志）"
            self.progress.emit(100, 100)
            return success
        except Exception as e:
            log_exception(log, f"执行加载器安装失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_extract_natives(self) -> bool:
        """把 natives 分类器 jar 解压到 versions/<id>/<id>-natives。

        这一步以前完全缺失：build_command 会把 -Djava.library.path 指向该目录，
        目录不存在 -> LWJGL 原生库加载失败 -> 游戏启动即崩。
        """
        import zipfile

        try:
            target = self.game_dir / "versions" / self.version_name / f"{self.version_name}-natives"
            target.mkdir(parents=True, exist_ok=True)

            specs = specs_for_libraries(self._version_json, self.game_dir)
            natives = [s for s in specs if s.kind == "native" and s.dest.exists()]
            extracted = 0
            for spec in natives:
                try:
                    with zipfile.ZipFile(spec.dest) as zf:
                        for member in zf.namelist():
                            if member.startswith("META-INF/") or member.endswith("/"):
                                continue
                            name = os.path.basename(member)
                            if not name:
                                continue
                            if member.lower().endswith((".dll", ".so", ".dylib", ".jnilib", ".sig", ".sha1")):
                                dest_file = target / name
                                if dest_file.exists():
                                    continue      # 同一个原生库的多个架构变体：先到先得，不覆盖
                                with zf.open(member) as src, open(dest_file, "wb") as dst:
                                    shutil.copyfileobj(src, dst)
                                extracted += 1
                except Exception as exc:
                    log.warning("natives 解压失败 %s: %s", spec.name, exc)

            # 老版本（1.12 及更早）natives 在客户端 jar 里
            if extracted == 0:
                jar = self.game_dir / "versions" / self.version_name / f"{self.version_name}.jar"
                if jar.exists():
                    try:
                        with zipfile.ZipFile(jar) as zf:
                            for member in zf.namelist():
                                if member.startswith("META-INF/") or member.endswith("/"):
                                    continue
                                if member.lower().endswith((".dll", ".so", ".dylib", ".jnilib")):
                                    name = os.path.basename(member)
                                    with zf.open(member) as src, open(target / name, "wb") as dst:
                                        shutil.copyfileobj(src, dst)
                                    extracted += 1
                    except Exception as exc:
                        log.warning("客户端 jar 内 natives 解压失败: %s", exc)

            log.info("安装 | natives 解压完成: %d 个文件 -> %s", extracted, target)
            self.detail.emit(f"natives: {extracted} 个文件")
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"解压 natives 失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            self._last_summary = str(e)
            return False

    def _stage_finish(self) -> bool:
        """整理文件"""
        try:
            self.detail.emit("清理临时文件…")
            if self._installer_path and self._installer_path.exists():
                try:
                    self._installer_path.unlink()
                except OSError:
                    pass
            version_dir = self.game_dir / "versions" / self.version_name
            for leftover in list(version_dir.glob("*.part*")):
                try:
                    leftover.unlink()
                except OSError:
                    pass
            self.progress.emit(100, 100)
            return True
        except Exception as e:
            log_exception(log, f"整理文件失败: {e}")
            self.detail.emit(f"错误: {str(e)}")
            return False
