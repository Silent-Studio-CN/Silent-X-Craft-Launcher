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
"""模组加载器安装器 - 流程编排"""

from pathlib import Path
from typing import Optional, Callable, List
import shutil
import json
import subprocess

from src.app.common.launcher_config import cfg
from src.app.common.logger import log, log_exception


class ModLoaderInstaller:
    """模组加载器安装器"""
    
    def __init__(self, game_dir: Path):
        self.game_dir = game_dir
        self._progress_callback: Optional[Callable] = None
        self._manual_mode = False  # True 时跳过 _handle_generated_files
    
    def set_progress_callback(self, callback: Callable):
        self._progress_callback = callback
    
    def _notify_progress(self, current: int, total: int, status: str):
        if self._progress_callback:
            self._progress_callback(current, total, status)
    
    def install(
        self,
        mc_version: str,
        loader_type: str,
        loader_version: str,
        installer_path: Path,
        custom_name: str
    ) -> bool:
        """安装模组加载器。先尝试子进程 CLI，失败后回退到手动安装。"""
        try:
            # 1. 获取 Java
            java_path = cfg.javaPath.value
            if not java_path:
                import shutil
                java_path = shutil.which("java")
                if not java_path:
                    raise RuntimeError("未找到 Java 运行时")

            # 2. 尝试 CLI 静默安装
            self._notify_progress(0, 100, f"执行 {loader_type} 安装器")
            success, gui_used = self._try_cli_install(java_path, installer_path, mc_version, loader_version, custom_name)

            # 3. 若 CLI 全失败，回退到手动安装（绕过安装器 GUI）
            if not success:
                self._notify_progress(30, 100, f"{loader_type} CLI 失败，尝试手动安装")
                success = self._manual_install(installer_path, mc_version, loader_type, loader_version, custom_name)

            if not success:
                self._notify_progress(100, 100, f"{loader_type} 安装失败")
                return False

            # 4. 处理生成的文件（手动模式已生成，跳过）
            if not self._manual_mode:
                self._notify_progress(80, 100, f"整理 {loader_type} 文件")
                self._handle_generated_files(mc_version, loader_type, loader_version, custom_name)

            self._notify_progress(100, 100, f"{loader_type} 安装完成")
            return True

        except Exception as e:
            log_exception(log, f"[ModLoaderInstaller] 安装失败: {e}")
            self._notify_progress(100, 100, f"安装失败: {str(e)}")
            return False

    def _try_cli_install(self, java_path, installer_path, mc_version, loader_version, custom_name):
        """尝试 CLI 静默安装，返回 (success, gui_used)。"""
        cmd_variants = self._build_command_variants(
            java_path, installer_path, mc_version, loader_version, custom_name
        )

        for cmd, desc in cmd_variants:
            try:
                log.info(f"[ModLoaderInstaller] 尝试 {desc}: {' '.join(cmd)}")
                result = subprocess.run(
                    cmd, capture_output=True, text=True, timeout=300,
                    cwd=str(self.game_dir),
                )

                if result.returncode == 0:
                    log.info(f"[ModLoaderInstaller] 安装成功 (使用 {desc})")
                    return True, False
                else:
                    error_output = (result.stderr or result.stdout or "无输出")[:200]
                    log.warning(f"[ModLoaderInstaller] {desc} 失败: {error_output}")
                    continue
            except subprocess.TimeoutExpired:
                log.warning(f"[ModLoaderInstaller] {desc} 超时")
                continue
            except Exception as e:
                log.warning(f"[ModLoaderInstaller] {desc} 异常: {e}")
                continue

        return False, False

    def _manual_install(self, installer_path, mc_version, loader_type, loader_version, custom_name):
        """绕过安装器 GUI，通过分析 install_profile.json 手动生成版本文件。"""
        log.info(f"[ModLoaderInstaller] 手动安装 {loader_type} {loader_version}")
        try:
            from src.app.services.installer.forge_analyzer import ForgeAnalyzer, NeoForgeAnalyzer

            if loader_type == "forge":
                analyzer = ForgeAnalyzer(installer_path)
            elif loader_type == "neoforge":
                analyzer = NeoForgeAnalyzer(installer_path)
            else:
                return False

            version_info = analyzer.get_version_info()
            if not version_info:
                log.error("[ModLoaderInstaller] 无法从安装器中提取版本信息")
                return False

            # 生成版本 JSON（patched by loader）
            version_dir = self.game_dir / "versions" / custom_name
            version_dir.mkdir(parents=True, exist_ok=True)

            json_path = version_dir / f"{custom_name}.json"
            with open(json_path, "w", encoding="utf-8") as f:
                json.dump(version_info, f, indent=2, ensure_ascii=False)

            # 复制 client jar（从原版版本目录中取）
            client_jar = version_dir / f"{custom_name}.jar"
            if not client_jar.exists():
                vanilla_jar = self.game_dir / "versions" / mc_version / f"{mc_version}.jar"
                if vanilla_jar.exists():
                    shutil.copy2(vanilla_jar, client_jar)
                    log.info(f"[ModLoaderInstaller] client.jar 已复制: {client_jar}")

            self._manual_mode = True
            log.info(f"[ModLoaderInstaller] 手动安装完成: {custom_name}")
            return True
        except Exception as e:
            log_exception(log, f"[ModLoaderInstaller] 手动安装失败: {e}")
            return False
    
    def _build_command_variants(
        self,
        java_path: str,
        installer_path: Path,
        mc_version: str,
        loader_version: str,
        custom_name: str
    ) -> List[tuple]:
        """构建参数组合"""
        loader_lower = str(installer_path).lower()
        variants = []
        
        if "forge" in loader_lower:
            variants = [
                ([java_path, "-jar", str(installer_path), "--installDir", str(self.game_dir)], "新版 --installDir"),
                ([java_path, "-jar", str(installer_path), "--installClient", "--target", str(self.game_dir)], "旧版 --installClient --target"),
                ([java_path, "-jar", str(installer_path)], "无参数"),
            ]
        elif "neoforge" in loader_lower:
            # 新 NeoForge: --nogui 加 = 语法；最后再试无参数（会弹 GUI，做最后保底）
            d = str(self.game_dir)
            variants = [
                ([java_path, "-jar", str(installer_path), "--installClient", f"--installDir={d}", "--nogui"], "新 --installClient --installDir= --nogui"),
                ([java_path, "-jar", str(installer_path), "--installClient", "--installDir", d, "--nogui"], "新 --installClient --installDir --nogui"),
                ([java_path, "-jar", str(installer_path), "--installClient", f"--installDir={d}"], "新 --installClient --installDir="),
                ([java_path, "-jar", str(installer_path), "--installClient", "--installDir", d], "旧 --installClient --installDir"),
                ([java_path, "-jar", str(installer_path), "--installDir", d], "仅 --installDir"),
            ]
        elif "fabric" in loader_lower:
            variants = [
                ([java_path, "-jar", str(installer_path), "client", "--mcversion", mc_version, "--loader", loader_version, "--dir", str(self.game_dir), "--name", custom_name], "带 --name"),
                ([java_path, "-jar", str(installer_path), "client", "--mcversion", mc_version, "--loader", loader_version, "--dir", str(self.game_dir)], "不带 --name"),
            ]
        else:
            variants = [([java_path, "-jar", str(installer_path)], "默认")]
        
        return variants
    
    def _handle_generated_files(self, mc_version: str, loader_type: str, loader_version: str, custom_name: str):
        """处理安装器生成的文件"""
        possible_dirs = [
            self.game_dir / "versions" / mc_version,
            self.game_dir / "versions" / f"{mc_version}-{loader_type}-{loader_version}",
            self.game_dir / "versions" / f"{loader_type}-{loader_version}-{mc_version}",
            self.game_dir / "versions" / custom_name,
        ]
        
        if loader_type == "fabric":
            possible_dirs.insert(0, self.game_dir / "versions" / f"fabric-loader-{loader_version}-{mc_version}")
        
        source_dir = None
        for d in possible_dirs:
            if d.exists():
                has_jar = any(f.name.endswith('.jar') and 'installer' not in f.name for f in d.iterdir() if f.is_file())
                has_json = any(f.name.endswith('.json') for f in d.iterdir() if f.is_file())
                if has_jar and has_json:
                    source_dir = d
                    break
        
        if not source_dir:
            log.warning(f"[ModLoaderInstaller] 找不到生成的版本目录")
            return
        
        target_dir = self.game_dir / "versions" / custom_name
        
        if source_dir == target_dir:
            log.info(f"[ModLoaderInstaller] 版本目录已就位: {target_dir}")
            return
        
        log.info(f"[ModLoaderInstaller] 复制 {source_dir} -> {target_dir}")
        target_dir.mkdir(parents=True, exist_ok=True)
        
        for item in source_dir.iterdir():
            if item.is_file():
                dest = target_dir / item.name
                shutil.copy2(item, dest)
        
        # 修改 version.json
        json_files = list(target_dir.glob("*.json"))
        for json_file in json_files:
            try:
                with open(json_file, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                
                if data.get('id') != custom_name:
                    data['id'] = custom_name
                    new_json_path = target_dir / f"{custom_name}.json"
                    with open(new_json_path, 'w', encoding='utf-8') as f:
                        json.dump(data, f, ensure_ascii=False, indent=2)
                    if json_file != new_json_path:
                        json_file.unlink()
            except Exception as e:
                log.warning(f"[ModLoaderInstaller] 修改 version.json 失败: {e}")
        
        # 重命名 jar
        for jar_file in list(target_dir.glob("*.jar")):
            if jar_file.stem != custom_name and 'installer' not in jar_file.name:
                new_jar_path = target_dir / f"{custom_name}.jar"
                try:
                    jar_file.rename(new_jar_path)
                except Exception as e:
                    log.warning(f"[ModLoaderInstaller] 重命名 jar 失败: {e}")
                    