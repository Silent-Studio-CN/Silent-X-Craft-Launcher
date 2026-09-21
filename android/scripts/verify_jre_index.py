# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# verify_jre_index.py - 独立校验 index.json 与它描述的产物是不是一回事(**只读**)。
#
# 校验链:index.json -> 每个包的 size/sha256/sha1 -> 解开两个 tar.xz -> 逐文件复算 sha256/sha1
#        -> 与 manifest.files 一条条对 -> NOTICE/version 也逐文件对 -> UPLOAD.txt 里列的每个文件
#        也复算一遍(上传清单本身不能撒谎)。
#
# 用法:
#   python android/scripts/verify_jre_index.py --out D:\SilentStudio\_termux_jre\out
#   python android/scripts/verify_jre_index.py --out <out> --component jre17
# 退出码:0 = 全对;1 = 有对不上的;2 = 用法/文件缺失。

import argparse
import hashlib
import json
import os
import sys
import tarfile


def h256_1(path):
    h1 = hashlib.sha256()
    h2 = hashlib.sha1()
    n = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            n += len(b)
            h1.update(b)
            h2.update(b)
    return n, h1.hexdigest(), h2.hexdigest()


def check_file(label, path, ent, problems, checks):
    if not os.path.isfile(path):
        problems.append("%s: 文件不存在 %s" % (label, path))
        return
    size, s256, s1 = h256_1(path)
    checks[0] += 1
    if ent.get("size") is not None and size != ent["size"]:
        problems.append("%s: size 不符 %s (%d != %d)" % (label, os.path.basename(path), size, ent["size"]))
    if ent.get("sha256") and s256 != ent["sha256"]:
        problems.append("%s: sha256 不符 %s" % (label, os.path.basename(path)))
    if ent.get("sha1") and s1 != ent["sha1"]:
        problems.append("%s: sha1 不符 %s" % (label, os.path.basename(path)))


def tar_members(path, want_sha1=True):
    man = {}
    with tarfile.open(path, "r:xz") as tf:
        for m in tf:
            name = m.name.lstrip("./")
            if m.isdir():
                man[name] = {"type": "directory", "size": 0}
            elif m.issym() or m.islnk():
                man[name] = {"type": "link", "size": 0, "target": m.linkname,
                             "sha256": hashlib.sha256(b"").hexdigest(),
                             "sha1": hashlib.sha1(b"").hexdigest() if want_sha1 else None}
            elif m.isfile():
                h1 = hashlib.sha256()
                h2 = hashlib.sha1() if want_sha1 else None
                n = 0
                f = tf.extractfile(m)
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    n += len(b)
                    h1.update(b)
                    if h2 is not None:
                        h2.update(b)
                man[name] = {"type": "file", "size": n, "sha256": h1.hexdigest(),
                             "sha1": (h2.hexdigest() if h2 is not None else None)}
            else:
                man[name] = {"type": "other"}
    return man


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--component", default="")
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass

    index_path = os.path.join(a.out, "index.json")
    if not os.path.isfile(index_path):
        print("[FAIL] 没有 index.json:", index_path)
        return 2
    with open(index_path, "r", encoding="utf-8") as f:
        idx = json.load(f)
    problems = []
    checks = [0]
    if idx.get("schema") != "sxcl.jre.index/1":
        problems.append("schema 不是 sxcl.jre.index/1:%r" % idx.get("schema"))

    for comp_name in sorted(idx.get("components", {})):
        if a.component and comp_name != a.component:
            continue
        c = idx["components"][comp_name]
        print("== %s (%s, %s, id=%s)" % (comp_name, c.get("version"), c.get("abi"), c.get("id")))
        comp_dir = os.path.join(a.out, comp_name)
        # 1) 包 + version + notice:每个文件都必须有 size/sha256/sha1,且对得上
        all_entries = list(c.get("packages", []))
        if c.get("version_file"):
            all_entries.append(c["version_file"])
        all_entries += list(c.get("notice", []))
        host = idx.get("host") or {}
        url_base = host.get("url_base", "")
        mirror_base = (host.get("url_mirror") or {}).get("base", "")
        if not url_base:
            problems.append("index.json 缺 host.url_base(客户端要靠它拼下载地址)")
        for ent in all_entries:
            rel = ent.get("path") or ""
            if not rel:
                u = ent.get("url", "")
                i = u.find("/" + comp_name + "/")
                rel = u[i + 1:] if i >= 0 else u
            if not rel.startswith(comp_name + "/"):
                problems.append("%s: 推不出仓库内相对路径:%r" % (comp_name, ent.get("url")))
                continue
            if url_base and ent.get("url") != url_base + "/" + rel:
                problems.append("%s: url 与 host.url_base 拼不出来:%s" % (comp_name, ent.get("url")))
            if mirror_base and ent.get("url_mirror") != mirror_base + "/" + rel:
                problems.append("%s: url_mirror 与 host.url_mirror.base 拼不出来:%s" % (comp_name, ent.get("url_mirror")))
            if not ent.get("url_mirror"):
                problems.append("%s: 条目缺 url_mirror:%s" % (comp_name, ent.get("name")))
            local = os.path.join(a.out, rel.replace("/", os.sep))
            for k in ("size", "sha256", "sha1"):
                if not ent.get(k):
                    problems.append("%s: 条目 %s 缺 %s" % (comp_name, ent.get("name"), k))
            check_file("pkg", local, ent, problems, checks)
        vf = c.get("version_file") or {}
        vp = os.path.join(comp_dir, "version")
        if os.path.isfile(vp):
            first = open(vp, "r", encoding="utf-8").readline().strip()
            if vf.get("first_line") and vf["first_line"] != first:
                problems.append("%s: version 的第一行与 index 记的不一样" % comp_name)
            if ("id=" + str(c.get("id"))) not in open(vp, "r", encoding="utf-8").read():
                problems.append("%s: version 里没有 id=%s" % (comp_name, c.get("id")))

        # 2) manifest 逐条:字段齐 + 能在对应 tar 里找到且内容一致
        files = (c.get("manifest") or {}).get("files") or {}
        if not files:
            problems.append("%s: manifest.files 是空的" % comp_name)
        by_pkg = {}
        for name, item in files.items():
            for k in ("type",):
                if k not in item:
                    problems.append("%s: manifest[%s] 缺 %s" % (comp_name, name, k))
            if item.get("type") == "directory":
                continue
            for k in ("size", "sha256", "sha1"):
                if not item.get(k):
                    problems.append("%s: manifest[%s] 缺 %s" % (comp_name, name, k))
            pkg = item.get("package")
            if not pkg:
                problems.append("%s: manifest[%s] 缺 package" % (comp_name, name))
                continue
            by_pkg.setdefault(pkg, {})[name] = item
        for pkg_name, want in sorted(by_pkg.items()):
            tarp = os.path.join(comp_dir, pkg_name)
            if not os.path.isfile(tarp):
                problems.append("%s: 缺包 %s" % (comp_name, pkg_name))
                continue
            got = tar_members(tarp)
            for name, item in sorted(want.items()):
                checks[0] += 1
                g = got.get(name)
                if g is None:
                    problems.append("%s/%s: manifest 里有 %s,tar 里没有" % (comp_name, pkg_name, name))
                    continue
                for k in ("size", "sha256", "sha1"):
                    if item.get(k) != g.get(k):
                        problems.append("%s/%s: %s 的 %s 不一致" % (comp_name, pkg_name, name, k))
                if item.get("type") != g.get("type"):
                    problems.append("%s/%s: %s 的类型不一致" % (comp_name, pkg_name, name))
            extra = [n for n in got if n not in want and got[n].get("type") == "file"]
            if extra:
                problems.append("%s/%s: tar 里有 %d 个文件不在 manifest(例:%s)"
                                % (comp_name, pkg_name, len(extra), ", ".join(sorted(extra)[:3])))
        n_files = sum(1 for i in files.values() if i.get("type") == "file")
        if c.get("file_count") and n_files != c["file_count"]:
            problems.append("%s: file_count=%s 但 manifest 里 %d 个文件" % (comp_name, c["file_count"], n_files))
        print("   包 %d 个 / manifest 文件 %d / NOTICE %d / 校验点 %d"
              % (len(c.get("packages", [])), n_files, len(c.get("notice", [])), checks[0]))

    # 3) UPLOAD.txt 不能撒谎:逐行复算
    up = os.path.join(a.out, "UPLOAD.txt")
    if os.path.isfile(up):
        listed = 0
        for line in open(up, "r", encoding="utf-8"):
            s = line.rstrip("\r\n")
            if not s or s[0] in "#-=" or s.startswith("相对路径"):
                continue
            # 新格式是 TAB 分隔;旧格式(空格)用"第一个纯数字=size、紧跟 16 位 hex=sha16"反推路径,
            # 这样带空格的路径("Public Domain.txt")不会被拆错。
            if "\t" in s:
                f = s.split("\t")
                if len(f) < 3 or not f[1].strip().isdigit() or len(f[2].strip()) != 16:
                    continue
                rel, size, sha16 = f[0], int(f[1]), f[2].strip()
            else:
                toks = s.split()
                hit = None
                for i in range(len(toks) - 1):
                    if toks[i].isdigit() and len(toks[i + 1]) == 16:
                        hit = (" ".join(toks[:i]), int(toks[i]), toks[i + 1])
                        break
                if hit is None:
                    continue
                rel, size, sha16 = hit
            p = os.path.join(a.out, rel.replace("/", os.sep))
            if not os.path.isfile(p):
                problems.append("UPLOAD.txt 列了不存在的文件:%s" % rel)
                continue
            n, s256, _s1 = h256_1(p)
            checks[0] += 1
            listed += 1
            if n != size or s256[:16] != sha16:
                problems.append("UPLOAD.txt 与磁盘不符:%s" % rel)
        print("== UPLOAD.txt:逐行复算 %d 个文件" % listed)
    else:
        problems.append("没有 UPLOAD.txt")

    print("")
    if problems:
        for p in problems[:40]:
            print("[BAD ] " + p)
        print("[FAIL] 共 %d 条对不上(校验点 %d)" % (len(problems), checks[0]))
        return 1
    print("[ok  ] 全部一致(校验点 %d)" % checks[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
