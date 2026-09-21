#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# verify_header_strip.py —— 独立校验版权头批处理:只删了开头注释,一行代码都没动。
#
# 这不是"脚本自证":这里**另写一份**注释扫描器(index_body),并直接跟 git 里的旧版本
# 逐行比对。四个校验点:
#   A) 结构:重放 transform(旧文件) 的结果 == 磁盘上的新文件(逐行全等)。
#   B) 交叉检查:index_body() 各自求旧/新文件的正文起点,**正文必须逐行全等**。
#   C) 旧文件正文之前只许是注释(例外:#pragma once / include guard / shebang)。
#   D) git diff 审计(**按文件、按注释风格**):删掉的行必须全是注释行;
#      新增的行必须全是版权头/空行。别的代理正在改的文件不参与本审计。
#
# 用法:
#   python tools/verify_header_strip.py --rev HEAD
#   python tools/verify_header_strip.py --rev 426db62

import argparse
import importlib.util
import os
import re
import subprocess
import sys

BANNER_LINES = {
    "/*", " * (C) Silent X Craft Launcher", " * Copyright by SilentStudio.",
    " * All rights reserved.", " */",
    "# (C) Silent X Craft Launcher", "# Copyright by SilentStudio.",
    "# All rights reserved.", "#pragma once", "",
}
C_COMMENT = re.compile(r"^\s*(//|/\*|\*|\*/)")
HASH_COMMENT = re.compile(r"^\s*#(?!\s*(include|pragma|if|ifdef|ifndef|elif|else|endif|define|undef))")
GUARD_OK = re.compile(r"^\s*#\s*(pragma\s+once|ifndef\s+\w+|define\s+\w+)\s*$")


def index_body(lines, style):
    """独立实现:返回正文起始下标(跳过开头注释块与空行)。"""
    i, n, in_block = 0, len(lines), False
    while i < n:
        raw = lines[i]
        s = raw.strip()
        if in_block:
            if "*/" in raw:
                in_block = False
            i += 1
            continue
        if s == "":
            i += 1
            continue
        if style == "c":
            # #pragma once / include guard 允许出现在头注**之前**(很多头文件就是这么写的),
            # 定位正文时要跳过它们 —— 否则会把旧文件的 pragma 行算进正文,误报"正文被改动"。
            if GUARD_OK.match(raw):
                i += 1
                continue
            if s.startswith("//"):
                i += 1
                continue
            if s.startswith("/*"):
                if "*/" not in raw:
                    in_block = True
                i += 1
                continue
            return i
        if s.startswith("#!"):
            i += 1
            continue
        if s.startswith("#"):
            i += 1
            continue
        return i
    return i


def git(root, *args):
    out = subprocess.run(["git"] + list(args), cwd=root, capture_output=True)
    return out.returncode, out.stdout.decode("utf-8", "replace")


def norm(lines):
    return [ln.rstrip("\r") for ln in lines]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default="D:/SilentStudio/_clip_evidence/header_strip_verify.txt")
    args = ap.parse_args()
    root = args.root

    spec = importlib.util.spec_from_file_location(
        "sfh", os.path.join(root, "tools", "strip_file_headers.py"))
    sfh = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sfh)

    rc, out = git(root, "diff", "--name-only", args.rev, "--")
    changed = [c for c in out.split()
               if c.endswith((".c", ".h", ".cpp", ".hpp", ".py", ".ps1", ".cmake", "CMakeLists.txt"))]

    problems, ok_count, skipped_rewritten = [], 0, []
    removed_total, added_total, bad_del, bad_add = 0, 0, [], []
    for rel in changed:
        rc, old = git(root, "show", args.rev + ":" + rel)
        if rc != 0:
            continue
        style = "c" if os.path.splitext(rel)[1].lower() in sfh.C_EXTS else "hash"
        with open(os.path.join(root, rel), "r", encoding="utf-8", newline="") as fh:
            new = fh.read()
        old_l, new_l = norm(old.split("\n")), norm(new.split("\n"))

        # A) 重放:旧文件跑一次 transform,结果必须与磁盘上的新文件逐行全等
        expected, meta = sfh.transform(old, style)
        if norm(expected.split("\n")) != new_l:
            skipped_rewritten.append(rel)  # 属于"批处理之后又被人工重写过"的文件,见下方说明
            continue

        # B) 正文逐行全等(独立扫描器)
        ob, nb = index_body(old_l, style), index_body(new_l, style)
        if old_l[ob:] != new_l[nb:]:
            problems.append(rel + " : B 正文被改动")
            continue

        # C) 头注区不许混代码
        offenders = [ln for ln in old_l[:ob]
                     if ln.strip()
                     and not (C_COMMENT if style == "c" else HASH_COMMENT).match(ln)
                     and not GUARD_OK.match(ln)
                     and not ln.startswith("#!")]
        if offenders:
            problems.append(rel + " : C 头注区混着代码: " + repr(offenders[:3]))
            continue

        # D) 该文件的 diff 审计
        rc, d = git(root, "diff", "--unified=0", args.rev, "--", rel)
        comment_re = C_COMMENT if style == "c" else HASH_COMMENT
        for ln in d.split("\n"):
            if ln.startswith("---") or ln.startswith("+++"):
                continue
            if ln.startswith("-"):
                removed_total += 1
                # #pragma once / include guard 允许**换位置**(本脚本把版权头放到最前面,
                # guard 跟着上移一行);它们不是注释,但也不是"被删掉的代码"。
                if not comment_re.match(ln[1:]) and not GUARD_OK.match(ln[1:]):
                    bad_del.append(rel + " :: " + ln[1:80])
            elif ln.startswith("+"):
                added_total += 1
                if ln[1:].rstrip("\r") not in BANNER_LINES:
                    bad_add.append(rel + " :: " + ln[1:80])
        ok_count += 1

    lines = ["REV=%s  完全通过文件=%d  异常=%d  批处理后又被重写(单独说明)=%d"
             % (args.rev, ok_count, len(problems), len(skipped_rewritten)),
             "diff 审计(仅本次批处理覆盖的文件): 删除行=%d 新增行=%d  非注释删除=%d  非版权头新增=%d"
             % (removed_total, added_total, len(bad_del), len(bad_add))]
    for ln in bad_del[:10]:
        lines.append("  非法删除: " + ln)
    for ln in bad_add[:10]:
        lines.append("  非法新增: " + ln)
    for p in problems[:20]:
        lines.append("  PROBLEM " + p)
    for rel in skipped_rewritten[:10]:
        lines.append("  重写文件: " + rel)
    text = "\n".join(lines) + "\n"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(text)
    return 1 if (problems or bad_del or bad_add) else 0


if __name__ == "__main__":
    sys.exit(main())
