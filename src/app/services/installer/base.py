"""加载器安装器分析基类"""

from abc import ABC, abstractmethod
from pathlib import Path
from typing import List, Dict, Optional, Tuple
import zipfile
import json


class LoaderAnalyzer(ABC):
    """加载器分析器基类"""
    
    def __init__(self, installer_path: Path):
        self.installer_path = installer_path
        self._jar_data: Optional[Dict] = None
    
    @abstractmethod
    def get_loader_type(self) -> str:
        """返回加载器类型: forge, neoforge, fabric"""
        pass
    
    @abstractmethod
    def get_libraries(self) -> List[Dict[str, str]]:
        """获取需要下载的依赖库列表"""
        pass
    
    @abstractmethod
    def get_main_class(self) -> Optional[str]:
        """获取主类（Fabric 需要）"""
        pass
    
    @abstractmethod
    def get_version_info(self) -> Dict:
        """获取版本信息（用于生成 version.json）"""
        pass
    
    @abstractmethod
    def get_processors(self) -> List[Dict]:
        """获取处理器列表（Forge/NeoForge 需要）"""
        pass
    
    def _read_file_from_jar(self, filename: str) -> Optional[str]:
        """从 JAR 中读取文件内容"""
        try:
            with zipfile.ZipFile(self.installer_path, 'r') as zip_ref:
                if filename in zip_ref.namelist():
                    return zip_ref.read(filename).decode('utf-8')
            return None
        except Exception as e:
            print(f"[LoaderAnalyzer] 读取 {filename} 失败: {e}")
            return None
    
    def _parse_maven_coordinate(self, coord: str) -> Dict[str, str]:
        """解析 Maven 坐标: group:artifact:version[:classifier]"""
        parts = coord.split(':')
        result = {
            'group': parts[0],
            'artifact': parts[1],
            'version': parts[2],
        }
        if len(parts) > 3:
            result['classifier'] = parts[3]
        return result
    
    def _coord_to_path(self, coord: str) -> str:
        """Maven 坐标转路径: net.minecraftforge:forge:1.21.1-52.0.16 -> net/minecraftforge/forge/1.21.1-52.0.16/forge-1.21.1-52.0.16.jar"""
        parsed = self._parse_maven_coordinate(coord)
        path = f"{parsed['group'].replace('.', '/')}/{parsed['artifact']}/{parsed['version']}/{parsed['artifact']}-{parsed['version']}"
        if 'classifier' in parsed:
            path += f"-{parsed['classifier']}"
        return path + ".jar"