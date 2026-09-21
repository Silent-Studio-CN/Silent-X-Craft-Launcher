#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# -*- coding: utf-8 -*-
#   4) 特殊保留(不删):含 clang-format / NOLINT / coding: / SPDX 的行(工具指令,删了会改行为)。
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


def extract_shebang(lines, style):
    """把 shebang 摘出来(它必须在第 1 行),顺带修复"版权头插在它前面"的历史文件。"""
    if style != "hash":
        return "", lines
    if lines and lines[0].startswith("#!"):
        return lines[0], lines[1:]
    if [l.rstrip("\r") for l in lines[:3]] == BANNER_HASH:
        j = 3
        while j < len(lines) and lines[j].strip() == "":
            j += 1
        if j < len(lines) and lines[j].startswith("#!"):
            return lines[j], lines[:j] + lines[j + 1:]
    return "", lines


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
    shebang, lines = extract_shebang(lines, style)
    header = []
    i = eat_leading_comments(lines, 0, header, style)

    empty_meta = {"header_lines": 0, "removed": [], "guard": "", "pragma": False}

    # 幂等保护:开头已经是我们的版权头 -> 除了把 shebang 摆正,别的一个字都不动
    if any("Copyright by SilentStudio" in ln for ln in header):
        body = "\n".join(lines)
        return ((shebang + "\n" + body) if shebang else body), empty_meta

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

    out = ([shebang] if shebang else []) + banner_for(style) + [""]
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
            files.append((os.path.join(dirpath, name), style))
    return sorted(files)


def load_archive(path):
    """已有归档 -> {路径: 原文};重跑时据此增量合并,不冲掉历史。"""
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    parts = re.split(r"^## (.+?)\s*$", text, flags=re.M)
    out = {}
    for k in range(1, len(parts) - 1, 2):
        out[parts[k].strip()] = parts[k + 1].strip("\n")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--archive", default="docs/14-源码头注归档.md")
    ap.add_argument("--only", default="", help="只处理相对路径里含这个子串的文件")
    args = ap.parse_args()

    root = args.root
    files = collect(root)
    changed, skipped, total_removed, warnings, fresh = [], [], 0, [], {}
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
            fresh[rel] = "\n".join(meta["removed"])
        if meta["guard"]:
            warnings.append("include-guard 起头(只加不删): " + rel)
        if not args.dry_run and new_text != text:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)

    if not args.dry_run and fresh:
        apath = os.path.join(root, args.archive)
        os.makedirs(os.path.dirname(apath), exist_ok=True)
        merged = load_archive(apath)
        merged.update(fresh)
        total_lines = sum(len(v.split("\n")) for v in merged.values())
        with open(apath, "w", encoding="utf-8", newline="") as fh:
            fh.write("# 源码头注归档\n\n")
            fh.write("> 本文件由 tools/strip_file_headers.py 生成:源文件开头的设计说明已从这里"
                     "移到统一版权头,原文**一字不改**留档,便于检索历史决策。\n"
                     "> 需要改行为时以源码为准;需要知道\"当初为什么这么写\"时来这里搜。\n\n")
            fh.write("共 %d 个文件,归档 %d 行。\n\n" % (len(merged), total_lines))
            for rel in sorted(merged):
                fh.write("## " + rel + "\n\n" + FENCE + "text\n")
                fh.write(merged[rel])
                fh.write("\n" + FENCE + "\n\n")

    print("FILES_SCANNED=%d  CHANGED=%d  NO_HEADER=%d  ARCHIVED_LINES=%d"
          % (len(files), len(changed), len(skipped), total_removed))
    for w in warnings:
        print("WARN " + w)
    for rel in changed[:20]:
        print("  " + rel)
    if skipped:
        print("--- no header (只加版权头) %d ---" % len(skipped))
        for rel in skipped[:12]:
            print("  " + rel)


if __name__ == "__main__":
    main()
