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
"""模组加载器安装器 —— 全静默，两条路（参考 PCL 的做法）。

为什么是两条路（PCL 也是这么干的）：

    方式 A  跑安装器自己的静默入口（新 Forge >= 20 / NeoForge / Fabric）。
            关键是先把 launcher_profiles.json 补上 —— 安装器找不到这个文件会
            直接报 "There is no minecraft launcher profile" 然后什么都不装。
            进度按安装器自己打印的标记推进（Extracting json / Downloading libraries /
            Building Processors / Task: xxx），和 PCL 解析的是同一批字符串。

    方式 B  解包安装（老 Forge，以及任何 CLI 走不通的安装器）。
            完全不启动安装器进程，只从 jar 里取 install_profile.json / version.json，
            拼出版本 JSON，必要时把安装器自带的 maven/ 解包进 libraries/：
              * 新版（jar 里有 version.json）
              * Legacy 方式 1（install_profile.json 没有 install 键）
              * Legacy 方式 2（有 install 键，通用 jar 写在 install.filePath 里）

绝不回退到"无参数启动安装器"：那会弹出 Forge 的图形安装器，用户关掉窗口同样返回 0，
会被误判成安装成功（这个坑我们踩过）。
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import threading
import time
import zipfile
from collections import deque
from queue import Empty, Queue
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from src.core.logger import log, log_exception
from src.core.settings import settings


def _pump_output(stream, target: Queue) -> None:
    """把安装器输出送进队列；读到结尾放一个 None 表示结束。

    单独开线程是因为读 stdout 本身会阻塞 —— 阻塞发生在子线程，
    主线程才能同时盯着超时和"用户点了取消"。
    """
    try:
        if stream is not None:
            for raw in stream:
                target.put(raw.rstrip())
    except Exception:
        pass
    finally:
        target.put(None)


class ModLoaderInstaller:
    """模组加载器安装器（静默 / 可取消 / 有进度）。"""

    # 安装器自己打印的进度标记 -> (进度百分比, 中文说明)，取自 PCL 的解析表
    PROGRESS_MARKERS: Tuple[Tuple[str, int, str], ...] = (
        ("Extracting json", 35, "解压版本信息"),
        ("Downloading libraries", 45, "下载支持库"),
        ("Building Processors", 60, "执行安装处理器"),
    )
    TIMEOUT = 1800          # 安装器最长跑 30 分钟（大版本要下几百 MB 库）

    def __init__(self, game_dir: Path):
        self.game_dir = Path(game_dir)
        self._progress_callback: Optional[Callable] = None
        self._manual_mode = False            # True 时跳过 _handle_generated_files
        self._options_cache: Optional[set] = None
        self._last_installer_error = ""      # 最后一次失败的真实原因（给界面看）
        self._last_output: deque = deque(maxlen=80)   # 安装器输出的最后若干行
        self._cancel_check: Optional[Callable[[], bool]] = None   # 界面点了取消就杀进程

    # ── 进度 ───────────────────────────────────────────────────

    def set_progress_callback(self, callback: Callable) -> None:
        self._progress_callback = callback

    def set_cancel_check(self, callback: Callable[[], bool]) -> None:
        """注册"是否已取消"的回调：安装器跑一半用户点取消时能真的停下来。"""
        self._cancel_check = callback

    def _notify_progress(self, current: int, total: int, status: str) -> None:
        if self._progress_callback:
            self._progress_callback(current, total, status)

    # ── 主流程 ─────────────────────────────────────────────────

    def install(
        self,
        mc_version: str,
        loader_type: str,
        loader_version: str,
        installer_path: Path,
        custom_name: str,
    ) -> bool:
        """安装模组加载器：方式 A（静默 CLI）失败就走方式 B（解包安装）。"""
        try:
            java_path = settings.java_path()
            if not java_path:
                java_path = shutil.which("java")
            if not java_path:
                raise RuntimeError("未找到 Java 运行时")

            # Forge/NeoForge 安装器的硬性要求：游戏目录里得有 launcher_profiles.json
            self._ensure_launcher_profiles()

            self._notify_progress(0, 100, f"执行 {loader_type} 安装器")
            if loader_type == "optifine":
                success = self._install_optifine(
                    java_path, Path(installer_path), mc_version, custom_name)
            else:
                success = self._run_installer_silently(
                    java_path, Path(installer_path), mc_version, loader_version, custom_name, loader_type)

            if not success:
                self._notify_progress(40, 100, f"改用解包安装 {loader_type}")
                success = self._extract_install(
                    Path(installer_path), mc_version, loader_type, loader_version, custom_name)

            if not success:
                reason = self._last_installer_error or "安装器没有给出可用信息"
                log.error(f"[ModLoaderInstaller] {loader_type} 安装失败: {reason}")
                self._notify_progress(100, 100, f"{loader_type} 安装失败：{reason[:160]}")
                return False

            if not self._manual_mode:
                self._notify_progress(85, 100, f"整理 {loader_type} 文件")
                if not self._handle_generated_files(mc_version, loader_type, loader_version, custom_name):
                    log.error("[ModLoaderInstaller] 版本目录没整理出来，按失败处理")
                    self._notify_progress(100, 100, f"{loader_type} 安装失败：没有生成版本目录")
                    return False

            if not self._version_ready(custom_name):
                log.error("[ModLoaderInstaller] 版本目录里没有 JSON，算失败")
                self._notify_progress(100, 100, f"{loader_type} 安装失败：版本文件不完整")
                return False

            # 版本目录被我们动过了：把 PCL 的缓存同步成事实，并清掉它的版本列表缓存。
            # 不做这一步，PCL 那边会一直显示旧信息（它命中缓存后不再解析版本 JSON）。
            try:
                from src.services.minecraft.pcl_compat import sync_version
                sync_version(self.game_dir, custom_name)
            except Exception as exc:
                log.debug("[ModLoaderInstaller] 同步 PCL 缓存失败（不影响安装）: %s", exc)

            self._notify_progress(100, 100, f"{loader_type} 安装完成")
            return True

        except Exception as e:
            log_exception(log, f"[ModLoaderInstaller] 安装失败: {e}")
            self._notify_progress(100, 100, f"安装失败: {str(e)}")
            return False

    def _install_optifine(self, java_path: str, installer_path: Path, mc_version: str,
                          custom_name: str) -> bool:
        """OptiFine 的静默安装（它没有"能指定目录的静默 CLI"）。

        实测事实（2026-09，OptiFine 1.21.11_HD_U_J8_pre12 安装器）：
          * 不认 --help，直接开 GUI（会挂住，必须避免无参数运行）；
          * --installClient <目录> 被接受、不开 GUI，但它把游戏目录解析成
            %APPDATA%\.minecraft，**完全忽略我们传的路径**；
          * 所以做法是把 APPDATA 指到一个临时目录，在里面摆好原版版本 + launcher_profiles.json，
            让它"装到临时目录"，再把产物搬回真正的游戏目录（PCL 也是用临时 MC 目录干的）。
        """
        import os
        import tempfile
        from src.services.minecraft.pcl_compat import merge_launcher_profile

        real_game = Path(self.game_dir)
        temp_root = Path(tempfile.mkdtemp(prefix="sxcl-optifine-"))
        # OptiFine 看的是 %APPDATA%\.minecraft，所以造一个假的 APPDATA
        fake_appdata = temp_root / "appdata"
        fake_game = fake_appdata / ".minecraft"
        try:
            (fake_game / "versions" / mc_version).mkdir(parents=True, exist_ok=True)
            for suffix in (".json", ".jar"):
                source = real_game / "versions" / mc_version / f"{mc_version}{suffix}"
                if source.is_file():
                    shutil.copy2(source, fake_game / "versions" / mc_version / source.name)
                elif suffix == ".json":
                    log.error("[ModLoaderInstaller] 缺原版 %s.json，先装原版再装 OptiFine", mc_version)
                    self._last_installer_error = f"缺少原版 {mc_version} 的版本 JSON"
                    return False
            merge_launcher_profile(fake_game)
            before = {item.name for item in (fake_game / "versions").iterdir()}

            env = dict(os.environ)
            env["APPDATA"] = str(fake_appdata)
            self._notify_progress(20, 100, "OptiFine 正在安装（静默，不打图形界面）")
            result = subprocess.run(
                [str(java_path), "-jar", str(installer_path), "--installClient", str(fake_game)],
                capture_output=True, text=True, timeout=900, cwd=str(fake_game), env=env,
                encoding="utf-8", errors="replace",
                creationflags=(subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0),
            )
            output = ((result.stdout or "") + (result.stderr or "")).strip()
            if output:
                log.info("[ModLoaderInstaller] OptiFine 输出: %s", output[:400])

            after = {item.name for item in (fake_game / "versions").iterdir()}
            fresh = [name for name in sorted(after - before) if "optifine" in name.lower()]
            if not fresh:
                self._last_installer_error = output[-300:] or "OptiFine 没有生成版本目录"
                log.error("[ModLoaderInstaller] OptiFine 没生成版本目录，输出: %s", output[-300:])
                return False

            source_dir = fake_game / "versions" / fresh[0]
            log.info("[ModLoaderInstaller] OptiFine 装到了 %s，搬回 %s", fresh[0], custom_name)
            target_dir = real_game / "versions" / custom_name
            target_dir.mkdir(parents=True, exist_ok=True)
            for item in source_dir.iterdir():
                if item.is_file():
                    shutil.copy2(item, target_dir / item.name)

            # 它写的库（optifine/OptiFine、launchwrapper-of…）也要搬
            moved = 0
            fake_libs = fake_game / "libraries"
            if fake_libs.is_dir():
                for source in fake_libs.rglob("*.jar"):
                    rel = source.relative_to(fake_libs)
                    dest = real_game / "libraries" / rel
                    if dest.is_file():
                        continue
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, dest)
                    moved += 1
            log.info("[ModLoaderInstaller] 搬了 %d 个库文件", moved)

            self._normalize_version_files(custom_name)
            self._manual_mode = True          # 版本目录已经收拾好了，别再走 _handle_generated_files
            return self._version_ready(custom_name)
        except subprocess.TimeoutExpired:
            self._last_installer_error = "OptiFine 安装器超时（15 分钟）"
            log.error("[ModLoaderInstaller] OptiFine 安装超时")
            return False
        except Exception as exc:
            self._last_installer_error = str(exc)[:200]
            log_exception(log, f"[ModLoaderInstaller] OptiFine 安装失败: {exc}")
            return False
        finally:
            shutil.rmtree(temp_root, ignore_errors=True)


    # ── 方式 A：静默跑安装器 ───────────────────────────────────

    def _run_installer_silently(
        self, java_path: str, installer_path: Path, mc_version: str,
        loader_version: str, custom_name: str, loader_type: str,
    ) -> bool:
        """按安装器自己声明的参数静默运行（绝不弹 GUI）。"""
        before = self._versions_snapshot()
        for cmd, desc in self._build_command_variants(
                java_path, installer_path, mc_version, loader_version, custom_name):
            log.info(f"[ModLoaderInstaller] 尝试 {desc}: {' '.join(cmd)}")
            self._last_output.clear()
            try:
                process = subprocess.Popen(
                    cmd, cwd=str(self.game_dir), stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                    text=True, encoding="utf-8", errors="replace",
                    creationflags=(subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0),
                )
            except Exception as e:
                log.warning(f"[ModLoaderInstaller] {desc} 启动失败: {e}")
                continue

            # 直接 for line in process.stdout 会永久阻塞（安装器卡住 = 界面卡死），
            # 所以输出交给后台线程，主线程只管超时和取消。
            stream_queue: Queue = Queue()
            threading.Thread(target=_pump_output, args=(process.stdout, stream_queue),
                             daemon=True).start()
            started = time.monotonic()
            killed_reason = ""
            while True:
                try:
                    line = stream_queue.get(timeout=0.5)
                except Empty:
                    line = ""
                    if process.poll() is not None:
                        break
                if line is None:            # 输出结束
                    break
                if line:
                    self._last_output.append(line)
                    self._track_progress(line)
                if self._cancel_check and self._cancel_check():
                    killed_reason = "用户取消了安装"
                    break
                if time.monotonic() - started > self.TIMEOUT:
                    killed_reason = f"安装器超过 {self.TIMEOUT // 60} 分钟没有完成"
                    break
            if killed_reason:
                process.kill()
                try:
                    process.wait(timeout=30)
                except Exception:
                    pass
                log.warning(f"[ModLoaderInstaller] {desc} 中止：{killed_reason}")
                self._last_installer_error = killed_reason
                continue
            try:
                returncode = process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                self._last_installer_error = "安装器进程没有正常退出"
                continue

            # 成功判据：返回 0，且版本目录真的出现了（PCL 还会看最后几行有没有 true，这里当参考）
            printed_true = any(l.strip().lower() == "true" for l in list(self._last_output)[-6:])
            ready = self._version_ready(custom_name)
            if returncode == 0 and not ready:
                ready = self._adopt_generated_version(custom_name, before, loader_type)
            if returncode == 0 and ready:
                log.info(f"[ModLoaderInstaller] 安装成功（{desc}，安装器输出 true={printed_true}）")
                return True

            tail = self._installer_log_tail(installer_path) or " | ".join(list(self._last_output)[-4:])
            if tail:
                self._last_installer_error = tail
            log.warning(f"[ModLoaderInstaller] {desc} 没成功 (rc={returncode}, 版本就绪={ready}): {tail}")
        return False

    def _track_progress(self, line: str) -> None:
        """把安装器输出翻译成进度（字符串和 PCL 解析的同一批）。"""
        for marker, percent, label in self.PROGRESS_MARKERS:
            if marker in line:
                self._notify_progress(percent, 100, label)
                return
        if line.startswith("Task: "):
            self._notify_progress(70, 100, f"处理器任务 {line[6:].strip()}")

    def _installer_options(self, java_path: str, installer_path: Path) -> set:
        """跑一次 --help，问安装器它认哪些参数（不猜）。"""
        if self._options_cache is not None:
            return self._options_cache
        options: set = set()
        try:
            result = subprocess.run(
                [str(java_path), "-jar", str(installer_path), "--help"],
                capture_output=True, text=True, timeout=120, cwd=str(self.game_dir),
                creationflags=(subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0),
            )
            text = (result.stdout or "") + (result.stderr or "")
            options = set(re.findall(r"--([A-Za-z][A-Za-z0-9-]*)", text))
        except Exception as e:
            log.warning(f"[ModLoaderInstaller] 读取安装器参数失败: {e}")
        log.info("[ModLoaderInstaller] 安装器支持的参数: %s",
                 sorted(options) if options else "（无输出，走兜底/解包安装）")
        self._options_cache = options
        return options

    def _mirror_args(self, options: set) -> List[str]:
        """配置里选了 BMCLAPI 就把镜像地址也告诉安装器（下载依赖库快很多）。"""
        if "mirror" not in options:
            return []
        try:
            from src.core.constants import DownloadSource
            source = settings.download_source()
            name = getattr(source, "value", source)
            if str(name).lower() == DownloadSource.BMCLAPI.value:
                return ["--mirror", "https://bmclapi2.bangbang93.com/maven/"]
        except Exception as e:
            log.debug(f"[ModLoaderInstaller] 镜像参数跳过: {e}")
        return []

    def _build_command_variants(
        self, java_path: str, installer_path: Path, mc_version: str,
        loader_version: str, custom_name: str,
    ) -> List[tuple]:
        """构建静默参数组合：先用安装器自报的参数，历史写法放最后兜底。"""
        loader_lower = str(installer_path).lower()
        d = str(self.game_dir)
        base = [str(java_path), "-jar", str(installer_path)]
        options = self._installer_options(java_path, installer_path)
        mirror = self._mirror_args(options)

        # Fabric 是另一套 CLI
        if "fabric" in loader_lower:
            return [
                (base + ["client", "--mcversion", mc_version, "--loader", loader_version,
                         "--dir", d, "--name", custom_name], "Fabric client --name"),
                (base + ["client", "--mcversion", mc_version, "--loader", loader_version,
                         "--dir", d], "Fabric client"),
            ]

        # Forge 1.13+ / NeoForge：目录是参数的「值」
        # 正确写法：java -jar forge-installer.jar --installClient <游戏目录>
        variants: List[tuple] = []
        if "installClient" in options:
            variants.append((base + ["--installClient", d] + mirror, "现代 --installClient <目录>"))
            variants.append((base + ["--installClient"] + mirror, "现代 --installClient（装到当前目录）"))
        if "installDir" in options:
            variants.append((base + ["--installDir=" + d] + mirror, "旧 --installDir=<目录>"))

        # 兜底：很老的安装器 --help 没输出，只能按历史写法试（依旧不带参数 = 不弹 GUI）
        variants.extend([
            (base + ["--installClient", "--installDir=" + d], "兜底 --installClient --installDir="),
            (base + ["--installDir=" + d], "兜底 --installDir="),
            (base + ["--installClient", "--target", d], "兜底 --installClient --target"),
        ])
        return variants

    # ── 方式 B：解包安装（PCL 的做法） ────────────────────────

    def _extract_install(
        self, installer_path: Path, mc_version: str, loader_type: str,
        loader_version: str, custom_name: str,
    ) -> bool:
        """不启动安装器进程，直接从 jar 里拼出版本 JSON（老 Forge 只能这么装）。"""
        if loader_type not in ("forge", "neoforge"):
            return False
        log.info(f"[ModLoaderInstaller] 解包安装 {loader_type} {loader_version}")
        version_dir = self.game_dir / "versions" / custom_name
        try:
            with zipfile.ZipFile(installer_path) as jar:
                names = set(jar.namelist())
                profile = {}
                if "install_profile.json" in names:
                    profile = json.loads(jar.read("install_profile.json").decode("utf-8", "replace"))
                install = profile.get("install") or {}
                version_info = profile.get("versionInfo") or {}

                if version_info:
                    source = "install_profile.json 的 versionInfo（老格式）"
                elif "version.json" in names:
                    version_info = json.loads(jar.read("version.json").decode("utf-8", "replace"))
                    source = "安装器内的 version.json（Forge 1.13+ / NeoForge）"
                elif profile.get("json"):
                    entry = str(profile["json"]).lstrip("/")
                    version_info = json.loads(jar.read(entry).decode("utf-8", "replace"))
                    source = f"安装器内的 {entry}"
                else:
                    log.error("[ModLoaderInstaller] 安装器里没有可用的版本信息，解包安装做不了")
                    self._last_installer_error = "安装器里没有 version.json / versionInfo"
                    return False

                log.info(f"[ModLoaderInstaller] 解包安装：版本信息来自 {source}")
                version_info["id"] = custom_name
                version_info.setdefault("inheritsFrom", mc_version)
                version_dir.mkdir(parents=True, exist_ok=True)
                (version_dir / f"{custom_name}.json").write_text(
                    json.dumps(version_info, ensure_ascii=False, indent=2), encoding="utf-8")

                # 老格式：通用 jar 在 install.filePath 里，要落到 install.path 指的位置
                rel = str(install.get("path") or "").replace(chr(92), "/")
                entry = install.get("filePath")
                if rel and entry and entry in names:
                    dest = self.game_dir / "libraries" / Path(*rel.split("/"))
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(jar.read(entry))
                    log.info(f"[ModLoaderInstaller] 已解包通用 jar -> {dest}")

                # 安装器自带 maven/ 目录的一并解包进 libraries/
                bundled = [n for n in names if n.startswith("maven/") and not n.endswith("/")]
                for bundled_entry in bundled:
                    rel_path = bundled_entry[len("maven/"):]
                    dest = self.game_dir / "libraries" / Path(*rel_path.split("/"))
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(jar.read(bundled_entry))
                if bundled:
                    log.info(f"[ModLoaderInstaller] 已解包安装器自带的 {len(bundled)} 个库文件")
        except Exception as e:
            log_exception(log, f"[ModLoaderInstaller] 解包安装失败: {e}")
            self._last_installer_error = str(e)[:200]
            return False

        self._copy_vanilla_client(mc_version, custom_name)
        self._manual_mode = True
        log.info(f"[ModLoaderInstaller] 解包安装完成: {version_dir}")
        return self._version_ready(custom_name)

    def _copy_vanilla_client(self, mc_version: str, custom_name: str) -> None:
        """原版 client.jar 有就复制一份（继承也能跑，复制更保险）。"""
        version_dir = self.game_dir / "versions" / custom_name
        target = version_dir / f"{custom_name}.jar"
        if target.exists():
            return
        vanilla = self.game_dir / "versions" / mc_version / f"{mc_version}.jar"
        if vanilla.is_file():
            try:
                shutil.copy2(vanilla, target)
                log.info(f"[ModLoaderInstaller] 已复制原版 client.jar: {target.name}")
            except Exception as e:
                log.warning(f"[ModLoaderInstaller] 复制 client.jar 失败: {e}")

    # ── 收尾：整理版本目录 ─────────────────────────────────────

    def _versions_snapshot(self) -> set:
        folder = self.game_dir / "versions"
        try:
            return {p.name for p in folder.iterdir() if p.is_dir()}
        except Exception:
            return set()

    def _version_ready(self, custom_name: str) -> bool:
        """版本目录里必须有 JSON（jar 可以继承原版，不强制）。"""
        version_dir = self.game_dir / "versions" / custom_name
        if not version_dir.is_dir():
            return False
        try:
            return any(p.suffix == ".json" for p in version_dir.iterdir() if p.is_file())
        except Exception:
            return False

    def _adopt_generated_version(self, custom_name: str, before: set, loader_type: str) -> bool:
        """PCL 的招：安装器可能把版本写到别的文件夹（甚至名字写错），
        所以对比安装前后的目录列表，找出新增的那个，再搬成我们的命名。"""
        target = self.game_dir / "versions" / custom_name
        if self._version_ready(custom_name):
            return True
        fresh: List[Path] = []
        for name in sorted(self._versions_snapshot() - before):
            candidate = self.game_dir / "versions" / name
            if not candidate.is_dir():
                continue
            files = [f for f in candidate.iterdir() if f.is_file()]
            if not files:
                continue
            if loader_type.lower() in name.lower():
                fresh.append(candidate)
            elif any(f.suffix == ".json" for f in files):
                fresh.append(candidate)
        if not fresh:
            return False
        if len(fresh) > 1:
            log.warning("[ModLoaderInstaller] 新增了多个疑似版本目录，取第一个: %s",
                        [p.name for p in fresh])
        source = fresh[0]
        log.info(f"[ModLoaderInstaller] 安装器把版本写到了 {source.name}，搬成 {custom_name}")
        target.mkdir(parents=True, exist_ok=True)
        for item in source.iterdir():
            if item.is_file():
                shutil.copy2(item, target / item.name)
        try:
            if source != target and source.name != custom_name:
                shutil.rmtree(source, ignore_errors=True)
        except Exception:
            pass
        self._normalize_version_files(custom_name)
        return self._version_ready(custom_name)

    def _normalize_version_files(self, custom_name: str) -> None:
        """把版本 JSON 的 id 和文件名统一成 custom_name（启动器是按目录名找的）。"""
        target_dir = self.game_dir / "versions" / custom_name
        for json_file in list(target_dir.glob("*.json")):
            try:
                data = json.loads(json_file.read_text(encoding="utf-8"))
            except Exception as e:
                log.warning(f"[ModLoaderInstaller] 读不了 {json_file.name}: {e}")
                continue
            desired = target_dir / f"{custom_name}.json"
            changed = data.get("id") != custom_name
            data["id"] = custom_name
            if changed or json_file != desired:
                desired.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
                if json_file != desired:
                    json_file.unlink()

    def _handle_generated_files(
        self, mc_version: str, loader_type: str, loader_version: str, custom_name: str,
    ) -> bool:
        """安装器生成的文件不一定落在我们预设的目录名里，这里统一收拾。"""
        target_dir = self.game_dir / "versions" / custom_name
        if self._version_ready(custom_name):
            self._normalize_version_files(custom_name)
            log.info(f"[ModLoaderInstaller] 版本目录已就位: {target_dir}")
            return True

        possible = [
            self.game_dir / "versions" / f"{mc_version}-{loader_type}-{loader_version}",
            self.game_dir / "versions" / f"{loader_type}-{loader_version}-{mc_version}",
            self.game_dir / "versions" / f"{loader_type}-{loader_version}",
        ]
        if loader_type == "fabric":
            possible.insert(0, self.game_dir / "versions" / f"fabric-loader-{loader_version}-{mc_version}")

        for candidate in possible:
            if self._version_ready(candidate.name):
                log.info(f"[ModLoaderInstaller] 复制 {candidate} -> {target_dir}")
                target_dir.mkdir(parents=True, exist_ok=True)
                for item in candidate.iterdir():
                    if item.is_file():
                        shutil.copy2(item, target_dir / item.name)
                self._normalize_version_files(custom_name)
                return True

        log.warning("[ModLoaderInstaller] 找不到生成的版本目录")
        return False

    # ── 杂项 ───────────────────────────────────────────────────

    def _ensure_launcher_profiles(self) -> None:
        """保证 launcher_profiles.json 存在，并合并我们自己的 profile。

        Forge/NeoForge 的安装器找不到这个文件就直接罢工；但文件里可能还有别的启动器的
        数据（PCL 的 profile、微软登录信息），所以**只能合并，不能整份覆盖**。
        """
        target = self.game_dir / "launcher_profiles.json"
        try:
            from src.services.minecraft.pcl_compat import merge_launcher_profile
        except Exception as exc:
            merge_launcher_profile = None
            log.debug("[ModLoaderInstaller] 兼容层不可用: %s", exc)

        if target.exists():
            # 已存在：只把我们的条目并进去（缺少它也不会影响安装器，但合并一次更规矩）
            if merge_launcher_profile is not None:
                merge_launcher_profile(self.game_dir)
            return

        try:
            self.game_dir.mkdir(parents=True, exist_ok=True)
            from datetime import datetime, timezone
            stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.0000Z")
            profile = {
                "profiles": {
                    "SXCL": {
                        "icon": "Grass",
                        "name": "Silent X Craft Launcher",
                        "lastVersionId": "latest-release",
                        "type": "latest-release",
                        "lastUsed": stamp,
                    }
                },
                "selectedProfile": "SXCL",
                "clientToken": "23323323323323323323323323323333",
            }
            target.write_text(json.dumps(profile, indent=4), encoding="utf-8")
            log.info(f"[ModLoaderInstaller] 已补 launcher_profiles.json（安装器需要它）: {target}")
        except Exception as e:
            log.warning(f"[ModLoaderInstaller] 写 launcher_profiles.json 失败: {e}")

    def _installer_log_tail(self, installer_path: Path) -> str:
        """安装器把异常写进 jar 同目录（或工作目录）的 .log，捞出来给用户看。"""
        found = []
        for folder in (Path(installer_path).parent, self.game_dir):
            try:
                found.extend(folder.glob("*.log"))
            except Exception:
                continue
        if not found:
            return ""
        try:
            newest = max(found, key=lambda p: p.stat().st_mtime)
            lines = [line.strip() for line in
                     newest.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]
        except Exception:
            return ""
        hits = [line for line in lines
                if "Exception" in line or "Error" in line or "not a recognized" in line]
        return " | ".join((hits or lines)[-3:])[:400]
