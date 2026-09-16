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
"""下载引擎基准测试（真实下载 + 真实校验）。

用法::

    python scripts/bench_download.py --mode scale --source official --conns 1,8,32,64
    python scripts/bench_download.py --mode limit --source official --conns 32 --limit 5,2
    python scripts/bench_download.py --mode switch          # 镜像优先，验证"慢源自动切换"
    python scripts/bench_download.py --mode legacy          # 旧引擎基线

说明：结果受当前网络到两条源的实时速度影响，跑之前建议先执行
scripts/probe_sources.py 看看两条路通不通。
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import requests  # noqa: E402

from src.core.download import AsyncDownloadEngine, spec_for, verify_file  # noqa: E402
from src.core.download.limiter import RateLimiter  # noqa: E402
from src.core.mirror import candidates  # noqa: E402


def fetch_version_json(version_id: str) -> dict:
    manifest = requests.get(
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", timeout=30).json()
    entry = next(v for v in manifest["versions"] if v["id"] == version_id)
    resp = requests.get(entry["url"], timeout=30)
    resp.raise_for_status()
    return resp.json()


def run_case(label: str, official_url: str, sha1: str, size: int, dest: Path, *,
             source: str, conns: int, parts: int = 16, limit_bps: float = 0.0) -> dict:
    if dest.exists():
        dest.unlink()
    spec = spec_for(dest, official_url, sha1=sha1, size=size, kind="client-jar",
                    label=dest.name, source=("bmclapi" if source == "bmclapi" else "official"))
    engine = AsyncDownloadEngine(max_connections=conns, max_parts_per_file=parts,
                                 limiter=RateLimiter(limit_bps), verify=True)
    t0 = time.perf_counter()
    result = engine.download([spec], progress_interval=999)[0]
    elapsed = time.perf_counter() - t0
    mb = dest.stat().st_size / 1024 / 1024 if dest.exists() else 0
    check = verify_file(dest, sha1=sha1, size=size, use_cache=False) if dest.exists() else None
    return {
        "label": label, "ok": result.ok, "seconds": elapsed, "mb": mb,
        "mbps": mb / elapsed if elapsed else 0,
        "sha1_ok": bool(check and check.ok), "used": result.source or "-",
        "stats": engine.stats(),
    }


def print_table(rows: list[dict]) -> None:
    print("\n" + "=" * 104)
    print(f"{'方案':34s} {'结果':5s} {'秒':>7s} {'MB':>7s} {'MB/s':>7s} {'SHA1':>5s} {'实际源':>10s}")
    print("-" * 104)
    for r in rows:
        print(f"{r['label']:34s} {('OK' if r['ok'] else 'FAIL'):5s} {r['seconds']:7.2f} "
              f"{r['mb']:7.1f} {r['mbps']:7.2f} {('OK' if r['sha1_ok'] else 'BAD'):>5s} "
              f"{str(r['used']):>10s}")
    print("=" * 104)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.21.1")
    ap.add_argument("--mode", default="scale", choices=["scale", "limit", "switch", "all"])
    ap.add_argument("--source", default="official", choices=["official", "bmclapi"])
    ap.add_argument("--conns", default="1,8,32,64")
    ap.add_argument("--limit", default="5,2")
    args = ap.parse_args()

    out_dir = Path(os.environ.get("TEMP", ".")) / "sxcl-bench"
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / f"{args.version}.jar"

    vj = fetch_version_json(args.version)
    client = vj["downloads"]["client"]
    url, sha1, size = client["url"], client["sha1"], client["size"]
    print(f"目标: {args.version} client.jar {size / 1024 / 1024:.1f}MB")
    print(f"候选路: {[c[0] for c in candidates(url)]}（当前首选由设置里的下载源决定）")

    rows: list[dict] = []

    if args.mode in ("scale", "all"):
        for conns in [int(x) for x in args.conns.split(",")]:
            print(f"[scale] {args.source} conn={conns} …", flush=True)
            rows.append(run_case(f"新引擎 {args.source} conn={conns}", url, sha1, size, dest,
                                 source=args.source, conns=conns))

    if args.mode in ("limit", "all"):
        for mb in [float(x) for x in args.limit.split(",")]:
            print(f"[limit] {mb} MB/s …", flush=True)
            rows.append(run_case(f"限速 {mb:.0f}MB/s ({args.source})", url, sha1, size, dest,
                                 source=args.source, conns=32, limit_bps=mb * 1024 * 1024))

    if args.mode in ("switch", "all"):
        print("[switch] 镜像优先 + 慢源自动切换 …", flush=True)
        rows.append(run_case("镜像优先(自动切换)", url, sha1, size, dest,
                             source="bmclapi", conns=32))

    print_table(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
