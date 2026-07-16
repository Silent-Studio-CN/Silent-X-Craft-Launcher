"""Forge 安装器分析器"""

import json
from pathlib import Path
from typing import List, Dict, Optional

from .base import LoaderAnalyzer

_OFFICIAL_LIBRARIES = "https://libraries.minecraft.net/"
_OFFICIAL_FORGE_MAVEN = "https://maven.minecraftforge.net/"


def _mirror_url(url: str) -> str:
    """Replace official URL with BMCLAPI mirror when configured."""
    from src.core.mirror import maybe_mirror_url
    return maybe_mirror_url(url)


class ForgeAnalyzer(LoaderAnalyzer):
    """Forge 安装器分析器"""
    
    def __init__(self, installer_path: Path):
        super().__init__(installer_path)
        self._profile = None
        self._version_info = None
        self._load_metadata()
    
    def get_loader_type(self) -> str:
        return "forge"
    
    def _load_metadata(self):
        """加载 install_profile.json"""
        content = self._read_file_from_jar('install_profile.json')
        if content:
            self._profile = json.loads(content)
            self._version_info = self._profile.get('versionInfo', {})
    
    def get_libraries(self) -> List[Dict[str, str]]:
        """获取 Forge 需要的所有依赖库"""
        libraries = []
        
        if not self._profile:
            return libraries
        
        install = self._profile.get('install', {})
        
        # 1. install.libraries 中的依赖
        for lib in install.get('libraries', []):
            name = lib.get('name')
            url = _mirror_url(lib.get('url', _OFFICIAL_LIBRARIES))
            if name:
                libraries.append({
                    'name': name,
                    'url': url,
                    'path': self._coord_to_path(name)
                })
        
        # 2. processors 的 classpath 中的依赖
        for processor in install.get('processors', []):
            for cp_entry in processor.get('classpath', []):
                # cp_entry 是 Maven 坐标
                libraries.append({
                    'name': cp_entry,
                    'url': _mirror_url(_OFFICIAL_FORGE_MAVEN),
                    'path': self._coord_to_path(cp_entry)
                })
        
        # 3. 去重（按 name 去重）
        seen = set()
        unique = []
        for lib in libraries:
            if lib['name'] not in seen:
                seen.add(lib['name'])
                unique.append(lib)
        
        return unique
    
    def get_main_class(self) -> Optional[str]:
        return self._version_info.get('mainClass') if self._version_info else None
    
    def get_version_info(self) -> Dict:
        return self._version_info or {}
    
    def get_processors(self) -> List[Dict]:
        if not self._profile:
            return []
        return self._profile.get('install', {}).get('processors', [])
    
    def get_minecraft_version(self) -> Optional[str]:
        """获取 Minecraft 版本"""
        if not self._profile:
            return None
        return self._profile.get('install', {}).get('minecraft')