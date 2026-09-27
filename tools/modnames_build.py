#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""(C) Silent X Craft Launcher
Copyright by SilentStudio.
All rights reserved.

模组中文名表生成器（把上游数据源转成我们的 assets/data/modnames.tsv）。

数据来源与许可（逐条见 docs/29，**不要凭印象改这里**）：
  * 默认来源 --source hmcl：Hello Minecraft! Launcher 的 mod_data.txt
    （HMCL 仓库 GPL-3.0；该文件头自己声明"mcmod.cn Copyright (C) 2025. All Rights Reserved."，
     所以我们**保留每行的 mcmod 编号**把出处钉住 —— PCL 的 WikiEntries.txt 我们一个字都不抄）。
  * 可选 --cfpa-info <config/info.json>：用 CFPA 翻译项目的 info.json 把 cf_slug 换成
    CurseForge 的**数字 id**（CC BY-NC-SA 4.0，见 docs/29 的取舍说明，默认不开）。

表格式（写表与读表用同一套；sha256 是"表头之后第一个数据字节到文件结尾"）：
    # sxcl-modnames<TAB>1
    # version<TAB><单调递增的整数>
    # count<TAB><数据行条数>
    # sha256<TAB><正文摘要>
    # source / # license / # generated / # columns
    <slug>\t<cf_id>\t<cf_slug>\t<modid>\t<name_zh>\t<name_en>\t<source>\t<mcmod_id>

用法：
    python tools/modnames_build.py --source hmcl --hmcl <mod_data.txt> --out assets/data/modnames.tsv
    python tools/modnames_build.py --source hmcl --hmcl <mod_data.txt> --cfpa-info <info.json> --out ...
    python tools/modnames_build.py --stats --hmcl <mod_data.txt>       # 只看过滤前后的条数
"""

import argparse
import datetime
import hashlib
import json
import os
import sys
import unicodedata

MAGIC = "sxcl-modnames"
FORMAT = 1
COLUMNS = ["slug", "cf_id", "cf_slug", "modid", "name_zh", "name_en", "source", "mcmod_id"]
SOURCE_NOTE = "HMCL mod_data.txt (GPL-3.0; 数据出处 mcmod.cn,每行保留 mcmod 编号)"

# 只收"真的是中文名"的行：至少一个 CJK 字符。纯拉丁的 name_zh 一律丢掉 ——
# 那种"中文名"跟原名一模一样，收进来等于给自己造噪音（也不符合"绝不猜"）。
def has_cjk(text):
    for ch in text:
        if "\u3400" <= ch <= "\u4dbf" or "\u4e00" <= ch <= "\u9fff" or "\uf900" <= ch <= "\ufaff":
            return True
        if "\U00020000" <= ch <= "\U0002ffff":
            return True
    return False


def norm_key(text):
    """与 C 版 normalize_key 同一套：去掉空白与常见标点、ASCII 转小写。"""
    drop = set(" \t\r\n\v\f-_.'\":;!?,&()[]{}"+chr(92)+"/+|~*#@%^=<>")
    out = []
    for ch in text:
        if ch in drop:
            continue
        if ch == "\u3000":
            continue
        out.append(ch.lower() if "A" <= ch <= "Z" else ch)
    return "".join(out)


def parse_hmcl(path):
    """HMCL mod_data.txt：curseforge_slug;mcmod_id;modids(逗号分隔);中文名;英文名;缩写"""
    rows = []
    bad = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            if not line or line.startswith("#"):
                continue
            parts = line.split(";")
            if len(parts) != 6:
                bad += 1
                continue
            cf_slug, mcmod, modids, name_zh, name_en, _abbr = [p.strip() for p in parts]
            if not name_zh:
                bad += 1
                continue
            modid_list = [m.strip() for m in modids.split(",") if m.strip()]
            for i, modid in enumerate(modid_list if modid_list else [""]):
                rows.append({
                    "slug": "",
                    "cf_id": "",
                    "cf_slug": cf_slug,
                    "modid": modid,
                    "name_zh": name_zh,
                    "name_en": name_en,
                    "source": "hmcl",
                    "mcmod_id": mcmod,
                    # 第一行带上"还有别的 modid"的标记，纯粹给统计用
                    "_first": (i == 0),
                })
    return rows, bad


def apply_cfpa_info(rows, path):
    """把 cf_slug 换成 CurseForge 数字 id（可选，许可见 docs/29）。"""
    with open(path, "r", encoding="utf-8") as fh:
        info = json.load(fh)
    by_slug = {}
    for slug, item in info.items():
        if isinstance(item, dict) and isinstance(item.get("id"), int):
            by_slug[slug] = item["id"]
    hit = 0
    for row in rows:
        cid = by_slug.get(row["cf_slug"])
        if cid is not None:
            row["cf_id"] = str(cid)
            hit += 1
    return hit, len(by_slug)


def build(rows, version, source_note):
    # 去重：同一个 (cf_id, cf_slug, modid, name_zh) 只留一条
    seen = set()
    uniq = []
    for row in rows:
        key = (row["slug"], row["cf_id"], row["cf_slug"], row["modid"], row["name_zh"])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(row)
    # 稳定排序：先 cf_slug 再 modid，输出对同一份输入逐字节可复现
    uniq.sort(key=lambda r: (r["cf_slug"], r["modid"], r["name_zh"]))

    body_lines = []
    for row in uniq:
        cells = [row[c] for c in COLUMNS]
        body_lines.append("\t".join(cells))
    body = "".join(line + "\n" for line in body_lines)

    sha = hashlib.sha256(body.encode("utf-8")).hexdigest()
    head = [
        "# %s\t%d" % (MAGIC, FORMAT),
        "# version\t%d" % version,
        "# count\t%d" % len(uniq),
        "# sha256\t%s" % sha,
        "# source\t%s" % source_note,
        "# license\tAGPL-3.0(本仓) / 数据出处见 assets/data/modnames.NOTICE.md",
        "# generated\t%s" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "# columns\t" + "\t".join(COLUMNS),
    ]
    return "".join(l + "\n" for l in head) + body, len(uniq), sha


def main():
    ap = argparse.ArgumentParser(description="生成模组中文名表(assets/data/modnames.tsv)")
    ap.add_argument("--source", default="hmcl", choices=["hmcl"], help="上游数据源(目前只有 hmcl)")
    ap.add_argument("--hmcl", help="HMCL 的 mod_data.txt 路径")
    ap.add_argument("--cfpa-info", help="可选：CFPA 的 config/info.json(给 cf_slug 补数字 id)")
    ap.add_argument("--out", help="输出路径(assets/data/modnames.tsv)")
    ap.add_argument("--version", type=int, default=0, help="表版本号(默认取 UTC 当天 YYYYMMDD)")
    ap.add_argument("--stats", action="store_true", help="只打统计，不写文件")
    args = ap.parse_args()

    if not args.hmcl:
        ap.error("--hmcl 是必填的（这份脚本不替你联网抓数据）")
    rows, bad = parse_hmcl(args.hmcl)
    total_rows = len(rows)
    cjk_rows = [r for r in rows if has_cjk(r["name_zh"])]
    dropped_no_cjk = total_rows - len(cjk_rows)
    rows = cjk_rows
    with_key = [r for r in rows if r["cf_slug"] or r["modid"] or r["name_en"]]
    dropped_no_key = len(rows) - len(with_key)
    rows = with_key

    cfpa_hit = 0
    cfpa_total = 0
    note = SOURCE_NOTE
    if args.cfpa_info:
        cfpa_hit, cfpa_total = apply_cfpa_info(rows, args.cfpa_info)
        note = SOURCE_NOTE + " + CFPA config/info.json (CC BY-NC-SA 4.0, 只取数字 id)"

    version = args.version or int(datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d"))
    text, count, sha = build(rows, version, note)

    mods = len(set((r["cf_slug"], r["name_zh"]) for r in rows))
    print("坏行/空名丢弃               : %d" % bad)
    print("展开 modid 后的候选行       : %d" % total_rows)
    print("丢掉(中文名里没有汉字)      : %d" % dropped_no_cjk)
    print("丢掉(一个可查键都没有)      : %d" % dropped_no_key)
    print("最终条数                    : %d" % count)
    print("不同模组数(cf_slug+名字)    : %d" % mods)
    print("带 modid 的条数             : %d" % len([r for r in rows if r["modid"]]))
    print("带 cf_slug 的条数           : %d" % len([r for r in rows if r["cf_slug"]]))
    print("带 cf_id 的条数             : %d" % len([r for r in rows if r["cf_id"]]))
    if args.cfpa_info:
        print("CFPA info.json 补到 id 的条数: %d / 表里 %d 个 slug" % (cfpa_hit, cfpa_total))
    print("version=%d sha256=%s" % (version, sha))

    if args.stats:
        return 0
    if not args.out:
        ap.error("--out 是必填的（或者加 --stats 只看统计）")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    size = os.path.getsize(args.out)
    print("已写出 %s (%d 字节)" % (args.out, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
