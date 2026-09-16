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
"""不用 Gradle 也能打出安卓 APK（aapt2 + javac + d8 + zipalign + apksigner）。

    python scripts/build_android_apk.py              # 打一个 debug 签名 APK
    python scripts/build_android_apk.py --install     # 打完直接 adb install
    python scripts/build_android_apk.py --check       # 只报告工具链，不编译

为什么不用 Gradle：
    * 这台机器没有 gradle，也不想去下几百 MB 的 AGP 依赖
    * 安卓端是纯 Java + 无第三方依赖，build-tools 自带的工具就够
    * 结果一样是真 APK，能装、能开、能签名校验
    android/ 目录下仍然保留了完整的 Gradle 工程（Android Studio 里可以直接开），
    两条路产出的东西是同一个。

产物：
    build/android/SXCL-Keymap-<version>.apk
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from src.core.constants import APP_VERSION                                # noqa: E402

ANDROID_DIR = ROOT / "android"
APP_DIR = ANDROID_DIR / "app"
SRC_DIR = APP_DIR / "src" / "main"
OUT_DIR = ROOT / "build" / "android"
PACKAGE_VERSION_NAME = APP_VERSION
PACKAGE_VERSION_CODE = 1
DEBUG_KEYSTORE = OUT_DIR / "debug.keystore"
KEYSTORE_PASS = "android"
KEY_ALIAS = "androiddebugkey"


def sdk_root() -> Path | None:
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(name)
        if value and Path(value).is_dir():
            return Path(value)
    for candidate in (Path.home() / "AppData" / "Local" / "Android" / "Sdk",):
        if candidate.is_dir():
            return candidate
    return None


def find_tool(sdk: Path, name: str) -> Path | None:
    build_tools = sdk / "build-tools"
    if not build_tools.is_dir():
        return None
    for version in sorted(build_tools.iterdir(), reverse=True):
        candidate = version / name
        if candidate.is_file():
            return candidate
    return None


def find_platform(sdk: Path) -> Path | None:
    platforms = sdk / "platforms"
    if not platforms.is_dir():
        return None
    for version in sorted(platforms.iterdir(), reverse=True):
        jar = version / "android.jar"
        if jar.is_file():
            return jar
    return None


def find_java_home() -> Path | None:
    value = os.environ.get("JAVA_HOME")
    if value and (Path(value) / "bin" / "javac.exe").is_file():
        return Path(value)
    for candidate in (Path("D:/jdk17"), Path("C:/Program Files/Java")):
        if (candidate / "bin" / "javac.exe").is_file():
            return candidate
    return None


def run(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    if result.returncode != 0:
        tail = ((result.stdout or "") + (result.stderr or "")).strip().splitlines()[-12:]
        print(f"[FAIL] {what}")
        for line in tail:
            print("   ", line)
        raise SystemExit(1)
    print(f"[ok] {what}")


def check_components(classes_dir: Path, manifest: Path) -> int:
    """核对 AndroidManifest 里的组件名与编译出来的 class 是否一致。"""
    import re

    text = manifest.read_text(encoding="utf-8")
    package = re.search(r'package="([^"]+)"', text)
    if not package:
        print("[FAIL] AndroidManifest.xml 没有 package 属性（aapt2 需要，类名解析也靠它）")
        raise SystemExit(1)
    root = package.group(1)
    if not (classes_dir / Path(*root.split(".")) / "ui" / "MainActivity.class").is_file():
        print(f"[FAIL] 清单 package={root} 和 Java 源码的包名对不上")
        raise SystemExit(1)

    names = re.findall(r'<(?:activity|service|receiver|provider)\s[^>]*android:name="(\.?[A-Za-z0-9_.]+)"',
                       text, flags=re.S)
    if not names:
        print("[FAIL] 清单里没有解析到任何组件")
        raise SystemExit(1)
    for name in names:
        full = name if name.startswith(root) else root + name
        path = classes_dir / Path(*full.split("."))
        if not path.with_suffix(".class").is_file():
            print(f"[FAIL] 清单里的组件在代码里不存在: {full}")
            raise SystemExit(1)
    print(f"[ok] 清单组件与类名一致（{len(names)} 个，package={root}）")
    return len(names)


def java_sources() -> list[str]:
    return [str(path) for path in sorted((APP_DIR / "src" / "main" / "java").rglob("*.java"))]


def main() -> int:
    parser = argparse.ArgumentParser(description="SXCL 安卓端打包（无需 Gradle）")
    parser.add_argument("--check", action="store_true", help="只检查工具链")
    parser.add_argument("--install", action="store_true", help="打完后 adb install -r")
    parser.add_argument("--keep", action="store_true", help="保留中间产物")
    args = parser.parse_args()

    sdk = sdk_root()
    if sdk is None:
        print("[FAIL] 找不到 Android SDK（设置 ANDROID_HOME）")
        return 1
    java_home = find_java_home()
    tools = {name: find_tool(sdk, f"{name}.bat") or find_tool(sdk, f"{name}.exe")
             for name in ("aapt2", "d8", "zipalign", "apksigner")}
    android_jar = find_platform(sdk)

    print(f"SDK        : {sdk}")
    print(f"JAVA_HOME  : {java_home}")
    print(f"android.jar: {android_jar}")
    for name, path in tools.items():
        print(f"{name:<11}: {path}")

    missing = [name for name, path in tools.items() if path is None]
    if missing or android_jar is None or java_home is None:
        print(f"[FAIL] 缺工具: {missing}")
        return 1
    if args.check:
        print("[ok] 工具链齐全，可以打包")
        return 0

    # 1) 先把桌面端的预设/教学导出成安卓资源（单一数据源）
    from scripts.export_keymap_assets import export as export_assets
    items = export_assets()
    print(f"[ok] 导出按键资源 {len(items)} 份")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    res_zip = OUT_DIR / "res.zip"
    base_apk = OUT_DIR / "base.apk"
    classes_dir = OUT_DIR / "classes"
    classes_jar = OUT_DIR / "classes.jar"
    dex_dir = OUT_DIR / "dex"
    aligned_apk = OUT_DIR / "aligned.apk"
    final_apk = OUT_DIR / f"SXCL-Keymap-{PACKAGE_VERSION_NAME}.apk"
    for path in (res_zip, base_apk, classes_jar, aligned_apk, final_apk):
        path.unlink(missing_ok=True)
    shutil.rmtree(classes_dir, ignore_errors=True)
    shutil.rmtree(dex_dir, ignore_errors=True)

    # 2) 资源 + 清单 -> 基础 APK（同时把 assets/keymaps 打进去）
    run([str(tools["aapt2"]), "compile", "--dir", str(SRC_DIR / "res"), "-o", str(res_zip)],
        "aapt2 编译资源")
    run([str(tools["aapt2"]), "link", "-o", str(base_apk),
         "-I", str(android_jar),
         "--manifest", str(SRC_DIR / "AndroidManifest.xml"),
         "-R", str(res_zip),
         "-A", str(SRC_DIR / "assets"),
         "--min-sdk-version", "26", "--target-sdk-version", "36",
         "--version-code", str(PACKAGE_VERSION_CODE),
         "--version-name", PACKAGE_VERSION_NAME,
         "--auto-add-overlay"],
        "aapt2 链接清单与资源")

    # 3) Java -> class -> dex
    javac = java_home / "bin" / "javac.exe"
    run([str(javac), "-encoding", "UTF-8", "--release", "11",
         "-cp", str(android_jar), "-d", str(classes_dir)] + java_sources(),
        f"javac 编译 {len(java_sources())} 个源文件")
    # 3.5) 清单里写的组件必须真有对应的 class —— 打出来的包装上去点不开，
    #      十次有九次是包名/类名对不上，这一步专门拦它。
    check_components(classes_dir, SRC_DIR / "AndroidManifest.xml")

    jar = java_home / "bin" / "jar.exe"
    run([str(jar), "cf", str(classes_jar), "-C", str(classes_dir), "."], "打包 classes.jar")
    dex_dir.mkdir(parents=True, exist_ok=True)      # d8 要求输出目录先存在
    run([str(tools["d8"]), "--lib", str(android_jar), "--min-api", "26", "--release",
         "--output", str(dex_dir), str(classes_jar)],
        "d8 转成 classes.dex")

    # 4) dex 塞进 APK（zip 用 Python 自己写，Windows 上没有 zip 命令）
    dex_file = dex_dir / "classes.dex"
    if not dex_file.is_file():
        print("[FAIL] d8 没有产出 classes.dex")
        return 1
    with zipfile.ZipFile(base_apk, "a", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex_file, "classes.dex")
    print(f"[ok] 写入 classes.dex（{dex_file.stat().st_size / 1024:.0f} KB）")

    # 5) 对齐 + 签名
    run([str(tools["zipalign"]), "-f", "-p", "4", str(base_apk), str(aligned_apk)], "zipalign 对齐")
    if not DEBUG_KEYSTORE.is_file():
        keytool = java_home / "bin" / "keytool.exe"
        run([str(keytool), "-genkeypair", "-keystore", str(DEBUG_KEYSTORE),
             "-storepass", KEYSTORE_PASS, "-keypass", KEYSTORE_PASS,
             "-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
             "-dname", "CN=SXCL Debug,O=SilentCodeTeams,C=CN"],
            "生成调试签名证书")
    run([str(tools["apksigner"]), "sign", "--ks", str(DEBUG_KEYSTORE),
         "--ks-pass", f"pass:{KEYSTORE_PASS}", "--key-pass", f"pass:{KEYSTORE_PASS}",
         "--ks-key-alias", KEY_ALIAS, "--out", str(final_apk), str(aligned_apk)],
        "apksigner 签名")
    run([str(tools["apksigner"]), "verify", "--print-certs", str(final_apk)], "验证签名")

    size_mb = final_apk.stat().st_size / 1024 / 1024
    print()
    print(f"APK: {final_apk}  ({size_mb:.2f} MB)")

    # 6) 看看包里到底有什么（不装也能确认资源/清单正确）
    aapt2 = tools["aapt2"]
    result = subprocess.run([str(aapt2), "dump", "badging", str(final_apk)],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in (result.stdout or "").splitlines():
        if line.startswith(("package:", "launchable-activity:", "uses-permission:",
                            "application-label:", "sdkVersion:", "targetSdkVersion:")):
            print("   ", line.strip())

    if not args.keep:
        for path in (res_zip, base_apk, aligned_apk, classes_jar):
            path.unlink(missing_ok=True)
        shutil.rmtree(classes_dir, ignore_errors=True)
        shutil.rmtree(dex_dir, ignore_errors=True)

    if args.install:
        adb = Path("C:/platform-tools/adb.exe")
        if not adb.is_file():
            adb = Path(shutil.which("adb") or "")
        if adb.is_file():
            run([str(adb), "install", "-r", str(final_apk)], "adb 安装到手机")
        else:
            print("[warn] 没找到 adb，跳过安装")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
