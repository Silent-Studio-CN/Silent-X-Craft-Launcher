#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
"""generate.py - 重新生成 tests/fixtures/tar/ 下的夹具(签入仓库的裸 .tar)。

用例不依赖 Python;这个脚本只是"这些字节是怎么来的"的可追溯记录。
夹具与用途见 tests/tar_test.c 的文件头。
"""

import io
import os
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
LONGNAME = ("modules/java.base/" + ("very-long-component-name-" * 5) +
            "module-info.class")


def build(name, fmt, entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as tf:
        for nm, data, mode in entries:
            ti = tarfile.TarInfo(nm)
            ti.size = len(data)
            ti.mode = mode
            ti.mtime = 0
            ti.uid = 0
            ti.gid = 0
            ti.uname = ""
            ti.gname = ""
            tf.addfile(ti, io.BytesIO(data))
    with open(os.path.join(HERE, name), "wb") as fh:
        fh.write(buf.getvalue())
    print("%-24s %d" % (name, len(buf.getvalue())))


def hdr(name, size, typeflag=b"0", mode=0o644):
    h = bytearray(512)

    def put(off, s):
        b = s.encode() if isinstance(s, str) else s
        h[off:off + len(b)] = b

    put(0, name)
    put(100, ("%07o\0" % mode).encode())
    put(108, b"0000000\0")
    put(116, b"0000000\0")
    put(124, ("%011o\0" % size).encode())
    put(136, b"00000000000\0")
    h[148:156] = b" " * 8
    h[156] = typeflag[0]
    put(257, b"ustar\x00")
    put(263, b"00")
    h[148:156] = ("%06o\0 " % sum(h)).encode()
    return bytes(h)


def main():
    build("pax-long.tar", tarfile.PAX_FORMAT,
          [(LONGNAME, b"PAXDATA" * 100, 0o644), ("short.txt", b"hello\n", 0o644)])
    build("ustar-prefix.tar", tarfile.USTAR_FORMAT,
          [("release-notes/" + "x" * 80 + "/README.txt", b"prefix-split\n", 0o644)])
    build("gnu-long.tar", tarfile.GNU_FORMAT, [(LONGNAME, b"GNUDATA" * 50, 0o755)])
    # 链接 + 目录 + 可执行位
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for nm, data, mode in [("lib/libjli.so", b"\x7fELFJLI", 0o755),
                               ("bin/java", b"#!/bin/sh\n", 0o755)]:
            ti = tarfile.TarInfo(nm)
            ti.size = len(data)
            ti.mode = mode
            ti.mtime = 0
            ti.uid = 0
            ti.gid = 0
            ti.uname = ""
            ti.gname = ""
            tf.addfile(ti, io.BytesIO(data))
        ti = tarfile.TarInfo("lib/libjli-link.so")
        ti.type = tarfile.SYMTYPE
        ti.linkname = "libjli.so"
        ti.mode = 0o777
        ti.mtime = 0
        ti.uid = 0
        ti.gid = 0
        ti.uname = ""
        ti.gname = ""
        tf.addfile(ti)
    with open(os.path.join(HERE, "links.tar"), "wb") as fh:
        fh.write(buf.getvalue())
    print("%-24s %d" % ("links.tar", len(buf.getvalue())))
    # 负例:不安全路径 / 不支持的条目类型
    with open(os.path.join(HERE, "unsafe-dotdot.tar"), "wb") as fh:
        fh.write(hdr("../escape.txt", 6) + b"PWNED\n" + b"\0" * (512 - 6) + b"\0" * 1024)
    with open(os.path.join(HERE, "unsafe-abs.tar"), "wb") as fh:
        fh.write(hdr("/etc/passwd", 6) + b"PWNED\n" + b"\0" * (512 - 6) + b"\0" * 1024)
    with open(os.path.join(HERE, "unsupported-type.tar"), "wb") as fh:
        fh.write(hdr("dev/node", 0, typeflag=b"3") + b"\0" * 1024)
    print("done ->", HERE)


if __name__ == "__main__":
    main()
