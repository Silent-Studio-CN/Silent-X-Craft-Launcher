#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
"""generate.py - 重新生成 tests/fixtures/xz/ 下的夹具(签入仓库的真 .xz 文件)。

为什么签入而不是测试时生成:用例必须**不依赖 Python**、不联网、可离线复现。
这个脚本只是"这些字节是怎么来的"的可追溯记录;平时不需要跑。

  python tests/fixtures/xz/generate.py        # 会在本目录覆盖写夹具

夹具与用途见 tests/xz_test.c 的文件头。
"""

import hashlib
import io
import lzma
import os
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))


def make_tar(entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for name, data, mode in entries:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            ti.mtime = 0
            ti.uid = 0
            ti.gid = 0
            ti.uname = ""
            ti.gname = ""
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def lcg(n, seed=12345):
    """确定性"看着随机"的字节(LCG):用来逼 xz 走未压缩块那条路。"""
    out = bytearray()
    x = seed
    for _ in range(n):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def w(name, data, **kw):
    comp = lzma.compress(data, format=lzma.FORMAT_XZ, **kw)
    with open(os.path.join(HERE, name), "wb") as fh:
        fh.write(comp)
    print("%-24s xz=%-8d raw=%-8d sha256=%s" % (name, len(comp), len(data),
                                                hashlib.sha256(data).hexdigest()))


def main():
    tar = make_tar([
        ("bin/java", b"#!/bin/sh\n" + bytes(range(256)) * 4, 0o755),
        ("lib/libjli.so", b"\x7fELF" + b"JLI" * 5000, 0o755),
        ("lib/server/libjvm.so", b"\x7fELF" + bytes(range(256)) * 300, 0o755),
        ("release", b'JAVA_VERSION="17.0.9"\nMODULES="java.base"\n', 0o644),
        ("conf/security/java.security", b"# policy\n" + b"x" * 900, 0o644),
    ])
    with open(os.path.join(HERE, "jre-tiny.tar"), "wb") as fh:
        fh.write(tar)
    repeat = (b"SXCL-JRE-PAYLOAD-0123456789" * 12000)[:300000]
    w("jre-tiny-crc64.xz", tar, check=lzma.CHECK_CRC64, preset=6)
    w("jre-tiny-crc32.xz", tar, check=lzma.CHECK_CRC32, preset=6)
    w("jre-tiny-none.xz", tar, check=lzma.CHECK_NONE, preset=6)
    w("jre-tiny-uncomp.xz", tar, check=lzma.CHECK_CRC64, preset=0)
    w("repeat.xz", repeat, check=lzma.CHECK_CRC64, preset=6)
    w("multiblock.xz", repeat * 24, check=lzma.CHECK_CRC64, preset=1)
    w("bigdict.xz", tar, check=lzma.CHECK_CRC64, preset=9)
    w("rand-uncomp.xz", lcg(100000), check=lzma.CHECK_CRC64, preset=0)
    a = lzma.compress(tar, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=6)
    b = lzma.compress(repeat[:50000], format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, preset=6)
    with open(os.path.join(HERE, "concat.xz"), "wb") as fh:
        fh.write(a + b"\x00\x00\x00\x00" + b)
    bcj = lzma.compress(tar, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64,
                        filters=[{"id": lzma.FILTER_X86},
                                 {"id": lzma.FILTER_LZMA2, "preset": 6}])
    with open(os.path.join(HERE, "bcj-x86.xz"), "wb") as fh:
        fh.write(bcj)
    bad = bytearray(a)
    bad[40] ^= 0x5A          # 压缩数据里翻一个字节(算术流发散)
    with open(os.path.join(HERE, "corrupt.xz"), "wb") as fh:
        fh.write(bytes(bad))
    bad2 = bytearray(a)
    bad2[641] ^= 0xFF        # 块校验字段(CRC64)里翻一个字节
    with open(os.path.join(HERE, "badcheck.xz"), "wb") as fh:
        fh.write(bytes(bad2))
    with open(os.path.join(HERE, "truncated.xz"), "wb") as fh:
        fh.write(a[:len(a) // 2])
    print("done ->", HERE)


if __name__ == "__main__":
    main()
