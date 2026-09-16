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
# (at your option) any later version, WITH the Additional Terms described in
# the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""下载源体检工具 —— 逐条验证"官方路"与"BMCLAPI 路"是否都通。

用法::

    python scripts/probe_sources.py             # 全量体检
    python scripts/probe_sources.py --quick     # 只测关键几条

输出会直接告诉你 mirror.py 里的规则是否还有效（BMCLAPI 会变），
任何一条 404/超时都说明该资源的映射需要调整或加入 MIRROR_UNSAFE。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import requests  # noqa: E402
from src.core.mirror import candidates, rewrite_url  # noqa: E402

S = requests.Session()
S.headers.update({"User-Agent": "SXCL-sources-probe/1.0"})

VERSION = "1.21.1"
FORGE = "1.20.1-47.2.0"
NEOFORGE = "21.1.72"
FABRIC_INSTALLER = "1.1.2"


def _probe(url: str) -> str:
    try:
        resp = S.get(url, timeout=20, stream=True, headers={"Range": "bytes=0-63"})
        body = next(resp.iter_content(64), b"")
        resp.close()
        size = resp.headers.get("content-length", "-")
        return f"{resp.status_code} len={size} {body[:12]!r}"
    except Exception as exc:
        return f"ERR {type(exc).__name__}: {str(exc)[:60]}"


CASES = [
    ("版本清单 v2", "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", ""),
    ("客户端 jar", "https://piston-data.mojang.com/v1/objects/30c73b1c5da787909b2f73340419fdf13b9def88/client.jar", ""),
    ("资源对象", "https://resources.download.minecraft.net/b6/b62ca8ec10d07e6bf5ac8dae0c8c1d2e6a1e3356", ""),
    ("依赖库", "https://libraries.minecraft.net/com/google/code/gson/gson/2.10.1/gson-2.10.1.jar", ""),
    ("Forge 安装器", f"https://maven.minecraftforge.net/net/minecraftforge/forge/{FORGE}/forge-{FORGE}-installer.jar", ""),
    ("NeoForge 安装器", f"https://maven.neoforged.net/releases/net/neoforged/neoforge/{NEOFORGE}/neoforge-{NEOFORGE}-installer.jar", ""),
    ("Fabric 安装器", f"https://maven.fabricmc.net/net/fabricmc/fabric-installer/{FABRIC_INSTALLER}/fabric-installer-{FABRIC_INSTALLER}.jar", "fabric-installer"),
    ("Fabric loader 元数据", f"https://meta.fabricmc.net/v2/versions/loader/{VERSION}", ""),
    ("官方 JRE 清单", "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json", ""),
    ("Quilt maven", "https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/maven-metadata.xml", ""),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    cases = CASES[:4] if args.quick else CASES
    bad = 0
    print(f"{'资源':16s} {'源':9s} {'结果':44s} URL")
    print("-" * 130)
    for label, official, kind in cases:
        for source, url in candidates(official, kind=kind):
            result = _probe(url)
            flag = "" if result.startswith("200") or result.startswith("206") else "  <-- 失败"
            if flag:
                bad += 1
            print(f"{label:16s} {source:9s} {result:44s} {url[:70]}{flag}")
        mirror = rewrite_url(official)
        if mirror is None and kind not in ("fabric-installer",):
            print(f"{label:16s} {'(镜像)':9s} {'不可镜像（UNSAFE/未登记）':44s}")
        print()

    print("=" * 130)
    print(f"失败条目: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
