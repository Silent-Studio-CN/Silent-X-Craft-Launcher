"""Launch progress page — PCL2-style real-time launch status."""

from __future__ import annotations

import shutil
import subprocess
import threading
import time
from pathlib import Path

from PySide6.QtCore import Qt, QThread, Signal
from src.core.logger import log
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
    isDarkTheme,
)

from src.app.common.base_page import BasePage
from src.app.common.launcher_config import cfg
from src.app.services.version_manifest import GameVersion
from src.services.java.finder import inspect_java
from src.services.java.compatibility import detect_java_version, get_supported_jvm_args, is_java_compatible
from src.services.minecraft.launcher import build_command, launch_process, load_version_info
from src.core.platform import is_windows, is_macos, is_linux


# ── Window Detection Helpers ──────────────────────────────────────


def _find_minecraft_window() -> bool:
    """Check if a Minecraft game window exists (cross-platform)."""
    if is_windows():
        try:
            import ctypes
            user32 = ctypes.windll.user32
            # LWJGL class name varies by version, check multiple
            for class_name in (b"LWJGL", b"GLFW30", b"SunAWTFrame"):
                handle = user32.FindWindowA(class_name, None)
                if handle:
                    return True
            # Also try by title
            handle = user32.FindWindowA(None, b"Minecraft")
            return bool(handle)
        except Exception:
            pass
    # Fallback: process is still running and has had time to create window
    return False


def _poll_game_window(process: subprocess.Popen, timeout: float = 30.0, interval: float = 0.5) -> bool:
    """Poll for game window; return True if found within *timeout* seconds."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            # Process exited before window appeared — probably crashed
            return False
        if _find_minecraft_window():
            return True
        time.sleep(interval)
    # Timeout — game is running but window didn't appear (could be still loading)
    return process.poll() is None  # True if still alive


# ── Launch Worker ─────────────────────────────────────────────────


class LaunchPhase:
    CHECKING_JAVA = "检测 Java 运行时…"
    BUILDING_COMMAND = "构建启动命令…"
    STARTING_PROCESS = "启动游戏进程…"
    WAITING_WINDOW = "等待游戏窗口…"
    RUNNING = "游戏运行中"
    FINISHED = "启动完成"
    FAILED = "启动失败"


class LaunchWorker(QThread):
    """Background thread for game launching."""

    phase_changed = Signal(int, int, str)   # phase_index, total, name
    log_line = Signal(str)                  # real-time log line
    process_started = Signal(object)        # Popen handle
    window_detected = Signal()
    finished = Signal(bool, str)            # success, message

    def __init__(self, version: GameVersion, parent=None):
        super().__init__(parent)
        self.version = version
        self._cancel = False
        self._process: subprocess.Popen | None = None
        self._run_dir: Path | None = None

    def cancel(self):
        self._cancel = True
        if self._process and self._process.poll() is None:
            self._process.terminate()

    def run(self):
        phases = [
            (0, LaunchPhase.CHECKING_JAVA, self._phase_check_java),
            (1, LaunchPhase.BUILDING_COMMAND, self._phase_build_command),
            (2, LaunchPhase.STARTING_PROCESS, self._phase_start_process),
            (3, LaunchPhase.WAITING_WINDOW, self._phase_wait_window),
            (4, LaunchPhase.RUNNING, self._phase_running),
        ]
        total = len(phases)

        for idx, name, func in phases:
            if self._cancel:
                self.finished.emit(False, "已取消")
                return
            self.phase_changed.emit(idx, total, name)
            success, message = func()
            if not success:
                self.finished.emit(False, message)
                return

        # ── Post-exit: collect crash reports ──
        self._collect_crash_reports()

        self.finished.emit(True, "游戏已启动")

    # ── Phases ─────────────────────────────────────────────────

    # ── 资源完整性检查 ─────────────────────────────────────────

    def _check_integrity(self, game_dir: Path, version_id: str) -> list[str]:
        """Run integrity checks on game resources, return list of issues."""
        issues: list[str] = []
        version_dir = game_dir / "versions" / version_id
        jar = version_dir / f"{version_id}.jar"
        json_f = version_dir / f"{version_id}.json"

        if not version_dir.exists():
            issues.append(f"版本目录缺失: {version_dir}")
        if not json_f.exists():
            issues.append(f"版本 JSON 缺失: {json_f}")
        elif not jar.exists():
            issues.append(f"客户端 JAR 缺失: {jar}")

        # 检查关键库是否存在
        if json_f.exists():
            import json
            try:
                info = json.loads(json_f.read_text(encoding="utf-8"))
                libs_dir = game_dir / "libraries"
                missing = 0
                for lib in info.get("libraries", []):
                    art = lib.get("downloads", {}).get("artifact", {})
                    lib_path = libs_dir / art.get("path", "")
                    if art.get("path") and not lib_path.exists():
                        missing += 1
                if missing > 0:
                    issues.append(f"缺失 {missing}/{len(info.get('libraries', []))} 个依赖库")
            except Exception:
                issues.append("版本 JSON 解析失败")

        return issues

    def _phase_check_java(self):
        java_path = cfg.javaPath.value
        if not java_path:
            return False, "未设置 Java 路径"
        if not Path(java_path).exists():
            return False, f"Java 文件不存在: {java_path}"
        self.log_line.emit(f"Java: {java_path}")
        log.info("启动 | Java 路径: %s", java_path)
        major, _ = detect_java_version(java_path)
        if major > 0:
            compatible, msg = is_java_compatible(major, self.version.id)
            self.log_line.emit(msg)
            log.info("启动 | Java 版本: %d, 兼容性: %s", major, msg)
        return True, ""

    def _phase_build_command(self):
        java_path = cfg.javaPath.value
        java_major, _ = detect_java_version(java_path)
        game_dir = Path(cfg.gameDirectory.value)
        version_id = self.version.id

        res_issues = self._check_integrity(game_dir, version_id)
        for issue in res_issues:
            self.log_line.emit(f"⚠ {issue}")
            log.warning("启动 | 资源问题: %s", issue)

        # Load version info with inheritsFrom resolution
        version_info = load_version_info(version_id, game_dir)
        if not version_info:
            return False, f"版本信息文件缺失: {game_dir / 'versions' / version_id / f'{version_id}.json'}"

        log.info("启动 | 版本 %s JSON 加载完成 (inheritsFrom: %s)",
                 version_id, version_info.get("inheritsFrom", "无"))

        # Main class — mod loader JSONs already set the correct mainClass
        main_class = version_info.get("mainClass", "net.minecraft.client.main.Main")
        version_lower = version_id.lower()
        if "fabric" in version_lower:
            self.log_line.emit("检测到 Fabric 版本")
            log.info("启动 | 模组加载器: Fabric")
        elif "forge" in version_lower and "neoforge" not in version_lower:
            self.log_line.emit("检测到 Forge 版本")
            log.info("启动 | 模组加载器: Forge")
        elif "neoforge" in version_lower:
            self.log_line.emit("检测到 NeoForge 版本")
            log.info("启动 | 模组加载器: NeoForge")

        # ── JVM 参数验证 ──
        supported_args = get_supported_jvm_args(java_major)
        removed = []
        if java_major < 9:
            removed.extend(a for a in supported_args if "sun-misc-unsafe" in a)
            supported_args = [a for a in supported_args if "sun-misc-unsafe" not in a]
        if java_major < 22:
            removed.extend(a for a in supported_args if "enable-native-access" in a)
            supported_args = [a for a in supported_args if "enable-native-access" not in a]
        if java_major < 23:
            removed.extend(a for a in supported_args if "UseCompactObjectHeaders" in a)
            supported_args = [a for a in supported_args if "UseCompactObjectHeaders" not in a]
        if removed:
            log.info("启动 | JVM 参数过滤: 移除了 %d 个不支持的参数", len(removed))

        self._command = build_command(
            java_path=java_path,
            version_id=version_id,
            version_info=version_info,
            game_dir=game_dir,
            memory_mb=cfg.maxMemoryMb.value,
            java_major=java_major,
            username=cfg.username.value,
            main_class=main_class,
        )
        self.log_line.emit(f"命令参数: {len(self._command)} 个")
        log.info("启动 | 命令构建完成 (%d 个参数)", len(self._command))
        return True, ""

    def _phase_start_process(self):
        game_dir = Path(cfg.gameDirectory.value)
        version_id = self.version.id
        self._run_dir = game_dir / "versions" / version_id if cfg.versionIsolation.value else game_dir

        try:
            self._process = launch_process(self._command, self._run_dir)
            pid = self._process.pid
            self.log_line.emit(f"进程 PID: {pid}")
            log.info("启动 | 游戏进程已启动 (PID: %d)", pid)

            # ── Windows 进程优先级 ──
            if is_windows():
                try:
                    import ctypes
                    handle = ctypes.windll.kernel32.OpenProcess(0x1F0FFF, False, pid)
                    if handle:
                        ctypes.windll.kernel32.SetPriorityClass(handle, 0x00004000)
                        ctypes.windll.kernel32.CloseHandle(handle)
                        self.log_line.emit("进程优先级: 低于正常")
                        log.info("启动 | 进程优先级已设为 BELOW_NORMAL")
                except Exception:
                    pass

            self.process_started.emit(self._process)

            def _read_stream(stream, label):
                try:
                    for line in iter(stream.readline, ""):
                        if not line:
                            break
                        self.log_line.emit(f"[{label}] {line.rstrip()}")
                except Exception:
                    pass

            threading.Thread(
                target=_read_stream, args=(self._process.stdout, "OUT"), daemon=True
            ).start()
            threading.Thread(
                target=_read_stream, args=(self._process.stderr, "ERR"), daemon=True
            ).start()

            return True, ""
        except Exception as e:
            log.error("启动 | 进程启动失败: %s", e)
            return False, f"进程启动失败: {e}"

    def _phase_wait_window(self):
        self.log_line.emit("等待游戏窗口… (最长 30 秒)")
        log.info("启动 | 开始轮询游戏窗口…")
        found = _poll_game_window(self._process, timeout=30.0)
        if found:
            self.window_detected.emit()
            self.log_line.emit("✅ 游戏窗口已出现")
            log.info("启动 | ✅ 游戏窗口已出现")
            return True, ""
        if self._process.poll() is not None:
            code = self._process.returncode
            log.warning("启动 | 游戏进程已退出 (exit code: %d)", code)
            return False, f"游戏进程已退出 (exit code: {code})"
        self.log_line.emit("⚠ 未检测到窗口，但进程仍在运行")
        log.info("启动 | 未检测到窗口，进程仍在运行")
        return True, ""

    def _phase_running(self):
        self.log_line.emit("游戏运行中…")
        log.info("启动 | 游戏运行中")
        return True, ""

    def _collect_crash_reports(self):
        """After game exit, scan for crash logs and copy them to launcher logs."""
        if not self._run_dir or not self._run_dir.exists():
            return

        from src.core.platform import default_log_directory
        crash_dst = default_log_directory("SilentXCraftLauncher") / "crashes"
        crash_dst.mkdir(parents=True, exist_ok=True)

        found_any = False

        # 1. Minecraft crash reports: run_dir/crash-reports/crash-*.txt
        for crash_file in self._run_dir.glob("crash-reports/crash-*.txt"):
            try:
                import shutil
                dst = crash_dst / f"{self.version.id}_{crash_file.name}"
                shutil.copy2(crash_file, dst)
                self.log_line.emit(f"📄 崩溃报告已保存: {dst.name}")
                found_any = True
            except Exception:
                pass

        # 2. JVM hs_err logs: run_dir/hs_err_pid*.log
        for hs_file in self._run_dir.glob("hs_err_pid*.log"):
            try:
                import shutil
                dst = crash_dst / f"{self.version.id}_{hs_file.name}"
                shutil.copy2(hs_file, dst)
                self.log_line.emit(f"📄 JVM 崩溃日志已保存: {dst.name}")
                found_any = True
            except Exception:
                pass

        # 3. Latest game log (last 50 lines of crash)
        latest_log = self._run_dir / "logs" / "latest.log"
        if latest_log.exists() and not found_any:
            try:
                log_dst = crash_dst / f"{self.version.id}_latest.log"
                # Copy last 100 lines
                with open(latest_log, "r", encoding="utf-8", errors="replace") as sf:
                    lines = sf.readlines()[-100:]
                with open(log_dst, "w", encoding="utf-8") as df:
                    df.writelines(lines)
            except Exception:
                pass

        if found_any:
            self.log_line.emit(f"💾 崩溃日志已保存至: {crash_dst}")


# ── Phase Indicator ───────────────────────────────────────────────


class LaunchIndicator(QWidget):
    """Small colored indicator dot for each phase."""

    DONE = 0
    ACTIVE = 1
    PENDING = 2
    FAILED = 3

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(12, 12)
        self._state = self.PENDING

    def set_state(self, state: int):
        self._state = state
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = self.rect().adjusted(1, 1, -1, -1)
        if self._state == self.DONE:
            painter.setBrush(QColor(82, 196, 26))   # green
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(r)
        elif self._state == self.ACTIVE:
            painter.setBrush(QColor(0, 120, 212))    # blue
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(r)
        elif self._state == self.FAILED:
            painter.setBrush(QColor(255, 77, 79))    # red
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(r)
        else:  # PENDING
            painter.setBrush(QColor(200, 200, 200))  # gray
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(r)


# ── Page ──────────────────────────────────────────────────────────


class LaunchProgressPage(BasePage):
    """Launch progress page — PCL2-style."""

    def __init__(self, version: GameVersion, parent=None):
        super().__init__(
            title=f"启动 {version.id}",
            subtitle="",
            parent=parent,
        )
        self.subtitleLabel.hide()
        self.version = version
        self._worker: LaunchWorker | None = None

        self._build_content()
        self._start_launch()

    def _build_content(self):
        card = CardWidget(self.view)
        card.setStyleSheet("CardWidget { border-radius: 8px; }")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(28, 20, 28, 20)
        layout.setSpacing(12)

        # ── Title row ──
        title_row = QHBoxLayout()
        self.phase_label = StrongBodyLabel("准备启动", card)
        title_row.addWidget(self.phase_label)
        title_row.addStretch(1)
        self.status_badge = BodyLabel("● 准备中", card)
        self.status_badge.setStyleSheet("color: #888888; font-weight: 500;")
        title_row.addWidget(self.status_badge)
        layout.addLayout(title_row)

        # ── Progress bar ──
        self.progress_bar = QProgressBar(card)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFixedHeight(4)
        self.progress_bar.setTextVisible(False)
        self.progress_bar.setStyleSheet("""
            QProgressBar { border: none; background: rgba(128,128,128,0.2); border-radius: 2px; }
            QProgressBar::chunk { background: #0078d4; border-radius: 2px; }
        """)
        layout.addWidget(self.progress_bar)

        # ── Phase list ──
        self.phases = [
            "检测 Java 运行时",
            "构建启动命令",
            "启动游戏进程",
            "等待游戏窗口",
            "运行完成",
        ]
        self._phase_widgets: list[tuple[BodyLabel, LaunchIndicator]] = []

        for name in self.phases:
            row = QHBoxLayout()
            row.setSpacing(8)
            dot = LaunchIndicator(card)
            row.addWidget(dot)
            label = BodyLabel(name, card)
            label.setTextColor("#999999", "#666666")
            row.addWidget(label)
            row.addStretch(1)
            layout.addLayout(row)
            self._phase_widgets.append((label, dot))

        # ── Log output ──
        self.log_output = BodyLabel("", card)
        self.log_output.setWordWrap(True)
        self.log_output.setTextColor("#888888", "#888888")
        self.log_output.setFixedHeight(40)
        layout.addWidget(self.log_output)

        self.add_content(card)

        # ── Buttons ──
        btn_layout = QHBoxLayout()
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

    def _start_launch(self):
        self._worker = LaunchWorker(self.version, self)
        self._worker.phase_changed.connect(self._on_phase_changed)
        self._worker.log_line.connect(self._on_log_line)
        self._worker.window_detected.connect(self._on_window_detected)
        self._worker.finished.connect(self._on_finished)
        self._worker.start()

    def _on_phase_changed(self, idx: int, total: int, name: str):
        # Mark previous phase as done
        if idx > 0 and idx - 1 < len(self._phase_widgets):
            prev_label, prev_dot = self._phase_widgets[idx - 1]
            prev_dot.set_state(LaunchIndicator.DONE)
            prev_label.setTextColor("#52c41a", "#73d13d")

        # Mark current as active
        if idx < len(self._phase_widgets):
            label, dot = self._phase_widgets[idx]
            dot.set_state(LaunchIndicator.ACTIVE)
            label.setTextColor("#0078d4", "#00bcf2")
            self.phase_label.setText(name)

        progress = int((idx / total) * 100) if total > 0 else 0
        self.progress_bar.setValue(progress)
        self.status_badge.setText("● 进行中")
        self.status_badge.setStyleSheet("color: #0078d4; font-weight: 500;")

    def _on_log_line(self, line: str):
        self.log_output.setText(line[:80])

    def _on_window_detected(self):
        # Auto close after 2 seconds
        from PySide6.QtCore import QTimer
        self.status_badge.setText("✓ 已启动")
        self.status_badge.setStyleSheet("color: #52c41a; font-weight: 500;")
        QTimer.singleShot(2000, self._auto_close)

    def _on_finished(self, success: bool, message: str):
        self.cancel_btn.setEnabled(False)
        self.back_btn.setEnabled(True)

        for label, dot in self._phase_widgets:
            if dot._state == LaunchIndicator.ACTIVE:
                if success:
                    dot.set_state(LaunchIndicator.DONE)
                    label.setTextColor("#52c41a", "#73d13d")
                else:
                    dot.set_state(LaunchIndicator.FAILED)
                    label.setTextColor("#ff4d4f", "#ff7875")

        if success:
            self.progress_bar.setValue(100)
            self.phase_label.setText("启动完成")
            self.status_badge.setText("✓ 完成")
            self.status_badge.setStyleSheet("color: #52c41a; font-weight: 500;")
            from PySide6.QtCore import QTimer
            QTimer.singleShot(2000, self._auto_close)
        else:
            self.progress_bar.setStyleSheet("""
                QProgressBar { border: none; background: rgba(128,128,128,0.2); border-radius: 2px; }
                QProgressBar::chunk { background: #ff4d4f; border-radius: 2px; }
            """)
            self.phase_label.setText(message)
            self.status_badge.setText("✗ 失败")
            self.status_badge.setStyleSheet("color: #ff4d4f; font-weight: 500;")
            self.log_output.setText(message)

    def _on_cancel(self):
        if self._worker:
            self._worker.cancel()
            self.cancel_btn.setEnabled(False)
            self.cancel_btn.setText("正在取消…")

    def _on_back(self):
        self._auto_close()

    def _auto_close(self):
        window = self.window()
        if hasattr(window, "go_back_to_versions"):
            window.go_back_to_versions()
