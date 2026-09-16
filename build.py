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
        "--include-data-dir=assets=assets",
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
    """Build macOS app bundle via Nuitka.

    macOS 上只给一个裸二进制体验很差（没有 Dock 图标/菜单栏，Gatekeeper 还会拦），
    所以这里用 --macos-create-app-bundle 直接产出 SXCL.app。
    未签名的包首次打开需要：右键 -> 打开，或 xattr -dr com.apple.quarantine SXCL.app
    正式分发请自行 codesign + notarytool（见 README「跨平台」一节）。
    """
    info("Building for macOS…")
    target_arch = get_target_arch()
    arch_flag = f"--macos-target-arch={target_arch}" if target_arch else ""

    args = [
        sys.executable, "-m", "nuitka",
        "--standalone",
        "--macos-create-app-bundle",
        "--macos-app-name=Silent X Craft Launcher",
        "--macos-app-version=0.1.0",
        "--enable-plugin=pyside6",
        "--include-data-dir=config=config",
        "--include-data-dir=assets=assets",
        f"--output-dir={DIST}",
        str(ROOT / "main.py"),
    ]
    if arch_flag:
        args.insert(2, arch_flag)

    info("Running Nuitka…")
    result = subprocess.run(args, cwd=ROOT)
    if result.returncode != 0:
        error("Nuitka build failed")
        sys.exit(result.returncode)

    app = DIST / "main.app"
    if app.exists():
        final = DIST / ("SXCL-" + target_arch + ".app")
        if final.exists():
            shutil.rmtree(final, ignore_errors=True)
        app.rename(final)
        ok(f"Build successful! {final}")
        info("提示：未签名时首次运行需 右键→打开；分发前请 codesign --deep 并 notarize")
    else:
        for f in DIST.iterdir():
            if f.name.endswith(".app"):
                ok(f"Build successful! {f}")
                return
        error("Expected .app bundle not found")


def build_linux():
    """Build Linux single-file executable via Nuitka."""
    info("Building for Linux…")
    args = [
        sys.executable, "-m", "nuitka",
        "--onefile",
        "--enable-plugin=pyside6",
        "--include-data-dir=config=config",
        "--include-data-dir=assets=assets",
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
