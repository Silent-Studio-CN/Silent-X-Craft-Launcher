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
"""一份代码 -> 各平台安装包（打包矩阵入口）。

    python scripts/build_installers.py --list                 # 看这台机器能打哪些包
    python scripts/build_installers.py --prepare              # 生成 Inno/.desktop/AppRun 等模板
    python scripts/build_installers.py --target windows-portable
    python scripts/build_installers.py --all                   # 打本机支持的全部目标

设计：
    桌面端（Windows/macOS/Linux）用 PyInstaller（有 Nuitka 就用 Nuitka），
    同一个 `main.py` + `src/` 打包成三方各自的原生形态；
    安卓端单独走 `scripts/build_android_apk.py`（aapt2 + javac + d8），
    共享的是 `src/core/keymap/` 这套数据契约，不是二进制。

缺失的工具不会静默跳过：脚本会明确告诉你缺什么、怎么装。
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from src.core.constants import APP_NAME, APP_VERSION                        # noqa: E402

DIST = ROOT / "dist"
OUT = ROOT / "build" / "installers"
PACKAGING = ROOT / "build" / "packaging"
HOST_SYSTEM = platform.system()          # Windows / Darwin / Linux
HOST_ARCH = platform.machine().lower()
ARCH = "arm64" if HOST_ARCH in ("arm64", "aarch64") else "x64"
SLUG = "windows" if HOST_SYSTEM == "Windows" else ("macos" if HOST_SYSTEM == "Darwin" else "linux")


def which(name: str) -> str | None:
    return shutil.which(name)


def has_module(name: str) -> bool:
    try:
        __import__(name)
        return True
    except Exception:                    # noqa: BLE001
        return False


def packer() -> tuple[str, list[str]] | None:
    """返回 (说明, 命令行前缀)。优先 Nuitka（快且体积小），否则 PyInstaller。"""
    if has_module("nuitka"):
        return "Nuitka", [sys.executable, "-m", "nuitka"]
    if has_module("PyInstaller"):
        return "PyInstaller", [sys.executable, "-m", "PyInstaller"]
    return None


def pyinstaller_args(name: str, bundle: bool = False) -> list[str]:
    args = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
            "--name", name, "--add-data", f"config{';' if HOST_SYSTEM == 'Windows' else ':'}config",
            "--add-data", f"assets{chr(59) if HOST_SYSTEM == chr(39) + chr(87) + chr(105) + chr(110) + chr(100) + chr(111) + chr(119) + chr(115) + chr(39) else chr(58)}assets",
            "--distpath", str(DIST), "--workpath", str(ROOT / "build" / "pyinstaller"),
            "--specpath", str(ROOT / "build" / "pyinstaller")]
    if HOST_SYSTEM == "Darwin":
        args += ["--windowed", "--osx-bundle-identifier", "com.silentstudios.sxcl"]
    elif HOST_SYSTEM == "Windows":
        args += ["--windowed"]
    args.append(str(ROOT / "main.py"))
    return args


# ── 各目标 ─────────────────────────────────────────────────────

def target_windows_portable(run: bool) -> None:
    """Windows 免安装绿色版（onedir + zip）。"""
    if HOST_SYSTEM != "Windows":
        print("  [跳过] 只有 Windows 主机能打 Windows 包")
        return
    if not run:
        return
    info = packer()
    if info is None:
        raise SystemExit("缺少打包器：pip install nuitka 或 pip install pyinstaller")
    kind, _ = info
    name = f"SXCL-{APP_VERSION}"
    print(f"  使用 {kind} 打包 onedir…")
    if kind == "PyInstaller":
        subprocess.run(pyinstaller_args(name), cwd=ROOT, check=True)
    else:
        subprocess.run([sys.executable, "-m", "nuitka", "--standalone",
                        "--enable-plugin=pyside6", "--windows-console-mode=disable",
                        "--include-data-dir=config=config",
                        "--include-data-dir=assets=assets",
                        f"--output-dir={DIST}", str(ROOT / "main.py")], cwd=ROOT, check=True)
    folder = DIST / name
    if not folder.is_dir():
        raise SystemExit(f"没找到打包产物目录 {folder}")
    OUT.mkdir(parents=True, exist_ok=True)
    archive = shutil.make_archive(str(OUT / f"SXCL-{APP_VERSION}-{SLUG}-{ARCH}-portable"), "zip", folder)
    print(f"  ✔ {archive}")


def target_windows_setup(run: bool) -> None:
    """Windows 安装包（Inno Setup）。没有 ISCC 就只生成 .iss，让人/CI 自己编。"""
    if HOST_SYSTEM != "Windows":
        print("  [跳过] 只有 Windows 主机能打 Windows 安装包")
        return
    iss = prepare_inno()
    iscc = which("ISCC") or which("iscc")
    if iscc is None:
        print(f"  ! 没装 Inno Setup，已生成脚本：{iss}")
        print("    装法：winget install JRSoftware.InnoSetup  （或去 jrsoftware.org 下载）")
        print(f"    装好后：ISCC \"{iss}\"")
        return
    if not run:
        return
    subprocess.run([iscc, f"/DMyAppVersion={APP_VERSION}", str(iss)], cwd=ROOT, check=True)
    print(f"  ✔ 安装包输出目录：{OUT}")


def target_macos(run: bool) -> None:
    """macOS .app + dmg（dmg 需要 hdiutil，只在 macOS 上有）。"""
    if HOST_SYSTEM != "Darwin":
        print("  [跳过] 只有 macOS 主机能打 .app / .dmg")
        return
    if not run:
        return
    name = f"SXCL-{APP_VERSION}"
    subprocess.run(pyinstaller_args(name, bundle=True), cwd=ROOT, check=True)
    app = DIST / f"{name}.app"
    if not app.is_dir():
        raise SystemExit(f"没找到 {app}")
    OUT.mkdir(parents=True, exist_ok=True)
    archive = shutil.make_archive(str(OUT / f"SXCL-{APP_VERSION}-macos-{ARCH}-app"), "zip", app)
    print(f"  ✔ {archive}")
    if which("hdiutil"):
        dmg = OUT / f"SXCL-{APP_VERSION}-macos-{ARCH}.dmg"
        subprocess.run(["hdiutil", "create", "-volname", APP_NAME, "-srcfolder", str(app),
                        "-ov", "-format", "UDZO", str(dmg)], check=True)
        print(f"  ✔ {dmg}")
    else:
        print("  ! 没有 hdiutil（非 macOS），只出了 zip")


def target_linux(run: bool) -> None:
    """Linux：AppImage（有 appimagetool）否则 tar.gz；deb 需要 dpkg-deb。"""
    if HOST_SYSTEM != "Linux":
        print("  [跳过] 只有 Linux 主机能打 AppImage/deb")
        return
    if not run:
        return
    name = f"SXCL-{APP_VERSION}"
    subprocess.run(pyinstaller_args(name), cwd=ROOT, check=True)
    folder = DIST / name
    if not folder.is_dir():
        raise SystemExit(f"没找到打包产物目录 {folder}")
    OUT.mkdir(parents=True, exist_ok=True)
    appdir = prepare_appdir(folder)
    tool = which("appimagetool")
    if tool:
        subprocess.run([tool, str(appdir), str(OUT / f"SXCL-{APP_VERSION}-linux-{ARCH}.AppImage")],
                       check=True)
    else:
        print("  ! 没有 appimagetool，先出 tar.gz（https://github.com/AppImage/appimagetool）")
    archive = shutil.make_archive(str(OUT / f"SXCL-{APP_VERSION}-linux-{ARCH}"), "gztar", folder)
    print(f"  ✔ {archive}")
    if which("dpkg-deb"):
        print("  （已检测到 dpkg-deb：需要 .deb 的话按 build/packaging/deb 模板补一个 control 文件即可）")


def target_android(run: bool) -> None:
    """安卓 APK：走免 Gradle 的 aapt2 + d8 流程。"""
    if not run:
        return
    subprocess.run([sys.executable, str(ROOT / "scripts" / "build_android_apk.py")], cwd=ROOT,
                   check=True)


TARGETS = {
    "windows-portable": ("Windows 免安装绿色版（zip）", target_windows_portable),
    "windows-setup": ("Windows 安装包（Inno Setup）", target_windows_setup),
    "macos": ("macOS .app / .dmg", target_macos),
    "linux": ("Linux AppImage / tar.gz", target_linux),
    "android": ("安卓 APK（共享按键布局与教学）", target_android),
}


# ── 模板生成 ───────────────────────────────────────────────────

def prepare_inno() -> Path:
    PACKAGING.mkdir(parents=True, exist_ok=True)
    iss = PACKAGING / "sxcl.iss"
    body = """; 由 scripts/build_installers.py 生成，可自行调整
[Setup]
AppId={{8F1C2A64-2C4B-4E77-9B31-5F2C7A9E1D02}
AppName=""" + APP_NAME + """
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\\Silent X Craft Launcher
DefaultGroupName=Silent X Craft Launcher
OutputDir=""" + str(OUT) + """
OutputBaseFilename=SXCL-{#MyAppVersion}-windows-""" + ARCH + """-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "chinese"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "dist\\SXCL-""" + APP_VERSION + """\\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs

[Icons]
Name: "{group}\\Silent X Craft Launcher"; Filename: "{app}\\SXCL-""" + APP_VERSION + """.exe"
Name: "{autodesktop}\\Silent X Craft Launcher"; Filename: "{app}\\SXCL-""" + APP_VERSION + """.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Run]
Filename: "{app}\\SXCL-""" + APP_VERSION + """.exe"; Description: "启动"; Flags: nowait postinstall skipifsilent
"""
    iss.write_text(body, encoding="utf-8-sig")     # Inno Setup 认 BOM，中文注释才不会乱码
    return iss


def prepare_appdir(folder: Path) -> Path:
    """把 PyInstaller 的产物包成 AppDir（AppImage 要求的结构）。"""
    PACKAGING.mkdir(parents=True, exist_ok=True)
    appdir = PACKAGING / "SXCL.AppDir"
    (appdir / "usr" / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copytree(folder, appdir / "usr" / "bin", dirs_exist_ok=True)
    icon = appdir / "sxcl.png"
    if not icon.exists():
        icon.write_bytes(b"")                        # 占位：正式发布请放 256x256 图标
    (appdir / "AppRun").write_text(
        "#!/bin/sh\nHERE=$(dirname \"$(readlink -f \"$0\")\")\n"
        f'exec \"$HERE/usr/bin/SXCL-{APP_VERSION}\" \"$@\"\n', encoding="utf-8")
    (appdir / "AppRun").chmod(0o755)
    (appdir / "sxcl.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=" + APP_NAME + "\n"
        f"Exec=SXCL-{APP_VERSION}\nIcon=sxcl\nCategories=Game;Utility;\n"
        "Terminal=false\n", encoding="utf-8")
    share = appdir / "usr" / "share" / "applications"
    share.mkdir(parents=True, exist_ok=True)
    shutil.copy(appdir / "sxcl.desktop", share / "sxcl.desktop")
    return appdir


def prepare_all() -> None:
    print("生成打包模板：")
    if HOST_SYSTEM == "Windows":
        print(f"  ✔ Inno Setup 脚本: {prepare_inno()}")
    else:
        print("  - Inno 脚本只在 Windows 主机上生成")
    print(f"  ✔ 输出目录: {OUT}")


def show_list() -> None:
    info = packer()
    print(f"主机：{HOST_SYSTEM} {HOST_ARCH}（目标架构 {ARCH}）")
    print(f"桌面打包器：{info[0] if info else '缺失（pip install nuitka / pyinstaller）'}")
    print(f"安卓工具：aapt2={which('aapt2') or '（构建脚本会去 SDK 里找）'}  "
          f"JAVA_HOME={__import__('os').environ.get('JAVA_HOME', '未设置')}")
    print()
    for name, (desc, _func) in TARGETS.items():
        supported = {
            "windows-portable": HOST_SYSTEM == "Windows",
            "windows-setup": HOST_SYSTEM == "Windows",
            "macos": HOST_SYSTEM == "Darwin",
            "linux": HOST_SYSTEM == "Linux",
            "android": True,
        }[name]
        mark = "可打" if supported and info else ("本机不可打" if not supported else "缺打包器")
        print(f"  {name:<18} {desc:<28} [{mark}]")


def main() -> int:
    parser = argparse.ArgumentParser(description="SXCL 打包矩阵")
    parser.add_argument("--list", action="store_true", help="列出目标与可用工具")
    parser.add_argument("--prepare", action="store_true", help="只生成安装包模板文件")
    parser.add_argument("--target", choices=sorted(TARGETS), help="只打一个目标")
    parser.add_argument("--all", action="store_true", help="打本机支持的全部目标")
    args = parser.parse_args()

    if args.list:
        show_list()
        return 0
    if args.prepare:
        prepare_all()
        return 0
    if not args.target and not args.all:
        parser.print_help()
        return 0

    if args.all:
        for name, (desc, func) in TARGETS.items():
            print(f"=== {name}: {desc}")
            func(True)
        return 0

    desc, func = TARGETS[args.target]
    print(f"=== {args.target}: {desc}")
    func(True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
