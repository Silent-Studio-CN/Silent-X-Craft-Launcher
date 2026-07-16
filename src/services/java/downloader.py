"""Java runtime auto-download and installation.

Platform info is loaded from a version-index JSON so URLs can be
updated independently (published on GitHub, mirrored via gh-proxy).
"""

from __future__ import annotations

import os
import subprocess
import tarfile
import zipfile
from pathlib import Path
from typing import Callable

import requests
import yaml

from src.core.logger import log
from src.core.platform import current_platform, current_arch, PlatformType, ArchType

# ── Index sources ─────────────────────────────────────────────────

_LOCAL_INDEX = Path(__file__).resolve().parents[3] / "config" / "Java_index.yaml"
_GITHUB_URL = "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/Java_index.yaml"
_GITHUB_MIRROR = "https://gh-proxy.com/https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/Java_index.yaml"


# ── Index loading ─────────────────────────────────────────────────

def _load_index() -> dict:
    """Load the JDK index — try remote (with mirror), fallback to local."""
    for src_url in [_GITHUB_MIRROR, _GITHUB_URL]:
        try:
            resp = requests.get(src_url, timeout=10)
            resp.raise_for_status()
            log.info("JavaIndex | 从远端加载成功: %s", src_url[:60])
            return yaml.safe_load(resp.text)
        except Exception as e:
            log.warning("JavaIndex | 远端加载失败: %s", e)

    # Local fallback
    if _LOCAL_INDEX.exists():
        with open(_LOCAL_INDEX, encoding="utf-8") as f:
            log.info("JavaIndex | 从本地加载: %s", _LOCAL_INDEX)
            return yaml.safe_load(f)

    raise RuntimeError("无法加载 JDK 版本索引（本地和远端均失败）")


# ── Platform query ────────────────────────────────────────────────

_OS_MAP = {
    PlatformType.WINDOWS: "windows",
    PlatformType.MACOS: "macos",
    PlatformType.LINUX: "linux",
}
_ARCH_MAP = {
    ArchType.X86_64: "x64",
    ArchType.ARM64: "arm64",
}


def _best_entry(index: dict) -> tuple[str, str, dict]:
    """Return ``(version_key, platform_key, entry_dict)`` for current platform."""
    plat_name = _OS_MAP.get(current_platform())
    arch_name = _ARCH_MAP.get(current_arch(), "x64")
    if not plat_name:
        raise RuntimeError(f"不支持的系统: {current_platform()}")

    versions = index.get("versions", {})

    # First try recommended version
    for ver_key, ver_data in versions.items():
        if ver_data.get("recommended"):
            plat = ver_data.get("platforms", {}).get(plat_name, {})
            entry = plat.get(arch_name) or plat.get("x64")
            if entry:
                return ver_key, plat_name, {**entry, "_version_name": ver_data.get("name", f"JDK {ver_key}")}

    # Fallback: pick highest version number
    for ver_key in sorted(versions.keys(), reverse=True):
        ver_data = versions[ver_key]
        plat = ver_data.get("platforms", {}).get(plat_name, {})
        entry = plat.get(arch_name) or plat.get("x64")
        if entry:
            return ver_key, plat_name, {**entry, "_version_name": ver_data.get("name", f"JDK {ver_key}")}

    raise RuntimeError(f"索引中无适用于 {plat_name} 的 JDK")


# ── Public API ────────────────────────────────────────────────────


def display_name() -> str:
    """Return a human-readable name for the recommended JDK."""
    _, _, entry = _best_entry(_load_index())
    return f"{entry['_version_name']} - {entry.get('format', 'unknown')}"


def install_java(
    on_status: Callable[[str], None] = None,
    on_progress: Callable[[int, int], None] = None,
) -> Path | None:
    """Download and install JDK. Returns JDK home path, or ``None``."""
    status = on_status or (lambda s: None)
    progress = on_progress or (lambda c, t: None)

    index = _load_index()
    ver_key, plat_name, entry = _best_entry(index)
    ver_name = entry["_version_name"]
    fmt = entry["format"]

    # Build URL list: mirrors first, then original
    urls = list(entry.get("mirrors", []))
    if entry.get("url"):
        urls.append(entry["url"])

    if not urls:
        log.error("JavaDownload | %s 无可用下载链接", ver_name)
        status(f"❌ {ver_name} 无可用下载链接")
        return None

    runtime_dir = Path.home() / ".silent-x-craft-launcher" / "runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)

    file_ext = fmt  # exe / msi / tar.gz
    dest_path = runtime_dir / f"jdk-{ver_key}.{file_ext}"

    # ── Download (try each URL) ──
    from src.app.services.download.download_engine import DownloadEngine

    status(f"正在下载 {ver_name}…")
    log_prefix = f"JavaDownload | {ver_name}"

    ok = False
    for url in urls:
        try:
            engine = DownloadEngine(max_workers=4)
            engine.set_progress_callback(lambda c, t, s: progress(c, t))
            ok = engine.download(url, dest_path)
            if ok and dest_path.exists():
                log.info("%s 下载成功 | %s", log_prefix, url[:80])
                break
        except Exception as e:
            log.warning("%s 下载失败 | %s | %s", log_prefix, url[:60], e)

    if not ok or not dest_path.exists():
        log.error("%s 所有下载源均失败", log_prefix)
        status(f"❌ {ver_name} 下载失败，请检查网络")
        return None

    log.info("%s 下载完成 (%d MB)", log_prefix, dest_path.stat().st_size // 1024 // 1024)

    # ── Install ──
    status(f"正在安装 {ver_name}…")
    java_home = None
    install_args = entry.get("install_args", [])

    try:
        if fmt == "exe":
            target = runtime_dir / f"jdk-{ver_key}"
            args = [str(dest_path), *[a.replace("{target}", str(target)) for a in install_args]]
            subprocess.run(args, check=True, timeout=300)
            java_home = _find_java_home(target)

        elif fmt == "msi":
            subprocess.run(["msiexec", "/i", str(dest_path), "/quiet", "/norestart"],
                           check=True, timeout=300)
            java_home = _find_java_home(runtime_dir)

        elif fmt == "dmg":
            mount = Path(f"/Volumes/jdk-{ver_key}")
            subprocess.run(["hdiutil", "attach", str(dest_path), "-mountpoint", str(mount), "-quiet"],
                           check=True, timeout=120)
            pkgs = list(mount.glob("*.pkg"))
            if pkgs:
                subprocess.run(["sudo", "installer", "-pkg", str(pkgs[0]), "-target", "/"],
                               check=True, timeout=300)
            subprocess.run(["hdiutil", "detach", str(mount), "-quiet"], timeout=30)
            java_home = _find_java_home(runtime_dir)

        elif fmt in ("tar.gz", "tar.xz"):
            extracted = _extract_archive(dest_path, runtime_dir)
            if extracted:
                java_home = extracted
            else:
                status("❌ 解压失败")

    except Exception as e:
        log.error("%s 安装失败: %s", log_prefix, e)
        status(f"❌ 安装失败: {e}")

    if java_home and java_home.exists():
        log.info("%s 安装完成: %s", log_prefix, java_home)
        status(f"✅ {ver_name} 安装完成")
        return java_home

    log.error("%s 安装后无法定位 JDK", log_prefix)
    status("❌ 安装后无法定位 JDK 路径")
    return None


# ── Helpers ───────────────────────────────────────────────────────


def _extract_archive(archive: Path, target: Path) -> Path | None:
    """Extract .tar.gz / .tar.xz / .zip archive, return top-level dir."""
    target.mkdir(parents=True, exist_ok=True)
    top_dirs: set[str] = set()

    if archive.name.endswith(".tar.gz") or archive.name.endswith(".tar.xz"):
        mode = "r:gz" if archive.name.endswith(".gz") else "r:xz"
        with tarfile.open(archive, mode) as tf:
            top_dirs = {m.name.split("/")[0] for m in tf.getmembers() if "/" in m.name}
            tf.extractall(target)
    elif archive.suffix == ".zip":
        with zipfile.ZipFile(archive, "r") as zf:
            top_dirs = {n.split("/")[0] for n in zf.namelist() if "/" in n}
            zf.extractall(target)

    if top_dirs:
        return target / list(top_dirs)[0]
    return None


def _find_java_home(hint_dir: Path) -> Path | None:
    """Find the JDK home directory after installation."""
    bin_name = "java.exe" if current_platform() == PlatformType.WINDOWS else "java"

    # Check hint dir
    for candidate in [hint_dir, hint_dir / "bin"]:
        if (candidate / bin_name).exists():
            return candidate.parent if candidate.name == "bin" else candidate

    # Check Program Files (Windows)
    if current_platform() == PlatformType.WINDOWS:
        for root in [os.environ.get("ProgramFiles", "C:\\Program Files"),
                     os.environ.get("ProgramFiles(x86)", "C:\\Program Files (x86)")]:
            for d in Path(root).glob("Java/jdk-*/bin/java.exe"):
                return d.parent.parent

    # Check /Library (macOS)
    if current_platform() == PlatformType.MACOS:
        for d in Path("/Library/Java/JavaVirtualMachines").glob("*.jdk/Contents/Home/bin/java"):
            return d.parent.parent

    # Check /usr/lib/jvm (Linux)
    if current_platform() == PlatformType.LINUX:
        for d in Path("/usr/lib/jvm").glob("*/bin/java"):
            return d.parent.parent

    return None
