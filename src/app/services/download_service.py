# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher
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
"""Minecraft version download and installation service."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import threading
import time
import socket
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

import requests

from src.app.common.config import DownloadSource
from src.app.common.launcher_config import cfg
from src.app.common.logger import log, log_exception
from src.app.services.version_manifest import GameVersion
from src.app.services.mod_loader_service import ForgeAPI, FabricAPI, NeoForgeAPI


@dataclass
class DownloadTask:
    url: str
    path: Path
    size: int = 0


class VersionInstaller:
    """版本安装器 - 下载并安装指定 Minecraft 版本"""
    
    def __init__(self, game_dir: Path = None):
        try:
            self.game_dir = game_dir or Path(cfg.gameDirectory.value)
            self.source = cfg.downloadSource.value
            self._cancel = False
            self._progress = 0
            self._total = 0
            self._status = "准备下载"
            self._callbacks: List[Callable] = []
            self._file_progress_callback: Optional[Callable] = None
            
            self._byte_count = 0
            self._last_byte_count = 0
            self._last_time = time.time()
            self._current_file_downloaded = 0
            self._current_file_total = 0
            
            self._temp_files: List[Path] = []
            self._created_version_dir: Optional[Path] = None
            self._domain_reachable_cache: dict = {}
            
            log.info(f"VersionInstaller 初始化: game_dir={self.game_dir}, source={self.source}")
        except Exception as e:
            log_exception(log, f"VersionInstaller 初始化失败: {e}")
            raise
    
    # ================================================================
    # Ping 检测
    # ================================================================
    
    def _is_domain_reachable(self, domain: str, timeout: float = 2.0) -> bool:
        """检测域名是否可访问"""
        try:
            if domain in self._domain_reachable_cache:
                return self._domain_reachable_cache[domain]
            
            clean_domain = domain.replace("https://", "").replace("http://", "").split("/")[0].split(":")[0]
            ip = socket.gethostbyname(clean_domain)
            log.debug(f"Ping {clean_domain} -> {ip}")
            
            port = 443 if domain.startswith("https") else 80
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            result = sock.connect_ex((ip, port))
            sock.close()
            
            reachable = (result == 0)
            self._domain_reachable_cache[domain] = reachable
            log.info(f"Ping {clean_domain}: {'✅ 可达' if reachable else '❌ 不可达'}")
            return reachable
            
        except Exception as e:
            log_exception(log, f"Ping 检测失败: {domain}")
            self._domain_reachable_cache[domain] = False
            return False
    
    def _get_mirror_url(self, url: str) -> str:
        """生成 BMCLAPI 镜像 URL — 委托给 ``src.core.mirror``"""
        from src.core.mirror import maybe_mirror_url
        return maybe_mirror_url(url, source=self.source)
    
    # ================================================================
    # 下载核心
    # ================================================================
    
    def _download_file(self, url: str, path: Path, expected_size: int = 0) -> bool:
        """下载文件"""
        try:
            if self._cancel:
                return False
            
            download_url = self._get_mirror_url(url)
            
            log.info(f"下载: {path.name} ({'镜像' if download_url != url else '官方'})")
            log.info(f"  URL: {download_url}")
            
            # 如果是 BMCLAPI 的 assets 路径，需要特殊处理
            if "bmclapi2.bangbang93.com/assets" in download_url:
                # BMCLAPI assets 路径格式: https://bmclapi2.bangbang93.com/assets/{hash[:2]}/{hash}
                # 确保路径正确
                pass
            
            return self._download_single(download_url, path, expected_size)
            
        except Exception as e:
            log_exception(log, f"_download_file 失败: {url}")
            return False
    
    def _download_single(self, url: str, path: Path, expected_size: int = 0) -> bool:
        """单线程下载"""
        try:
            if self._cancel:
                return False
            
            if path.exists():
                if expected_size > 0 and path.stat().st_size == expected_size:
                    log.debug(f"文件已存在且大小匹配，跳过: {path.name}")
                    return True
                path.unlink()
            
            path.parent.mkdir(parents=True, exist_ok=True)
            
            # 使用 requests 下载
            response = requests.get(url, stream=True, timeout=60)
            response.raise_for_status()
            
            downloaded = 0
            with open(path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    if self._cancel:
                        return False
                    if chunk:
                        f.write(chunk)
                        downloaded += len(chunk)
                        self._byte_count += len(chunk)
                        # 通知进度
                        if self._file_progress_callback and expected_size > 0:
                            self._notify_file_progress(downloaded, expected_size)
            
            log.debug(f"下载完成: {path.name} ({downloaded} bytes)")
            return True
            
        except Exception as e:
            log.error(f"下载失败 {path.name}: {e}")
            return False
    
    def _notify_file_progress(self, downloaded: int, total: int) -> None:
        """通知文件进度"""
        try:
            if not self._file_progress_callback:
                return
            
            now = time.time()
            elapsed = now - self._last_time
            bytes_delta = self._byte_count - self._last_byte_count
            speed_mb = (bytes_delta / (1024 * 1024)) / elapsed if elapsed > 0 else 0
            
            eta = "计算中..."
            if speed_mb > 0 and total > downloaded:
                remaining_bytes = total - downloaded
                remaining_sec = remaining_bytes / (speed_mb * 1024 * 1024)
                if remaining_sec > 0:
                    if remaining_sec > 60:
                        eta = f"{int(remaining_sec // 60)}分{int(remaining_sec % 60)}秒"
                    elif remaining_sec > 1:
                        eta = f"{int(remaining_sec)}秒"
                    else:
                        eta = "< 1秒"
            
            self._last_byte_count = self._byte_count
            self._last_time = now
            
            self._file_progress_callback(downloaded, total, speed_mb, eta)
            
        except Exception as e:
            log_exception(log, "_notify_file_progress 失败")
    
    # ================================================================
    # 安装流程
    # ================================================================
    
    def set_progress_callback(self, callback: Callable[[int, int, str], None]) -> None:
        try:
            self._callbacks.append(callback)
        except Exception as e:
            log_exception(log, "set_progress_callback 失败")
    
    def set_file_progress_callback(self, callback: Callable[[int, int, float, str], None]) -> None:
        try:
            self._file_progress_callback = callback
        except Exception as e:
            log_exception(log, "set_file_progress_callback 失败")
    
    def _notify_progress(self) -> None:
        try:
            for cb in self._callbacks:
                cb(self._progress, self._total, self._status)
        except Exception as e:
            log_exception(log, "_notify_progress 失败")
    
    def cancel(self) -> None:
        self._cancel = True
        log.info("下载已取消")
    
    def _cleanup(self, keep_version_dir: bool = False) -> None:
        """清理临时文件"""
        try:
            for file in self._temp_files:
                try:
                    if file.exists():
                        file.unlink()
                        log.debug(f"删除临时文件: {file}")
                except Exception as e:
                    log.warning(f"删除临时文件失败: {file} - {e}")
            self._temp_files.clear()
            
            if not keep_version_dir and self._created_version_dir:
                try:
                    if self._created_version_dir.exists():
                        shutil.rmtree(self._created_version_dir)
                        log.info(f"删除空版本目录: {self._created_version_dir}")
                except Exception as e:
                    log.warning(f"删除版本目录失败: {e}")
                    
        except Exception as e:
            log_exception(log, "_cleanup 失败")
    
    def _check_version_exists(self, version_name: str) -> bool:
        """检查版本是否已存在"""
        try:
            version_dir = self.game_dir / "versions" / version_name
            if not version_dir.exists():
                return False
            
            jar_path = version_dir / f"{version_name}.jar"
            json_path = version_dir / f"{version_name}.json"
            if jar_path.exists() and json_path.exists():
                return True
            return True
        except Exception as e:
            log_exception(log, "_check_version_exists 失败")
            return True
    
    def install_version(
        self,
        version: GameVersion,
        loader_type: str = "none",
        loader_version: str = None,
        version_name: str = None
    ) -> bool:
        """安装指定版本"""
        try:
            version_id = version.id
            version_name = version_name or version_id
            
            log.info(f"开始安装版本: {version_id} -> {version_name}, 加载器: {loader_type}/{loader_version}")
            
            if self._check_version_exists(version_name):
                self._status = f"版本 {version_name} 已存在"
                self._notify_progress()
                log.warning(f"版本已存在: {version_name}")
                return False
            
            self._cancel = False
            self._progress = 0
            self._total = 0
            self._status = "开始安装"
            self._notify_progress()
            
            version_dir = self.game_dir / "versions" / version_name
            self._created_version_dir = version_dir
            
            # ============================================
            # Step 1: 获取版本详情
            # ============================================
            self._status = "获取版本信息"
            self._notify_progress()
            
            version_data = self._fetch_version_detail(version.url)
            if not version_data:
                self._status = "获取版本信息失败"
                self._notify_progress()
                self._cleanup(keep_version_dir=False)
                return False
            
            # ============================================
            # Step 2: 下载版本 JSON
            # ============================================
            self._progress += 1
            self._status = "下载版本配置"
            self._notify_progress()
            
            version_json_path = version_dir / f"{version_name}.json"
            if not self._download_file(version.url, version_json_path):
                self._status = "下载版本配置失败"
                self._notify_progress()
                self._cleanup(keep_version_dir=False)
                return False
            
            # ============================================
            # Step 3: 下载客户端 JAR
            # ============================================
            self._progress += 1
            self._status = "下载客户端"
            self._notify_progress()
            
            if not self._download_client(version_data, version_dir, version_name):
                self._status = "下载客户端失败"
                self._notify_progress()
                self._cleanup(keep_version_dir=False)
                return False
            
            # ============================================
            # Step 4: 下载 Libraries
            # ============================================
            self._status = "下载依赖库"
            self._notify_progress()
            
            if not self._download_libraries(version_data):
                self._status = "下载依赖库失败"
                self._notify_progress()
                self._cleanup(keep_version_dir=False)
                return False
            
            # ============================================
            # Step 5: 下载 Assets
            # ============================================
            self._status = "下载资源文件"
            self._notify_progress()
            
            if not self._download_assets(version_data):
                self._status = "下载资源文件失败"
                self._notify_progress()
                self._cleanup(keep_version_dir=False)
                return False
            
            # ============================================
            # Step 6: 保存版本元数据
            # ============================================
            self._progress += 1
            self._status = "保存版本信息"
            self._notify_progress()
            
            self._save_version_meta(version_dir, version_data, version_name)
            
            # ============================================
            # Step 7: 安装模组加载器
            # ============================================
            if loader_type != "none" and loader_version:
                self._progress += 1
                
                if loader_type == "forge":
                    self._status = "安装 Forge"
                    self._notify_progress()
                    if not self._install_forge(version_id, loader_version, version_name):
                        self._cleanup(keep_version_dir=False)
                        return False
                elif loader_type == "neoforge":
                    self._status = "安装 NeoForge"
                    self._notify_progress()
                    if not self._install_neoforge(version_id, loader_version, version_name):
                        self._cleanup(keep_version_dir=False)
                        return False
                elif loader_type == "fabric":
                    self._status = "安装 Fabric"
                    self._notify_progress()
                    if not self._install_fabric(version_id, loader_version, version_name):
                        self._cleanup(keep_version_dir=False)
                        return False
            
            # ============================================
            # 完成
            # ============================================
            self._status = "正在清理..."
            self._notify_progress()
            self._cleanup(keep_version_dir=True)
            
            self._status = "安装完成"
            self._progress = self._total
            self._notify_progress()
            log.info(f"安装完成: {version_name}")
            return True
            
        except Exception as e:
            log_exception(log, f"install_version 失败: {version_name}")
            self._status = f"安装失败: {str(e)}"
            self._notify_progress()
            self._cleanup(keep_version_dir=False)
            return False
    
    # ================================================================
    # 各步骤实现
    # ================================================================
    
    def _fetch_version_detail(self, url: str) -> dict | None:
        try:
            if self.source == DownloadSource.BMCLAPI:
                url = url.replace("piston-meta.mojang.com", "bmclapi2.bangbang93.com")
            response = requests.get(url, timeout=30)
            response.raise_for_status()
            return response.json()
        except Exception as e:
            log_exception(log, f"_fetch_version_detail 失败: {url}")
            return None
    
    def _download_client(self, version_data: dict, version_dir: Path, version_name: str) -> bool:
        try:
            downloads = version_data.get("downloads", {})
            client_info = downloads.get("client")
            if not client_info:
                log.error("版本数据中缺少 client 信息")
                return False
            
            url = client_info.get("url", "")
            size = client_info.get("size", 0)
            
            log.info(f"原版 JAR: {url}")
            log.info(f"原版 JAR 大小: {size / 1024 / 1024:.2f} MB")
            
            jar_path = version_dir / f"{version_name}.jar"
            return self._download_file(url, jar_path, size)
            
        except Exception as e:
            log_exception(log, "_download_client 失败")
            return False
    
    def _download_libraries(self, version_data: dict) -> bool:
        try:
            libraries = version_data.get("libraries", [])
            total = len(libraries)
            
            for i, lib in enumerate(libraries):
                if self._cancel:
                    return False
                
                self._progress = 2 + int(i / total * 2)
                self._status = f"下载依赖库 ({i+1}/{total})"
                self._notify_progress()
                
                downloads = lib.get("downloads", {})
                artifact = downloads.get("artifact")
                if not artifact:
                    continue
                
                url = artifact.get("url", "")
                path = artifact.get("path", "")
                size = artifact.get("size", 0)
                
                lib_path = self.game_dir / "libraries" / path
                lib_path.parent.mkdir(parents=True, exist_ok=True)
                
                if lib_path.exists() and lib_path.stat().st_size == size:
                    continue
                
                if not self._download_file(url, lib_path, size):
                    log.warning(f"库下载失败，继续: {path}")
                    continue
            
            return True
            
        except Exception as e:
            log_exception(log, "_download_libraries 失败")
            return False
    
    def _download_assets(self, version_data: dict) -> bool:
        try:
            asset_index = version_data.get("assetIndex", {})
            url = asset_index.get("url", "")
            if not url:
                return True
            
            index_path = self.game_dir / "assets" / "indexes" / f"{asset_index.get('id', '')}.json"
            index_path.parent.mkdir(parents=True, exist_ok=True)
            
            if not self._download_file(url, index_path):
                return False
            
            try:
                with open(index_path, 'r', encoding='utf-8') as f:
                    index_data = json.load(f)
                
                objects = index_data.get("objects", {})
                total = len(objects)
                items = list(objects.items())
                
                for i, (key, info) in enumerate(items):
                    if self._cancel:
                        return False
                    
                    if i % 10 == 0:
                        self._progress = 4 + int(i / total * 1)
                        self._status = f"下载资源 ({i+1}/{total})"
                        self._notify_progress()
                    
                    hash_val = info.get("hash", "")
                    size = info.get("size", 0)
                    
                    obj_path = self.game_dir / "assets" / "objects" / hash_val[:2] / hash_val
                    obj_path.parent.mkdir(parents=True, exist_ok=True)
                    
                    if obj_path.exists() and obj_path.stat().st_size == size:
                        continue
                    
                    from src.core.mirror import maybe_mirror_url
                    asset_url = maybe_mirror_url(
                        f"https://resources.download.minecraft.net/{hash_val[:2]}/{hash_val}",
                        source=self.source,
                    )
                    if not self._download_file(asset_url, obj_path, size):
                        continue
                        
            except Exception as e:
                log.warning(f"资源索引处理失败: {e}")
            
            return True
            
        except Exception as e:
            log_exception(log, "_download_assets 失败")
            return False
    
    def _save_version_meta(self, version_dir: Path, version_data: dict, version_name: str) -> None:
        try:
            meta_path = version_dir / f"{version_name}.json"
            version_data['id'] = version_name
            with open(meta_path, 'w', encoding='utf-8') as f:
                json.dump(version_data, f, ensure_ascii=False, indent=2)
            log.debug(f"版本元数据已保存: {meta_path}")
        except Exception as e:
            log_exception(log, "_save_version_meta 失败")
    
    # ================================================================
    # 模组加载器安装
    # ================================================================
    
    def _install_forge(self, mc_version: str, forge_version: str, version_name: str) -> bool:
        """安装 Forge"""
        installer_path = None
        try:
            forge_id = f"{mc_version}-{forge_version}"
            installer_url = ForgeAPI.get_installer_url(forge_id)
            
            installer_path = self.game_dir / "versions" / version_name / f"forge-{forge_id}-installer.jar"
            installer_path.parent.mkdir(parents=True, exist_ok=True)
            self._temp_files.append(installer_path)
            
            log.info(f"Forge 安装器: {forge_id}")
            
            self._status = f"下载 Forge 安装器 ({forge_version})"
            self._notify_progress()
            
            if not self._download_file(installer_url, installer_path):
                error_msg = f"Forge 安装器下载失败: {installer_url}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            if not installer_path.exists() or installer_path.stat().st_size == 0:
                error_msg = f"Forge 安装器文件无效: {installer_path}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            java_path = cfg.javaPath.value
            if not java_path:
                import shutil
                java_path = shutil.which("java")
                if not java_path:
                    error_msg = "未找到 Java 运行时，请安装 Java 17 或更高版本"
                    log.error(error_msg)
                    self._status = error_msg
                    self._notify_progress()
                    return False
            
            self._status = "执行 Forge 安装器"
            self._notify_progress()
            
            # 尝试多种参数
            cmd_variants = [
                # 新版 Forge: --installDir
                ([java_path, "-jar", str(installer_path), "--installDir", str(self.game_dir)], "新版参数 --installDir"),
                # 旧版 Forge: --installClient --target
                ([java_path, "-jar", str(installer_path), "--installClient", "--target", str(self.game_dir)], "旧版参数 --installClient --target"),
                # 不带参数
                ([java_path, "-jar", str(installer_path)], "无参数"),
            ]
            
            success = False
            last_error = None
            
            for cmd, desc in cmd_variants:
                try:
                    log.info(f"Forge 尝试 {desc}: {' '.join(cmd)}")
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=180, cwd=str(self.game_dir))
                    
                    if result.returncode == 0:
                        log.info(f"Forge 安装成功 (使用 {desc})")
                        success = True
                        break
                    else:
                        error_output = result.stderr or result.stdout or "无输出"
                        log.warning(f"Forge {desc} 失败: {error_output[:200]}")
                        last_error = error_output
                        continue
                except subprocess.TimeoutExpired:
                    log.warning(f"Forge {desc} 超时")
                    last_error = "超时"
                    continue
                except Exception as e:
                    log.warning(f"Forge {desc} 异常: {e}")
                    last_error = str(e)
                    continue
            
            if not success:
                error_msg = f"Forge 安装失败: {last_error}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            # 处理自定义版本名
            default_dir = self.game_dir / "versions" / forge_id
            custom_dir = self.game_dir / "versions" / version_name
            
            if version_name != forge_id and default_dir.exists():
                self._copy_version_dir(default_dir, custom_dir, version_name)
            
            log.info(f"Forge 安装完成: {version_name}")
            return True
            
        except Exception as e:
            error_msg = f"Forge 安装异常: {str(e)}"
            log_exception(log, error_msg)
            self._status = error_msg
            self._notify_progress()
            return False
    
    def _install_fabric(self, mc_version: str, loader_version: str, version_name: str) -> bool:
        """安装 Fabric"""
        installer_path = None
        try:
            installer_url = FabricAPI.get_installer_url()
            
            version_dir = self.game_dir / "versions" / version_name
            version_dir.mkdir(parents=True, exist_ok=True)
            installer_path = version_dir / "fabric-installer.jar"
            self._temp_files.append(installer_path)
            
            log.info(f"Fabric 安装器: {loader_version}")
            
            self._status = "下载 Fabric 安装器"
            self._notify_progress()
            
            if not self._download_file(installer_url, installer_path):
                error_msg = f"Fabric 安装器下载失败: {installer_url}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            java_path = cfg.javaPath.value
            if not java_path:
                import shutil
                java_path = shutil.which("java")
                if not java_path:
                    error_msg = "未找到 Java 运行时"
                    log.error(error_msg)
                    self._status = error_msg
                    self._notify_progress()
                    return False
            
            self._status = f"安装 Fabric ({loader_version})"
            self._notify_progress()
            
            # Fabric: client --mcversion --loader --dir [--name]
            cmd_variants = [
                # 带 --name
                ([java_path, "-jar", str(installer_path), "client", "--mcversion", mc_version, "--loader", loader_version, "--dir", str(self.game_dir), "--name", version_name], "带 --name"),
                # 不带 --name
                ([java_path, "-jar", str(installer_path), "client", "--mcversion", mc_version, "--loader", loader_version, "--dir", str(self.game_dir)], "不带 --name"),
            ]
            
            success = False
            last_error = None
            source_dir = None
            
            for cmd, desc in cmd_variants:
                try:
                    log.info(f"Fabric 尝试 {desc}: {' '.join(cmd)}")
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120, cwd=str(self.game_dir))
                    
                    if result.returncode == 0:
                        log.info(f"Fabric 安装成功 (使用 {desc})")
                        success = True
                        break
                    else:
                        error_output = result.stderr or result.stdout or "无输出"
                        log.warning(f"Fabric {desc} 失败: {error_output[:200]}")
                        last_error = error_output
                        continue
                except subprocess.TimeoutExpired:
                    log.warning(f"Fabric {desc} 超时")
                    last_error = "超时"
                    continue
                except Exception as e:
                    log.warning(f"Fabric {desc} 异常: {e}")
                    last_error = str(e)
                    continue
            
            if not success:
                error_msg = f"Fabric 安装失败: {last_error}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            # 查找生成的版本目录
            possible_dirs = [
                self.game_dir / "versions" / mc_version,
                self.game_dir / "versions" / f"fabric-loader-{loader_version}-{mc_version}",
                self.game_dir / "versions" / version_name,
            ]
            
            for d in possible_dirs:
                if d.exists() and any(f.name.endswith('.jar') for f in d.iterdir() if f.is_file()):
                    source_dir = d
                    break
            
            if source_dir and source_dir != version_dir:
                self._copy_version_dir(source_dir, version_dir, version_name)
            
            log.info(f"Fabric 安装完成: {version_name}")
            return True
            
        except Exception as e:
            error_msg = f"Fabric 安装异常: {str(e)}"
            log_exception(log, error_msg)
            self._status = error_msg
            self._notify_progress()
            return False
    
    def _install_neoforge(self, mc_version: str, neoforge_version: str, version_name: str) -> bool:
        """安装 NeoForge"""
        installer_path = None
        try:
            installer_url = NeoForgeAPI.get_installer_url(neoforge_version)
            
            installer_path = self.game_dir / "versions" / version_name / f"neoforge-{neoforge_version}-installer.jar"
            installer_path.parent.mkdir(parents=True, exist_ok=True)
            self._temp_files.append(installer_path)
            
            log.info(f"NeoForge 安装器: {neoforge_version}")
            
            self._status = f"下载 NeoForge 安装器 ({neoforge_version})"
            self._notify_progress()
            
            if not self._download_file(installer_url, installer_path):
                error_msg = f"NeoForge 安装器下载失败: {installer_url}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            if not installer_path.exists() or installer_path.stat().st_size == 0:
                error_msg = f"NeoForge 安装器文件无效: {installer_path}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            java_path = cfg.javaPath.value
            if not java_path:
                import shutil
                java_path = shutil.which("java")
                if not java_path:
                    error_msg = "未找到 Java 运行时"
                    log.error(error_msg)
                    self._status = error_msg
                    self._notify_progress()
                    return False
            
            self._status = "执行 NeoForge 安装器"
            self._notify_progress()
            
            # NeoForge 参数尝试
            cmd_variants = [
                # 标准: --installClient --installDir
                ([java_path, "-jar", str(installer_path), "--installClient", "--installDir", str(self.game_dir)], "标准参数 --installClient --installDir"),
                # 仅 --installDir
                ([java_path, "-jar", str(installer_path), "--installDir", str(self.game_dir)], "仅 --installDir"),
                # 带 --headless
                ([java_path, "-jar", str(installer_path), "--installClient", "--installDir", str(self.game_dir), "--headless"], "带 --headless"),
                # 不带参数
                ([java_path, "-jar", str(installer_path)], "无参数"),
            ]
            
            success = False
            last_error = None
            source_dir = None
            
            for cmd, desc in cmd_variants:
                try:
                    log.info(f"NeoForge 尝试 {desc}: {' '.join(cmd)}")
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, cwd=str(self.game_dir))
                    
                    if result.returncode == 0:
                        log.info(f"NeoForge 安装成功 (使用 {desc})")
                        success = True
                        break
                    else:
                        error_output = result.stderr or result.stdout or "无输出"
                        log.warning(f"NeoForge {desc} 失败: {error_output[:200]}")
                        last_error = error_output
                        continue
                except subprocess.TimeoutExpired:
                    log.warning(f"NeoForge {desc} 超时")
                    last_error = "超时"
                    continue
                except Exception as e:
                    log.warning(f"NeoForge {desc} 异常: {e}")
                    last_error = str(e)
                    continue
            
            if not success:
                error_msg = f"NeoForge 安装失败: {last_error}"
                log.error(error_msg)
                self._status = error_msg
                self._notify_progress()
                return False
            
            # 查找生成的版本目录
            possible_dirs = [
                self.game_dir / "versions" / f"neoforge-{neoforge_version}",
                self.game_dir / "versions" / mc_version,
                self.game_dir / "versions" / version_name,
            ]
            
            for d in possible_dirs:
                if d.exists() and any(f.name.endswith('.jar') for f in d.iterdir() if f.is_file()):
                    source_dir = d
                    break
            
            if source_dir and source_dir != version_dir:
                self._copy_version_dir(source_dir, version_dir, version_name)
            
            log.info(f"NeoForge 安装完成: {version_name}")
            return True
            
        except Exception as e:
            error_msg = f"NeoForge 安装异常: {str(e)}"
            log_exception(log, error_msg)
            self._status = error_msg
            self._notify_progress()
            return False
    
    def _copy_version_dir(self, source_dir: Path, target_dir: Path, version_name: str):
        """复制版本目录并修改 version.json"""
        try:
            target_dir.mkdir(parents=True, exist_ok=True)
            
            # 复制所有文件
            for item in source_dir.iterdir():
                if item.is_file():
                    dest = target_dir / item.name
                    shutil.copy2(item, dest)
            
            # 修改 version.json 中的 id
            json_files = list(target_dir.glob("*.json"))
            for json_file in json_files:
                try:
                    with open(json_file, 'r', encoding='utf-8') as f:
                        data = json.load(f)
                    
                    if data.get('id') != version_name:
                        data['id'] = version_name
                        new_json_path = target_dir / f"{version_name}.json"
                        with open(new_json_path, 'w', encoding='utf-8') as f:
                            json.dump(data, f, ensure_ascii=False, indent=2)
                        
                        if json_file != new_json_path:
                            json_file.unlink()
                except Exception as e:
                    log.warning(f"修改 version.json 失败: {e}")
            
            # 重命名 jar 文件
            jar_files = list(target_dir.glob("*.jar"))
            for jar_file in jar_files:
                if jar_file.stem != version_name and "installer" not in jar_file.name:
                    new_jar_path = target_dir / f"{version_name}.jar"
                    try:
                        jar_file.rename(new_jar_path)
                    except Exception as e:
                        log.warning(f"重命名 jar 失败: {e}")
                        
        except Exception as e:
            log.warning(f"复制版本目录失败: {e}")


# ================================================================
# 辅助函数
# ================================================================

def get_installed_versions(game_dir: Path = None) -> List[str]:
    try:
        game_dir = game_dir or Path(cfg.gameDirectory.value)
        versions_dir = game_dir / "versions"
        
        if not versions_dir.exists():
            return []
        
        versions = []
        for version_dir in versions_dir.iterdir():
            if version_dir.is_dir():
                jar_path = version_dir / f"{version_dir.name}.jar"
                json_path = version_dir / f"{version_dir.name}.json"
                if jar_path.exists() and json_path.exists():
                    versions.append(version_dir.name)
        
        return sorted(versions, reverse=True)
    except Exception as e:
        log_exception(log, "get_installed_versions 失败")
        return []


def get_version_info(version_id: str, game_dir: Path = None) -> dict | None:
    try:
        game_dir = game_dir or Path(cfg.gameDirectory.value)
        json_path = game_dir / "versions" / version_id / f"{version_id}.json"
        
        if not json_path.exists():
            return None
        
        with open(json_path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        log_exception(log, f"get_version_info 失败: {version_id}")
        return None