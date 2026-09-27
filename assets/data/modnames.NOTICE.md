# assets/data/modnames.tsv 的出处与许可

这份文件是**模组中文名表**（查表代码见 `include/sxcl/modnames.h`，机制与取舍见 `docs/29-模组中文名与数据来源.md`）。
它是一张**随包带的静态表**，不是运行时翻译 —— 与 PCL 的做法同形（PCL 也是随包带一张
`PCLCS/Resource/WikiEntries.txt`，"有就用中文名、没有就用原名"）。**但这份表不是抄 PCL 的**：
PCL 那张表的许可证说不清，我们不用；我们的来源、处理方式与残余风险都写在这里。

## 数据来源

**Hello Minecraft! Launcher 的 `HMCL/src/main/resources/assets/mod_data.txt`**（HMCL 仓库以 GPL-3.0 分发）。

那份文件自己的文件头逐字写着（原文照抄）：

```
#
# Hello Minecraft! Launcher
# Copyright (C) 2025 huangyuhui <huanghongxun2008@126.com> and contributors
#
# mcmod.cn
# Copyright (C) 2025. All Rights Reserved.
#
```

即：**HMCL 自己声明这份数据的内容版权属于 mcmod.cn**。我们不藏这件事 —— 所以：

* 我们生成的每一行都带 `mcmod_id`（MC 百科的条目编号，列名 `mcmod_id`），出处**逐行可回溯**；
* 我们**没有**直接抓 mcmod.cn（它没有公开 API，也没有给出再分发许可；robots.txt 只说明爬虫礼节，
  不等于授权），也**没有**使用 PCL 的 `WikiEntries.txt`（许可证不明，用户 2026-09-27 点名不许抄）；
* 候选数据源的实测（可达性 HTTP 码）与许可逐条列在 `docs/29` 里，包括被否掉的 CFPA（它只有翻译键，
  实测 11108 个语言文件里只有 4 个带 `modmenu.nameTranslation.*`，**拿不到模组显示名**）。

## 我们做了什么处理

* 只取六个字段：`cf_slug;mcmod_id;modids;中文名;英文名;缩写`；
* **丢掉"中文名里没有汉字"的行**（17207 行）—— 那些行的"中文名"就是英文原名本身（例如 `Fabric API`），
  留着等于把原名再抄一遍，没有价值，也不符合"绝不猜"；
* 一个模组的多个 `modid` **展开成多行**（同一中文名），这样按 modid 查（本地 jar）也能命中；
* 丢掉一个可查键都没有的行（58 行）；
* 结果：**12497 行 / 11660 个模组**，920 KB。

生成命令（可复现）：

```
python tools/modnames_build.py --source hmcl \
    --hmcl <HMCL 仓库>/HMCL/src/main/resources/assets/mod_data.txt \
    --out assets/data/modnames.tsv
```

覆盖率（Modrinth 按下载量前 200 条）：见 `docs/modnames_coverage.md`（自动生成）。

## 这份表以什么许可分发

本仓库自己的许可是 AGPL-3.0（见根目录 `LICENSE`）。这份**派生数据**沿用同一声明，
并**保留上表中 mcmod.cn 的版权声明与逐行编号**。

## 如果上游主张权利，怎么办（我们留好的退路）

**直接删掉这个文件就行**：

* 表不存在 / 读不出来时，客户端**不报错、不阻塞启动**，只是"这次没有中文名"（模组名照原样显示）
  —— 这条路径有单测（`src/services/modnames/tests/modnames_test.c` 的"两份都没有 = 空表"）；
* 想换成别的数据源也**不用发版**：把新表（格式见 `docs/28` 的 `/meta/modnames.tsv`）放到云端就行，
  客户端校验 sha256/条数后才换，校验不过继续用随包那份。
