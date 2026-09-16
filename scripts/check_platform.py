# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
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
"""跨平台自检 —— 在任意系统上跑一遍，看这台机器支持哪些能力。

用法::

    python scripts/check_platform.py                 # 只看平台能力
    python scripts/check_platform.py --version 1.21.1  # 顺带列出会下载哪些 natives

排查"在 macOS / Linux 上跑不起来"这类问题时，把这份输出贴出来即可。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from src.core.platform import (  # noqa: E402
    classpath_separator,
    create_no_window_flag,
    current_arch,
    current_platform,
    default_config_directory,
    default_game_directory,
    default_jvm_directory,
    is_arm64,
    java_executable_name,
    supports_window_detection,
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="", help="顺便列出该版本的 natives 选择")
    args = ap.parse_args()

    print("=" * 72)
    print("Silent X Craft Launcher 跨平台自检")
    print("=" * 72)
    print(f"平台        : {current_platform().value}")
    print(f"架构        : {current_arch().value} (arm64={is_arm64()})")
    print(f"Python      : {sys.version.split()[0]}")
    print(f"java 可执行名: {java_executable_name()}")
    print(f"classpath 分隔符: {classpath_separator()!r}")
    print(f"子进程无窗口标志: {create_no_window_flag()}")
    print(f"窗口枚举能力 : {'有（Windows）' if supports_window_detection() else '无（退化为等待进程）'}")
    print(f"配置目录    : {default_config_directory()}")
    print(f"游戏目录    : {default_game_directory()}")
    print(f"JVM 目录     : {default_jvm_directory()}")

    try:
        from src.services.java.mojang_runtime import platform_key
        print(f"官方 JRE 平台键: {platform_key()}")
    except Exception as exc:      # noqa: BLE001
        print(f"官方 JRE 平台键: 读取失败 {exc}")

    try:
        from src.services.java.finder import discover_java_installations, best_java_installation
        installs = discover_java_installations()
        best = best_java_installation(installs)
        print(f"已装 Java   : {len(installs)} 个" + (f" | 推荐: {best.display_name}" if best else " | 未找到可用运行时"))
    except Exception as exc:      # noqa: BLE001
        print(f"已装 Java   : 检测失败 {exc}")

    if args.version:
        try:
            import requests
            from src.core.download.spec import platform_arch_name, platform_os_name, specs_for_libraries
            manifest = requests.get(
                "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", timeout=25).json()
            entry = next(v for v in manifest["versions"] if v["id"] == args.version)
            version_json = requests.get(entry["url"], timeout=25).json()
            specs = specs_for_libraries(version_json, Path("."))
            natives = [s for s in specs if s.kind == "native"]
            print(f"\n{args.version} 在本机({platform_os_name()}-{platform_arch_name()})需要:")
            print(f"  依赖库 {len([s for s in specs if s.kind == 'library'])} 个, natives {len(natives)} 个")
            for spec in natives:
                print(f"    {spec.name.split('/')[-1]}")
        except Exception as exc:      # noqa: BLE001
            print(f"natives 预览失败: {exc}")

    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
