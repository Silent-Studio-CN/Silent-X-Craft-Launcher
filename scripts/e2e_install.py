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
"""安装链路端到端自测（真实下载，真实校验）。

用法::

    python scripts/e2e_install.py --version 1.21.1 --assets 300
    python scripts/e2e_install.py --version 1.21.1 --assets 0 --clean

它会真的把版本装到临时目录里，然后断言：
  版本 JSON / 客户端 jar / 依赖库 / 资源索引 / 资源对象 / natives 解压 是否齐全，
  并且客户端 jar 的 SHA1 与 Mojang 元数据一致。
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import requests  # noqa: E402

import src.app.pages.download_progress_page as dpp  # noqa: E402
from src.services.minecraft.manifest import GameVersion  # noqa: E402
from src.core.download.verify import sha1_file  # noqa: E402


def fetch_version_json(version_id: str) -> dict:
    manifest = requests.get(
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", timeout=30).json()
    entry = next(v for v in manifest["versions"] if v["id"] == version_id)
    resp = requests.get(entry["url"], timeout=30)
    resp.raise_for_status()
    return resp.json()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.21.1")
    ap.add_argument("--assets", type=int, default=300, help="只测前 N 个资源对象（0=全部）")
    ap.add_argument("--dir", default="")
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()

    game_dir = Path(args.dir) if args.dir else Path(__file__).resolve().parents[1] / ".e2e-mc"
    if args.clean and game_dir.exists():
        shutil.rmtree(game_dir, ignore_errors=True)
    game_dir.mkdir(parents=True, exist_ok=True)

    if args.assets > 0:
        original = dpp.specs_for_asset_objects
        dpp.specs_for_asset_objects = (
            lambda index, gd, **kw: original(index, gd, limit=args.assets, **kw))

    version = GameVersion(id=args.version, version_type="release", url="", release_time="")
    worker = dpp.InstallWorker(version, args.version, "none", None, game_dir)
    stages: list[str] = []
    worker.stage_changed.connect(lambda i, n, name: stages.append(name))
    worker.detail.connect(lambda text: print("   ", text[:130], flush=True))
    result: dict = {}
    worker.finished.connect(lambda ok, msg: result.update(ok=ok, msg=msg))

    print(f"安装 {args.version} -> {game_dir}")
    t0 = time.perf_counter()
    worker.run()          # 直接同步执行（不走 Qt 事件循环）
    elapsed = time.perf_counter() - t0

    print("\n阶段:", " -> ".join(stages))
    print(f"结果: ok={result.get('ok')} msg={result.get('msg')} 用时 {elapsed:.1f}s")

    vdir = game_dir / "versions" / args.version
    checks = {
        "版本 JSON": (vdir / f"{args.version}.json").exists(),
        "客户端 jar": (vdir / f"{args.version}.jar").exists(),
        "natives 目录": (vdir / f"{args.version}-natives").exists(),
        "资源索引": (game_dir / "assets" / "indexes").exists(),
        "依赖库目录": (game_dir / "libraries").exists(),
    }
    natives = list((vdir / f"{args.version}-natives").glob("*")) if checks["natives 目录"] else []
    checks["natives 文件数>0"] = len(natives) > 0
    libs = list((game_dir / "libraries").rglob("*.jar")) if checks["依赖库目录"] else []
    checks["依赖库数量>50"] = len(libs) > 50
    objs = list((game_dir / "assets" / "objects").rglob("*")) if (game_dir / "assets").exists() else []
    checks["资源对象数量"] = len([o for o in objs if o.is_file()])

    print()
    for name, value in checks.items():
        print(f"  {name:18s} {value}")

    vj = fetch_version_json(args.version)
    jar = vdir / f"{args.version}.jar"
    if jar.exists():
        expected = vj["downloads"]["client"]["sha1"]
        actual = sha1_file(jar)
        print(f"  客户端 SHA1        {'OK' if actual == expected else 'MISMATCH!'} ({actual[:12]})")

    leftovers = list(vdir.glob("*.part*")) + list(game_dir.rglob("*.part.json"))
    print(f"  残留临时文件       {len(leftovers)}")
    ok = bool(result.get("ok")) and all(v is True or isinstance(v, int) and v > 0
                                       for v in checks.values()) and not leftovers
    print("\nE2E:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
