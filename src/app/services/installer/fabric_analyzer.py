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
"""Fabric 安装器分析器"""

import json
from pathlib import Path
from typing import List, Dict, Optional

from .base import LoaderAnalyzer


class FabricAnalyzer(LoaderAnalyzer):
    """Fabric 安装器分析器"""
    
    def __init__(self, installer_path: Path):
        super().__init__(installer_path)
        self._version_data = None
        self._load_metadata()
    
    def get_loader_type(self) -> str:
        return "fabric"
    
    def _load_metadata(self):
        """加载 version.json"""
        content = self._read_file_from_jar('version.json')
        if content:
            self._version_data = json.loads(content)
    
    def get_libraries(self) -> List[Dict[str, str]]:
        """获取 Fabric 需要的依赖库"""
        libraries = []
        
        if not self._version_data:
            return libraries
        
        for lib in self._version_data.get('libraries', []):
            name = lib.get('name')
            url = lib.get('url', 'https://maven.fabricmc.net/')
            if name:
                libraries.append({
                    'name': name,
                    'url': url,
                    'path': self._coord_to_path(name)
                })
        
        return libraries
    
    def get_main_class(self) -> Optional[str]:
        if not self._version_data:
            return None
        return self._version_data.get('mainClass')
    
    def get_version_info(self) -> Dict:
        return self._version_data or {}
    
    def get_processors(self) -> List[Dict]:
        return []  # Fabric 不需要处理器
    
    def get_loader_version(self) -> Optional[str]:
        """获取 Fabric Loader 版本"""
        if not self._version_data:
            return None
        # 从 id 中提取，如 fabric-loader-0.16.9-1.21.1
        loader_id = self._version_data.get('id', '')
        if 'fabric-loader-' in loader_id:
            parts = loader_id.split('-')
            if len(parts) >= 3:
                return parts[2]
        return None