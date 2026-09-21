# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# pack_jre_from_termux.py - 把 Termux 的公开 OpenJDK 构建(用户自己取来的 .deb 或已解出的
# usr/ 目录)打成**我们自己的远端包形态**(照 FCL 的目录思路,见 docs/19):
#
#   <out>/<component>/universal.tar.xz     架构无关:conf/ legal/ lib/ 里的非 ELF 数据
#   <out>/<component>/bin-arm64.tar.xz     架构相关:bin/*、lib/**.so、lib/server/**、release
#   <out>/<component>/version              版本标记(内容派生,用来判断要不要重装)
#   <out>/<component>/NOTICE/**            包**旁**的许可/来源/源码获取方式(见 docs/19)
#   <out>/index.json                       每文件 size + sha256 + sha1(schema: sxcl.jre.index/1)
#
# 硬规矩:
#   * **完全离线**:本脚本不发任何网络请求,也不含任何 URL 字面量;上游地址一律由参数/文件给。
#   * **不许偷偷放过**:NEEDED 动态库闭包缺一个、许可文本缺一份、包自检不过 -> 非 0 退出。
#   * **可重复**:同一份输入 + 同一个 --stamp/--mtime -> 同样的 tar.xz 字节(条目排序、uid/gid=0、
#     时间戳固定、无 pax 垃圾字段),sha256 可复现。
#   * 切分按**内容**判定(ELF 魔数 / .jsa / lib/<arch>/ / release),所以 lib/jspawnhelper 这种
#     没有 .so 后缀的可执行也会进 bin-arm64(与 FCL 的做法不同,理由见 docs/19)。
#
# 用法(例子见 docs/19 §打包):
#   python android/scripts/pack_jre_from_termux.py \
#       --input D:/sxcl_src/openjdk-17_17.0.20_aarch64.deb \
#       --input D:/sxcl_src/libandroid-shmem_0.7_aarch64.deb \
#       --input D:/sxcl_src/libandroid-spawn_0.3_aarch64.deb \
#       --input D:/sxcl_src/libiconv_1.19_aarch64.deb \
#       --input D:/sxcl_src/libjpeg-turbo_3.2.0_aarch64.deb \
#       --input D:/sxcl_src/zlib_1.3.2_aarch64.deb \
#       --input D:/sxcl_src/littlecms_2.19.1_aarch64.deb \
#       --input D:/sxcl_src/alsa-plugins_1.2.12-1_aarch64.deb \
#       --input D:/sxcl_src/alsa-lib_1.2.16.1_aarch64.deb \
#       --input D:/sxcl_src/libandroid-sysv-semaphore_0.3_aarch64.deb \
#       --input D:/sxcl_src/termux-licenses_2.0-3_all.deb \
#       --component jre17 --build-sh D:/sxcl_src/openjdk-17_build.sh \
#       --out D:/sxcl_out/jre
#
# 想先只看切分不动盘:加 --dry-run(只打印计划,不写任何文件)。

import argparse
import atexit
import bz2
import gzip
import hashlib
import json
import lzma
import os
import re
import shutil
import struct
import sys
import tarfile
import tempfile
import time

# ──────────────────────────── 常量 ────────────────────────────

SCHEMA = "sxcl.jre.index/1"

# 安卓系统库:这些**不应该**出现在包里,缺了也不算"闭包没闭合"。
SYSTEM_LIBS = frozenset([
    "libc.so", "libm.so", "libdl.so", "ld-android.so", "liblog.so", "libz.so",
    "libstdc++.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
    "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so",
    "libnativewindow.so", "libsync.so", "libvulkan.so", "libaaudio.so",
    "libmediandk.so", "libcamera2ndk.so", "libneuralnetworks.so", "libbinder_ndk.so",
    "libcutils.so", "libhardware.so", "libutils.so", "libui.so", "libgui.so",
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libgcc_s.so.1", "libatomic.so.1", "libOpenSLES.so.1",
])

# jre8 时代架构相关的子目录名(里面的东西无论是不是 ELF 都算架构相关)
ARCH_DIRS = frozenset([
    "aarch64", "arm", "arm64", "armv7l", "amd64", "x86", "x86_64", "i386", "i686",
    "riscv64", "ppc64le", "s390x", "aarch64-v8a",
])

# 默认裁掉的 JDK-only 部分(运行时不需要;--keep-all 可关)。
# 注意:**legal/ 永远不裁**——它是 GPL/Classpath 的义务。
TRIM_DEFAULT = ["jmods", "include", "man", "demo", "sample", "lib/src.zip"]

JRE_LIB_SHIMS = ("libawt_xawt.so", "libjsound.so")


def out(*parts):
    sys.stdout.write("".join(str(p) for p in parts) + "\n")


def warn(*parts):
    sys.stdout.write("[warn] " + "".join(str(p) for p in parts) + "\n")


def die(code, *parts):
    sys.stdout.write("[FAIL] " + "".join(str(p) for p in parts) + "\n")
    sys.stdout.flush()
    sys.exit(code)


def human(n):
    if n is None:
        return "-"
    units = ["B", "KiB", "MiB", "GiB"]
    v = float(n)
    i = 0
    while v >= 1024.0 and i < len(units) - 1:
        v /= 1024.0
        i += 1
    if i == 0:
        return "%d %s" % (n, units[0])
    return "%.2f %s (%d B)" % (v, units[i], n)


# ──────────────────────── 小工具:哈希/路径 ────────────────────────

def hash_file(path, want_sha1=True):
    """一遍读完算 sha256(+sha1)与字节数。"""
    h256 = hashlib.sha256()
    h1 = hashlib.sha1() if want_sha1 else None
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            size += len(chunk)
            h256.update(chunk)
            if h1 is not None:
                h1.update(chunk)
    return {"size": size, "sha256": h256.hexdigest(),
            "sha1": (h1.hexdigest() if h1 is not None else None)}


def hash_bytes(data, want_sha1=True):
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "sha1": hashlib.sha1(data).hexdigest() if want_sha1 else None}


def to_rel(path):
    return path.replace("\\", "/")


def is_rel_safe(name):
    n = name.replace("\\", "/")
    if n.startswith("/") or n.startswith("../") or "/../" in n or n == "..":
        return False
    if re.match(r"^[A-Za-z]:", n):
        return False
    return True


def walk_tree(root):
    """产出 (relpath, abspath, is_dir, is_symlink),relpath 用 / 分隔,按字节序排序。"""
    items = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        rel_dir = to_rel(os.path.relpath(dirpath, root))
        if rel_dir == ".":
            rel_dir = ""
        for d in list(dirnames):
            ap = os.path.join(dirpath, d)
            rp = (rel_dir + "/" + d) if rel_dir else d
            items.append((rp, ap, True, os.path.islink(ap)))
            if os.path.islink(ap):
                dirnames.remove(d)  # 不跟着链接往下走
        for fn in filenames:
            ap = os.path.join(dirpath, fn)
            rp = (rel_dir + "/" + fn) if rel_dir else fn
            items.append((rp, ap, False, os.path.islink(ap)))
    items.sort(key=lambda t: t[0])
    return items


# ──────────────────────── ELF:DT_NEEDED / SONAME ────────────────────────

# Windows 上 os.symlink 需要 SeCreateSymbolicLinkPrivilege(默认没有)。为了不静默丢链接,
# 解 .deb 时把每个链接记在这里:key = 落盘绝对路径(normcase),value = 链接目标原文。
SYMLINK_HINT = {}
SYMLINK_LINKS = []      # [(原始绝对路径, 链接目标)] —— 断链时盘上没有文件,许可收集要能看见它们


def hint_set(path, target):
    ap = os.path.abspath(path)
    SYMLINK_HINT[os.path.normcase(ap)] = target
    SYMLINK_LINKS.append((ap, target))


def hint_get(path):
    return SYMLINK_HINT.get(os.path.normcase(os.path.abspath(path)))


ELF_MAGIC = b"\x7fELF"
_MACHINES = {0x28: "arm", 0xB7: "aarch64", 0x03: "i386", 0x3E: "x86_64"}


def elf_info(path):
    """读 ELF 头/程序头/动态段,拿 DT_NEEDED、DT_SONAME、机器。不是 ELF 返回 None。"""
    try:
        with open(path, "rb") as f:
            ident = f.read(16)
            if len(ident) < 16 or ident[:4] != ELF_MAGIC:
                return None
            cls = ident[4]           # 1=32 位 2=64 位
            data = ident[5]          # 1=小端 2=大端
            if cls not in (1, 2) or data not in (1, 2):
                return None
            endian = "<" if data == 1 else ">"
            is64 = (cls == 2)
            if is64:
                hdr = f.read(48)
                if len(hdr) < 48:
                    return None
                (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags,
                 _ehsize, e_phentsize, e_phnum, _shentsize, _shnum,
                 _shstrndx) = struct.unpack(endian + "HHIQQQIHHHHHH", hdr)
                ph_fmt, ph_size = endian + "IIQQQQQQ", 56
                dyn_fmt, dyn_size = endian + "qQ", 16
            else:
                hdr = f.read(36)
                if len(hdr) < 36:
                    return None
                (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags,
                 _ehsize, e_phentsize, e_phnum, _shentsize, _shnum,
                 _shstrndx) = struct.unpack(endian + "HHIIIIIHHHHHH", hdr)
                ph_fmt, ph_size = endian + "IIIIIIII", 32
                dyn_fmt, dyn_size = endian + "iI", 8
            loads = []
            dyn_off = dyn_size_total = None
            f.seek(e_phoff)
            phs = f.read(e_phentsize * e_phnum)
            for i in range(e_phnum):
                raw = phs[i * e_phentsize:(i + 1) * e_phentsize]
                if len(raw) < ph_size:
                    break
                vals = struct.unpack(ph_fmt, raw[:ph_size])
                p_type = vals[0]
                if is64:
                    p_offset, p_vaddr, _p_paddr, p_filesz = vals[2], vals[3], vals[4], vals[5]
                else:
                    p_offset, p_vaddr, _p_paddr, p_filesz = vals[1], vals[2], vals[3], vals[4]
                if p_type == 1:      # PT_LOAD
                    loads.append((p_vaddr, p_offset, p_filesz))
                elif p_type == 2:    # PT_DYNAMIC
                    dyn_off, dyn_size_total = p_offset, p_filesz
            if dyn_off is None:
                return {"needed": [], "soname": None,
                        "machine": _MACHINES.get(e_machine, hex(e_machine)), "dynamic": False}
            f.seek(dyn_off)
            dvals = f.read(min(dyn_size_total or 0, 1 << 20))
            entries = []
            for i in range(0, len(dvals) - dyn_size + 1, dyn_size):
                tag, val = struct.unpack(dyn_fmt, dvals[i:i + dyn_size])
                if tag == 0:
                    break
                entries.append((tag, val))
            strtab_va = strsz = None
            for tag, val in entries:
                if tag == 5:      # DT_STRTAB
                    strtab_va = val
                elif tag == 10:   # DT_STRSZ
                    strsz = val
            if strtab_va is None:
                return {"needed": [], "soname": None,
                        "machine": _MACHINES.get(e_machine, hex(e_machine)), "dynamic": True}

            def va_to_off(va):
                for vaddr, offset, filesz in loads:
                    if vaddr <= va < vaddr + filesz:
                        return offset + (va - vaddr)
                return None

            off = va_to_off(strtab_va)
            strs = b""
            if off is not None and strsz:
                f.seek(off)
                strs = f.read(min(strsz, 1 << 20))

            def s(idx):
                if idx >= len(strs):
                    return ""
                end = strs.find(b"\x00", idx)
                if end < 0:
                    end = len(strs)
                return strs[idx:end].decode("utf-8", "replace")

            needed = [s(val) for tag, val in entries if tag == 1]
            soname = None
            for tag, val in entries:
                if tag == 14:     # DT_SONAME
                    soname = s(val)
                    break
            return {"needed": [n for n in needed if n], "soname": soname,
                    "machine": _MACHINES.get(e_machine, hex(e_machine)), "dynamic": True}
    except (OSError, struct.error):
        return None


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == ELF_MAGIC
    except OSError:
        return False


# ──────────────────────── .deb(ar + control)────────────────────────

def ar_members(path):
    with open(path, "rb") as f:
        if f.read(8) != b"!<arch>\n":
            raise RuntimeError("不是 ar 归档(缺 !<arch> 魔数):%s" % path)
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                return
            name = hdr[0:16].decode("ascii", "replace").strip()
            try:
                size = int(hdr[48:58].decode("ascii", "replace").strip() or "0")
            except ValueError:
                raise RuntimeError("ar 头里的长度字段非法:%s" % name)
            off = f.tell()
            yield name.rstrip("/"), size, off
            f.seek(off + size + (size % 2))


def open_member_stream(path, offset, size, comp):
    f = open(path, "rb")
    f.seek(offset)
    raw = _BoundedReader(f, size)
    if comp in ("xz", "lzma"):
        return lzma.LZMAFile(raw)
    if comp == "gz":
        return gzip.GzipFile(fileobj=raw)
    if comp == "bz2":
        return bz2.BZ2File(raw)
    if comp == "zst":
        try:
            import compression.zstd as zstd
        except ImportError:
            return None
        return zstd.ZstdFile(raw, "rb")
    return None


class _BoundedReader:
    """只让读到 size 字节的只读流(tarfile 会往后读,不能越过成员边界)。"""

    def __init__(self, f, size):
        self.f = f
        self.left = size

    def read(self, n=-1):
        if self.left <= 0:
            return b""
        if n is None or n < 0 or n > self.left:
            n = self.left
        data = self.f.read(n)
        self.left -= len(data)
        return data

    def readable(self):
        return True


def deb_control(path):
    """从 .deb 里读 control 文件,返回 dict(Package/Version/Architecture/Depends/...)。"""
    for name, size, off in ar_members(path):
        if not name.startswith("control.tar"):
            continue
        comp = name.rsplit(".", 1)[-1]
        stream = open_member_stream(path, off, size, comp)
        if stream is None:
            raise RuntimeError("control.tar.%s 解不开(本机 Python 没有这个解压器)" % comp)
        with tarfile.open(fileobj=stream, mode="r|") as tf:
            for m in tf:
                base = os.path.basename(m.name)
                if base in ("control", "./control") and m.isfile():
                    text = tf.extractfile(m).read().decode("utf-8", "replace")
                    fields = {}
                    cur = None
                    for line in text.splitlines():
                        if line.startswith(" ") and cur:
                            fields[cur] += "\n" + line.strip()
                        elif ":" in line:
                            k, v = line.split(":", 1)
                            cur = k.strip()
                            fields[cur] = v.strip()
                    return fields
    raise RuntimeError("这个 .deb 里没有 control.tar.*:%s" % path)


def extract_deb(path, dest):
    """把 .deb 的 data.tar.* 解到 dest;返回 data 成员名。"""
    data = None
    for name, size, off in ar_members(path):
        if name.startswith("data.tar"):
            data = (name, size, off)
            break
    if data is None:
        raise RuntimeError("这个 .deb 里没有 data.tar.*:%s" % path)
    name, size, off = data
    comp = name.rsplit(".", 1)[-1]
    stream = open_member_stream(path, off, size, comp)
    if stream is None:
        raise RuntimeError("data.tar.%s 解不开(本机 Python 没有这个解压器);"
                           "请用 dpkg -x / 7z 先解开,再把 usr/ 目录喂给 --input" % comp)
    links = {}
    with tarfile.open(fileobj=stream, mode="r|") as tf:
        for m in tf:
            if not is_rel_safe(m.name):
                raise RuntimeError("data.tar 里有不安全的路径(拒绝解):%s" % m.name)
            rel = to_rel(m.name).lstrip("./")
            if m.issym() or m.islnk():
                links[rel] = to_rel(m.linkname)
                continue
            if m.isfile() or m.isdir():
                m.name = rel
                tf.extract(m, dest, filter="data")
            elif m.isdev() or m.isfifo():
                warn("跳过设备/FIFO 条目:", rel)
    _resolve_links(dest, links)
    return name


def _resolve_links(dest, links):
    """把 data.tar 里的符号/硬链接落地:能建 symlink 就建(保真),不行就复制目标内容。
    目标不存在(典型:termux-licenses 包没给)的链接**不造假文件**,只记 hint,交给上层报缺。"""
    dest_abs = os.path.abspath(dest)
    made, copied = 0, 0
    outside = []
    for _pass in range(5):          # 链接可能是链(libjpeg.so -> libjpeg.so.8 -> libjpeg.so.8.3.2),多跑几轮
        progressed = False
        for name, target in sorted(links.items()):
            dst = os.path.join(dest, name)
            hint_set(dst, target)
            if os.path.exists(dst) or os.path.islink(dst):
                continue
            resolved = os.path.normpath(os.path.join(os.path.dirname(dst), target))
            if not (os.path.abspath(resolved).lower().startswith(dest_abs.lower())):
                if (name, target) not in outside:
                    outside.append((name, target))
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            try:
                os.symlink(target, dst)
                made += 1
                progressed = True
                continue
            except (OSError, NotImplementedError):
                pass
            if os.path.isfile(resolved):
                shutil.copyfile(resolved, dst)
                copied += 1
                progressed = True
            elif os.path.isdir(resolved):
                try:
                    shutil.copytree(resolved, dst)
                    copied += 1
                    progressed = True
                except OSError:
                    pass
        if not progressed:
            break
    dangling = [(n, t, "链接目标在解包目录之外") for n, t in outside]
    dangling += [(n, t, "目标不存在") for n, t in sorted(links.items())
                 if not os.path.exists(os.path.join(dest, n))]
    if made or copied:
        out2 = "  symlink=%d, 复制目标=%d" % (made, copied)
    else:
        out2 = ""
    if dangling:
        warn("有 %d 个链接没落地(目标不在这些输入里,常见:termux-licenses):%s"
             % (len(dangling), ", ".join(n for n, _t, _w in dangling[:4])))
    if out2:
        out("[sym ]" + out2)


# ──────────────────────── 输入定位 ────────────────────────

def find_prefix_roots(root, max_depth=8):
    """在一个解开的 deb 树里找 "usr" 前缀根(含 lib/ 或 bin/ 的那一层)。"""
    hits = []
    root = os.path.abspath(root)
    base_depth = root.rstrip("\\/").count(os.sep)
    for dirpath, dirnames, _filenames in os.walk(root, followlinks=False):
        if dirpath.rstrip("\\/").count(os.sep) - base_depth > max_depth:
            dirnames[:] = []
            continue
        if os.path.basename(dirpath) == "usr" and any(
                os.path.isdir(os.path.join(dirpath, sub))
                for sub in ("lib", "bin", "share", "include", "etc", "var", "opt")):
            hits.append(dirpath)
            dirnames[:] = []
    return hits


def looks_like_jdk_home(path):
    if not os.path.isfile(os.path.join(path, "release")):
        return False
    if os.path.isfile(os.path.join(path, "bin", "java")):
        return True
    if os.path.isfile(os.path.join(path, "lib", "server", "libjvm.so")):
        return True
    for d in ARCH_DIRS:
        if os.path.isfile(os.path.join(path, "lib", d, "server", "libjvm.so")):
            return True
    return False


def find_jdk_homes(roots, max_depth=5):
    """有界递归找 JDK/JRE home(不猜:找到多个就交给上层报错)。
    先判自己再往下走,所以 JDK8 的 "JDK 外壳(带 jre/ 子目录)" 会选外壳,不会选里面的 jre/。"""
    hits = []
    for r in roots:
        stack = [(os.path.abspath(r), 0)]
        while stack:
            d, depth = stack.pop()
            if looks_like_jdk_home(d):
                hits.append(d)
                continue
            if depth >= max_depth:
                continue
            try:
                names = sorted(os.listdir(d))
            except OSError:
                continue
            for name in names:
                sub = os.path.join(d, name)
                if os.path.isdir(sub) and not os.path.islink(sub):
                    stack.append((sub, depth + 1))
    uniq = []
    for h in sorted(set(hits)):
        if h not in uniq and os.path.isdir(h):
            uniq.append(h)
    return uniq


def parse_release(path):
    fields = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                fields[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return fields


def major_of(java_version):
    """从 release 的 JAVA_VERSION 里读主版本。Java 8 是 "1.8.0_xxx" -> 8,不是 1。"""
    if not java_version:
        return 0
    m = re.match(r"^1\.(\d+)", java_version)
    if m:
        return int(m.group(1))
    m = re.match(r"(\d+)", java_version)
    return int(m.group(1)) if m else 0


def parse_build_sh(path):
    """从 termux-packages 的 packages/openjdk-*/build.sh 里读来源信息(离线,不联网)。"""
    info = {}
    if not path:
        return info
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # build.sh 里两种写法都有:'K="v"' 与 K=v(openjdk-17 的 SRCURL/SHA256 就是不引号那种),
    # 所以这里两种都认;认不出来就别装作认出来了(NOTICE 会写"未提供"并让脚本非 0 退出)。
    for key in ("TERMUX_PKG_VERSION", "TERMUX_PKG_SRCURL", "TERMUX_PKG_SHA256",
                "TERMUX_PKG_LICENSE", "TERMUX_PKG_DEPENDS", "TERMUX_PKG_BUILD_DEPENDS"):
        m = re.search(r'^\s*' + key + r'=(?:"([^"]*)"|(\S+))\s*$', text, re.M)
        if m:
            info[key] = m.group(1) if m.group(1) is not None else m.group(2)
    # build.sh 里长这样:.../tags/jdk-${TERMUX_PKG_VERSION}-ga.tar.gz
    # 写进 NOTICE 的必须是**能直接用的 URL**(展开变量),不然"源码获取方式"等于没写。
    ver = info.get("TERMUX_PKG_VERSION", "")
    for k in list(info):
        if isinstance(info[k], str):
            info[k] = info[k].replace("${TERMUX_PKG_VERSION}", ver)
    return info


# ──────────────────────── 切分判定 ────────────────────────

def classify(rel, abspath, elf, trim_prefixes):
    """返回 "universal" / "bin" / "skip",以及理由。"""
    for t in trim_prefixes:
        if rel == t or rel.startswith(t + "/"):
            return "skip", "trim:" + t
    base = rel.rsplit("/", 1)[-1]
    if rel == "release":
        return "bin", "release(带 OS_ARCH)"
    if rel.endswith(".jsa"):
        return "bin", "CDS 归档(*.jsa 跟架构绑定)"
    if elf is not None:
        return "bin", "ELF(" + (elf["machine"] or "?") + ")"
    parts = rel.split("/")
    if len(parts) >= 3 and parts[0] == "lib" and parts[1] in ARCH_DIRS:
        return "bin", "lib/" + parts[1] + "/ 架构目录"
    if base in ("jspawnhelper", "jexec", "unpack200", "pack200"):
        return "bin", "按名字判定的本机可执行"
    if parts[0] == "bin":
        return "bin", "bin/ 下的东西"
    return "universal", "非 ELF 数据"


def shim_dir_for(home):
    """我们的两个 shim 该放进哪个目录(记录用):与 launcher 的规则对齐。"""
    jre_lib = os.path.join(home, "jre", "lib")
    if os.path.isdir(jre_lib):
        return "jre/lib"
    plain = os.path.join(home, "lib")
    for d in sorted(ARCH_DIRS):
        cand = os.path.join(plain, d)
        if os.path.isdir(cand) and (os.path.isfile(os.path.join(cand, "libjava.so")) or
                                    os.path.isfile(os.path.join(cand, "libawt.so"))):
            return "lib/" + d
    return "lib"


# ──────────────────────── 打包 ────────────────────────

def add_tar_entry(tf, rel, abspath, is_dir, is_link, exec_hint):
    ti = tarfile.TarInfo(rel)
    ti.uid = 0
    ti.gid = 0
    ti.uname = ""
    ti.gname = ""
    ti.mtime = ARGS.mtime
    if is_dir:
        ti.type = tarfile.DIRTYPE
        ti.mode = 0o755
        ti.size = 0
        tf.addfile(ti)
        return
    if is_link:
        ti.type = tarfile.SYMTYPE
        ti.mode = 0o777
        ti.size = 0
        ti.linkname = to_rel(os.readlink(abspath))
        tf.addfile(ti)
        return
    ti.type = tarfile.REGTYPE
    ti.mode = 0o755 if exec_hint else 0o644
    size = os.path.getsize(abspath)
    ti.size = size
    with open(abspath, "rb") as f:
        tf.addfile(ti, f)


def build_archive(entries, tar_path, preset):
    """entries: (rel, abspath, is_dir, is_link, exec_hint) 已排序。确定性:uid/gid=0,时间戳固定。"""
    with tarfile.open(tar_path, "w:xz", format=tarfile.PAX_FORMAT, preset=preset) as tf:
        for rel, abspath, is_dir, is_link, exec_hint in entries:
            add_tar_entry(tf, rel, abspath, is_dir, is_link, exec_hint)


def archive_manifest(tar_path, want_sha1=True):
    """重读一遍归档,逐成员算 size/sha256/sha1(自检 + index.json 的 manifest)。"""
    man = {}
    with tarfile.open(tar_path, "r:xz") as tf:
        for m in tf:
            name = m.name.lstrip("./")
            if m.isdir():
                man[name] = {"type": "directory"}
            elif m.issym() or m.islnk():
                man[name] = {"type": "link", "target": m.linkname, "size": 0,
                             "sha256": hashlib.sha256(b"").hexdigest(),
                             "sha1": hashlib.sha1(b"").hexdigest() if want_sha1 else None}
            elif m.isfile():
                h256 = hashlib.sha256()
                h1 = hashlib.sha1() if want_sha1 else None
                f = tf.extractfile(m)
                size = 0
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    size += len(chunk)
                    h256.update(chunk)
                    if h1 is not None:
                        h1.update(chunk)
                man[name] = {"type": "file", "size": size, "sha256": h256.hexdigest(),
                             "sha1": (h1.hexdigest() if h1 is not None else None),
                             "executable": bool(m.mode & 0o111)}
            else:
                man[name] = {"type": "other:" + str(m.type), "size": 0}
    return man


# ──────────────────────── NOTICE ────────────────────────

NOTICE_README = """这份 NOTICE 目录是 <component> 的**随包许可与来源说明**,与包放在同一个远端目录下
(即 jreN/universal.tar.xz、jreN/bin-arm64.tar.xz、jreN/version 的旁边)。

为什么放"包旁"而不是"包里":
 1) 包里(universal.tar.xz)本来就有 JDK 自带的 legal/ 目录,那是 OpenJDK 的义务、必须原样保留;
    但它只有在解开包之后才看得到,下载前/安装前无法呈现。
 2) 我们要一起重发布的几个共享库(libiconv、libjpeg-turbo、littlecms、zlib、libandroid-shmem/-spawn、
    alsa 等)**不是 JDK 镜像的一部分**,它们的许可文本来自各自的 Termux 包(share/doc/<包名>/copyright),
    放进 FCL 形状的运行时树里会污染运行时目录,也会让"哪个文件属于哪个许可"失去对应关系。
 3) 目录一旦列进 index.json(带 size/sha256/sha1),它就和包一样可校验、可发现;启动器的"许可/关于"
    页可以直接按 index.json 里的路径去取,不用先安装。
""" 


def collect_license_docs(roots):
    """把所有输入树的 share/doc/*/copyright* 收到一起(含 termux-licenses 的 share/LICENSES)。"""
    docs = []
    lic_dir = {}
    for r in roots:
        doc_root = os.path.join(r, "share", "doc")
        if os.path.isdir(doc_root):
            for pkg in sorted(os.listdir(doc_root)):
                pd = os.path.join(doc_root, pkg)
                if not os.path.isdir(pd):
                    continue
                for fn in sorted(os.listdir(pd)):
                    if fn.startswith("copyright") or fn.startswith("LICENSE") or fn.startswith("license"):
                        docs.append((pkg, os.path.join(pd, fn), "share/doc/" + pkg + "/" + fn))
        gdir = os.path.join(r, "share", "LICENSES")
        if os.path.isdir(gdir):
            for fn in sorted(os.listdir(gdir)):
                lic_dir[fn] = os.path.join(gdir, fn)
    # 断链(目标在别的包里,例如 termux-licenses)在盘上**没有文件**,os.listdir 看不到它们;
    # 不补这一步,openjdk/libiconv/alsa-lib 的许可就会被"安静地漏掉"——这正是最不能接受的那种错。
    seen = set(os.path.normcase(os.path.abspath(p)) for _pk, p, _rel in docs)
    doc_roots = [os.path.normcase(os.path.abspath(os.path.join(r, "share", "doc"))) for r in roots]
    for orig, _target in SYMLINK_LINKS:
        norm = os.path.normcase(orig)
        if norm in seen:
            continue
        for dr in doc_roots:
            if not (norm.startswith(dr + os.sep)):
                continue
            fn = os.path.basename(orig)
            if not (fn.startswith("copyright") or fn.startswith("LICENSE") or fn.startswith("license")):
                continue
            pkg = os.path.basename(os.path.dirname(orig))
            docs.append((pkg, orig, "share/doc/" + pkg + "/" + fn))
            seen.add(norm)
            break
    return docs, lic_dir


def copy_notice_tree(component_dir, home, roots, provenance, packages_info, args):
    """写 NOTICE/:许可全文 + 版权头 + 源码获取方式。返回文件清单(相对 NOTICE/ 的路径)。"""
    notice = os.path.join(component_dir, "NOTICE")
    if os.path.isdir(notice):
        shutil.rmtree(notice)
    os.makedirs(notice)
    written = []

    docs, lic_dir = collect_license_docs(roots)

    # 1) JDK 自带的 legal/(权威:OpenJDK 的 GPLv2 + Assembly/Classpath 例外)
    legal_src = os.path.join(home, "legal")
    legal_missing = []
    if os.path.isdir(legal_src):
        for rel, abspath, is_dir, is_link in walk_tree(legal_src):
            target = os.path.join(notice, "jdk-legal", rel)
            if is_dir:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            try:
                if is_link:
                    real = os.path.realpath(abspath)
                    if os.path.isfile(real):
                        shutil.copyfile(real, target)   # 链接目标也一并落地(传上去不会断链)
                    else:
                        legal_missing.append(rel + " -> " + to_rel(os.readlink(abspath)))
                        continue
                else:
                    shutil.copyfile(abspath, target)
                written.append("jdk-legal/" + rel)
            except OSError as e:
                legal_missing.append(rel + " (" + str(e) + ")")
    key_files = ["legal/java.base/LICENSE", "legal/java.base/ASSEMBLY_EXCEPTION"]
    # Java 8 的 JRE/JDK 镜像**没有** legal/(那是 9+ 的布局):GPLv2 全文和例外条款在**顶层**
    # LICENSE / ASSEMBLY_EXCEPTION(第三方声明在 THIRD_PARTY_README)。两种布局都认,认不出来照样报错。
    for cand in ("LICENSE", "ASSEMBLY_EXCEPTION", "THIRD_PARTY_README"):
        if cand not in key_files and os.path.isfile(os.path.join(home, cand)):
            key_files.append(cand)
    have_key = [k for k in key_files if os.path.isfile(os.path.join(home, k))]
    for k in key_files:
        if k in have_key and "/" not in k:      # 顶层那三份也复制到 NOTICE/jdk-legal/
            try:
                os.makedirs(os.path.join(notice, "jdk-legal"), exist_ok=True)
                shutil.copyfile(os.path.join(home, k), os.path.join(notice, "jdk-legal", k))
                written.append("jdk-legal/" + k)
            except OSError as e:
                legal_missing.append(k + " (" + str(e) + ")")

    # 2) 每个被重发布的 Termux 包的许可文本文本(优先 .deb 自带的 share/doc)
    lic_written = []
    for pkg, path, rel in docs:
        base = os.path.basename(rel)
        target_rel = "packages/" + pkg + "/" + base
        target = os.path.join(notice, "packages", pkg, base)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        try:
            hint = hint_get(path)
            islink = os.path.islink(path) or hint is not None
            if islink:
                link = to_rel(hint if hint is not None else os.readlink(path))
                real = os.path.realpath(path)
                if os.path.isfile(real):
                    shutil.copyfile(real, target)
                else:
                    cand = os.path.basename(link)
                    if cand in lic_dir:
                        shutil.copyfile(lic_dir[cand], target)
                    else:
                        # 断链:通用许可文本在 termux-licenses 包里,用户没给
                        with open(target, "w", encoding="utf-8") as f:
                            f.write("（缺原文）这个包只带了一个符号链接:%s -> %s\n" % (rel, link))
                            f.write("termux-licenses 包提供 %s;请把它的 .deb 也一起 --input,"
                                    "或从 termux-packages/packages/termux-licenses/LICENSES/ 取。\n" % link)
                        lic_written.append((pkg, target_rel, "DANGLING:" + link))
                        continue
            else:
                shutil.copyfile(path, target)
                try:
                    if os.path.getsize(target) < 32:
                        lic_written.append((pkg, target_rel, "SUSPICIOUS:只有 %d 字节"
                                            % os.path.getsize(target)))
                        continue
                except OSError:
                    pass
            lic_written.append((pkg, target_rel, "ok"))
        except OSError as e:
            lic_written.append((pkg, target_rel, "ERR:" + str(e)))
    # 2b) termux-licenses 里的通用文本(如果给了)
    for fn, path in sorted(lic_dir.items()):
        target_rel = "licenses/" + fn
        target = os.path.join(notice, target_rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(path, target)
        lic_written.append(("termux-licenses", target_rel, "ok"))

    for _pkg, rel, _st in lic_written:
        written.append(rel)

    # 3) 作者自己的两份:NOTICE.txt(来源/改动/谁优先) 与 SOURCE_OFFER.txt(源码获取方式)
    community = bool(provenance.get("source_url") or provenance.get("source_name"))
    src_url = (provenance.get("TERMUX_PKG_SRCURL") or provenance.get("source_url")
               or "(未提供:必须补)")
    src_sha = (provenance.get("TERMUX_PKG_SHA256") or provenance.get("source_sha256")
               or "(未提供:必须补)")
    src_ver = (provenance.get("TERMUX_PKG_VERSION") or provenance.get("java_version")
               or provenance.get("version") or "(未提供)")
    src_git = provenance.get("release_source_git", "")
    lines = []
    lines.append("SXCL 安卓 JRE 随包说明(NOTICE)")
    lines.append("=" * 60)
    lines.append("")
    lines.append("组件(component) : %s" % args.component)
    lines.append("Java 版本        : %s" % provenance.get("java_version", "(读不到)"))
    lines.append("实现者(vendor)   : %s" % provenance.get("implementor", "(读不到)"))
    lines.append("目标 ABI         : %s" % args.abi)
    lines.append("打包时间戳       : %s" % provenance.get("stamp"))
    lines.append("打包脚本         : android/scripts/pack_jre_from_termux.py(SXCX-C 仓库)")
    lines.append("")
    lines.append("一、这套二进制是从哪来的(可核对的物证)")
    lines.append("-" * 60)
    for p in packages_info:
        lines.append("  * Termux 包 %s %s (%s)" % (p.get("Package", "?"), p.get("Version", "?"),
                                                p.get("Architecture", "?")))
        lines.append("      .deb sha256 = %s" % p.get("deb_sha256", "?"))
        for fn in p.get("files", []):
            lines.append("      解出的文件  = %s" % fn)
    if community:
        lines.append("  * 上游产物(我们真正下载并核过 sha256 的那份字节):")
        lines.append("      名字     = %s" % (provenance.get("source_name") or "(未提供)"))
        lines.append("      URL      = %s" % (provenance.get("source_url") or "(未提供)"))
        lines.append("      sha256   = %s" % (provenance.get("source_sha256") or "(未提供)"))
        lines.append("      拿到日期 = %s" % (provenance.get("fetched_at") or "(未提供)"))
        lines.append("      构建方   = %s" % (provenance.get("source_built_by") or "(未提供)"))
        if provenance.get("source_upstream_source"):
            lines.append("      对应源码 = %s" % provenance["source_upstream_source"])
        if src_git:
            lines.append("      镜像自带 release 文件里的 SOURCE = %s(这份二进制对应的源码提交)" % src_git)
        for n in provenance.get("source_notes", []):
            lines.append("      备注     = %s" % n)
        lines.append("  * 明确一句:这个包**不是** Termux 的 dpkg 构建,也不是我们编的;")
        lines.append("    我们只做了“从上游产物里原样取出 -> 按内容切两个 tar.xz -> 补齐依赖库”这三步(见第三节)。")
    else:
        lines.append("  * 上游 OpenJDK 源码(termux build.sh 里的 TERMUX_PKG_SRCURL):")
        lines.append("      %s" % src_url)
        lines.append("      sha256 = %s" % src_sha)
        lines.append("  * Termux 包版本变量 TERMUX_PKG_VERSION = %s" % src_ver)
        if provenance.get("build_sh"):
            lines.append("  * 来源 build.sh(本机副本,打包时按其内容记录,未联网) = %s" % provenance["build_sh"])
    lines.append("")
    lines.append("二、许可(逐条)")
    lines.append("-" * 60)
    lines.append("  * OpenJDK(本包主体):GPL-2.0 only + OpenJDK Assembly/Classpath 例外。")
    lines.append("      全文在本目录 jdk-legal/java.base/LICENSE(GPLv2)、")
    lines.append("      jdk-legal/java.base/ASSEMBLY_EXCEPTION(例外条款)、")
    lines.append("      jdk-legal/java.base/ADDITIONAL_LICENSE_INFO(混合许可说明);")
    lines.append("      同样的 legal/ 也原样留在 universal.tar.xz 里(没有裁掉,也不许裁)。")
    lines.append("      版权头:每个源文件/每个镜像文件里的版权声明原样保留;本包不做任何二进制修改。")
    lines.append("  * 额外一起重发布的共享库(不是 JDK 镜像的一部分,见本目录 packages/):")
    for pkg, rel, st in lic_written:
        flag = "" if st == "ok" else ("  <-- " + st)
        lines.append("      %s -> %s%s" % (pkg, rel, flag))
    lines.append("  * 我们自己的两个 shim(libawt_xawt.so / libjsound.so)不在这两个包里:")
    lines.append("      它们随 APK 发布(android/jni/jre-libs/,来源与许可见该目录 README.md),")
    lines.append("      由启动器在安装 FINISH 阶段拷进 <%s>/%s。" % (args.component, provenance.get("shim_dir", "lib")))
    lines.append("      谁优先:拷贝是**后写覆盖**,所以同样文件名时我们自己的 shim 一定赢;")
    if community:
        lines.append("      这不是“可选优化”——上游镜像自带的 libawt_xawt.so / libjsound.so 是**桩版**")
        lines.append("      (jre8 那份实测 5,392 B / 5,464 B),功能靠我们这两个 shim 顶;同名文件由我们覆盖。")
    else:
        lines.append("      这不是“可选优化”——Termux 主包不含 libawt_xawt.so(它在 openjdk-*-x 子包里),")
        lines.append("      而它自带的 libjsound.so 链 ALSA(libasound.so.2),我们的包里不带 alsa。")
    lines.append("")
    lines.append("三、我们做了什么改动(repackage,不是 rebuild)")
    lines.append("-" * 60)
    lines.append("  * 二进制**一个字节都没改**(没有 patchelf、没有 strip、没有重链接)。")
    lines.append("  * 只做了三件事:① 按内容把镜像切成 universal/bin-arm64 两个 tar.xz;")
    lines.append("    ② 把上面列的依赖库搬进 <jre>/lib(让 LD_LIBRARY_PATH=<jre>/lib 一个路径够用);")
    lines.append("    ③ 裁掉了 JDK-only 的部分:%s。" % (", ".join(provenance.get("trimmed", [])) or "(没裁)"))
    if not community:
        lines.append("  * 因此二进制里仍留着 Termux 的绝对 RUNPATH(%s),重打包后那是死路径;"
                     % provenance.get("termux_prefix", "/data/data/com.termux/files/usr"))
        lines.append("    运行时必须由启动器设 LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir>(见 docs/18 §4.3)。")
    lines.append("")
    lines.append("四、免责")
    lines.append("-" * 60)
    lines.append("  本包由第三方构建(%s)产出,SXCX-C 只做原样重打包与分发;"
                 % (provenance.get("source_built_by") or "Termux 项目"))
    lines.append("  无任何担保。许可全文见本目录与包内 legal/。")
    lines.append("")
    notice_text = "\n".join(lines) + "\n"
    with open(os.path.join(notice, "NOTICE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(notice_text)
    written.append("NOTICE.txt")

    src_lines = []
    src_lines.append("对应源码的获取方式(SOURCE OFFER)")
    src_lines.append("=" * 60)
    src_lines.append("")
    src_lines.append("本目录/本包分发的是 OpenJDK 的**未经修改的**二进制(Android/aarch64)。")
    src_lines.append("GPLv2 第 3 节要求随二进制一起提供“对应源码”的获取途径,下面就是这份途径")
    src_lines.append("(全部为公开地址,不需要向我们索取):")
    src_lines.append("")
    if community:
        src_lines.append("1) 我们拿到的上游产物(能点到具体文件,sha256 是我们下载后实测的):")
        src_lines.append("     名字     : %s" % (provenance.get("source_name") or "(未提供)"))
        src_lines.append("     URL      : %s" % (provenance.get("source_url") or "(未提供)"))
        src_lines.append("     sha256   : %s" % (provenance.get("source_sha256") or "(未提供)"))
        src_lines.append("     拿到日期 : %s" % (provenance.get("fetched_at") or "(未提供)"))
        src_lines.append("     构建方   : %s" % (provenance.get("source_built_by") or "(未提供)"))
        src_lines.append("")
        src_lines.append("2) 这份二进制对应的 OpenJDK 源码:")
        src_lines.append("     源码仓库 : %s" % (provenance.get("source_upstream_source") or "(未提供)"))
        if src_git:
            src_lines.append("     构建提交 : %s" % src_git)
            src_lines.append("     (取自镜像自带的 release 文件里的 SOURCE 字段 —— 这是构建方编译时用的那棵源码树)")
        src_lines.append("     许可     : GPLv2 + Classpath 例外(全文见本目录 jdk-legal/)")
        src_lines.append("")
    else:
        src_lines.append("1) OpenJDK 源码(本包主体的对应源码,版本 %s):" % src_ver)
        src_lines.append("     URL    : %s" % src_url)
        src_lines.append("     sha256 : %s" % src_sha)
        src_lines.append("   注:这就是 Termux 的 build.sh 里 TERMUX_PKG_SRCURL/TERMUX_PKG_SHA256 指向的那份,")
        src_lines.append("   即本包二进制的构建输入;我们把它原样记录在这里,不代替你下载。")
        src_lines.append("")
        src_lines.append("2) 构建脚本与补丁(Termux 的公开仓库,packages/openjdk-%s/):" %
                         (provenance.get("major", "?")))
        src_lines.append("     https://github.com/termux/termux-packages/tree/master/packages/openjdk-%s" %
                         (provenance.get("major", "?")))
        src_lines.append("   该目录下的 build.sh 与 *.patch 就是“源码 -> 本包二进制”的全部配方。")
        src_lines.append("")
    src_lines.append("3) 我们自己的重打包脚本(把上面产物切成两个 tar.xz 的那一步,不是构建):")
    src_lines.append("     SXCX-C 仓库 android/scripts/pack_jre_from_termux.py(连同本目录的索引可复现)")
    src_lines.append("")
    if community:
        src_lines.append("4) 本包**不含**额外的第三方共享库(依赖闭包已在上游镜像里,实测无缺);")
        src_lines.append("   如果将来补进来,它们的源码地址会逐包写在本目录 packages/<包名>/ 里。")
    else:
        src_lines.append("4) 额外一起分发的共享库的对应源码:见本目录 packages/<包名>/,")
        src_lines.append("   每个包名对应 Termux 仓库 packages/<包名>/ 里的 build.sh(源码地址写在里面)。")
    src_lines.append("")
    src_lines.append("以上地址都是公开的;如果任一地址失效,可以换用同名镜像,或者直接向本项目要")
    src_lines.append("(联系方式见仓库 README)。")
    src_lines.append("")
    with open(os.path.join(notice, "SOURCE_OFFER.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(src_lines) + "\n")
    written.append("SOURCE_OFFER.txt")

    readme = NOTICE_README.replace("<component>", args.component)
    with open(os.path.join(notice, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(readme)
    written.append("README.txt")

    problems = []
    # 两种布局认一种就行:JDK9+ 是 legal/java.base/{LICENSE,ASSEMBLY_EXCEPTION};
    # JDK8 的 JRE/JDK 镜像没有 legal/,GPLv2 全文与例外条款在**顶层** LICENSE / ASSEMBLY_EXCEPTION。
    have_legal = os.path.isfile(os.path.join(home, "legal", "java.base", "LICENSE")) and                  os.path.isfile(os.path.join(home, "legal", "java.base", "ASSEMBLY_EXCEPTION"))
    have_top = os.path.isfile(os.path.join(home, "LICENSE")) and                os.path.isfile(os.path.join(home, "ASSEMBLY_EXCEPTION"))
    if not (have_legal or have_top):
        problems.append("GPLv2 全文/例外条款没找到:既没有 legal/java.base/{LICENSE,ASSEMBLY_EXCEPTION}"
                        "(JDK9+ 布局),也没有顶层 LICENSE + ASSEMBLY_EXCEPTION(JDK8 布局)")
    if legal_missing:
        problems.append("legal/ 里有 %d 个条目没能落地(断链或读不了):%s" %
                        (len(legal_missing), ", ".join(legal_missing[:6])))
    dangling = [t for t in lic_written if t[2] != "ok"]
    if dangling:
        problems.append("有 %d 份许可没拿到原文(多半是 share/doc 里的断链,需要 termux-licenses 包):%s" %
                        (len(dangling), ", ".join(d[1] for d in dangling[:6])))
    if "未提供:必须补" in src_url or "未提供:必须补" in src_sha:
        problems.append("来源 URL/sha256 无从记录:Termux 那条要给 --build-sh,社区/上游那条要给 "
                        "--source-name/--source-url/--source-sha256/--source-built-by/--fetched-at")
    return written, problems


def parse_upload_line(line):
    """从 UPLOAD.txt 的一行里拿 (rel, size, sha16)。**路径里可能有空格**(如 "Public Domain.txt"),
    所以:优先按 TAB 分;旧格式(空格分)就找"第一个纯数字 token = size、紧跟的 16 位 hex = sha16",
    它前面的全部算路径 —— 这样带空格的路径不会被拆错(上一版就是这里把 6 个许可文件误判成 NEW)。"""
    s = line.rstrip("\r\n")
    if not s or s[0] in "#-=" or s.startswith("相对路径"):
        return None
    if "\t" in s:
        f = s.split("\t")
        if len(f) >= 3 and f[1].strip().isdigit() and len(f[2].strip()) == 16:
            return f[0], int(f[1]), f[2].strip()
        return None
    toks = s.split()
    for i in range(len(toks) - 1):
        if toks[i].isdigit() and len(toks[i + 1]) == 16:
            return " ".join(toks[:i]), int(toks[i]), toks[i + 1]
    return None


def parse_upload(path):
    """读上一份 UPLOAD.txt:rel -> (size, sha16)。用来算"本次新增/变更"。"""
    old = {}
    if not os.path.isfile(path):
        return old
    for line in open(path, "r", encoding="utf-8", errors="replace"):
        got = parse_upload_line(line)
        if got:
            old[got[0]] = (got[1], got[2])
    return old


def write_upload_files(out_dir, stamp, url_base, mirror_base, mirror_name):
    """生成 UPLOAD.txt(逐文件 path+size+sha256[0:16],并标出本次新增/变更)与 GIT_UPLOAD.txt(Git 操作清单)。"""
    up_path = os.path.join(out_dir, "UPLOAD.txt")
    prev = parse_upload(up_path)
    rows = []
    total = 0
    for dirpath, dirnames, filenames in os.walk(out_dir):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn in ("UPLOAD.txt", "GIT_UPLOAD.txt"):
                continue
            fp = os.path.join(dirpath, fn)
            rel = to_rel(os.path.relpath(fp, out_dir))
            st = hash_file(fp, want_sha1=False)
            total += st["size"]
            sha16 = st["sha256"][:16]
            if rel not in prev:
                state = "NEW"
            elif prev[rel] != (st["size"], sha16):
                state = "CHG"
            else:
                state = "SAME"
            rows.append((rel, st["size"], sha16, state))
    rows.sort()
    delta = [r for r in rows if r[3] in ("NEW", "CHG")]
    placeholder = ("<repo>" in url_base) or ("<repo>" in mirror_base)

    lines = [
        "SXCX-C 安卓 JRE 上传清单(生成时间 %s)" % stamp,
        "=" * 96,
        "怎么用:把本文件所在的整个目录(%s)的**内容**放到托管仓库的根;" % out_dir,
        "        本地相对路径 == 远端相对路径(index.json 里的 url 就是这么拼的)。",
        "  * 标记:NEW=本次新增 / CHG=内容变了 / SAME=没变(增量上传只需要传 NEW+CHG)",
        "  * 校验:每行 sha256 的前 16 位;完整 sha256+sha1 在 index.json 里(逐文件都有)。",
        "  * url 前缀:%s" % url_base,
        "    备用前缀:%s" % mirror_base,
    ]
    if placeholder:
        lines.append("  * **注意:上面两个前缀里还有 <repo> 占位符**(仓库名还没定)。")
        lines.append("    建好仓库后跑一次:python android/scripts/pack_jre_from_termux.py --retarget-urls "
                     "--out <本目录> --url-base https://raw.githubusercontent.com/<owner>/<repo>/main "
                     "--url-mirror-base https://gh-proxy.com/https://raw.githubusercontent.com/<owner>/<repo>/main")
    lines += ["-" * 96, "相对路径\t字节\tsha256[0:16]\t状态"]
    for rel, size, sha16, state in rows:
        lines.append("%s\t%d\t%s\t%s" % (rel, size, sha16, state))
    lines.append("-" * 96)
    lines.append("合计 %d 个文件 / %s;本次新增或变更 %d 个 / %s"
                 % (len(rows), human(total), len(delta), human(sum(r[1] for r in delta))))
    lines.append("来源与许可:见 <component>/NOTICE/NOTICE.txt 与 SOURCE_OFFER.txt;")
    lines.append("index.json 的 source 字段记着每个输入(.deb 或上游产物)的名字/size/sha256。")
    with open(up_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    # ── GIT_UPLOAD.txt:给用户看的 Git 操作清单(本脚本**不做**任何 git 操作)──
    biggest = max(rows, key=lambda r: r[1]) if rows else ("-", 0, "-", "-")
    example = "jre17/universal.tar.xz" if any(r[0] == "jre17/universal.tar.xz" for r in rows) else (rows[0][0] if rows else "index.json")
    g = []
    g.append("SXCX-C 安卓 JRE —— GitHub 托管操作清单(%s)" % stamp)
    g.append("=" * 96)
    g.append("")
    g.append("1) 放哪:建议**独立仓库**(例:Silent-Studio-CN/sxcl-jre),分支 main。")
    g.append("   理由:这里是 %d 个文件 / %s 的二进制资产,和业务代码无关;塞进已有业务仓库会让 clone/push 变重。" % (len(rows), human(total)))
    g.append("   仓库根的**内容** = 本目录(%s)的内容,即仓库根下就应该是 index.json、UPLOAD.txt、jre17/…" % out_dir)
    g.append("")
    g.append("2) 怎么推(下面四行是给**人**跑的,本脚本不做任何 git 操作):")
    g.append("   git init && git add -A && git commit -m \"jre packages %s\"" % stamp)
    g.append("   git remote add origin https://github.com/<owner>/<repo>.git")
    g.append("   git push -u origin main")
    g.append("   (增量上传:只 add 下面\"本次新增/变更\"的文件即可,不用重传全部)")
    g.append("")
    g.append("3) 单文件上限:GitHub 单文件 **100 MiB**(>50 MiB 会警告)。")
    g.append("   本目录最大的文件是 %s(%s)→ **不需要 Git LFS**。" % (biggest[0], human(biggest[1])))
    g.append("")
    g.append("4) 下载地址怎么拼(两种前缀拼同一段相对路径):")
    g.append("   主:%s/%s" % (url_base, example))
    g.append("   备:%s/%s" % (mirror_base, example))
    g.append("   → %s" % (("**把 <repo> 换成真仓库名后**这两条才是能用的地址" if placeholder else "这两条现在就是可用地址")))
    g.append("")
    g.append("5) **jsDelivr 不能当镜像**:它给 GitHub 端点的单文件上限是 20 MiB(社区/官方 FAQ 口径),")
    g.append("   而 universal.tar.xz 是 31~38 MiB(超了),只有 bin-arm64.tar.xz(4.6~6.4 MiB)在限内 →")
    g.append("   整体镜像不了,别把它写进清单。国内备用就用 gh-proxy 前缀(第 4 条那种)。")
    g.append("")
    g.append("6) 增量上传:UPLOAD.txt 里标了 NEW/CHG/SAME;本次 NEW+CHG 共 %d 个 / %s。" % (len(delta), human(sum(r[1] for r in delta))))
    g.append("")
    with open(os.path.join(out_dir, "GIT_UPLOAD.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(g) + "\n")
    return len(rows), total, delta


def host_block(args):
    """index.json 顶层的托管说明(GitHub raw 主 + gh-proxy 备)。"""
    return {
        "kind": "github-raw",
        "repo": "Silent-Studio-CN/index",
        "branch": "main",
        "dir": "SXCL/jre",
        "url_base": args.url_base,
        "url_mirror": {"name": args.url_mirror_name, "base": args.url_mirror_base},
        "layout": "仓库根下的相对路径 == 本文件里的 path 字段(例如 SXCL/jre/jre17/universal.tar.xz)",
        "note": "主用 raw.githubusercontent.com;国内备用给 gh-proxy 前缀。"
                "jsDelivr **不能**当镜像:它单文件上限 20 MiB,而 universal.tar.xz 是 31~38 MiB。",
    }


def rel_of(entry, comp_name):
    """从条目里拿"仓库内相对路径":优先 path,其次从旧 url 里剥出 /<component>/ 之后那段。"""
    rel = entry.get("path") or ""
    if rel:
        return rel
    u = entry.get("url", "")
    i = u.find("/" + comp_name + "/")
    if i >= 0:
        return u[i + 1:]
    if u.startswith(comp_name + "/"):
        return u
    return ""


def retarget_urls(args, stamp):
    """不重新打包:按新的前缀重写 index.json 里所有条目的 url/url_mirror,并重生成两个清单。"""
    index_path = os.path.join(args.out, "index.json")
    if not os.path.isfile(index_path):
        die(2, "没有 index.json:", index_path)
    with open(index_path, "r", encoding="utf-8") as f:
        idx = json.load(f)
    if idx.get("schema") != SCHEMA:
        die(2, "index.json 的 schema 不是 %s:%r" % (SCHEMA, idx.get("schema")))
    n = 0
    for comp_name, comp in sorted((idx.get("components") or {}).items()):
        if "upstream_artifact" not in comp:
            comp["upstream_artifact"] = None
        entries = list(comp.get("packages", []))
        if comp.get("version_file"):
            entries.append(comp["version_file"])
        entries += list(comp.get("notice", []))
        for ent in entries:
            rel = rel_of(ent, comp_name)
            if not rel:
                warn("条目既没有 path 也推不出相对路径,跳过:", ent.get("name"))
                continue
            ent["path"] = rel
            ent["url"] = args.url_base + "/" + rel
            ent["url_mirror"] = args.url_mirror_base + "/" + rel
            n += 1
    idx["host"] = host_block(args)
    idx["generated"] = stamp
    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(idx, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    files, total, delta = write_upload_files(args.out, stamp, args.url_base,
                                            args.url_mirror_base, args.url_mirror_name)
    out("[host] url 前缀   -> %s" % args.url_base)
    out("[host] 备用前缀   -> %s" % args.url_mirror_base)
    out("[host] 重写 %d 个条目的 url;UPLOAD.txt %d 个文件 / %s;本次新增/变更 %d 个 / %s"
        % (n, files, human(total), len(delta), human(sum(r[1] for r in delta))))
    for rel, size, _s, state in delta:
        out("[host]   %-4s %-52s %12d" % (state, rel, size))
    return 0


# ──────────────────────── 主流程 ────────────────────────

ARGS = None


def main(argv):
    global ARGS
    ap = argparse.ArgumentParser(add_help=True, description="把 Termux OpenJDK 打成 SXCL 的 jreN/*.tar.xz 形态(离线)")
    ap.add_argument("--input", action="append", default=[],
                    help=".deb 文件 或 已解出的 usr/ 目录(可重复;依赖包也要给,否则闭包不闭合)")
    ap.add_argument("--out", required=True, help="输出根目录(生成 <out>/<component>/... 与 <out>/index.json)")
    ap.add_argument("--component", default="", help="组件名,如 jre17(默认按 release 的主版本推 jreN)")
    ap.add_argument("--abi", default="arm64-v8a", help="目标 ABI(默认 arm64-v8a)")
    ap.add_argument("--jdk-home", default="", help="直接指定 JDK home(不猜;多个候选时必须给)")
    ap.add_argument("--build-sh", default="", help="termux-packages 的 packages/openjdk-*/build.sh(记录来源,不联网)")
    ap.add_argument("--extra-lib-dir", action="append", default=[], help="额外的 .so 搜索目录(补闭包用)")
    ap.add_argument("--stamp", default="", help="写进 index.json/version 的时间戳(默认当前 UTC;可重复性测试时固定它)")
    ap.add_argument("--mtime", type=int, default=0, help="tar 里所有条目的 mtime(默认 0 = 可重复)")
    ap.add_argument("--xz-preset", type=int, default=6, help="xz 预设(0-9;6 默认,>6 会更慢)")
    ap.add_argument("--keep-all", action="store_true", help="不裁 JDK-only 部分(默认裁 jmods/include/man/demo/sample/lib/src.zip)")
    ap.add_argument("--dry-run", action="store_true", help="只打印切分计划,不写任何文件")
    ap.add_argument("--allow-unresolved", action="store_true", help="闭包里有缺库也继续(默认非 0 退出)")
    ap.add_argument("--allow-incomplete-notice", action="store_true", help="NOTICE 不完整也继续(默认非 0 退出)")
    ap.add_argument("--allow-foreign-arch", action="store_true", help="release 里不是 aarch64 也继续(默认拒绝)")
    ap.add_argument("--no-verify", action="store_true", help="不做打包后的自检(默认做)")
    # ── 托管(GitHub;仓库名没定之前先留 <repo> 占位,拿到名字后用 --retarget-urls 一次性替换)──
    ap.add_argument("--url-base", default="https://raw.githubusercontent.com/Silent-Studio-CN/<repo>/main",
                    help="index.json 里 url 的前缀(仓库根的**内容** = 本 out 目录的内容)")
    ap.add_argument("--url-mirror-base",
                    default="https://gh-proxy.com/https://raw.githubusercontent.com/Silent-Studio-CN/<repo>/main",
                    help="备用(国内)前缀;与主前缀拼同一段相对路径")
    ap.add_argument("--url-mirror-name", default="gh-proxy", help="备用前缀的名字(只写进 index.json 当说明)")
    ap.add_argument("--retarget-urls", action="store_true",
                    help="不打包:只按 --url-base/--url-mirror-base 重写 index.json 里的 url,并重生成 UPLOAD.txt/GIT_UPLOAD.txt")
    # ── 非 Termux 来源(社区/上游产物)时的来源链:必须写清,不许含糊 ──
    ap.add_argument("--source-name", default="", help="上游产物名字(如 PojavLauncher.apk / FCL 的 jre8 资产)")
    ap.add_argument("--source-url", default="", help="上游产物来源 URL(带 tag/commit,能点到具体文件)")
    ap.add_argument("--source-sha256", default="", help="我们实际拿到的那个产物文件的 sha256")
    ap.add_argument("--source-built-by", default="", help="构建方(谁编的这个 JRE)")
    ap.add_argument("--source-upstream-source", default="", help="构建方用的 OpenJDK 源码仓库(对应源码)")
    ap.add_argument("--fetched-at", default="", help="拿到它的日期(YYYY-MM-DD)")
    ap.add_argument("--source-note", action="append", default=[], help="来源链补充说明(可重复;逐条写进 NOTICE)")
    ap.add_argument("--accept-shim-dir", action="store_true",
                    help="接受 shim_dir 与 launcher 现有规则不一致(会写进 index/NOTICE,不静默放过)")
    ARGS = ap.parse_args(argv)

    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass

    if ARGS.xz_preset < 0 or ARGS.xz_preset > 9:
        die(2, "--xz-preset 要在 0..9 之间")
    stamp = ARGS.stamp or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    if ARGS.retarget_urls:                     # 只改 url + 重生成清单,不碰包
        return retarget_urls(ARGS, stamp)
    if not ARGS.input:
        die(2, "没有 --input(要打包就给 .deb/usr 目录;只想改 url 就加 --retarget-urls)")

    # ── 1) 收集输入树(解 .deb / 直接用目录),全部只读 ──
    tmpdirs = []
    atexit.register(lambda: [shutil.rmtree(d, ignore_errors=True) for d in tmpdirs])
    roots = []           # 每个元素:(root_path, 描述)
    deb_infos = []       # .deb 的 control 与 sha256
    for inp in ARGS.input:
        if not os.path.exists(inp):
            die(2, "输入不存在:", inp)
        if os.path.isdir(inp):
            hits = find_prefix_roots(inp)
            if not hits:
                roots.append((os.path.abspath(inp), "目录(直接当 usr/ 用)"))
            else:
                for h in hits:
                    roots.append((h, "目录 " + h))
        elif inp.lower().endswith(".deb"):
            ctl = deb_control(inp)
            info = {"Package": ctl.get("Package"), "Version": ctl.get("Version"),
                    "Architecture": ctl.get("Architecture"), "Depends": ctl.get("Depends"),
                    "deb": os.path.abspath(inp)}
            info.update(hash_file(inp))
            stage = tempfile.mkdtemp(prefix="sxcl-jre-deb-")
            tmpdirs.append(stage)
            member = extract_deb(inp, stage)
            info["data_member"] = member
            outs = find_prefix_roots(stage)
            if not outs:
                die(2, "解开的 .deb 里找不到 usr/ 前缀根:", inp)
            for h in outs:
                roots.append((h, "deb %s %s" % (ctl.get("Package", "?"), ctl.get("Version", "?"))))
            deb_infos.append(info)
            out("[in ] %s %s (%s) -> %s" % (ctl.get("Package", "?"), ctl.get("Version", "?"),
                                            human(info["size"]), h))
        else:
            die(2, "不认识的输入(只认 .deb 或目录):", inp)

    out("[in ] 前缀根 %d 个" % len(roots))

    # ── 2) 找 JDK home(不猜:多个候选就要 --jdk-home) ──
    if ARGS.jdk_home:
        home = os.path.abspath(ARGS.jdk_home)
        if not looks_like_jdk_home(home):
            die(2, "--jdk-home 不像 JDK home(需要 release + bin/java 或 lib/server/libjvm.so):", home)
        homes = [home]
    else:
        homes = []
        for r, _desc in roots:
            for h in find_jdk_homes([r]):
                if h not in homes:
                    homes.append(h)
    if not homes:
        die(2, "没找到 JDK home(release + bin/java)。你的输入里没有 openjdk-* 包?")
    if len(homes) > 1:
        # 给了 --component 就按主版本挑(一次跑三个组件时省事);挑不唯一照样报错不猜。
        want = 0
        mm = re.match(r"^jre(\d+)$", ARGS.component or "")
        if mm:
            want = int(mm.group(1))
        if want:
            picked = []
            for h in homes:
                rel = parse_release(os.path.join(h, "release"))
                if major_of(rel.get("JAVA_VERSION", "")) == want:
                    picked.append(h)
            if len(picked) == 1:
                homes = picked
            else:
                die(2, "--component %s 没能在 %d 个 JDK home 里挑出唯一一个(挑出 %d 个):\n  %s"
                    % (ARGS.component, len(homes), len(picked), "\n  ".join(homes)))
        else:
            die(2, "找到多个 JDK home,不猜:请给 --component jreN 或 --jdk-home:\n  " + "\n  ".join(homes))
    home = homes[0]

    release = parse_release(os.path.join(home, "release"))
    java_version = release.get("JAVA_VERSION", "")
    os_arch = release.get("OS_ARCH", "")
    implementor = release.get("IMPLEMENTOR", "")
    if os_arch and os_arch.lower() not in ("aarch64", "arm64") and not ARGS.allow_foreign_arch:
        die(2, "release 里 OS_ARCH=%s,不是 aarch64/arm64(要打别的架构请显式 --allow-foreign-arch)" % os_arch)
    major = major_of(java_version)
    if not ARGS.component:
        if not major:
            die(2, "release 里读不出 JAVA_VERSION,请显式给 --component")
        ARGS.component = "jre%d" % major
    component = ARGS.component
    major = major or 0
    if not re.match(r"^jre\d+$", component):
        warn("组件名不是 jreN 形状:", component)

    prov = parse_build_sh(ARGS.build_sh) if ARGS.build_sh else {}
    if release.get("SOURCE"):
        prov["release_source_git"] = release["SOURCE"]     # 例如 ".:git:1a6e3a5ea32d+"
    out("[jdk] home=%s" % home)
    out("[jdk] JAVA_VERSION=%s OS_ARCH=%s IMPLEMENTOR=%s major=%d component=%s"
        % (java_version or "?", os_arch or "?", implementor or "?", major, component))

    # ── 3) 逐条分类 ──
    trim = [] if ARGS.keep_all else list(TRIM_DEFAULT)
    jdk_entries = walk_tree(home)
    plan = []          # (rel, abspath, is_dir, is_link, exec_hint, bucket, reason, size)
    skipped = []
    machine_seen = {}
    for rel, abspath, is_dir, is_link in jdk_entries:
        if is_dir:
            bucket, reason = classify(rel, abspath, None, trim)
            if bucket == "skip":
                skipped.append((rel, reason))
                continue
            plan.append((rel, abspath, True, False, True, bucket, reason, 0))
            continue
        if is_link:
            real = os.path.realpath(abspath)
            elf = elf_info(real) if os.path.isfile(real) else None
            size = os.path.getsize(real) if os.path.isfile(real) else 0
        else:
            elf = elf_info(abspath) if not is_dir else None
            size = os.path.getsize(abspath)
        bucket, reason = classify(rel, abspath, elf, trim)
        if bucket == "skip":
            skipped.append((rel, reason))
            continue
        if elf:
            machine_seen[elf["machine"]] = machine_seen.get(elf["machine"], 0) + 1
        exec_hint = bucket == "bin"
        plan.append((rel, abspath, False, is_link, exec_hint, bucket, reason, size))

    # ── 4) NEEDED 闭包 ──
    needed = {}
    for rel, abspath, is_dir, is_link, _ex, _b, _r, _s in plan:
        if is_dir:
            continue
        real = os.path.realpath(abspath) if is_link else abspath
        info = elf_info(real)
        if not info:
            continue
        for n in info["needed"]:
            needed.setdefault(n, []).append(rel)

    have = set()
    for rel, abspath, is_dir, is_link, _ex, _b, _r, _s in plan:
        if not is_dir:
            have.add(os.path.basename(rel))
    soname_index = {}
    for r, _desc in roots:
        for sub in ("lib", "lib64"):
            d = os.path.join(r, sub)
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                p = os.path.join(d, fn)
                if not os.path.isfile(p):
                    continue
                soname_index.setdefault(fn, (p, r))
                si = elf_info(p)
                if si and si.get("soname"):
                    soname_index.setdefault(si["soname"], (p, r))
    for d in ARGS.extra_lib_dir:
        if not os.path.isdir(d):
            die(2, "--extra-lib-dir 不存在:", d)
        for fn in sorted(os.listdir(d)):
            p = os.path.join(d, fn)
            if os.path.isfile(p):
                soname_index.setdefault(fn, (p, d))

    extras = []       # (soname, src, root, new_rel)
    unresolved = []
    for name in sorted(needed):
        if name in have or name in SYSTEM_LIBS:
            continue
        hit = soname_index.get(name)
        if hit is None:
            unresolved.append((name, needed[name]))
            continue
        src, root = hit
        new_rel = "lib/" + name
        if any(e[3] == new_rel for e in extras):
            continue
        extras.append((name, src, root, new_rel))

    # ── 5) 打印计划 ──
    total_u = sum(s for (_r, _a, d, _l, _e, b, _rr, s) in plan if not d and b == "universal")
    total_b = sum(s for (_r, _a, d, _l, _e, b, _rr, s) in plan if not d and b == "bin")
    n_u = sum(1 for (_r, _a, d, _l, _e, b, _rr, _s) in plan if not d and b == "universal")
    n_b = sum(1 for (_r, _a, d, _l, _e, b, _rr, _s) in plan if not d and b == "bin")
    out("[plan] universal: %d 文件 / %s" % (n_u, human(total_u)))
    out("[plan] bin-arm64: %d 文件 / %s(含 %d 个额外依赖库)"
        % (n_b + len(extras), human(total_b + sum(os.path.getsize(e[1]) for e in extras)), len(extras)))
    if skipped:
        out("[plan] 裁掉 %d 项:%s" % (len(skipped), ", ".join(sorted(set(r for _p, r in skipped)))))
    if machine_seen:
        out("[plan] ELF 机器分布:%s" % machine_seen)
    for name, src, root, new_rel in extras:
        out("[dep ] %-28s <- %s" % (name, src))
        if not new_rel:
            continue
    if unresolved:
        for name, users in unresolved:
            warn("闭包里缺 %s(被 %s 需要)" % (name, ", ".join(sorted(set(users))[:4])))
    if ARGS.dry_run:
        out("[dry ] --dry-run:没有写任何文件。")
        return 0 if (not unresolved or ARGS.allow_unresolved) else 4

    # ── 6) 落盘:两个 tar.xz + version + NOTICE + index.json ──
    component_dir = os.path.join(ARGS.out, component)
    if os.path.isdir(component_dir):
        for fn in ("universal.tar.xz", "bin-arm64.tar.xz", "version"):
            p = os.path.join(component_dir, fn)
            if os.path.isfile(p):
                os.remove(p)
    os.makedirs(component_dir, exist_ok=True)
    abi_tag = "arm64" if ARGS.abi.startswith("arm64") else ARGS.abi
    u_tar = os.path.join(component_dir, "universal.tar.xz")
    b_tar = os.path.join(component_dir, "bin-%s.tar.xz" % abi_tag)

    u_entries, b_entries = [], []
    for rel, abspath, is_dir, is_link, exec_hint, bucket, _r, _s in plan:
        item = (rel, abspath, is_dir, is_link, exec_hint)
        if bucket == "universal":
            u_entries.append(item)
        else:
            b_entries.append(item)
    for _name, src, _root, new_rel in extras:
        b_entries.append((new_rel, src, False, False, True))
    u_entries.sort(key=lambda t: t[0])
    b_entries.sort(key=lambda t: t[0])

    out("[pack] %s ..." % u_tar)
    build_archive(u_entries, u_tar, ARGS.xz_preset)
    out("[pack] %s ..." % b_tar)
    build_archive(b_entries, b_tar, ARGS.xz_preset)

    u_stat = hash_file(u_tar)
    b_stat = hash_file(b_tar)

    shim_dir = shim_dir_for(home)
    prov.update({"java_version": java_version, "implementor": implementor, "major": major,
                 "stamp": stamp, "trimmed": trim, "shim_dir": shim_dir,
                 "termux_prefix": "/data/data/com.termux/files/usr",
                 "source_name": ARGS.source_name, "source_url": ARGS.source_url,
                 "source_sha256": ARGS.source_sha256, "source_built_by": ARGS.source_built_by,
                 "source_upstream_source": ARGS.source_upstream_source,
                 "fetched_at": ARGS.fetched_at, "source_notes": list(ARGS.source_note)})

    id_line = "%s/%s/%s/%s" % (component, java_version or "?", ARGS.abi,
                               hashlib.sha256((u_stat["sha256"] + "\n" + b_stat["sha256"]).encode()).hexdigest()[:16])
    ver_lines = [
        # 第一行**就是**"要不要重装"的键:客户端读一行就能跟 index.json 的 id 对,
        # 不用先解析整个文件。(原来注释在第一行,于是 index.json 里记的 first_line 跟文件对不上 ——
        # 独立校验脚本 verify_jre_index.py 一把抓住,见 docs/19 §8.1。)
        "id=%s" % id_line,
        "component=%s" % component,
        "version=%s" % (java_version or ""),
        "abi=%s" % ARGS.abi,
        "shim_dir=%s" % shim_dir,
        "trimmed=%s" % ",".join(trim),
        "universal.size=%d" % u_stat["size"],
        "universal.sha256=%s" % u_stat["sha256"],
        "bin-%s.size=%d" % (abi_tag, b_stat["size"]),
        "bin-%s.sha256=%s" % (abi_tag, b_stat["sha256"]),
        "# sxcl jre package marker - id 由上面两个包的 sha256 派生(内容派生,不是手改的计数器)",
    ]
    ver_path = os.path.join(component_dir, "version")
    ver_bytes = ("\n".join(ver_lines) + "\n").encode("utf-8")
    with open(ver_path, "wb") as f:
        f.write(ver_bytes)
    ver_stat = hash_bytes(ver_bytes)
    out("[mark] %s  id=%s (shim 目标目录 = %s)" % (ver_path, id_line, shim_dir))

    notice_files, notice_problems = copy_notice_tree(component_dir, home, [r for r, _d in roots],
                                                     prov, deb_infos, ARGS)
    if major and major <= 8 and shim_dir not in ("jre/lib", "lib"):
        msg = ("这份镜像是 jre8 形状(shim 的目标目录 = %s),而 launcher 现在对 jre8 只认 "
               "<home>/jre/lib 与 <home>/lib;要么同步改 launcher,要么换镜像" % shim_dir)
        if ARGS.accept_shim_dir:
            warn(msg + " —— 已按 --accept-shim-dir 接受(写进 index.json 的 shim_dir,不静默)")
        else:
            notice_problems.append(msg)
    out("[note] NOTICE/ %d 个文件(问题 %d 条)" % (len(notice_files), len(notice_problems)))
    for p in notice_problems:
        warn("NOTICE: " + p)

    u_man = archive_manifest(u_tar)
    b_man = archive_manifest(b_tar)
    manifest = {}
    exec_map = {rel: ex for rel, _a, d, _l, ex, _b, _r, _s in plan if not d}
    origin_map = {}
    for _name, src, root, new_rel in extras:
        origin_map[new_rel] = "dep:" + os.path.basename(root)
    for name, item in u_man.items():
        item = dict(item)
        item["package"] = "universal.tar.xz"
        if item.get("type") == "file":
            item["executable"] = exec_map.get(name, False)
        manifest[name] = item
    for name, item in b_man.items():
        item = dict(item)
        item["package"] = "bin-%s.tar.xz" % abi_tag
        if name in origin_map:
            item["origin"] = origin_map[name]
        manifest[name] = item

    # 自检:重读归档算出来的 manifest 必须与写进去的一致(此处就是同一次读取,真校验看 --verify)
    self_check = "ok"
    if not ARGS.no_verify:
        bad = []
        for name, item in manifest.items():
            if item.get("type") != "file":
                continue
            src_rel = name
            if item["package"].startswith("bin-"):
                hit = [e for e in b_entries if e[0] == src_rel]
            else:
                hit = [e for e in u_entries if e[0] == src_rel]
            if len(hit) == 1 and not hit[0][3]:
                st = hash_file(hit[0][1])
                if st["sha256"] != item["sha256"] or st["size"] != item["size"]:
                    bad.append(name)
        if bad:
            self_check = "FAILED:" + ",".join(bad[:5])
        out("[self] 逐文件复算 sha256:%s(%d 个文件)" % (self_check, sum(1 for i in manifest.values() if i.get("type") == "file")))

    index_path = os.path.join(ARGS.out, "index.json")
    index = {"schema": SCHEMA, "generated": stamp, "abi": ARGS.abi,
             "host": host_block(ARGS),
             "components": {}}
    if os.path.isfile(index_path):
        try:
            with open(index_path, "r", encoding="utf-8") as f:
                old = json.load(f)
            if isinstance(old, dict) and old.get("schema") == SCHEMA and isinstance(old.get("components"), dict):
                index["components"] = old["components"]
            else:
                warn("旧 index.json 的 schema 不是 %s,整份重建(旧的不动,已备份)" % SCHEMA)
                shutil.copyfile(index_path, index_path + ".bak")
        except (OSError, ValueError) as e:
            warn("旧 index.json 读不了(%s),整份重建" % e)

    def pkg_entry(name, url_rel, stat_d):
        d = dict(stat_d)
        d["name"] = name
        d["path"] = url_rel                       # 仓库内相对路径(远端根 = 本 out 目录的内容)
        d["url"] = ARGS.url_base + "/" + url_rel
        d["url_mirror"] = ARGS.url_mirror_base + "/" + url_rel
        return d

    notice_entries = []
    for rel in sorted(notice_files):
        p = os.path.join(component_dir, "NOTICE", rel)
        if os.path.isfile(p):
            st = hash_file(p)
            notice_entries.append(pkg_entry(rel, "%s/NOTICE/%s" % (component, rel), st))

    comp = {
        "component": component,
        "major": major,
        "version": java_version,
        "java_version": java_version,
        "implementor": implementor,
        "os_arch": os_arch,
        "abi": ARGS.abi,
        "shim_dir": shim_dir,
        "id": id_line,
        "file_count": sum(1 for i in manifest.values() if i.get("type") == "file"),
        "installed_bytes": sum(i.get("size", 0) for i in manifest.values() if i.get("type") == "file"),
        "trimmed": trim,
        "source": {
            "kind": "termux-deb",
            "packages": [{"name": p.get("Package"), "version": p.get("Version"),
                          "architecture": p.get("Architecture"),
                          "deb": os.path.basename(p.get("deb", "")),
                          "size": p.get("size"), "sha256": p.get("sha256"),
                          "sha1": p.get("sha1")} for p in deb_infos],
            "build_script": {"path": os.path.basename(ARGS.build_sh) if ARGS.build_sh else "",
                             "src_url": prov.get("TERMUX_PKG_SRCURL", ""),
                             "src_sha256": prov.get("TERMUX_PKG_SHA256", ""),
                             "pkg_version": prov.get("TERMUX_PKG_VERSION", ""),
                             "license": prov.get("TERMUX_PKG_LICENSE", "")},
            "modified": False,
            "extra_libs": [{"soname": n, "from": os.path.basename(r), "path": new_rel}
                           for n, _s, r, new_rel in extras],
        },
        "upstream_artifact": ({
            "name": ARGS.source_name, "url": ARGS.source_url, "sha256": ARGS.source_sha256,
            "fetched_at": ARGS.fetched_at, "built_by": ARGS.source_built_by,
            "corresponding_source": ARGS.source_upstream_source, "notes": list(ARGS.source_note),
        } if (ARGS.source_url or ARGS.source_name) else None),
        "packages": [
            pkg_entry("universal.tar.xz", "%s/universal.tar.xz" % component, u_stat),
            pkg_entry("bin-%s.tar.xz" % abi_tag, "%s/bin-%s.tar.xz" % (component, abi_tag), b_stat),
        ],
        "version_file": dict(pkg_entry("version", "%s/version" % component, ver_stat),
                             first_line="id=" + id_line),
        "notice": notice_entries,
        "manifest": {"files": {k: manifest[k] for k in sorted(manifest)}},
    }
    if self_check != "ok":
        comp["self_check"] = self_check
    index["components"][component] = comp
    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    out("[idx ] %s(组件 %d 个:%s)" % (index_path, len(index["components"]),
                                     ", ".join(sorted(index["components"]))))

    # ── 6b) 上传清单 + Git 操作清单(增量用 NEW/CHG 标出来)──
    up_files, up_total, delta = write_upload_files(ARGS.out, stamp, ARGS.url_base,
                                                  ARGS.url_mirror_base, ARGS.url_mirror_name)
    out("[upl ] %s(%d 个文件 / %s;本次新增/变更 %d 个 / %s)"
        % (os.path.join(ARGS.out, "UPLOAD.txt"), up_files, human(up_total),
           len(delta), human(sum(r[1] for r in delta))))
    for rel, size, _s, state in delta:
        out("[upl ]   %-4s %-52s %12d" % (state, rel, size))

    # ── 7) 汇总 + 退出码 ──
    out("")
    out("[done] %s:universal.tar.xz %s / bin-%s.tar.xz %s"
        % (component, human(u_stat["size"]), abi_tag, human(b_stat["size"])))
    out("        sha256 %s" % u_stat["sha256"])
    out("        sha256 %s" % b_stat["sha256"])
    out("        NOTICE %d 个文件,index.json 已更新" % len(notice_entries))

    if self_check != "ok":
        die(5, "自检不过:", self_check)
    if unresolved and not ARGS.allow_unresolved:
        die(4, "动态库闭包缺 %d 个(用 --allow-unresolved 才能放过):%s"
            % (len(unresolved), ", ".join(n for n, _u in unresolved)))
    if notice_problems and not ARGS.allow_incomplete_notice:
        die(3, "NOTICE 不完整(用 --allow-incomplete-notice 才能放过),共 %d 条" % len(notice_problems))
    out("[ok  ] 打包完成,没有未决问题。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
