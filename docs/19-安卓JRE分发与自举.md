# 19. 安卓 JRE 的分发与自举(自托管清单 + tar.xz 安装 + 进程内起 JVM)

> 本轮**没有**下载任何 JRE 包(来源定下来之前不许下),没有做任何 git 操作,
> 没有改系统设置、没有碰 /storage/emulated/0/FCL、没有装任何 MC 版本。
> 本文的每条结论都能在仓库里找到对应的代码与用例;没做到的写在 §8,不吹。

## 0. 一句话路线

安卓的 JRE 用**我们自己的来源**(用户自己托管),启动器去**下载**;JVM 只能**进程内**起
(dlopen libjli.so + JLI_Launch),因为应用自己的 untrusted_app 域**不许 exec 私有目录里的文件**
(SELinux 原文见 docs/18 §4.2)。

三块交付:

| 块 | 内容 | 代码/文档 |
|---|---|---|
| A | 远端目录结构 + 清单 schema + 打包/上传清单 | 本文 §1~§4,tools/jre_pack.py,docs/fixtures/java-index.example.json |
| B | 客户端链路:来源可配 -> 下载(SHA 强校验)-> 解包(tar.xz)-> 落 jre.json -> 进度/错误 | include/sxcl/{jre_hosted,tar,xz}.h,src/services/install/jre_hosted.c,src/core/archive/{xz,tar}.c |
| C | 自举:进程内 dlopen libjli.so + JLI_Launch(自写) | include/sxcl/jvm.h,src/services/launch/jvm.c,android/jni/sxcl_jvm_bootstrap.c |

## 1. 远端落位(**GitHub 单点**;SilentCloud 那条已按用户要求去掉)

用户决定:SC 要删掉,只用 GitHub。**index.json 与包放同一个仓库**(根 = 现在 out/ 的内容),
下载走 GitHub raw,备用镜像用 gh-proxy 前缀:

    https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/index.json          <- 清单(sxcl.jre.index/1)
    https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/jre17/universal.tar.xz
    https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/jre17/bin-arm64.tar.xz
    https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/jre17/version
    备用(mirrors):https://gh-proxy.com/https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/<同一条相对路径>

这条地址**就是代码里的编译期默认值**(`SXCL_JRE_INDEX_URL_DEFAULT`,
由 `SXCL_JRE_GH_REPO` / `SXCL_JRE_GH_BRANCH` / `SXCL_JRE_GH_SUBDIR` 拼出来,
相对根是 `SXCL_JRE_RAW_BASE_DEFAULT`),都有单测钉住;换仓库/换分支只要改这三个宏或填设置项。

> 落位已定:**Silent-Studio-CN/index** 的 **SXCL/jre/** 子目录(用户选的)。
> 三级可配照旧:**显式 > 环境变量 `SXCL_JAVA_JRE_INDEX_URL` > 编译期默认**;
> 设置项 `java.jre_index_url` 与显式参数同一条优先级链(见 §6)。

**源包已经产好了**:Termux 那一路在 `D:\SilentStudio\_termux_jre\out\` 下给出 jre17/jre21/jre25,
用户**把这个 out 目录的内容整体传进仓库根**、相对路径保持不变即可:

    index.json                        <- 清单(sxcl.jre.index/1);packages[].url 全是**相对路径**
    jre17/  universal.tar.xz  bin-arm64.tar.xz  version  NOTICE/**
    jre21/  universal.tar.xz  bin-arm64.tar.xz  version  NOTICE/**
    jre25/  universal.tar.xz  bin-arm64.tar.xz  version  NOTICE/**

关于体积的一句实话(如实):jre17+jre21+jre25 的包合计约 **119 MB**(31.1+4.6 / 33.1+5.9 / 38.5+6.4),
对 GitHub 单文件 100 MiB 上限没问题,但仓库会变大;真要瘦身就按架构拆仓库、或只保留用户实际要玩的那几档。
清单里的 `packages[].url` 是相对路径,**换域名/换分支不用改清单**。

(用 `tools/jre_pack.py` 重打包时会多出 `*.sha256` 侧车 —— **可选**,不依赖它。)
## 2. 清单:两份各管什么(以**正式来源**为准,别打架)

| 清单 | 谁产出的 | 谁消费 | 状态 |
|---|---|---|---|
| **`sxcl.jre.index/1`**(安卓 JRE,**正式来源**) | Termux 那一路的产出(`D:\SilentStudio\_termux_jre\out\index.json`) | `sxcl_jre_*`(本轮的客户端链路) | **已接线并真读通过**(见 §7 证据) |
| `SXCL/Java_index.json`(桌面 Oracle JDK) | index 仓库既有那份,保持不动 | 桌面 JDK 那条线 | 安卓档(`platforms.android`)可作为**备用**扩展,不作为我们的安卓 JRE 来源 |
| `{"schema":1,"components":[…]}`(自带托管) | `tools/jre_pack.py` | `sxcl_jre_*` | 仍然支持(重打包/临时托管用),但**不是**正式来源 |

三种形式客户端**都吃**,判定顺序固定:`schema` 是字符串 -> 先看它;否则看 `versions`;否则看 `components`(必须是**数组**)。
不认识的 `schema` 串直接报错并说明只认哪一版(不猜、不降级)。

### 2.1 正式来源:`sxcl.jre.index/1` 的字段(照实读的那一份)

    {
      "schema": "sxcl.jre.index/1",
      "abi": "arm64-v8a",
      "generated": "2026-09-21T12:32:10Z",
      "components": {                       <-- **对象**,按组件名索引(不是数组)
        "jre17": {
          "component": "jre17",
          "major": 17,
          "version": "17.0.20",
          "java_version": "17.0.20",
          "abi": "arm64-v8a",
          "implementor": "Termux",
          "id": "jre17/17.0.20/arm64-v8a/9f87f8de24c52431",
          "file_count": 339,
          "installed_bytes": 158165753,
          "shim_dir": "lib",
          "packages": [                     <-- **运行时本体**:我们只下它
            { "name": "universal.tar.xz",  "size": 31132644,
              "sha256": "aa11db5f…", "sha1": "5161b287…", "url": "jre17/universal.tar.xz" },
            { "name": "bin-arm64.tar.xz",  "size": 4630460,
              "sha256": "cd6a527c…", "sha1": "07f3bde8…", "url": "jre17/bin-arm64.tar.xz" }
          ],
          "version_file": { "name": "version", "size": 470, "sha256": "06f58347…", "url": "jre17/version" },
          "notice": [ 290 条许可材料,形状与 packages 一样 ],
          "manifest": { "files": { "bin/java": { … } } },
          "source": { "kind": "termux-deb", … }
        },
        "jre21": { … }, "jre25": { … }
      }
    }

客户端怎么消费它(逐条对齐,都有单测):

| 清单字段 | 客户端行为 |
|---|---|
| `packages[].url` | **相对路径**:相对清单地址解析(`sxcl_jre_join_url`);写成绝对 URL 也照用 |
| `packages[].size/sha256/sha1` | 交给引擎**强校验**;sha256 的三种形态见 §3 |
| `component / version / major / abi` | 挑组件(按 `major` 或 `component` 精确匹配)、判重装(比 `version`)、卡 ABI(`arm64-v8a` 与本机 `arm64` 归一化后相等) |
| `id` | 原样记进 `jre.json` 的 `indexId`(可追溯) |
| 落盘位置 | `<配置目录>/runtime/<component>/<version>/<abi>`(即清单里的 `id` 去掉最后那截哈希),多版本并排 |
| `installed_bytes` | 记进 `jre.json`;**不做判据**(判据只有 `version`) |
| `version_file` / `notice[]` / `manifest` / `source` | **不进安装流程**(运行时不需要);形状与 packages 一样,将来做"许可/溯源"页时照同一条链路抓即可 |

### 2.2 备用:既有 `SXCL/Java_index.json` 的安卓档(保持不动)

那份清单是**桌面 Oracle JDK** 用的,本轮**没有改动它**。若将来要把它当备用来源,安卓档的形状如下
(客户端已经支持并有单测;只在 `versions.<major>.platforms` 下新增 android 那一档):

    "android": {
      "arm64": { "format": "tar.xz", "version": "17.0.20",
                  "size": 31132644, "size_mb": 29.69,
                  "sha256": "<64 位十六进制>", "sha1": "<40 位十六进制>",
                  "url": "https://cloud.silentstudio.cn/api/v1/<owner>/…/jre17/universal.tar.xz",
                  "mirrors": ["https://raw.githubusercontent.com/…/jre17/universal.tar.xz"],
                  "extra_files": [ { "name": "bin-arm64.tar.xz", … } ] } }

字段表(新增/扩展的**加粗**;windows/macos/linux 那几档原样保留):

| 字段 | 必填 | 含义 |
|---|---|---|
| **version** | 是 | 构建版本串,重装判据 |
| **size** | 是 | 字节数;缺了才退到 `size_mb`(不精确,不推荐) |
| **sha256** | 见 §3 | 校验信息(字面哈希 / 侧车 URL / 缺失) |
| **sha1** | 否 | 同样三种形态 |
| url | 是 | 包地址(**绝对地址**优先) |
| mirrors | 否 | 第二条候选(另一处落位) |
| **extra_files[]** | 否 | 同一个 ABI 还要额外解一份时(universal + bin-<abi>) |

模板文件:`docs/fixtures/java-index.example.json`。



## 3. sha256 的三种形态与**信任口径**

| 形态 | 例子 | 行为 |
|---|---|---|
| 1. **字面哈希(默认、推荐)** | ` "sha256": "477471c1…" ` | 直接用。**这是信任锚** |
| 2. 侧车 URL(备用) | ` "sha256": "https://…/universal.tar.xz.sha256" ` | GET 它,取**第一个连续的 64 位十六进制串**(兼容 sha256sum 的 "<hex>  <文件名>" 形态);主 URL 取不到就拿 mirrors 的同名 .sha256 再试;两条都不行 -> 明确报"校验信息拿不到"并**拒绝安装**(不拿"没有校验"糊过去) |
| 3. 缺失 | 不写 sha256 | **只校验 size**,并在结果(`files_without_hash`)与进度文案里写明"没有校验信息"。绝不假装校验过 |

**信任口径(一句话)**:清单里那份**字面哈希**放 GitHub index 仓库,是信任锚(有 git 历史、在另一个域);
SC 上的 .sha256 侧车与包在同一台服务器上,只能证明"传输没坏",证明不了"服务器没被换"。
所以**优先字面哈希**;清单里写的是 URL 时才去取侧车。

> 用户已确认:SC 提供侧车"上线有时间问题",因此**默认就是字面哈希**;侧车是可选的便利物,
> 我们的打包脚本会顺手生成(64 位十六进制 + 换行,text/plain),不传也不影响任何一条链路。

## 4. 打包与上传(用户只需要跑一条命令 + 上传)

    # 1) 把一份安卓 JRE 树拆成 universal/ 与 bin-arm64/ 两半(拆法见 §4.1),然后:
    python tools/jre_pack.py pack ^
        --universal <解开的 JRE 里"所有 ABI 共用"的那半> ^
        --bin       <解开的 JRE 里"ABI 专属"的那半> ^
        --abi arm64 --id jre17 --major 17 --version 17.0.9+11-sxcl.1 ^
        --out dist/jre17 ^
        --sc-base https://cloud.silentstudio.cn/api/v1/<owner>/JDK/Java/JRE/Android

    产出(dist/jre17/):
      universal.tar.xz          必传(SC)
      bin-arm64.tar.xz          必传(SC)
      universal.tar.xz.sha256   可选(SC)
      bin-arm64.tar.xz.sha256   可选(SC)
      version                   必传(SC):一行版本串,与清单里的 version 一致
      manifest.json             必传:自带托管形式的完整清单(可直接当 index.json 用)
      java_index.fragment.json  **把 platforms 并进 GitHub 上 SXCL/Java_index.json 的 versions.<major>**
      upload-checklist.md       逐文件的上传清单(路径 + size + sha256 前 16 位 + 两处落位)

    # 2) 上传后核对(逐文件比 size 与 sha256):
    python tools/jre_pack.py verify --index dist/jre17/manifest.json --dir dist/jre17
    #   或核对从 SC 下载回来的那份:
    python tools/jre_pack.py verify --index <Java_index.json> --dir <下载目录>

打包是**确定性**的(uid/gid/uname/gname/mtime 归零、条目按名字排序、补到 10240 的整数倍),
所以同一棵树每次打出来的字节完全一样,哈希可复现、清单可复核。

### 4.1 两半怎么拆

| 放 universal/ 的 | 放 bin-<abi>/ 的 |
|---|---|
| `release`、`conf/**`、`legal/**`、`lib/**`(除下面那几个)、`modules`(若有) | `bin/java`、`lib/libjli.so`、`lib/<arch>/jli/**`、ABI 专属的其它 `bin/**` 与 native 库 |

解包顺序固定:**universal 先、bin-<abi> 后**(后者覆盖前者的同名文件)。
客户端解完还会做安卓特有的一步:把 APK 的 `nativeLibraryDir` 里的 `libawt_xawt.so` / `libjsound.so`
补进 `<jre>/lib`(FCL RuntimeUtils.patchJava 的语义,见 include/sxcl/android.h)。

## 5. 客户端链路(代码位置 + 三道闸)

| 步骤 | 代码 |
|---|---|
| 来源解析(显式 > 环境变量 > 设置) | `sxcl_jre_resolve_index_url()`(include/sxcl/jre_hosted.h) |
| 取并解析清单(两种形式) | `sxcl_jre_index_parse()` / `sxcl_jre_index_pick()` |
| 下载(SHA-256 强校验 / 多连接 / 限速 / 断点 / 镜像) | 复用 `sxcl_engine_*`(include/sxcl/engine.h) |
| 解包 tar.xz | `sxcl_tar_extract_file()`(自己实现的 XZ/LZMA2 + tar,见 §5.1) |
| 落标记 | `jre.json`(version + 每个包的 sha256),`sxcl_jre_read_marker()` / `sxcl_jre_is_installed()` |
| 进度与错误 | `sxcl_jre_progress`(阶段/百分比/速度/剩余/人话)、`sxcl_jre_result.error` |

**三道闸,顺序固定**(越早拦住越好):

1. **version 判据**:`<目标>/jre.json` 里的 version 与清单一致 -> 整个跳过(不联网、不算哈希);
2. **磁盘**:下第一个字节之前算"还差多少字节",可用空间不够直接报 `disk`(拿不到可用空间就只报"未知",不编数字);
3. **逐文件 sha256**:`<目标>/.archives/<名字>` 已在盘上且 sha256 相符 -> 跳过下载;引擎对每个任务仍然按 sha256 强校验。

装到 `<配置目录>/runtime/<id>`(id = 清单里的 `jre17` 这种;**不是** java_runtime.c 那套 `<组件>-<平台>`)。
全绿之后才清 `.archives/`:半路失败留着,下一轮直接命中快路径(这就是断点续装)。

### 5.1 tar.xz 是自己实现的(如实说明选了哪条)

仓库原本只有 DEFLATE(src/core/inflate.c)与 ZIP(src/services/archive/zip.c),**没有任何 LZMA/XZ 能力**。
三条路里我们选了"自己写":

* 引公开实现(xz-embedded / liblzma):要 vendor 一大段第三方源码,许可与体积都不划算;
* 构建期解决(构建机上先解成 tar):等于把 xz 能力推给用户,分发链条上多一个不受控环节;
* **自己写**(选定):`src/core/archive/xz.c` + `src/core/archive/tar.c`。

支持子集(不支持的**明确报错**,不静默解出垃圾):XZ 容器(头/块/索引/尾、多流串联、4 字节填充)、
校验 none/CRC32/CRC64、**只有 LZMA2 过滤器**(BCJ/Delta 一律报 unsupported)、
未压缩块与 LZMA 块全支持、字典上限(默认 64 MiB,防解压炸弹)。
tar 侧支持 ustar / prefix / POSIX pax / GNU 'L'/'K' 长名 / 硬软链接 / 可执行位,
并且**拒绝**绝对路径、`..`、反斜杠、控制字符(越界的归档一个字节都写不出去)。

夹具是签入仓库的**真 .xz 文件**(Python 的 lzma/tarfile 生成,生成脚本一并签入:
`tests/fixtures/xz/generate.py` / `tests/fixtures/tar/generate.py`),用例把解出来的 sha256 钉死。
本轮实测(两棵树都跑):

| 用例 | 断言 | 覆盖 |
|---|---|---|
| `sxcl_xz_test` | 69 通过 / 0 失败 | CRC64/CRC32/无校验、未压缩块、长匹配、多块 + 索引核对、短读、多流串联、BCJ 拒解、坏数据/坏校验/截断、字典上限、便捷入口、sink 中止 |
| `sxcl_tar_test` | 58 通过 / 0 失败 | ustar / prefix / pax 长名 / GNU L 长名 / 软链接 / 可执行位 / 不安全路径拒写 / 不支持的条目类型 / 截断不留半个文件 / 总量上限 / 取消 / 一步到位解 tar.xz |
| `sxcl_jre_hosted_test` | 103 通过 / 0 失败 | 正常安装 + ABI 过滤 + version 判据跳过 + 版本变了重装、清单两种 schema、三种 sha256 形态(侧车 URL 含**镜像兜底**、缺失只校验大小)、SHA-256 不符拒装、取消不写 jre.json、来源优先级、目标目录 |
| `sxcl_jvm_test` | 64 通过 / 0 失败 | 四个环境变量(含 ld 路径三条)、六个 -D 属性、argv 拼装、**绝不带 -XstartOnFirstThread**、属性抠取、缺 JRE 的人话错误、盘上真有 JRE 时 dlopen 探针 |

**全部离线**:不联网、不依赖 Python、不要求机器上装了 Java。
| 优先级 | 来源 | 说明 |
|---|---|---|
| 1 | 调用方显式给的 URL(界面里填的) | 最优先 |
| 2 | 环境变量 `SXCL_JAVA_JRE_INDEX_URL` | 打包层/运维改一次不用重编 |
| 3 | 设置键 `java.jre_index_url`(settings.conf) | 设置页里填 |
| 4 | 编译期默认 `SXCL_JRE_INDEX_URL_DEFAULT` | **已定**:`https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/jre/index.json` |
| 5 | (都没有) | **报参数错并说清**:默认值若被改成占位就说"默认仓库名待确认";否则报"没有来源" |

这条口径与既有的 `sxcl_java_runtime_resolve_all_url()`(官方 Mojang 清单那条线)是**同一个形状**:
显式 > 环境变量 > 编译期/设置默认。官方那条线继续走 `sxcl_java_runtime_query.all_json_url`,
自托管这条线走 `sxcl_jre_*`;两条线共用同一个下载引擎与校验。

## 7. 自举:进程内起 JVM

    dlopen("<jre>/lib/libjli.so") -> dlsym("JLI_Launch") -> 调用

**思路**参考 Boardwalk / PojavLauncher 一系(FCL 走的也是同一条),**代码为本项目自写**:
`src/services/launch/jvm.c` 与 FCL/Pojav 的源码没有任何一行重叠,只有这条公开做法与
OpenJDK launcher(java.c)里 JLI_Launch 的**公开签名**。

起 JVM 之前必须补(少一样就起不来):

| 类别 | 内容 |
|---|---|
| 环境变量 | `JAVA_HOME`、`LD_LIBRARY_PATH`(含 `<jre>/lib`、`<jre>/lib/server`,再加上 APK 的 `nativeLibraryDir`)、`TMPDIR`、`HOME` |
| 系统属性 | `-Djava.home=<jre>`、`-Djava.io.tmpdir`、`-Duser.home`、`-Dos.name=Linux`、`-Dos.version=Android-<版本>`、`-Djava.library.path` |
| 其它 | `argv[0]` 给成 `<jre>/bin/java`(JLI 靠它算 application home;那个文件**存在**就行,不需要可执行位) |
| **绝不能加** | `-XstartOnFirstThread`(macOS 专用;安卓上加了直接起不来)。误传的会被**丢掉**并在结果里记一笔(`dropped_start_on_first_thread`) |
| 安卓特有 | dlopen 之前把 `LD_LIBRARY_PATH` 交给 linker 私有入口(`android_update_LD_LIBRARY_PATH` / `__loader_android_update_LD_LIBRARY_PATH`);拿不到就 RTLD_GLOBAL 预加载关键 .so(bionic 是启动时读 LD_LIBRARY_PATH 的) |

**自检(留证)**:`sxcl_jvm_selfcheck(opts, capture_path, out)` 用 `-XshowSettings:properties -version`
起一次 JVM,把**原始输出**写到 capture_path,再从输出里抠出 `java.version` 与 `java.home`
(并判断 java.home 与我们要的是否一致)。安卓侧入口:`sxcl_android_jvm_selfcheck()`
(android/jni/sxcl_jvm_bootstrap.c),它把同一份结论打到 logcat。

### 7.1 真清单真读的证据(不是测试里自己拼的形状)

    $env:SXCL_JRE_REAL_INDEX = 'D:\SilentStudio\_termux_jre\out\index.json'
    build-norm\src\services\install\Release\sxcl_jre_hosted_test.exe
    == 真清单(SXCL_JRE_REAL_INDEX)真读一遍
          jre17  major=17  version=17.0.20   abi=arm64-v8a  packages=2 dir=jre17/17.0.20/arm64-v8a
          jre21  major=21  version=21.0.12   abi=arm64-v8a  packages=2 dir=jre21/21.0.12/arm64-v8a
          jre25  major=25  version=25.0.4    abi=arm64-v8a  packages=2 dir=jre25/25.0.4/arm64-v8a
    jre_hosted_test: 通过 141,失败 0

其中逐字段钉死的是 jre17:universal.tar.xz 的 **size=31132644**、
**sha256=aa11db5ff7f38101b51d367af16e885702cc97ffcdabc5bea76c27360ce9bcaf**、url 是**相对路径**
(`jre17/universal.tar.xz`)。这条用例**只在设了环境变量时才跑**(没设就跳过),
所以仓库里的用例不依赖仓库外的文件。

## 8. 还没做 / 阻塞项(如实)

1. **真机没跑**(最硬的一条):这套 JRE 挪到应用私有目录后**能不能真跑**,目前只有静态层取证
   (有 `LD_LIBRARY_PATH` 就赢;`libjvm.so` 里有 Termux 的绝对路径与 `NEEDED libandroid-shmem.so`)。
   **aarch64 真机未实测** —— 要等 game 那条线的 dlopen 探针在我们自己的目录里真跑一次才算数。
   我这边能提供的是 `sxcl_jvm_selfcheck()`(起一次 JVM 打版本 + 属性,原始输出留证),但**没有设备在环**,
   所以这条只能如实挂着。
2. **两处自举实现未合并**:android/app 下已有一份 `sxcl_jre_bootstrap.c`(另一路并行改动),
   我新增的是 `android/jni/sxcl_jvm_bootstrap.c`(复用核心库)。建议收敛为:让那份也调用
   `sxcl_jvm_build_env()` / `sxcl_jvm_build_args()`,做到"参数与环境只有一份规则"。
   **本轮没有动他们那份文件**(避免与并行改动冲突)。
3. **界面入口**:核心接口、进度结构(阶段/百分比/速度/剩余)与错误文案都齐了,但设置页里那一项
   (填 index.json 地址 + 触发自托管安装)仍需接上 —— 这一层是纯 UI 接线,不阻塞链路。
4. **`notice[]`(290 条许可材料)不下载**:运行不需要;形状与 packages 一样,要做"许可/溯源"页时
   照同一条链路抓即可(清单里已经有 name/size/sha256/sha1/url)。
5. **xz 的 BCJ 过滤器不支持**:普通 JRE tar.xz 用不到;真遇到会明确报 unsupported。
6. 上一轮那条"JRE 包还没有来源"**已经消除**:Termux 那一路已产出 jre17/jre21/jre25,
   清单即 `sxcl.jre.index/1`(§2.1),客户端已真读通过(§7.1 证据)。

## 9. 复现命令(逐条,和本轮跑的一样)

    # 1) 打包 + 出清单 + 出上传清单(不需要网络)
    python tools/jre_pack.py pack --universal <dir> --bin <dir> --abi arm64 ^
        --id jre17 --major 17 --version <version> --out dist/jre17 ^
        --sc-base https://cloud.silentstudio.cn/api/v1/<owner>/JDK/Java/JRE/Android

    # 2) 上传后核对
    python tools/jre_pack.py verify --index dist/jre17/manifest.json --dir dist/jre17

    # 3) 两棵树 + 全部用例(核心库改动必须两棵都绿)
    cmake -S . -B build-norm -G "Visual Studio 18 2026" -A x64 ^
          -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 ^
          -DSXCL_BUILD_TESTS=ON -DSXCL_BUILD_UI=OFF -DSXCL_BUILD_CLI=ON -DSXCL_WERROR=ON
    cmake --build build-norm --config Release -j 8
    ctest --test-dir build-norm -C Release --output-on-failure

    cmake -S . -B build-ui -G "Visual Studio 18 2026" -A x64 ^
          -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 ^
          -DSXCL_BUILD_TESTS=ON -DSXCL_BUILD_UI=ON -DSXCL_WERROR=ON
    cmake --build build-ui --config Release -j 8
    ctest --test-dir build-ui -C Release --output-on-failure

    # 4) 安卓侧交叉编译(核心库 + JNI 自举层)
    clang --target=aarch64-none-linux-android24 --sysroot=<ndk>/toolchains/llvm/prebuilt/<host>/sysroot ^
          -DANDROID -O2 -std=gnu11 -fPIC -Wall -Wextra -Wpedantic -Werror -I include ^
          -c src/core/archive/xz.c -o /tmp/xz.o
    # (jre_hosted.c / jvm.c / android/jni/sxcl_jvm_bootstrap.c 同理)
