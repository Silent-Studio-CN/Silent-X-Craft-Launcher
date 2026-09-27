# 模组中文名表覆盖率报告（自动生成，别手改）
生成时间：2026-09-27T08:41:57Z UTC
- 表：`assets/data/modnames.tsv`（version=20260927，count=12497，实际读到 12497 条）
- 热门口径：Modrinth `index=downloads` + `facets=[["project_type:mod"]]` 前 200 条（数据来源：本地缓存(_modrinth_top200.json)）
- 重新生成：`python tools/modnames_coverage.py --table assets/data/modnames.tsv --top 200`

## 命中率

| 查法 | 命中 | 占前 200 的比例 |
| --- | --- | --- |
| 按 Modrinth slug（表里 slug 那一列） | 0 | 0.0% |
| 按 slug（含 cf_slug 兜底，界面实际走的就是这条） | 93 | 46.5% |
| 按项目名（英文原名整串比对） | 95 | 47.5% |
| **任一键命中（三键依次查的真实结果）** | **102** | **51.0%** |

## 缺哪些（前 200 名里没查到的，共 98 条）

这 98 条按上游原始表分两类：

- **源里压根没有这个模组**：10 条（换数据源才有救，不是查表的锅）
- **源里有、但那个名字是纯拉丁**（Fabric API / Cloth Config API 这种本身就是英文名）：88 条——它们被生成器的「中文名里必须有汉字」规则丢掉了，留着也只是把原名再抄一遍

下面这份清单就是那 98 条（按下载量排）。

| # | slug | 项目名 | 下载量 |
| --- | --- | --- | --- |
| 1 | `fabric-api` | Fabric API | 262047778 |
| 2 | `iris` | Iris Shaders | 179916241 |
| 3 | `cloth-config` | Cloth Config API | 171099103 |
| 4 | `immediatelyfast` | ImmediatelyFast | 125830888 |
| 5 | `yacl` | YetAnotherConfigLib (YACL) | 124718240 |
| 6 | `fabric-language-kotlin` | Fabric Language Kotlin | 120877176 |
| 7 | `entitytexturefeatures` | [ETF] Entity Texture Features | 102627459 |
| 8 | `architectury-api` | Architectury API | 101083028 |
| 9 | `3dskinlayers` | 3D Skin Layers | 77901093 |
| 10 | `continuity` | Continuity | 74222501 |
| 11 | `geckolib` | Geckolib | 70784622 |
| 12 | `zoomify` | Zoomify (Zoom) | 66833267 |
| 13 | `moreculling` | More Culling | 66809473 |
| 14 | `fancymenu` | FancyMenu | 66496154 |
| 15 | `forge-config-api-port` | Forge Config API Port | 66079491 |
| 16 | `collective` | Collective | 65358617 |
| 17 | `puzzles-lib` | Puzzles Lib | 62187558 |
| 18 | `konkrete` | Konkrete | 61764968 |
| 19 | `veinminer-client` | VeinMiner Hotkey | 60705315 |
| 20 | `balm` | Balm | 59838879 |
| 21 | `creativecore` | CreativeCore | 51830139 |
| 22 | `kotlin-for-forge` | Kotlin for Forge | 51360432 |
| 23 | `melody` | Melody | 49902409 |
| 24 | `owo-lib` | oωo (owo-lib) | 48273536 |
| 25 | `bookshelf-lib` | Bookshelf | 46143474 |
| 26 | `essential` | Essential Mod | 44691023 |
| 27 | `badoptimizations` | BadOptimizations | 43424096 |
| 28 | `language-reload` | Language Reload | 40958239 |
| 29 | `moonlight` | Moonlight Lib | 40784050 |
| 30 | `fzzy-config` | Fzzy Config | 40065585 |
| 31 | `terrablender` | TerraBlender | 39764081 |
| 32 | `searchables` | Searchables | 39356079 |
| 33 | `oculus` | Oculus | 36589589 |
| 34 | `debugify` | Debugify | 36232897 |
| 35 | `malilib` | MaLiLib | 35837099 |
| 36 | `embeddium` | Embeddium | 35514224 |
| 37 | `resourceful-lib` | Resourceful Lib | 35228765 |
| 38 | `distanthorizons` | Distant Horizons | 35081358 |
| 39 | `yungs-api` | YUNG's API | 34694501 |
| 40 | `libipn` | libIPN | 34301911 |
| 41 | `modelfix` | Model Gap Fix | 32474063 |
| 42 | `supermartijn642s-config-lib` | SuperMartijn642's Config Lib | 32032775 |
| 43 | `curios` | Curios API | 31404833 |
| 44 | `coroutil` | CoroUtil | 30503463 |
| 45 | `packet-fixer` | Packet Fixer | 29046657 |
| 46 | `emi` | EMI | 28949778 |
| 47 | `forgified-fabric-api` | Forgified Fabric API | 28816548 |
| 48 | `midnightlib` | MidnightLib | 28293330 |
| 49 | `cubes-without-borders` | Cubes Without Borders | 27818173 |
| 50 | `euphoria-patches` | Euphoria Patches | 27738430 |
| 51 | `playeranimator` | playerAnimator | 26904936 |
| 52 | `fallingleaves` | Falling Leaves | 26835719 |
| 53 | `rei` | Roughly Enough Items (REI) | 26190943 |
| 54 | `optigui` | OptiGUI | 25674389 |
| 55 | `puzzle` | Puzzle | 25553353 |
| 56 | `cit-resewn` | CIT Resewn | 24853471 |
| 57 | `resourceful-config` | Resourceful Config | 24790417 |
| 58 | `trinkets` | Trinkets | 24208156 |
| 59 | `e4mc` | e4mc | 24079827 |
| 60 | `capes` | Capes | 23928279 |

（只列前 60 条；完整清单跑一次上面的命令就有。）

## 命中的样例（前 20 条）

| slug | 项目名 | 我们给的中文名 | 走的哪个键 |
| --- | --- | --- | --- |
| `sodium` | Sodium | 钠 | cf_slug |
| `entityculling` | Entity Culling | 实体渲染机制优化 | cf_slug |
| `ferrite-core` | FerriteCore | 铁氧体磁芯 | name |
| `modmenu` | Mod Menu | 模组菜单 | cf_slug |
| `lithium` | Lithium | 锂 | cf_slug |
| `xaeros-minimap` | Xaero's Minimap | Xaero的小地图 | cf_slug |
| `sodium-extra` | Sodium Extra | 钠 · 扩展 | cf_slug |
| `xaeros-world-map` | Xaero's World Map | Xaero的世界地图 | cf_slug |
| `entity-model-features` | [EMF] Entity Model Features | 实体模型特性 | cf_slug |
| `appleskin` | AppleSkin | 苹果皮 | cf_slug |
| `not-enough-animations` | Not Enough Animations | 更多动画 | cf_slug |
| `veinminer` | VeinMiner | 连锁采矿/矿脉矿工 | cf_slug |
| `reeses-sodium-options` | Reese's Sodium Options | Reese的钠视频界面 | cf_slug |
| `jei` | Just Enough Items (JEI) | JEI物品管理器 | cf_slug |
| `modernfix` | ModernFix | 现代化修复 | cf_slug |
| `simple-voice-chat` | Simple Voice Chat | 简单的语音聊天 | cf_slug |
| `jade` | Jade | 玉 | cf_slug |
| `dynamic-fps` | Dynamic FPS | 动态 FPS | cf_slug |
| `placeholder-api` | Text Placeholder API | 文本占位符 API | name |
| `mouse-tweaks` | Mouse Tweaks | 鼠标手势 | cf_slug |
