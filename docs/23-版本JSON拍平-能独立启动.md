# 23 版本 JSON 拍平：让装出来的版本能独立启动

> 用户原话：「1. 两家的都产出能独立启动的版本 JSON 我们肯定要学，忘了说了**以后精读加上 FCL**」
> 「MOD 走 PCL 线路」。
> 本文记录这件事：为什么必须做、查出来的**三个真实缺陷**、做法、验收（单测 + 真机端到端）、还剩什么。

## 0 一句话

装加载器产出的 `versions/<实例>/<实例>.json` 现在是**自包含**的（PCL / HMCL / FCL 三家的共同形态）：
`id` == 目录名、**没有 `inheritsFrom`**、**没有 `jar`**、多一个 `clientVersion`；
加载器的库与原版的库合并、加载器独有的键（`processors`/`spec`/`patches`）原样保留。
启动层同时补了兜底：别人的（继承式）版本会在内存里合并一次，实例自己没有 jar 时改用原版的 jar。

---

## 1 为什么必须做（三个真实缺陷，各有证据）

### 1.1 启动层**从来不解析继承**

`grep -i inherit src/services/launch/` 只匹配到进程句柄继承（`process_win32.c` 的 `bInheritHandle`）。
`driver.c:309` 只读实例自己那份 JSON，`args.c:561`（libraries）、`:919-923`（arguments / mainClass）、
`:516`（assetIndex）全部取自它。于是"只有几个加载器库 + `inheritsFrom`"的版本：
**缺原版库、`assetIndex` 退化成 `legacy`**。

### 1.2 真机证据（这台机器上就有）

`%APPDATA%\.minecraft\versions\fabric-loader-0.19.5-26.3\fabric-loader-0.19.5-26.3.json`：

| 字段 | 值 |
| --- | --- |
| `inheritsFrom` | `26.3` |
| `clientVersion` | 空 |
| `assetIndex` | 空 |
| `libraries` | **7** 条（只有 Fabric 自己那几件） |
| 同目录 jar | **0 个**（`jar=0`） |
| `versions/26.3` | **不存在** |

也就是：这份版本既不是独立版本，前置也根本不在。它现在**启动不了**。

### 1.3 Fabric / Quilt 安装器的参数**全是双横线 —— 从来没成功过**

实测（fabric-installer **1.1.2**，2026-09-22，Java 17）：

| 怎么写 | 实测结果 |
| --- | --- |
| 双横线 `--mcversion --loader --dir --name` | 参数**整段被忽略**：安装器退到"当前最新正式版"（那天是 **26.3**），再去版本目录找 `launcher_profiles.json`，报 `Could not find a valid launcher profile .json`，**退出码 1** |
| 单横线 `-mcversion -loader -dir -name`，且 `-dir` = **游戏目录** | `Installing 1.21.11 with fabric 0.19.5` + `Creating profile`，**退出码 0**，生成 `versions/fabric-loader-0.19.5-1.21.11/` |

这同时解释了 §1.2 那个怪名字：实例里的 **`26.3` 不是用户选的版本**，是安装器自己退到的默认最新版
（用户选的 MC 是 1.21.11）。**双横线那一版从来没有装成功过。**

### 1.4 附带发现的第三个坑：引擎把"自己刚放下的原版 JSON"当成了安装产物

`install.c:845` 的 `version_json` 阶段把原版 JSON 落到 `versions/<实例>/<实例>.json`，
而 `variant_succeeded`（`installer.c:1023-1035`）判成功用的正是 `dir_has_json(instance_dir)` ——
**安装器什么都不干也会被判"安装成功"**。实测：装完 `1.21.11-flat2`，版本 JSON 还是原版那份
（107 个库、`mainClass=net.minecraft.client.main.Main`），Fabric 的 JSON 静静躺在旁边的
`versions/fabric-loader-0.19.5-1.21.11/` 里没被收编。

---

## 2 做法

### 2.1 合并引擎（纯逻辑，可单测）

```c
/* include/sxcl/loader.h */
int sxcl_loader_flatten_json(const sxcl_json_value *loader_root, const sxcl_json_value *base_root,
                             const char *instance_name, const char *base_version, char **out_text,
                             char *err, size_t err_len);
```

实现放在 `src/services/modloader/profiles.c`（与 JSON 序列化器同文件，复用 `dbuf`/`dump_value`）。
逐键规则：

| 键 | 规则 |
| --- | --- |
| `id` | `instance_name`（== 目录名） |
| `clientVersion` | `base_version`（PCL 的拍平标记，也是我们实例扫描最优先看的字段） |
| `inheritsFrom` / `jar` | **丢掉**（这两个键正是"不是独立版本"的标记） |
| `libraries` | 加载器的在前 + 原版里没被 `group:artifact` 覆盖的（Java classpath 先到先得 ⇒ 加载器钉的版本必须在前） |
| `arguments` | `jvm`/`game` 各自"原版在前 + 加载器在后"，整串相同的项只留一次 |
| `mainClass` / `minecraftArguments` | 加载器有就用加载器的 |
| 其余键 | 以原版为准（`assetIndex`/`assets`/`downloads`/`logging`/`javaVersion`/…） |
| 加载器独有的键 | 原样带上（`processors`/`spec`/`data`/…） |

**合不了就报错**（`base_root` 为空 → `SXCL_LOADER_ERR_ARG` + "没有可合并的原版版本 JSON"），
不退回旧行为、也不假装拍平过。

### 2.2 合并基准必须在安装器开工**之前**读下来

`install.c:845` 把原版 JSON 放在 `versions/<实例>/<实例>.json`，安装器随后会把它盖掉 ——
所以 `sxcl_loader_install` 一进来（校验完参数、拼好路径）就：

1. `load_base_doc()` 把它读进 `ctx.base_doc`，并从 JSON 的 `id` 取到 `ctx.base_id`（比调用方填的 base_version 更准）；
2. **把它从版本目录里挪走** `drop_stale_vanilla()`。三条理由（写进代码注释了）：
   ① 不让 `variant_succeeded` 的 `dir_has_json` 被骗（§1.4）；
   ② 不让它和安装器写出的 JSON 抢同一个文件名（`normalize` 把每份 JSON 都改成 `<实例名>.json`，
   谁赢看目录顺序 —— 不确定）；
   ③ 装失败时目录里不留"看着像装好了"的东西。

兜底路径：`versions/<原版>/<原版>.json`（原版自己单独装过的机器上有）。两处都必须"**看着像原版**"
（判据：没有 `inheritsFrom` **且** 有 `downloads.client`）才算合并基准 —— 宁可拍不了，也不拿加载器的 JSON 瞎合。

### 2.3 两条安装路都拍平

* **方式 B**（`extract_install`）：版本 JSON 直接写成拍平后的（原先把 `inheritsFrom` 补上，现在是合并）；
* **方式 A**（安装器产物）：`normalize_instance` 之后再 `flatten_instance_on_disk()` 拍一次；
  **已经是独立版本的会被跳过**（再合并会把原版的游戏参数与库再加一遍，那是损坏不是拍平）；
* `adopt_generated` 收编安装器生成的目录之后清掉那份原版 JSON（同 §2.2 的 ②）。

### 2.4 启动层兜底（`driver.c`）

继承式版本 JSON（HMCL/FCL 装的、我们老版本装的、手改过的）在**内存里**用同一条规则合并一次；
实例自己没有 jar 时改用原版的 jar（`ctx.client_jar`）—— 继承语义里"没有自己的 jar"就是这个意思。

> 踩到的坑：第一版**崩在 `ucrtbase strnlen`（0xC0000005）**。原因：`inheritsFrom` 的指针指向旧文档，
> 合并成功后旧文档被释放，指针成了野指针（use-after-free）。改成先把 id 拷进本地缓冲再合并。

### 2.5 Fabric / Quilt 参数改成单横线

`installer.c` 的 Fabric 分支：`-mcversion` / `-loader` / `-dir`（=**游戏目录**）/ `-name`。
Quilt 同构（同一分支）。测试里那 4 条断言同步改成新形态，并把实测依据写进注释。

---

## 3 验收

### 3.1 单测

* 核心 **46/46**、UI **49/49**（`ctest --test-dir build -C Release` / `build-ui`）。
* `tests/loader_test.c` 新增 `test_flatten()` 一节，逐条钉住：没有 `inheritsFrom`/`jar`、
  `id`/`clientVersion` 正确、库顺序（加载器那条 `asm:9.10.1` 必须排在原版 `asm:9.3` 前面且后者被去重）、
  原版独有的库保留、`assetIndex`/`downloads`/`logging`/`javaVersion` 都在、重复的 JVM 参数只写一次、
  `processors`/`spec` 原样保留、**没有原版时报错而不是假装成功**。该测试总数 309 项断言，全过。

### 3.2 端到端（真网络 + 真 Java 17）

```
build-ui\src\ui\Release\sxcl_fabric_installer_test.exe     （SXCL_ACCEPT_NETWORK=1）
  MC=1.21.11  实例=1.21.11-flat3  fabric-loader=0.19.5  Java=D:/jdk17/bin/java.exe
```

结果：`完成:版本 1.21.11-flat3 安装成功(阶段 8/8 · 95.5 MB · natives 138 个)`，退出码 0。

产物 `versions/1.21.11-flat3/`：

| 字段 | 值 | 说明 |
| --- | --- | --- |
| `id` | `1.21.11-flat3` | == 目录名 |
| `clientVersion` | `1.21.11` | 拍平标记 |
| `inheritsFrom` / `jar` | **不存在**（原文里也搜不到 `inheritsFrom`） | 独立版本 |
| `mainClass` | `net.fabricmc.loader.impl.launch.knot.KnotClient` | 加载器的（不是原版那个） |
| `libraries` | **115** 条，第一条 `org.ow2.asm:asm:9.10.1` | 加载器钉的版本在前，原版 107 条里被同名覆盖的已去重 |
| `assetIndex` / `assets` | `29` / `29` | 原版资源索引（不是 `legacy`） |
| `downloads.client` | 在 | 原版下载信息 |
| `1.21.11-flat3.jar` | 31,152,600 字节 | 原版客户端 jar 就在实例目录里 |

### 3.3 **独立启动**的证明

该游戏目录里**没有 `versions/1.21.11`**（只有这个实例）。仍然：

```
sxcl-dl launch 1.21.11-flat3 <game> --java D:/jdk17/bin/java.exe --dry-run --offline Tester
```

* classpath 第一条 = `...\versions\1.21.11-flat3\1.21.11-flat3.jar`；
* `--assetsDir` / `--assetIndex 29` 都在；
* 主类 = `net.fabricmc.loader.impl.launch.knot.KnotClient`，并带上 Fabric 自己的 `-DFabricMcEmu= net.minecraft.client.main.Main`；
* natives 解出 **138** 个文件；
* 结论：`只准备不启动:Java 与参数都已就绪,options.txt 已写入 default。`

### 3.4 继承式兜底的证明

造一份"子版本"：`versions/child1/child1.json` = `{"inheritsFrom":"base1","mainClass":"…KnotClient","libraries":[1 条]}`，
父版本 `versions/base1/base1.json` = 上面那份拍平结果，`base1.jar` 用硬链接指向真实 jar（31 MB）。
`child1` **自己没有 jar、只声明 1 个库**，dry-run 结果：

* classpath 第一条 = `...\versions\base1\base1.jar` ← **jar 兜底生效**；
* natives 仍然解出 **138** 个 ← 父版本的库确实被合并进来了（child1 只声明了 1 条）；
* 主类还是 child1 自己的 `KnotClient`。

---

## 4 还没做（如实列出）

1. **HMCL 的 `patches` 语义**：我们的拍平会**保留**加载器独有的键（`processors`/`spec`/`patches`），
   但还没有"卸载/替换单个加载器"的入口（见 `docs/22` B10③）。
2. **§1.2 那种"前置版本不在"的存量实例救不回来** —— 父版本 JSON 都不在，合并无从谈起；只能重装，
   而重装前要先清残骸（`docs/22` B2 的口径问题）。**本机那个 `fabric-loader-0.19.5-26.3` 就属于这一类。**
3. **超长参数在日志里被截断**：classpath 那一行只印出前约 1.5 KB（实测 1574 字符，结尾被切在半截路径上）。
   想核对完整 classpath 时不够用 —— 这是日志层的问题，与本次拍平无关。
4. 启动层的合并**只影响本次启动、不写盘**。要不要把合并结果写回去（=顺手修好别人的实例）是产品决定，
   现在没做。
5. 安装器生成的目录（`versions/fabric-loader-<loader>-<mc>/`）**收编后原样留着**，
   会在版本列表里显示成一个"幽灵版本"。`copy_dir_files` 是**非递归**的（`installer.c:322-356`），
   删源目录有丢数据的风险，所以这次没动它。
