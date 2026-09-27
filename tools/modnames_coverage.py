#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""(C) Silent X Craft Launcher
Copyright by SilentStudio.
All rights reserved.

模组中文名表的覆盖率报告：拿"前 N 个热门模组"逐条对着我们的表查一遍，报命中率与缺哪些。

"热门"的口径写死成**Modrinth 按下载量排序的 project_type:mod 前 N 条**（默认 N=200）——
换口径要改这里的 URL，报告里会原样印出来，跑的人知道自己看的是什么。

规范化规则与 C 版 sxcl_modnames_lookup 是**同一套**（大小写、空格、- _ . ' : 等标点不参与比较，
不做模糊匹配）—— 报告里的命中率就是界面上真实的命中率，不是乐观估计。

用法：
    python tools/modnames_coverage.py --table assets/data/modnames.tsv --top 200
    python tools/modnames_coverage.py --table assets/data/modnames.tsv --cache build/modrinth_top200.json
"""

import argparse
import datetime
import json
import os
import sys
import urllib.parse
import urllib.request

MODRINTH = "https://api.modrinth.com/v2/search"


def norm_key(text):
    """与 C 版 normalize_key 同一套规则。"""
    drop = set(" \t\r\n\v\f-_.'\":;!?,&()[]{}" + chr(92) + "/+|~*#@%^=<>")
    out = []
    for ch in text:
        if ch in drop or ch == "\u3000":
            continue
        out.append(ch.lower() if "A" <= ch <= "Z" else ch)
    return "".join(out)


def plain_title(text):
    """报告里不留 emoji：Modrinth 的标题里真的带（例如 Jade 那个放大镜），
    但我们的文档/界面口径是不出现 emoji —— 引号里的数据也照这个规矩来。"""
    out = []
    for ch in text:
        cp = ord(ch)
        if 0x1F000 <= cp <= 0x1FAFF or 0x2600 <= cp <= 0x27BF or 0x2B00 <= cp <= 0x2BFF or cp == 0xFE0F:
            continue
        out.append(ch)
    return "".join(out).strip()


def load_table(path):
    """读我们的表：返回 (slug, cf_id, cf_slug, modid, name_zh, name_en, source) 的索引。"""
    idx = {"slug": {}, "cf_id": {}, "cf_slug": {}, "modid": {}, "name": {}}
    meta = {}
    count = 0
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            if not line:
                continue
            if line.startswith("#"):
                body = line[1:].strip()
                parts = body.split("\t", 1)
                if len(parts) == 2:
                    meta[parts[0].strip()] = parts[1].strip()
                continue
            cells = line.split("\t")
            while len(cells) < 7:
                cells.append("")
            slug, cf_id, cf_slug, modid, name_zh, name_en = [c.strip() for c in cells[:6]]
            if not name_zh:
                continue
            count += 1
            for key, value in (("slug", slug), ("cf_id", cf_id), ("cf_slug", cf_slug),
                               ("modid", modid), ("name", name_en), ("name", name_zh)):
                k = norm_key(value)
                if k and key:
                    idx[key].setdefault(k, name_zh)
    return idx, meta, count


def fetch_top(top, cache_path):
    if cache_path and os.path.exists(cache_path):
        with open(cache_path, "r", encoding="utf-8") as fh:
            return json.load(fh), "本地缓存(" + os.path.basename(cache_path) + ")"
    hits = []
    offset = 0
    while len(hits) < top:
        limit = min(100, top - len(hits))
        url = ("%s?facets=%s&index=downloads&limit=%d&offset=%d"
               % (MODRINTH, urllib.parse.quote('[["project_type:mod"]]'), limit, offset))
        req = urllib.request.Request(url, headers={"User-Agent": "sxcl-modnames-coverage/1"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        page = data.get("hits", [])
        if not page:
            break
        hits.extend(page)
        offset += len(page)
        if len(page) < limit:
            break
    payload = {"url": "%s?facets=[[\"project_type:mod\"]]&index=downloads&limit=100" % MODRINTH,
               "fetched": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
               "hits": hits}
    if cache_path:
        os.makedirs(os.path.dirname(os.path.abspath(cache_path)), exist_ok=True)
        with open(cache_path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=1)
    return payload, "live"


def main():
    ap = argparse.ArgumentParser(description="模组中文名表覆盖率报告")
    ap.add_argument("--table", default="assets/data/modnames.tsv")
    ap.add_argument("--top", type=int, default=200)
    ap.add_argument("--cache", default="build/modnames_modrinth_top.json")
    ap.add_argument("--out", default="docs/modnames_coverage.md")
    ap.add_argument("--raw-hmcl", default="",
                    help="可选：上游原始表(mod_data.txt)，用来区分「源里就没有」与「源里有、但中文名是纯拉丁」")
    args = ap.parse_args()

    idx, meta, count = load_table(args.table)

    raw_idx = None
    if args.raw_hmcl:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        sys.dont_write_bytecode = True  # 别在 tools/ 里留 __pycache__（那不该进仓库）
        import modnames_build as mb  # 同目录的生成器：解析规则只写一份
        raw_rows, _bad = mb.parse_hmcl(args.raw_hmcl)
        raw_idx = {"slug": set(), "name": set()}
        for row in raw_rows:
            if row["cf_slug"]:
                raw_idx["slug"].add(norm_key(row["cf_slug"]))
            for value in (row["name_en"], row["name_zh"]):
                key = norm_key(value)
                if key:
                    raw_idx["name"].add(key)
    payload, src = fetch_top(args.top, args.cache)
    hits = payload["hits"]

    rows = []
    stat = {"slug": 0, "cf_slug": 0, "slug_or_cf": 0, "name": 0, "any": 0}
    for hit in hits:
        slug = hit.get("slug", "") or ""
        title = hit.get("title", "") or ""
        downloads = hit.get("downloads", 0) or 0
        by_slug = idx["slug"].get(norm_key(slug))
        by_cf_slug = idx["cf_slug"].get(norm_key(slug))
        by_name = idx["name"].get(norm_key(title))
        got = by_slug or by_cf_slug or by_name
        if by_slug:
            stat["slug"] += 1
        if by_cf_slug:
            stat["cf_slug"] += 1
        if by_slug or by_cf_slug:
            stat["slug_or_cf"] += 1
        if by_name:
            stat["name"] += 1
        if got:
            stat["any"] += 1
        rows.append({"slug": slug, "title": title, "downloads": downloads,
                     "zh": (plain_title(got) if got else None),
                     "by": "slug" if by_slug else ("cf_slug" if by_cf_slug else ("name" if by_name else ""))})

    total = len(rows)
    for r in rows:
        r["title"] = plain_title(r["title"])
    miss = [r for r in rows if not r["zh"]]
    # 缺的里面再分两类：源里压根没有这个模组 / 源里有、但那个名字是纯拉丁（等于没有中文名）
    miss_no_source = 0
    miss_latin_only = 0
    if raw_idx is not None:
        for r in miss:
            if norm_key(r["slug"]) in raw_idx["slug"] or norm_key(r["title"]) in raw_idx["name"]:
                miss_latin_only += 1
            else:
                miss_no_source += 1
    lines = []
    lines.append("# 模组中文名表覆盖率报告（自动生成，别手改）\n")
    lines.append("生成时间：%s UTC\n" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    lines.append("- 表：`%s`（version=%s，count=%s，实际读到 %d 条）\n"
                 % (args.table, meta.get("version", "?"), meta.get("count", "?"), count))
    lines.append("- 热门口径：Modrinth `index=downloads` + `facets=[[\"project_type:mod\"]]` 前 %d 条（数据来源：%s）\n"
                 % (total, src))
    lines.append("- 重新生成：`python tools/modnames_coverage.py --table %s --top %d`\n"
                 % (args.table, args.top))
    lines.append("\n## 命中率\n\n")
    lines.append("| 查法 | 命中 | 占前 %d 的比例 |\n| --- | --- | --- |\n" % total)
    lines.append("| 按 Modrinth slug（表里 slug 那一列） | %d | %.1f%% |\n"
                 % (stat["slug"], 100.0 * stat["slug"] / max(1, total)))
    lines.append("| 按 slug（含 cf_slug 兜底，界面实际走的就是这条） | %d | %.1f%% |\n"
                 % (stat["slug_or_cf"], 100.0 * stat["slug_or_cf"] / max(1, total)))
    lines.append("| 按项目名（英文原名整串比对） | %d | %.1f%% |\n"
                 % (stat["name"], 100.0 * stat["name"] / max(1, total)))
    lines.append("| **任一键命中（三键依次查的真实结果）** | **%d** | **%.1f%%** |\n"
                 % (stat["any"], 100.0 * stat["any"] / max(1, total)))
    lines.append("\n## 缺哪些（前 %d 名里没查到的，共 %d 条）\n\n" % (total, len(miss)))
    if raw_idx is not None:
        lines.append("这 %d 条按上游原始表分两类：\n\n" % len(miss))
        lines.append("- **源里压根没有这个模组**：%d 条（换数据源才有救，不是查表的锅）\n" % miss_no_source)
        lines.append("- **源里有、但那个名字是纯拉丁**（Fabric API / Cloth Config API 这种本身就是英文名）：%d 条"
                     "——它们被生成器的「中文名里必须有汉字」规则丢掉了，留着也只是把原名再抄一遍\n"
                     % miss_latin_only)
        lines.append("\n下面这份清单就是那 %d 条（按下载量排）。\n\n" % len(miss))
    lines.append("| # | slug | 项目名 | 下载量 |\n| --- | --- | --- | --- |\n")
    for i, r in enumerate(miss[:60], 1):
        lines.append("| %d | `%s` | %s | %d |\n" % (i, r["slug"], r["title"].replace("|", "/"), r["downloads"]))
    if len(miss) > 60:
        lines.append("\n（只列前 60 条；完整清单跑一次上面的命令就有。）\n")
    lines.append("\n## 命中的样例（前 20 条）\n\n")
    lines.append("| slug | 项目名 | 我们给的中文名 | 走的哪个键 |\n| --- | --- | --- | --- |\n")
    for r in [x for x in rows if x["zh"]][:20]:
        lines.append("| `%s` | %s | %s | %s |\n"
                     % (r["slug"], r["title"].replace("|", "/"), r["zh"].replace("|", "/"), r["by"]))
    text = "".join(lines)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    print("前 %d 条：任一键命中 %d（%.1f%%）；slug(含 cf_slug 兜底) %d / 名字 %d；缺 %d 条"
          % (total, stat["any"], 100.0 * stat["any"] / max(1, total), stat["slug_or_cf"],
             stat["name"], len(miss)))
    if args.out:
        print("报告已写出：" + args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
