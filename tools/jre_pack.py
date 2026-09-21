#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
"""jre_pack.py - 把一份安卓 JRE 树打成自托管包,并**就地算好哈希**写进清单。

用户只需要:跑这个脚本 -> 把产出的目录原样上传 -> 把清单里的两处路径提交到 index 仓库。
哈希、size、version 文件都由脚本产出,用户不需要自己算(用户已明确说不清楚 hash 怎么办)。

产出(在 <out>/ 下):
  <id>/universal.tar.xz              所有 ABI 都要的那一份
  <id>/bin-<abi>.tar.xz              ABI 专属那一份
  <id>/version                       版本串(一行;与清单里的 version 一致,给人/mirror 看)
  <id>/*.sha256                      **可选**侧车(64 位十六进制 + 换行);SC 没就绪也不影响链路
  <id>/upload-checklist.md           逐文件的上传清单(路径 + size + sha256 前 16 位 + 两处落位)
  <id>/java_index.fragment.json      **要合并进 SXCL/Java_index.json 的那一段**(字面哈希)
  <id>/manifest.json                 自带托管形式(components[])的完整清单,可直接当 index.json 用

口径(与 docs/19 一致):
  * 默认与推荐 = **字面哈希**(64 位十六进制直接写进清单);它同时进 GitHub index 仓库(信任锚)与 SC。
  * 侧车 .sha256 只是顺手产出的便利物,**可选**;清单里写成 URL 型是备用形态。
  * url 用**绝对地址**(SC),mirrors 放另一份(GitHub / gh-proxy)。
  * size 必填(字节);size_mb 只是给人看的。

用法:
  # 打包并生成清单(SC 放包、GitHub 放清单)
  python tools/jre_pack.py pack --universal <dir> --bin <dir> --abi arm64 \\
      --id jre17 --major 17 --version 17.0.9+11-sxcl.1 --out dist/jre17 \\
      --sc-base https://cloud.silentstudio.cn/api/v1/<owner>/JDK/Java/JRE/Android \\
      --gh-base https://raw.githubusercontent.com/Silent-Studio-CN/index/main/jre

  # 上传后核对(逐文件比 size 与 sha256)
  python tools/jre_pack.py verify --index dist/jre17/manifest.json --dir dist/jre17
"""

import argparse
import hashlib
import io
import json
import lzma
import os
import sys
import tarfile
import time

# Windows 控制台默认是 GBK:这个脚本的输出里有中文,不改成 UTF-8 会直接 UnicodeEncodeError。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def _tar_add(tf, src_root, rel, arcname):
    full = os.path.join(src_root, rel)
    ti = tarfile.TarInfo(arcname)
    st = os.lstat(full)
    ti.mode = st.st_mode & 0o7777
    ti.uid = 0
    ti.gid = 0
    ti.uname = ""
    ti.gname = ""
    ti.mtime = 0
    if os.path.islink(full):
        ti.type = tarfile.SYMTYPE
        ti.linkname = os.readlink(full)
        ti.size = 0
        tf.addfile(ti)
    elif os.path.isdir(full):
        ti.type = tarfile.DIRTYPE
        ti.size = 0
        tf.addfile(ti)
    else:
        ti.type = tarfile.REGTYPE
        ti.size = st.st_size
        with open(full, "rb") as fh:
            tf.addfile(ti, fh)


def build_tar_xz(src_dir, dest_path, check=lzma.CHECK_CRC64, preset=6):
    """把 src_dir 里的内容按相对路径打进一个 .tar.xz(**确定性**:uid/gid/mtime 归零、顺序排序)。

    确定性是为了哈希稳定:同一棵树每次打出来字节一样,清单才能复现。
    """
    if not os.path.isdir(src_dir):
        raise SystemExit("[jre_pack] 不是目录: " + src_dir)
    entries = []
    for root, dirs, files in os.walk(src_dir):
        dirs.sort()
        files.sort()
        rel_root = os.path.relpath(root, src_dir)
        if rel_root == ".":
            rel_root = ""
        for d in dirs:
            entries.append((os.path.join(rel_root, d) if rel_root else d).replace(os.sep, "/"))
        for f in files:
            entries.append((os.path.join(rel_root, f) if rel_root else f).replace(os.sep, "/"))
    entries.sort()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tf:
        for rel in entries:
            _tar_add(tf, src_dir, rel.replace("/", os.sep), rel)
    raw = buf.getvalue()
    raw += b"\x00" * ((-len(raw)) % 10240)   # 与 GNU tar 一致:补到 10240 的整数倍
    comp = lzma.compress(raw, format=lzma.FORMAT_XZ, check=check, preset=preset)
    os.makedirs(os.path.dirname(dest_path) or ".", exist_ok=True)
    with open(dest_path, "wb") as fh:
        fh.write(comp)
    return len(raw), comp


def digest_file(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sidecar(path, hexdigest):
    """可选的 <name>.sha256 侧车:64 位十六进制 + 换行,text/plain(与 Oracle 的形态一致)。"""
    with open(path + ".sha256", "w", encoding="utf-8", newline="\n") as fh:
        fh.write(hexdigest + "\n")


def file_entry(local_path, url, mirror, rel_name=None):
    return {
        "name": rel_name or os.path.basename(local_path),
        "path": local_path,
        "size": os.path.getsize(local_path),
        "sha256": digest_file(local_path, "sha256"),
        "sha1": digest_file(local_path, "sha1"),
        "url": url,
        "mirror": mirror,
    }


def cmd_pack(args):
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    # 主前缀:--base 优先,旧名 --sc-base / --gh-base 仍然认(换域名不该改架构)
    sc_base = (args.base or args.sc_base or args.gh_base or "").rstrip("/")
    # mirrors 前缀:默认 gh-proxy。**保留末尾的 '/'**(gh-proxy 的用法就是"前缀 + 完整 raw URL")
    gh_base = (args.mirror_base or "").strip()
    if gh_base and not gh_base.endswith("/"):
        gh_base += "/"

    produced = []
    if args.universal:
        dest = os.path.join(out, "universal.tar.xz")
        raw, comp = build_tar_xz(args.universal, dest, preset=args.preset)
        print("[jre_pack] universal.tar.xz   raw=%d  xz=%d" % (raw, len(comp)))
        produced.append(("universal", dest, args.id + "/universal.tar.xz"))
    if args.bin:
        dest = os.path.join(out, "bin-%s.tar.xz" % args.abi)
        raw, comp = build_tar_xz(args.bin, dest, preset=args.preset)
        print("[jre_pack] bin-%s.tar.xz     raw=%d  xz=%d" % (args.abi, raw, len(comp)))
        produced.append(("bin", dest, args.id + "/bin-%s.tar.xz" % args.abi))
    if not produced:
        raise SystemExit("[jre_pack] 既没给 --universal 也没给 --bin,没什么可打的")

    with open(os.path.join(out, "version"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(args.version + "\n")

    entries = []
    for kind, dest, rel in produced:
        url = ("%s/%s" % (sc_base, rel)) if sc_base else rel
        # mirrors:镜像前缀 + **完整的**主 URL(gh-proxy 就是这么用的)
        mirror = ("%s%s" % (gh_base, url)) if (gh_base and url.startswith("http")) else ""
        e = file_entry(dest, url, mirror, rel_name=os.path.basename(dest))
        e["kind"] = kind
        e["sc_path"] = rel
        e["gh_path"] = rel if mirror else ""
        entries.append(e)
        write_sidecar(dest, e["sha256"])
        print("[jre_pack] %-22s size=%-10d sha256=%s..." % (e["name"], e["size"], e["sha256"][:16]))
    # 顺序固定:universal 在前、bin 在后(解包就是这个顺序:bin 覆盖 universal 的同名文件)
    order = {"universal": 0, "bin": 1}
    entries.sort(key=lambda x: order.get(x["kind"], 9))

    universal = next((e for e in entries if e["kind"] == "universal"), None)
    bins = [e for e in entries if e["kind"] == "bin"]
    primary = universal if universal is not None else bins[0]

    android_entry = {
        "format": "tar.xz",
        "version": args.version,
        "size": primary["size"],
        "size_mb": round(primary["size"] / 1048576.0, 2),
        "sha256": primary["sha256"],
        "sha1": primary["sha1"],
        "url": primary["url"],
    }
    if primary["mirror"]:
        android_entry["mirrors"] = [primary["mirror"]]
    if bins:
        android_entry["extra_files"] = []
        for b in bins:
            item = {"name": b["name"], "size": b["size"], "sha256": b["sha256"], "sha1": b["sha1"],
                    "url": b["url"]}
            if b["mirror"]:
                item["mirrors"] = [b["mirror"]]
            android_entry["extra_files"].append(item)
    fragment = {
        "_comment": "把 platforms.android 这个节点的内容并进 SXCL/Java_index.json 的 versions.<major>",
        "platforms": {"android": {args.abi: android_entry}},
    }
    with open(os.path.join(out, "java_index.fragment.json"), "w", encoding="utf-8",
              newline="\n") as fh:
        json.dump(fragment, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    manifest = {
        "schema": 1,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "components": [{
            "id": args.id,
            "javaMajor": args.java_major,
            "version": args.version,
            "files": [{"file": e["url"], "size": e["size"], "sha256": e["sha256"],
                       "sha1": e["sha1"]} for e in entries],
        }],
    }
    mpath = os.path.join(out, "manifest.json")
    with open(mpath, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    lines = []
    lines.append("# " + args.id + " 上传清单(" + args.version + ")")
    lines.append("")
    lines.append("生成时间(UTC):" + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    lines.append("")
    lines.append("| # | 文件 | size(字节) | sha256(前 16) | SilentCloud 路径 | GitHub index 仓库路径 | 必选? |")
    lines.append("|---|---|---|---|---|---|---|")
    idx = 0
    for e in entries:
        idx += 1
        lines.append("| %d | %s | %d | %s | %s | %s | 必传 |" %
                     (idx, e["name"], e["size"], e["sha256"][:16], e["sc_path"],
                      e["gh_path"] or "(不放二进制,只放清单/侧车)"))
    for e in entries:
        idx += 1
        side_text = e["sha256"] + "\n"
        lines.append("| %d | %s.sha256 | %d | %s | %s.sha256 | %s | 可选 |" %
                     (idx, e["name"], len(side_text),
                      hashlib.sha256(side_text.encode()).hexdigest()[:16], e["sc_path"],
                      (e["gh_path"] + ".sha256") if e["gh_path"] else "(可选)"))
    idx += 1
    version_blob = args.version + "\n"
    lines.append("| %d | version | %d | %s | %s/version | %s/version | 必传 |" %
                 (idx, len(version_blob), hashlib.sha256(version_blob.encode()).hexdigest()[:16],
                  sc_base + "/" + args.id, args.id))
    idx += 1
    lines.append("| %d | manifest.json | %d | %s | %s/index.json | %s/manifest.json | 必传 |" %
                 (idx, os.path.getsize(mpath), digest_file(mpath, "sha256")[:16],
                  sc_base + "/" + args.id, args.id))
    lines.append("")
    lines.append("说明:")
    lines.append("* **必传**的是包 + version + 清单;清单里的 sha256 是**字面哈希**(信任锚),")
    lines.append("  所以一个侧车文件都不传,链路照样可用。")
    lines.append("* .sha256 侧车是**可选**便利物(64 位十六进制 + 换行,text/plain);SC 暂不提供也没关系。")
    lines.append("* 清单要同时更新两处:GitHub index 仓库的 SXCL/Java_index.json(把")
    lines.append("  java_index.fragment.json 里的 platforms 并进 versions[<major>])与 SC 上的同名副本。")
    lines.append("* GitHub 侧只放**清单与 .sha256**,不要放大包(大文件不进 git 仓库)。")
    lines.append("* 我们的客户端:file/url 是**绝对地址**就用绝对地址;相对路径才相对清单拼。")
    with open(os.path.join(out, "upload-checklist.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")

    print("[jre_pack] 清单: %s" % mpath)
    print("[jre_pack] 片段: %s" % os.path.join(out, "java_index.fragment.json"))
    print("[jre_pack] 上传清单: %s" % os.path.join(out, "upload-checklist.md"))
    print("[jre_pack] 核对命令: python tools/jre_pack.py verify --index \"%s\" --dir \"%s\"" %
          (mpath, out))
    return 0


def cmd_verify(args):
    with open(args.index, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    targets = []
    comps = doc.get("components")
    if isinstance(doc.get("schema"), str) and doc["schema"].startswith("sxcl.jre.index/"):
        # 我们自己的安卓 JRE 清单(packages[] 才是运行时本体;version_file 也顺手核一下)
        for _cid, c in (comps or {}).items():
            for pk in c.get("packages", []):
                targets.append((pk.get("url", ""), pk.get("size"), pk.get("sha256", "")))
            vf = c.get("version_file")
            if isinstance(vf, dict):
                targets.append((vf.get("url", ""), vf.get("size"), vf.get("sha256", "")))
    elif isinstance(comps, list):
        for c in comps:
            for f in c.get("files", []):
                targets.append((f.get("file", ""), f.get("size"), f.get("sha256", "")))
    elif isinstance(doc.get("versions"), dict):
        for _major, node in doc["versions"].items():
            android = (node.get("platforms") or {}).get("android") or {}
            for _abi, p in android.items():
                targets.append((p.get("url", ""), p.get("size"), p.get("sha256", "")))
                for ef in (p.get("extra_files") or []):
                    targets.append((ef.get("url", ""), ef.get("size"), ef.get("sha256", "")))
    else:
        raise SystemExit("[jre_pack] 认不出这份清单(既没有 components 也没有 versions)")

    ok = 0
    bad = 0
    for url, size, sha in targets:
        name = os.path.basename(url.split("?")[0])
        local = None
        if args.dir:
            cand = os.path.join(args.dir, name)
            if os.path.isfile(cand):
                local = cand
        if local is None and args.url_prefix:
            cand = args.url_prefix.rstrip("/") + "/" + name
            if os.path.isfile(cand):
                local = cand
        if local is None:
            print("  [skip] %-28s (本地找不到:用 --dir 或 --url-prefix 指到下载好的目录)" % name)
            continue
        actual_size = os.path.getsize(local)
        if size is not None and actual_size != size:
            print("  [FAIL] %-28s size %d != 清单里的 %s" % (name, actual_size, size))
            bad += 1
            continue
        want = None
        if isinstance(sha, str) and len(sha) == 64:
            want = sha.lower()
        elif isinstance(sha, str) and sha.startswith("http"):
            side = local + ".sha256"
            if os.path.isfile(side):
                with open(side, "r", encoding="utf-8") as fh:
                    want = fh.read().strip().split()[0].lower()
            else:
                print("  [skip] %-28s 清单里是 URL 型 sha256,本地没有 %s" % (name, side))
                continue
        if want is not None:
            actual = digest_file(local, "sha256")
            if actual != want:
                print("  [FAIL] %-28s sha256 %s != %s" % (name, actual[:16], want[:16]))
                bad += 1
                continue
        print("  [ ok ] %-28s size=%d sha256=%s..." % (name, actual_size, (want or "?")[:16]))
        ok += 1
    print("[jre_pack] 核对完成:通过 %d,失败 %d" % (ok, bad))
    return 1 if bad else 0


def main(argv):
    ap = argparse.ArgumentParser(description="SXCL-C 自托管 JRE 打包 + 清单生成 + 核对")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="打 tar.xz 并生成清单/上传清单")
    p.add_argument("--universal", help="所有 ABI 都要的那一半的目录")
    p.add_argument("--bin", help="ABI 专属那半的目录(通常含 bin/ 与 lib/libjli.so)")
    p.add_argument("--abi", default="arm64", help="ABI 名(bin-<abi>.tar.xz),默认 arm64")
    p.add_argument("--id", required=True, help="组件 id,例如 jre17")
    p.add_argument("--major", dest="java_major", type=int, required=True, help="Java 主版本")
    p.add_argument("--version", required=True, help="构建版本串(重装判据)")
    p.add_argument("--out", required=True, help="产出目录")
    p.add_argument("--base", default="",
                   help="清单与包的绝对前缀(默认 GitHub raw,例如 "
                        "https://raw.githubusercontent.com/Silent-Studio-CN/<repo>/main)")
    p.add_argument("--mirror-base", default="https://gh-proxy.com/",
                   help="mirrors 的前缀(默认 gh-proxy;留空 = 不写 mirrors)")
    p.add_argument("--sc-base", default="", help="(旧名,等价于 --base)")
    p.add_argument("--gh-base", default="", help="(旧名,等价于 --base)")
    p.add_argument("--mirror-host-packages", action="store_true",
                   help="旧开关:把 mirrors 指向 gh-base;新口径用 --mirror-base 即可")
    p.add_argument("--preset", type=int, default=6, help="xz 压缩档(默认 6)")
    p.set_defaults(func=cmd_pack)

    v = sub.add_parser("verify", help="按清单核对本地(或已下载)的文件")
    v.add_argument("--index", required=True)
    v.add_argument("--dir", help="本地目录(按文件名找)")
    v.add_argument("--url-prefix", help="把清单里的 URL 前缀换成本地路径")
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
