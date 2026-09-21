#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""strip_file_headers.py —— 把源文件开头的"大段头注"换成统一版权头。

为什么要有这个脚本(而不是手工改 160 个文件):
  * 手工改必然漏、必然风格不一;脚本可重跑(幂等),新文件也能用同一条规则;
  * 头注里那些设计说明**不丢** —— 全部原文归档到 docs/14-源码头注归档.md,
    再配合 git 历史,信息一条不少。

规则:
  1) 只动**文件最前面**的注释(允许中间夹空行);遇到第一行代码就停。
  2) 头文件若是 "#pragma once" 起头,把它留下,继续吃掉它后面的头注。
  3) 特殊保留(不删):含 clang-format / NOLINT / coding: 的行(工具指令,删了会改行为)。
  4) 输出 = 版权头 + 空行 + [pragma once] + 空行 + 正文(去掉正文前多余空行)。
  5) 行尾风格(CRLF/LF)原样保留;编码 UTF-8。

用法:
  python tools/strip_file_headers.py --dry-run     # 只报告,不写文件
  python tools/strip_file_headers.py               # 真改 + 归档
"""

import argparse
import os
import re

BANNER_C = [
    "/*",
    " * (C) Silent X Craft Launcher",
    " * Copyright by SilentStudio.",
    " * All rights reserved.",
    " */",
]
BANNER_HASH = [
    "# (C) Silent X Craft Launcher",
    "# Copyright by SilentStudio.",
    "# All rights reserved.",
]

C_EXTS = {".c", ".h", ".cpp", ".hpp", ".cc", ".hh"}
HASH_EXTS = {".py", ".ps1", ".cmake"}
HASH_NAMES = {"CMakeLists.txt"}

KEEP_PAT = re.compile(r"clang-format|NOLINT|coding[:=]|SPDX-License")
PRAGMA_ONCE = re.compile(r"^\s*#\s*pragma\s+once\s*$")
INCLUDE_GUARD = re.compile(r"^\s*#\s*(ifndef|define)\s+\w*_H\b", re.IGNORECASE)
FENCE = chr(96) * 3


def banner_for(style):
    return list(BANNER_C if style == "c" else BANNER_HASH)


def eat_leading_comments(lines, i, out, style):
    """从 i 起吃掉注释/空行,原文进 out;返回第一行代码的下标。"""
    n = len(lines)

    def is_comment(s):
        t = s.lstrip()
        if style == "c":
            return t.startswith("//") or t.startswith("/*")
        if t.startswith("#!") or t.startswith("#include") or t.startswith("#pragma"):
            return False
        if t.startswith("#if") or t.startswith("#define") or t.startswith("#endif"):
            return False
        if t.startswith("#else") or t.startswith("#elif"):
            return False
        return t.startswith("#")

    while i < n:
        s = lines[i]
        if is_comment(s):
            if style == "c" and s.lstrip().startswith("/*"):
                while i < n:
                    out.append(lines[i])
                    end = "*/" in lines[i]
                    i += 1
                    if end:
                        break
            else:
                out.append(lines[i])
                i += 1
            continue
        if s.strip() == "":
            j = i
            while j < n and lines[j].strip() == "":
                j += 1
            if j < n and is_comment(lines[j]):
                out.extend(lines[i:j])
                i = j
                continue
            break
        break
    return i


def transform(text, style):
    lines = text.split("\n")
    header = []
    i = eat_leading_comments(lines, 0, header, style)

    # 幂等保护:文件开头已经是我们的版权头 -> **一个字都不动**(脚本要能安全重跑)
    if any("Copyright by SilentStudio" in ln for ln in header):
        return text, {"header_lines": 0, "removed": [], "guard": "", "pragma": False}

    pragma = ""
    if i < len(lines) and PRAGMA_ONCE.match(lines[i].rstrip("\r")):
        pragma = lines[i]
        i += 1
        i = eat_leading_comments(lines, i, header, style)

    guard = ""
    if i < len(lines) and INCLUDE_GUARD.match(lines[i].rstrip("\r")):
        guard = lines[i]

    body = lines[i:]
    while body and body[0].strip() == "":
        body.pop(0)

    kept = [ln for ln in header if KEEP_PAT.search(ln)]
    removed = [ln for ln in header if not KEEP_PAT.search(ln)]
    while removed and removed[0].strip() == "":
        removed.pop(0)
    while removed and removed[-1].strip() == "":
        removed.pop()

    out = banner_for(style) + [""]
    if pragma:
        out.append(pragma)
        out.append("")
    if kept:
        out.extend(kept)
    out.extend(body)
    meta = {
        "header_lines": len(header),
        "removed": removed,
        "guard": guard,
        "pragma": bool(pragma),
    }
    return "\n".join(out), meta


def collect(root):
    files = []
    skip_dirs = {".git", "build", "build-ui", "_libqf", "assets", "reference"}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in skip_dirs and not d.startswith("build-") and not d.startswith(".")]
        rel = os.path.relpath(dirpath, root)
        top = rel.split(os.sep)[0]
        if top not in ("src", "include", "tools", "tests", "."):
            continue
        for name in filenames:
            ext = os.path.splitext(name)[1].lower()
            if ext in C_EXTS:
                style = "c"
            elif ext in HASH_EXTS or name in HASH_NAMES:
                style = "hash"
            else:
                continue
            if name == "strip_file_headers.py":
                continue
            files.append((os.path.join(dirpath, name), style))
    return sorted(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--archive", default="docs/14-源码头注归档.md")
    ap.add_argument("--only", default="", help="只处理相对路径里含这个子串的文件")
    args = ap.parse_args()

    root = args.root
    files = collect(root)
    changed, skipped, total_removed, warnings, archive = [], [], 0, [], []
    for path, style in files:
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        if args.only and args.only not in rel:
            continue
        with open(path, "r", encoding="utf-8", newline="") as fh:
            text = fh.read()
        new_text, meta = transform(text, style)
        if meta["header_lines"] == 0:
            skipped.append(rel)
        else:
            changed.append(rel)
            total_removed += len(meta["removed"])
            archive.append((rel, meta["removed"]))
        if meta["guard"]:
            warnings.append("include-guard 起头(只加不删): " + rel)
        if not args.dry_run and new_text != text:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)

    if not args.dry_run and archive:
        apath = os.path.join(root, args.archive)
        os.makedirs(os.path.dirname(apath), exist_ok=True)
        with open(apath, "w", encoding="utf-8", newline="") as fh:
            fh.write("# 源码头注归档\n\n")
            fh.write("> 本文件由 tools/strip_file_headers.py 生成:源文件开头的设计说明已从这里"
                     "移到统一版权头,原文**一字不改**留档,便于检索历史决策。\n"
                     "> 需要改行为时以源码为准;需要知道\"当初为什么这么写\"时来这里搜。\n\n")
            fh.write("共 %d 个文件,归档 %d 行。\n\n" % (len(archive), total_removed))
            for rel, removed in archive:
                fh.write("## " + rel + "\n\n" + FENCE + "text\n")
                fh.write("\n".join(removed))
                fh.write("\n" + FENCE + "\n\n")

    print("FILES_SCANNED=%d  CHANGED=%d  NO_HEADER=%d  ARCHIVED_LINES=%d"
          % (len(files), len(changed), len(skipped), total_removed))
    for w in warnings:
        print("WARN " + w)
    print("--- changed (first 30) ---")
    for rel in changed[:30]:
        print("  " + rel)
    if skipped:
        print("--- no header (只加版权头) %d ---" % len(skipped))
        for rel in skipped[:20]:
            print("  " + rel)


if __name__ == "__main__":
    main()
