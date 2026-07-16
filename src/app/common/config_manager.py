# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher Dev.
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
"""跨平台配置文件管理 - 使用 .sxclconfig 格式"""

import os
import sys
from pathlib import Path
from typing import Any, Dict


class ConfigManager:
    """配置文件管理器 - 读写 .sxclconfig 文件"""
    
    _instance = None
    _config: Dict[str, Any] = {}
    _config_path: Path = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._init_config_path()
            cls._instance._load()
        return cls._instance
    
    def _init_config_path(self):
        """初始化配置文件路径（跨平台）"""
        system = sys.platform
        
        if system == 'win32':
            base = Path(os.environ.get('PROGRAMDATA', 'C:\\ProgramData'))
            self._config_path = base / 'sxcl' / 'config.ini'
        elif system == 'darwin':
            self._config_path = Path.home() / 'Library' / 'Application Support' / 'sxcl' / 'config.ini'
        else:
            xdg = os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config'))
            self._config_path = Path(xdg) / 'sxcl' / 'config.ini'
        
        self._config_path.parent.mkdir(parents=True, exist_ok=True)
    
    def _load(self):
        """加载配置文件"""
        self._config = {}
        if not self._config_path.exists():
            self._set_defaults()
            return
        
        try:
            with open(self._config_path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#'):
                        continue
                    if '=' in line:
                        key, value = line.split('=', 1)
                        self._config[key.strip()] = self._parse_value(value.strip())
            
            # 确保所有默认键存在
            self._ensure_defaults()
        except Exception as e:
            print(f"[Config] 加载配置失败: {e}")
            self._set_defaults()
    
    def _ensure_defaults(self):
        """确保所有默认配置键存在"""
        defaults = {
            'download_source': 'bmclapi',
            'java_path': '',
            'max_memory': 4096,
            'game_directory': str(Path.home() / '.minecraft'),
            'theme': 'auto',
            'language': 'zh-CN',
            'auto_check_update': True,
            'debug_mode': False,
            'version_isolation': False,
            'country_code': 'CN',
            'window_width': 1280,
            'window_height': 720,
        }
        changed = False
        for key, value in defaults.items():
            if key not in self._config:
                self._config[key] = value
                changed = True
        if changed:
            self._save()
    
    def _save(self):
        """保存配置文件"""
        try:
            with open(self._config_path, 'w', encoding='utf-8') as f:
                f.write("# Silent X Craft Launcher 配置文件\n")
                f.write("# 请不要手动修改\n\n")
                for key, value in self._config.items():
                    f.write(f"{key}={self._serialize_value(value)}\n")
        except Exception as e:
            print(f"[Config] 保存配置失败: {e}")
    
    def _set_defaults(self):
        """设置默认配置"""
        self._config = {
            'download_source': 'bmclapi',
            'java_path': '',
            'max_memory': 4096,
            'game_directory': str(Path.home() / '.minecraft'),
            'theme': 'auto',
            'language': 'zh-CN',
            'auto_check_update': True,
            'debug_mode': False,
            'version_isolation': False,
            'country_code': 'CN',
            'window_width': 1280,
            'window_height': 720,
        }
        self._save()
    
    def _parse_value(self, value: str) -> Any:
        if value.lower() == 'true':
            return True
        if value.lower() == 'false':
            return False
        if value.isdigit():
            return int(value)
        return value
    
    def _serialize_value(self, value: Any) -> str:
        if isinstance(value, bool):
            return str(value).lower()
        return str(value)
    
    def get(self, key: str, default: Any = None) -> Any:
        return self._config.get(key, default)
    
    def set(self, key: str, value: Any):
        self._config[key] = value
        self._save()
    
    def get_all(self) -> Dict[str, Any]:
        return self._config.copy()


config = ConfigManager()