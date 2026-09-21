#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# -*- coding: utf-8 -*-
"""按键映射:C 版核心库 vs Python 版核心库 的一致性工具。

为什么要有这个工具
------------------
C 版(src/services/modloader/keymap_store.c / keymap_fcl.c)是 store.py / fcl.py 的移植,
"两边写出来的文件必须一模一样"这件事不能靠肉眼看。这个脚本:

  1. 用 Python 版跑**同一套操作**(下面的 ACTIONS),把产物写进一个临时目录;
  2. C 版那边的产物由 C 测试 sxcl_keymap_store_test 写在构建目录的 keymap_store_out/;
  3. 逐文件、逐字段比对(JSON 对象不看键顺序,数字按值比),把差异按 JSON 路径列出来;
  4. --write-fixtures 还会把 Python 的 FCL 导入结果写成 C 测试的夹具
     (src/services/modloader/tests/fcl_fixture_expected.json),让 ctest 自己也能守住一致性。

用法:
    python tools/keymap_parity.py                    # 比对(默认找 build/ 下的 C 产物)
    python tools/keymap_parity.py --c-out <目录>      # 指定 C 产物目录
    python tools/keymap_parity.py --write-fixtures   # 重新生成 C 测试夹具
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

C_REPO = Path(__file__).resolve().parents[1]
TESTS_DIR = C_REPO / "src" / "services" / "modloader" / "tests"
DEFAULT_PY_REPO = Path(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher")
DEFAULT_C_OUT = C_REPO / "build" / "src" / "services" / "modloader" / "keymap_store_out"
DEFAULT_PY_OUT = C_REPO / "build" / "ref" / "py_keymap_store"


def load_python_api(py_repo: Path):
    """import Python 版核心库(它不依赖 Qt)。"""
    if str(py_repo) not in sys.path:
        sys.path.insert(0, str(py_repo))
    from src.core.keymap import fcl, presets, store          # noqa: PLC0415

    return store, fcl, presets


def fcl_import(py_repo: Path, fixture: Path):
    _, fcl, _ = load_python_api(py_repo)
    data = json.loads(fixture.read_text(encoding="utf-8"))
    return fcl.import_layout(data)


def run_python(py_repo: Path, out_dir: Path) -> dict:
    """跑一遍 Python 侧,产物落在 out_dir;返回给报告用的摘要。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    # store.keymap_dir() = default_config_directory("SilentXCraftLauncher")/keymaps,
    # Windows 上就是 %APPDATA%/SilentXCraftLauncher —— 指到临时目录,别碰真实配置。
    os.environ["APPDATA"] = str(out_dir / "appdata")
    store, fcl, presets = load_python_api(py_repo)

    written = store.ensure_presets()
    # ① 预设(竖屏 + 指定版本号)
    survival = presets.build_preset("survival", "portrait", "1.21.1")
    store.save(survival, "my-pvp")
    # ② 布局名带全角括号与斜杠:文件名安全化两边必须一致
    pvp = presets.build_preset("pvp")
    pvp.name = "对战（推荐）/battle"
    store.save(pvp, "")
    # ③ FCL 导入后直接存(meta.fcl_raw 也要一路带着):对象形 + 数组形各一份
    imported = fcl_import(py_repo, TESTS_DIR / "fcl_fixture.json")
    store.save(imported, "fcl-imported")
    array_imported = fcl_import(py_repo, TESTS_DIR / "fcl_fixture_array.json")
    store.save(array_imported, "fcl-array-imported")
    # ④ active.json
    store.set_active("preset-survival")
    # ⑤ 列目录
    listing = store.list_layouts()
    # 与 C 测试一样把清单也写进 keymaps/(这样两边的文件集合能直接比)
    (store.keymap_dir() / "list_layouts.json").write_text(
        json.dumps(listing, ensure_ascii=False, indent=2), encoding="utf-8")
    return {
        "presets_written": [p.name for p in written],
        "active_name": store.active_name(),
        "list": listing,
        "keymaps_dir": str(store.keymap_dir()),   # 真正写文件的地方(比对用这个目录)
    }


# ── 逐字段比对 ────────────────────────────────────────────────────

def compare_json(a, b, path: str, diffs: list[str]) -> None:
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            sub = f"{path}.{key}"
            if key not in a:
                diffs.append(f"{sub}: C 版有、Python 没有({b[key]!r})")
            elif key not in b:
                diffs.append(f"{sub}: Python 有、C 版没有({a[key]!r})")
            else:
                compare_json(a[key], b[key], sub, diffs)
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            diffs.append(f"{path}: 长度不同 C={len(a)} Python={len(b)}")
            return
        for index, (x, y) in enumerate(zip(a, b)):
            compare_json(x, y, f"{path}[{index}]", diffs)
        return
    if isinstance(a, bool) or isinstance(b, bool):
        if a is not b:
            diffs.append(f"{path}: C={a!r} Python={b!r}")
        return
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if abs(float(a) - float(b)) > 1e-9:
            diffs.append(f"{path}: C={a!r} Python={b!r}")
        return
    if a != b:
        diffs.append(f"{path}: C={a!r} Python={b!r}")


def compare_tree(c_out: Path, py_out: Path) -> tuple[int, list[str]]:
    report: list[str] = []
    diffs: list[str] = []
    files_c = sorted(p.name for p in c_out.glob("*.json"))
    files_py = sorted(p.name for p in py_out.glob("*.json"))
    report.append(f"文件清单: C={len(files_c)} Python={len(files_py)}")
    if files_c != files_py:
        diffs.append(f"文件集合不同: 只在 C={sorted(set(files_c) - set(files_py))} "
                     f"只在 Python={sorted(set(files_py) - set(files_c))}")
    for name in files_py:
        if name == "list_layouts.json":
            continue
        path_c = c_out / name
        path_py = py_out / name
        if not path_c.exists():
            continue
        text_c = path_c.read_text(encoding="utf-8")
        text_py = path_py.read_text(encoding="utf-8")
        if name == "active.json":
            if text_c != text_py:
                diffs.append(f"active.json 字节不同: C={text_c!r} Python={text_py!r}")
            report.append(f"{name}: 逐字节一致")
            continue
        try:
            data_c = json.loads(text_c)
            data_py = json.loads(text_py)
        except Exception as exc:                      # noqa: BLE001
            diffs.append(f"{name}: JSON 解析失败 {exc}")
            continue
        before = len(diffs)
        compare_json(data_c, data_py, name, diffs)
        same_text = "文本也逐字节一致" if text_c == text_py else "文本写法不同(字段值一致)"
        report.append(f"{name}: 字段{'一致' if len(diffs) == before else '**不同**'} ({same_text})")
    if (py_out / "list_layouts.json").exists() and (c_out / "list_layouts.json").exists():
        before = len(diffs)
        compare_json(json.loads((c_out / "list_layouts.json").read_text(encoding="utf-8")),
                     json.loads((py_out / "list_layouts.json").read_text(encoding="utf-8")),
                     "list_layouts", diffs)
        report.append("list_layouts.json: " + ("一致" if len(diffs) == before else "**不同**"))
    return len(diffs), report, diffs


def main() -> int:
    parser = argparse.ArgumentParser(description="C 版/Python 版按键映射一致性比对")
    parser.add_argument("--py-repo", type=Path, default=DEFAULT_PY_REPO)
    parser.add_argument("--py-out", type=Path, default=DEFAULT_PY_OUT)
    parser.add_argument("--c-out", type=Path, default=DEFAULT_C_OUT)
    parser.add_argument("--write-fixtures", action="store_true",
                        help="把 Python 的 FCL 导入结果写进 C 测试夹具")
    parser.add_argument("--keep", action="store_true", help="保留上一次的 Python 产物目录")
    args = parser.parse_args()

    if args.write_fixtures:
        for source, target_name in (("fcl_fixture.json", "fcl_fixture_expected.json"),
                                    ("fcl_fixture_array.json", "fcl_fixture_array_expected.json")):
            layout = fcl_import(args.py_repo, TESTS_DIR / source)
            target = TESTS_DIR / target_name
            target.write_text(layout.to_json(), encoding="utf-8")
            print(f"已写入夹具 {target}({len(layout.buttons)} 个按钮 / "
                  f"{len(layout.directions)} 个方向控件)")
        return 0

    if args.py_out.exists() and not args.keep:
        shutil.rmtree(args.py_out)
    summary = run_python(args.py_repo, args.py_out)
    py_products = Path(summary["keymaps_dir"])
    print(f"Python 侧产物: {py_products}")
    print(f"  ensure_presets 写入 {len(summary['presets_written'])} 份: {', '.join(summary['presets_written'])}")
    print(f"  active_name() = {summary['active_name']}")

    if not args.c_out.exists():
        print(f"\n还没找到 C 版产物目录 {args.c_out}\n"
              f"先跑: ctest --test-dir build -C Release -R sxcl_keymap_store_test", file=sys.stderr)
        return 2
    print(f"C 版产物: {args.c_out}\n")
    total, report, diffs = compare_tree(args.c_out, py_products)
    for line in report:
        print("  " + line)
    print()
    if total == 0:
        print("结论: 逐字段完全一致(0 处差异)")
        return 0
    print(f"结论: {total} 处差异")
    for line in diffs[:80]:
        print("  [差异] " + line)
    if len(diffs) > 80:
        print(f"  ...(还有 {len(diffs) - 80} 处)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
