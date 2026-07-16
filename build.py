# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# 跨平台构建脚本 — 支持 Nuitka 单文件打包
# 用法:
#   python build.py              # 自动检测平台并构建
#   python build.py --target x64 # 强制指定目标架构 (ARM64 主机交叉编译 x64)
#   python build.py --clean      # 清理构建产物

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"

SYSTEM = platform.system()
MACHINE = platform.machine().lower()  # arm64 / AMD64 / x86_64


def info(msg: str):
    print(f"  ▶ {msg}")


def error(msg: str):
    print(f"  ✗ {msg}", file=sys.stderr)


def ok(msg: str):
    print(f"  ✔ {msg}")


def clean():
    """Clean build artifacts."""
    for d in ["dist", "main.dist", "main.build", "main.onefile-build", "__pycache__"]:
        p = ROOT / d
        if p.exists():
            shutil.rmtree(p, ignore_errors=True)
            info(f"Deleted {d}")
    ok("Clean done")


def get_target_arch() -> str:
    """Determine the target architecture."""
    if "--target" in sys.argv:
        idx = sys.argv.index("--target")
        return sys.argv[idx + 1]

    # Auto-detect: on ARM64 host we produce ARM64 by default
    if MACHINE in ("arm64", "aarch64"):
        return "arm64"
    return "x64"


def build_windows(target_arch: str):
    """Build Windows single-file executable via Nuitka."""
    info(f"Building for Windows {target_arch}…")

    # ── Base Nuitka arguments ──
    args = [
        sys.executable, "-m", "nuitka",
        "--onefile",
        "--enable-plugin=pyside6",
        "--windows-console-mode=disable",
        "--include-data-dir=config=config",
        f"--output-dir={DIST}",
        str(ROOT / "main.py"),
    ]

    # ── Cross-compilation from ARM64 → x64 ──
    if MACHINE in ("arm64", "aarch64") and target_arch == "x64":
        info("Setting up ARM64 → x64 cross-compilation…")
        os.environ["VSCMD_ARG_TARGET_ARCH"] = "x64"
        os.environ["VSCMD_ARG_HOST_ARCH"] = "arm64"

    # ── Run Nuitka ──
    info("Running Nuitka (this may take 10-30 minutes)…")
    result = subprocess.run(args, cwd=ROOT)
    if result.returncode != 0:
        error("Nuitka build failed")
        sys.exit(result.returncode)

    # Verify output
    exe = DIST / "main.exe"
    if exe.exists():
        size_mb = exe.stat().st_size / 1024 / 1024
        ok(f"Build successful! {exe} ({size_mb:.1f} MB)")

        # Rename with arch suffix
        final_name = DIST / f"SXCL-{target_arch}.exe"
        exe.rename(final_name)
        ok(f"Final: {final_name}")
    else:
        error("Expected output not found")


def build_macos():
    """Build macOS single-file executable via Nuitka."""
    info("Building for macOS…")
    target_arch = get_target_arch()
    arch_flag = f"--macos-target-arch={target_arch}" if target_arch else ""

    args = [
        sys.executable, "-m", "nuitka",
        "--onefile",
        "--enable-plugin=pyside6",
        "--include-data-dir=config=config",
        f"--output-dir={DIST}",
        str(ROOT / "main.py"),
    ]
    if arch_flag:
        args.insert(2, arch_flag)

    info("Running Nuitka…")
    subprocess.run(args, cwd=ROOT, check=True)

    binary = DIST / "main.bin"  # macOS onefile
    if binary.exists():
        size_mb = binary.stat().st_size / 1024 / 1024
        ok(f"Build successful! {binary} ({size_mb:.1f} MB)")
    else:
        # Nuitka might name it differently on macOS
        for f in DIST.iterdir():
            if f.is_file() and "main" in f.name:
                ok(f"Build successful! {f}")


def build_linux():
    """Build Linux single-file executable via Nuitka."""
    info("Building for Linux…")
    args = [
        sys.executable, "-m", "nuitka",
        "--onefile",
        "--enable-plugin=pyside6",
        "--include-data-dir=config=config",
        f"--output-dir={DIST}",
        str(ROOT / "main.py"),
    ]

    info("Running Nuitka…")
    subprocess.run(args, cwd=ROOT, check=True)

    binary = DIST / "main.bin"
    if binary.exists():
        size_mb = binary.stat().st_size / 1024 / 1024
        ok(f"Build successful! {binary} ({size_mb:.1f} MB)")


def main():
    if "--clean" in sys.argv:
        clean()
        return

    DIST.mkdir(exist_ok=True)

    target = get_target_arch()
    info(f"Host: {SYSTEM} {MACHINE}")
    info(f"Target arch: {target}")

    if SYSTEM == "Windows":
        build_windows(target)
    elif SYSTEM == "Darwin":
        build_macos()
    elif SYSTEM == "Linux":
        build_linux()
    else:
        error(f"Unsupported OS: {SYSTEM}")
        sys.exit(1)


if __name__ == "__main__":
    main()
