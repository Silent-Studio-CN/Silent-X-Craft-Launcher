# 19. 安卓 arm64 JRE 的来源包(Termux 公开构建 → 我们的 universal/bin-arm64)

> 本文回答三件事:**来源是哪个(含 Java 8 到底有没有)**、**Termux 的包和我们要的切分差在哪(含
> `$PREFIX` 硬编码到底碍不碍事)**、**它自带的 native 库和我们自己的 shim 谁优先**;然后给出
> 打包脚本、`index.json` 的 schema、NOTICE/许可随包方案、以及"只要传这个目录"的上传清单。
>
> 纪律:本轮**没有** git 操作;**没有**碰任何设备;FCL 的 JRE 资产**只用于诊断/取证**,没有作为来源;
> Termux 的包(参考输入)与产物都放在**仓库外** `D:\SilentStudio\_termux_jre`。

## 0. 结论表

| # | 结论 | 证据(实测/原文) |
|---|---|---|
| 1 | Termux 现在只提供 **openjdk-17 / openjdk-21 / openjdk-25**(各带一个 `-x` X11 子包),**没有 Java 8,也没有 11** | apt 索引 `binary-aarch64/Packages` 全量解析(3006 个 stanza);`packages/openjdk-8`、`packages/openjdk-11` 在 termux-packages 的 GitHub 历史里查 `commits?path=` 返回**空数组**(从未存在) |
| 2 | **Java 8 必须另找来源**(MC 1.13~1.16 要它);候选见 §1.3,**不许拿 17 冒充** | 同 #1;§1.3 列了两个公开候选 + 自建路线 |
| 3 | Termux 的包装出来是 **JDK 镜像**,路径前缀是 `data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/`;我们要把这段前缀剥掉,再按**内容**切成 universal / bin-arm64 | 解包实测(§2.1 体积表);切分规则见 §2.2 |
| 4 | `$PREFIX` **确实写死在二进制里**(RUNPATH + 若干默认值),但**不是死路**:RUNPATH 里同时有 `$ORIGIN`/`$ORIGIN/../lib`,而 `lib/server/libjvm.so` 那份没有 → **必须**由启动器设 `LD_LIBRARY_PATH=<jre>/lib`(并把依赖库搬进 `<jre>/lib`);**不需要 patchelf** | §3 的 readelf 原文(RUNPATH/DT_RUNPATH/`/proc/self/exe`) |
| 5 | `libc++_shared.so` **不需要**(Termux 的 JDK 里没有任何 ELF 的 NEEDED 里有它) | §3.4:全部 ELF 的 DT_NEEDED 逐个体检 |
| 6 | 主包**不含** `libawt_xawt.so`(在 `openjdk-17-x` 子包里);主包**含** `libjsound.so`(链 ALSA)。两个都由我们的 shim 覆盖 → **我们优先,而且是必须** | §4:主包 lib/ 43 项逐个点名 + `openjdk-17-x.subpackage.sh` 原文 |
| 7 | 产物形态 = `<out>/<component>/{universal.tar.xz,bin-arm64.tar.xz,version}` + `<out>/index.json` + `<out>/UPLOAD.txt`;每文件 size+sha256+sha1;打包**完全离线**(脚本里没有任何 URL 字面量) | `android/scripts/pack_jre_from_termux.py`;schema 见 §6 |

## 1. 来源事实(a):Termux 到底有哪几个大版本

### 1.1 实测(apt 索引,2026-09-21 取的 stable/binary-aarch64)

| 包 | 版本 | 下载体积 | 说明 |
|---|---|---|---|
| `openjdk-17` | 17.0.20 | 96,306,888 B | JDK 镜像(headless 可跑;AWT 的 X11 部分在 -x) |
| `openjdk-17-x` | 17.0.20 | 12,664,096 B | `libawt_xawt.so`/`libfontmanager.so`/`libjawt.so`/`libsplashscreen.so` + jawt 头 + `java.desktop.jmod` |
| `openjdk-21` | 21.0.12 | 106,132,164 B | 同上 |
| `openjdk-21-x` | 21.0.12 | 12,674,040 B | |
| `openjdk-25` | 25.0.4 | 116,860,800 B | 同上 |
| `openjdk-25-x` | 25.0.4 | 12,824,976 B | |

索引里**没有任何 openjdk-8 / openjdk-11**;apt 的 `dists/` 目录只有 `stable/` 与 `staging/`(没有历史归档),
所以"从旧仓库翻一份 8 出来"这条路在官方源上也不成立。

### 1.2 "从来没有过"这一条是查了历史的

`GET /repos/termux/termux-packages/commits?path=packages/openjdk-8&per_page=1` → `[]`,
`packages/openjdk-11` → `[]`,`packages/openjdk-17` → 有(最近一次提交带日期)。
也就是说:不是"下架了",是这个包名**从未存在**。

### 1.3 Java 8(MC 1.13~1.16 需要):**没有**,候选在这里

| 候选 | 是什么 | 许可/可查性 | 备注 |
|---|---|---|---|
| ① 自建(推荐) | 用公开的 Android 化 OpenJDK 8u 源码 + Android NDK 自己出 arm64 镜像:`PojavLauncherTeam/openjdk-multiarch-jdk8u`(jdk8u 的 Android 分支)、构建脚本 `PojavLauncherTeam/android-openjdk-build-multiarch` | GPLv2 + Classpath 例外,源码公开 | **注意**:那个 build 仓库的 GitHub Releases 里现成的 `jre8-*` 资产是 **iOS(sandboxed iOS)** 的(2022-08-11),`jre17-*` 是 2021~2022 的旧 Android 产物 —— **不能直接拿 jre8 的 release 当 Android 用**;要用的是它的**构建脚本/源码** |
| ② 参考实现自带 | FCL 的 `jre8` 资产(实测 `release`:`JAVA_VERSION="1.8.0_442"`、`OS_ARCH="aarch64"`、`SOURCE=".:git:1a6e3a5ea32d+"`) | GPL-3.0 仓库 + JRE 本体 GPLv2+CE | 按本轮纪律它**只是诊断夹具**;要用它当发布来源需要用户单独点头(且它同样带 `$PREFIX` 类问题,见 §3 的通用结论:靠 `LD_LIBRARY_PATH` + `$ORIGIN`) |
| ③ 第三方一键脚本(不建议) | `java-in-termux` 之类 | 来源/构建过程不透明 | 不采纳:违反"来源可查、许可明确" |

**红线**:Java 17 跑不了需要 8 的 MC(1.13~1.16 会在类版本/模块系统上直接炸),所以这一格在补上之前
**必须**标成"缺",不能凑。

## 2. 目录布局(b):Termux 的包 vs 我们要的切分

### 2.1 解出来是什么样(openjdk-17 主包实测)

deb 里的路径是 **`./data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/...`**
(经典 Termux prefix `/data/data/com.termux/files/usr`;脚本不写死这段——它按"找到含 `lib/jvm/*` 的 `usr`
根"自动定位)。JDK home 顶层实测:

| 目录/文件 | 文件数 | 体积 | 我们要不要 |
|---|---|---|---|
| `bin/` | 28 | 172,560 B | 要(bin-arm64) |
| `conf/` | 15 | 111,406 B | 要(universal) |
| `demo/` | 118 | 5,875,853 B | **裁掉** |
| `etc/` | 1 | 74 B | 要(universal,profile.d 里的 JAVA_HOME 提示) |
| `include/` | 6 | 194,688 B | **裁掉**(JNI 头,编译才用) |
| `jmods/` | 69 | 62,396,557 B | **裁掉**(jlink 才用,最大的那块) |
| `legal/` | 239 | 1,748,624 B | **必须原样留**(GPL/Classpath 义务) |
| `lib/` | 48 | 152,832,959 B | 要(`lib/modules` 130 MB + `lib/server/libjvm.so` 11 MB 是大头) |
| `man/` | 27 | 199,758 B | **裁掉** |
| `release` | 1 | 1,218 B | 要(进 bin-arm64:它带 `OS_ARCH`) |

解出来合计 **223,533,697 B(223 MB)**;裁掉上面四项后约 **155 MB**。
(默认裁 `jmods include man demo sample lib/src.zip`;`--keep-all` 可关。`legal/` 永不裁。)

### 2.2 切分规则(按**内容**,不按后缀)

| 进 `bin-arm64.tar.xz` | 判据 |
|---|---|
| `release` | 里面写着 `OS_ARCH` |
| 任何 ELF 文件(魔数 `\x7fELF`) | 含 `bin/*`、`lib/*.so`、`lib/server/libjvm.so`、**`lib/jspawnhelper`**、`lib/jexec` |
| `*.jsa` | CDS 归档跟架构绑定 |
| `lib/<arch>/` 下的任何东西 | jre8 时代的 `lib/aarch64/**`(连 `jvm.cfg`/`Xusage.txt` 这种文本也是架构相关的) |
| 其余(`conf/`、`legal/`、`lib/modules`、`lib/*.jar`、`lib/security/*`、`lib/tzdb.dat`、`lib/ct.sym`、`lib/fonts/*` …) | `universal.tar.xz` |

与 FCL 的两处**有意不同**:

1. **`lib/jspawnhelper` 我们进 bin-arm64**(FCL 放在 universal)。理由:它是 ELF 可执行,放 universal 会被
   "架构无关"的假设坑到;按内容判定更不容易错。
2. **我们不带 `openjdk-*-x` 子包**(见 §4):FCL 的资产里带 `libfontmanager.so`/`libjawt.so` 等;
   我们这版不带,理由是它们链 X11 一整串(实测 `libawt_xawt.so` 的 NEEDED 有 11 个:`libandroid-shmem.so`,
   `libXext.so`, `libX11.so`, `libXrender.so`, `libXtst.so` …),而我们要的是自己的 `libawt_xawt.so` shim。
   真需要 AWT 字体/图片时,把 `openjdk-*-x.deb` 与 X11 闭包一起 `--input` 即可(脚本会按 NEEDED 自动收)。

### 2.3 **哪些必须留在 `lib/` 而不是 `bin/`**

* `lib/libjli.so`:`bin/java` 的 NEEDED 就是它,而 `bin/java` 的 RUNPATH 里有 `$ORIGIN/../lib` ——
  放 `lib/` 正是它自己会去找的位置(`$ORIGIN` = `<jre>/bin`)。
* `lib/server/libjvm.so`:`libjli.so` 通过 `java.home` + `lib/server/libjvm.so` 找它(实测 `lib/libjli.so`
  里有 `lib/modules` 这种**相对**路径,没有绝对 modules 路径)。
* `lib/jspawnhelper`:**留在 `lib/`,并且要保留可执行位**。它是 `ProcessBuilder` 真正 exec 的那个 helper
  (顺带一提:安卓应用域 exec 被 SELinux 拒,见 docs/18 §4.2,所以这个能力在应用内本来就不可用)。
* **额外依赖库也必须放进 `<jre>/lib`**(不是 `bin/`,也不是包根):`libandroid-shmem.so`、`libiconv.so.2`、
  `libjpeg.so.8`、`liblcms2.so`、`libz.so.1`、`libasound.so` 等。理由见 §3.3 —— `lib/server/libjvm.so`
  自己的 RUNPATH **没有** `$ORIGIN`,只能靠 `LD_LIBRARY_PATH`,而我们会把它指到 `<jre>/lib`。
* `libc++_shared.so` **不需要**(§3.4)。这是"要不要跟着 Termux 一起带 `libc++` 包"的答案:不用。

## 3. `$PREFIX` 硬编码(实测,这是 Termux 包最大的坑)

### 3.1 RUNPATH 原文(从产物里读出来的,不是猜的)

    bin/java          RUNPATH = /data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/lib:
                               /data/data/com.termux/files/usr/lib:$ORIGIN:$ORIGIN/../lib
    lib/libawt.so     RUNPATH = /data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/lib:
                               /data/data/com.termux/files/usr/lib:$ORIGIN
    lib/libjli.so     RUNPATH = 同上 + $ORIGIN ;NEEDED = libz.so.1, libdl.so, libc.so
    lib/server/libjvm.so
                      RUNPATH = .../java-17-openjdk/lib:.../usr/lib:.../java-17-openjdk/lib:.../usr/lib
                                （**没有 $ORIGIN**）;NEEDED = libandroid-shmem.so, libm.so, libdl.so, libc.so

标签是 **DT_RUNPATH(29)**,不是 DT_RPATH(15)—— 这正是 build.sh 里 `-Wl,--enable-new-dtags` 的结果
(termux-packages `packages/openjdk-17/build.sh` 原文)。**这句代码很关键**:DT_RUNPATH 的搜索顺序在
`LD_LIBRARY_PATH` **之后**,所以我们设的路径一定赢,不会被死掉的 Termux 绝对路径抢走。

### 3.2 能自己算出来的部分(重定位是安全的)

* `lib/libjli.so` 里有 **`/proc/self/exe`**(1 处)→ java.home 由"可执行文件自己是谁"推出来,
  把它放进 `files/runtime/jre17` 也会得到正确 home(再加上启动器本来就显式传 `-Djava.home`)。
* `lib/server/libjvm.so` 里有 `lib/modules`(1 处,**相对**路径)→ `<java.home>/lib/modules`。
* `conf/security/java.security`、`lib/security/cacerts`、`lib/classlist`、`lib/tzdb.dat` 这些文件里
  **0 处** Termux 前缀 → 它们都是 `java.home` 相对的。

### 3.3 写死到"没用"的部分(以及为什么仍然不影响起 JVM)

* `lib/modules`(130 MB 的 jimage)里有 **61 处** `/data/data/com.termux/files/usr`。逐条看,全是
  **可选功能的默认值**:`etc/mtab`、`etc/mime.types`、`etc/resolv.conf`、`etc/mailcap`、
  `lib/libpkcs11.so`(SunPKCS11 默认库)、`share/themes`/`share/gnome/themes`(GTK 主题目录)。
  它们**不在 JVM 启动链上**(不参与 libjvm/libjli/modules/conf 的定位)。
* `lib/libawt_headless.so` / `libawt_xawt.so`(**后者在 -x 子包**)里有 `/data/data/com.termux/files/usr/share/fonts/TTF`
  → AWT 字体目录写死。结论:**AWT 字体这条链在重打包后不完整**(要么改 `fontconfig` 相关配置,要么就别用 AWT 字体);
  MC 客户端本体渲染不用 AWT,所以不影响"起 JVM + 起游戏"。
* 全局:**16 KB 页对齐**这类安卓新要求不影响我们(文件是原样搬运,没有重新链接)。

**判定:Termux 这条路不是"不通",也不需要打补丁**,前置条件只有两条(都已落实):
1) 把 NEEDED 闭包里的 Termux 依赖库一起搬进 `<jre>/lib`(打包脚本自动做,缺一个就非 0 退出);
2) 启动器设 `LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir>`(docs/18 §4.3 已经写死这条要求)。

### 3.4 `libc++_shared.so` 的位置问题:不需要它

逐个体检 `openjdk-17` 里所有 ELF 的 DT_NEEDED:`bin/java` 只 NEEDED `libjli.so` + `libc.so`;
`lib/server/libjvm.so` NEEDED `libandroid-shmem.so` + `libm/libdl/libc`;`lib/libzip.so` NEEDED `libz.so.1`;
`lib/libjsound.so` NEEDED `libasound.so`……**没有任何一个 ELF 需要 `libc++_shared.so`**
(Termux 这份构建是静态链 libc++ 的)。所以:不用带 `libc++` 包,也不用像 APK 的 `nativeLibraryDir`
那样给它留位置。

### 3.5 本轮**没跑**的部分(如实说)

* **真机/真跑没有做**:本机是 Windows x64,没有 aarch64 执行环境,也**不许碰设备**(平板与本任务无关);
  容器/WSL 也不可用(实测 `wsl -l` 只回帮助文本)。所以 §3 的结论是**静态取证 + 逐条推断**:
  RUNPATH/DT_RUNPATH/NEEDED/`$ORIGIN`/`/proc/self/exe`/字符串统计都是从**解包后的真实二进制**里读的,
  但"把树挪到 `files/runtime/jre17` 后真的跑起来"这一条**没有实测**,要在 docs/18 §7.1 的
  应用域 dlopen 探针里补(那一步本来就要做)。

## 4. 两个 shim 谁优先(c)

主包 `openjdk-17` 的 `lib/` 一共 43 项,逐个点名后的事实:

    libawt_xawt.so        **不在主包**(在 openjdk-17-x)
    libfontmanager.so     **不在主包**(在 openjdk-17-x)
    libjawt.so            **不在主包**(在 openjdk-17-x)
    libsplashscreen.so    **不在主包**(在 openjdk-17-x)
    libjsound.so          在,81,328 B,NEEDED = libasound.so, libc.so
    libjli.so             在,57,992 B
    lib/server/libjvm.so  在,11,095,536 B

子包独占清单来自 termux-packages 的 `packages/openjdk-17/openjdk-17-x.subpackage.sh`
(`TERMUX_SUBPKG_INCLUDE`:`jawt.h`、`linux/jawt_md.h`、`jmods/java.desktop.jmod`、`lib/libawt_xawt.so`、
`lib/libfontmanager.so`、`lib/libjawt.so`、`lib/libsplashscreen.so`)。

**谁优先:我们自己的。** 而且不是"更优",是"必须":

1. 我们的两个 shim 随 APK 发布(`android/jni/jre-libs/`,现有 10,416 B / 67,800 B),安装 FINISH 阶段由
   `sxcl_android_jre_patch_libs()` 拷进 `<jre>/lib`(jre8 形状是 `<home>/jre/lib`);**拷贝 = 后写覆盖**,
   同名时我们的那份一定赢。
2. Termux 主包**根本没有** `libawt_xawt.so`(JVM 按文件名从 `<jre>/lib` dlopen,缺了就是硬失败);
   它自带的 `libjsound.so` 链 `libasound.so`,而我们的包**不带 alsa**(带上只是给不用 shim 的场景兜底)。
3. 因此:装我们自己的 shim 这一步失败 = 安装失败(**不写标记文件**),这是既有行为,不改。

## 5. 打包:脚本、产物、怎么复现

* 脚本:**`android/scripts/pack_jre_from_termux.py`**(纯 Python 标准库,**完全离线**;脚本里没有任何 URL 字面量)
* 输入:Termux 的 `openjdk-*.deb`(主包 + 依赖包;也支持"已经用 dpkg -x / 7z 解出来的 `usr/` 目录")
* 输出:`<out>/<component>/{universal.tar.xz, bin-arm64.tar.xz, version, NOTICE/**}` + `<out>/index.json`
  + `<out>/UPLOAD.txt`
* 命令(本次真跑过的):

      # 1) 拉来源(只下 apt 池里的公开 deb;记录 URL/size/sha256 到 sources.json)
      pwsh -File D:\SilentStudio\_termux_jre\fetch_termux_jre_sources.ps1
      # 2) 打包(缺哪个组件跳过哪个;NOTICE 不全/闭包缺库/自检不过 -> 非 0 退出,不静默发布)
      pwsh -File D:\SilentStudio\_termux_jre\pack_all.ps1
      # 等价的手工调用:
      python android/scripts/pack_jre_from_termux.py \
        --input <openjdk-17_17.0.20_aarch64.deb> --input <libandroid-shmem_*.deb> ... \
        --component jre17 --build-sh <openjdk-17_build.sh> --out <out-dir>

* 可重复性:条目排序 + uid/gid=0 + `--mtime 0` + 固定 xz 参数 + `--stamp` 固定 → 同样的输入给出**同样的字节和同样的 sha256**
  (已自测:同一份夹具连跑两次,两个 tar.xz 的 sha256 完全一致)。
* 自检:打包后重读两个归档,逐文件复算 sha256 与 manifest 比;不一致 → 退出码 5。
* 退出码:2=用法/输入错;3=NOTICE 不全;4=动态库闭包缺;5=自检不过。**任何一个都意味着"这份产物不能发"**。

## 6. `index.json` 的 schema(`sxcl.jre.index/1`)

> 说明:仓库里**原本没有** index.json 的 schema(docs/18 讲的是 Mojang 的 `all.json`/`components`
> 形状,而 xz.h / tar.h 引用的 docs/19 在这一轮之前**并不存在**)。本文件就是那份"说清楚"的地方,
> 脚本按它生成。**每个文件条目都带 size + sha256 + sha1**。

    {
      "schema": "sxcl.jre.index/1",
      "generated": "2026-09-21T12:20:42Z",
      "abi": "arm64-v8a",
      "components": {
        "jre17": {
          "component": "jre17", "major": 17,
          "version": "17.0.20", "java_version": "17.0.20",
          "implementor": "Termux", "os_arch": "aarch64", "abi": "arm64-v8a",
          "shim_dir": "lib",              // 两个 shim 该拷进哪(与 launcher 的规则对齐;jre8 可能是 jre/lib 或 lib/aarch64)
          "id": "jre17/17.0.20/arm64-v8a/60c0376d2a90a8b0",   // 判断"要不要重装"的键(内容派生)
          "file_count": 198, "installed_bytes": 162000000,
          "trimmed": ["jmods","include","man","demo","sample","lib/src.zip"],
          "source": {
            "kind": "termux-deb",
            "packages": [ {"name":"openjdk-17","version":"17.0.20","architecture":"aarch64",
                           "deb":"openjdk-17_17.0.20_aarch64.deb","size":96306888,
                           "sha256":"…","sha1":"…"}, … ],
            "build_script": {"path":"openjdk-17_build.sh","src_url":"…","src_sha256":"…",
                             "pkg_version":"17.0.20","license":"GPL-2.0"},
            "modified": false,
            "extra_libs": [ {"soname":"liblcms2.so","from":"littlecms_2.19.1_aarch64.deb","path":"lib/liblcms2.so"} ]
          },
          "packages": [
            {"name":"universal.tar.xz","url":"jre17/universal.tar.xz","size":…,"sha256":"…","sha1":"…"},
            {"name":"bin-arm64.tar.xz","url":"jre17/bin-arm64.tar.xz","size":…,"sha256":"…","sha1":"…"}
          ],
          "version_file": {"name":"version","url":"jre17/version","size":…,"sha256":"…","sha1":"…",
                            "first_line":"id=jre17/17.0.20/arm64-v8a/9f87f8de24c52431"},   // version 的第一行就是这个 id
          "notice": [ {"name":"NOTICE.txt","url":"jre17/NOTICE/NOTICE.txt","size":…,"sha256":"…","sha1":"…"}, … ],
          "manifest": { "files": {
            "bin/java":            {"type":"file","package":"bin-arm64.tar.xz","size":6272,"sha256":"…","sha1":"…","executable":true},
            "lib/liblcms2.so":     {"type":"file","package":"bin-arm64.tar.xz","size":…,"sha256":"…","sha1":"…","origin":"dep:littlecms_2.19.1_aarch64.deb"},
            "lib/modules":         {"type":"file","package":"universal.tar.xz","size":130116694,"sha256":"…","sha1":"…"},
            "conf":                {"type":"directory","package":"universal.tar.xz"},
            "legal/java.base/LICENSE": {"type":"file","package":"universal.tar.xz","size":19274,"sha256":"…","sha1":"…"}
          } }
        }
      }
    }

* `manifest.files` 的键是**包内相对路径**(= 解开后 <jre> 下的相对路径),`package` 指明它在哪个 tar.xz 里;
  `origin` 只在"这一份不是 JDK 自带的、是我们搬进来的依赖库"时出现,值是它来自哪个 deb。
* 合并语义:再跑一次只更新自己那个 component,别的 component 原样保留(`generated` 跟着更新)。
* 与 Mojang `all.json` 的关系:两者是**两层清单**的同一思路(总清单 → 组件清单)。本轮**没有**改
  `java_runtime.c` 去直接吃这份 index.json(那要先把"下 tar.xz → 解包 → 逐文件校验"接上,
  仓库里的 `sxz/xz` + `tar` 解码器已经就位,见 `include/sxcl/xz.h`、`include/sxcl/tar.h`);
  这一步是**下一步**的活,不在本轮。

## 7. NOTICE / 许可随包方案(为什么放"包旁")

### 7.1 放哪、放什么

**放"包旁"**:远端目录形态是

    <远端根>/index.json
    <远端根>/jre17/universal.tar.xz
    <远端根>/jre17/bin-arm64.tar.xz
    <远端根>/jre17/version
    <远端根>/jre17/NOTICE/NOTICE.txt          ← 来源/改动/谁优先(脚本生成)
    <远端根>/jre17/NOTICE/SOURCE_OFFER.txt    ← **源码获取方式**(脚本生成)
    <远端根>/jre17/NOTICE/README.txt          ← 为什么放包旁(脚本生成)
    <远端根>/jre17/NOTICE/jdk-legal/**        ← JDK 自带 legal/ 的副本(GPLv2 全文 + Assembly/Classpath 例外 + ADDITIONAL_LICENSE_INFO)
    <远端根>/jre17/NOTICE/packages/<包名>/**  ← 每个一起重发布的 Termux 包的许可原文(取自它自己的 share/doc)
    <远端根>/jre17/NOTICE/licenses/*.txt      ← termux-licenses 里的通用许可全文(断链的正文在这儿)

**理由(三条,都写进了 NOTICE/README.txt)**:
1. 包里本来就有 JDK 的 `legal/`(必须原样留),但**解开包之前看不到**;放包旁才能"下载前/安装前"就呈现。
2. 一起重发布的依赖库(`libiconv`/`libjpeg-turbo`/`littlecms`/`zlib`/`libandroid-*`/`alsa-lib`)**不是 JDK
   镜像的一部分**,它们的许可文本来自各自 Termux 包的 `share/doc/<包名>/copyright`;塞进运行时树会污染
   目录、也会丢掉"哪个文件对应哪个许可"的对应关系。
3. NOTICE 目录里的每个文件都在 `index.json` 里有 size+sha256+sha1 → 和包一样可校验、可发现,
   启动器的"许可/关于"页可以直接按 index.json 取,不必先安装。

### 7.2 许可与义务(逐条)

* **OpenJDK(本包主体)**:GPL-2.0 only + OpenJDK Assembly/Classpath 例外。包里 `legal/java.base/LICENSE`
  = GPLv2 全文(347 行),`legal/java.base/ASSEMBLY_EXCEPTION` = 例外条款,`ADDITIONAL_LICENSE_INFO` = 混合许可说明;
  JDK 的 `legal/` 有 239 个条目(含 208 个指向 `java.base/` 的符号链接,Windows 上没有建链接权限时脚本会把
  目标内容复制过去,不是丢掉)。**版权头一律原样保留;本包二进制一个字节都没改**(没有 patchelf/strip/重链接)。
* **依赖库**:`libiconv`(LGPL-2.1+)、`alsa-lib`(LGPL-2.1+)、`libjpeg-turbo`(IJG/BSD-3 + zlib)、
  `zlib`(ZLIB)、`littlecms`(MIT)、`libandroid-shmem`/`libandroid-spawn`/`libandroid-sysv-semaphore`(Apache-2.0)——
  逐份原文进 `NOTICE/packages/<包名>/`,不会被"忘了带"。
* **"源码获取方式"**(`SOURCE_OFFER.txt`):写明 (1) 上游 OpenJDK 源码 tarball 的**确切 URL + sha256**
  (来自 Termux 的 `build.sh` 里 `TERMUX_PKG_SRCURL`/`TERMUX_PKG_SHA256`,打包时**从本地文件读**,不联网);
  (2) termux-packages 的 `packages/openjdk-<N>/`(build.sh + 补丁 = "源码→二进制"的全部配方);
  (3) 我们自己的重打包脚本;(4) 依赖库各自的 Termux 包目录。GPLv2 §3 要的"获取途径"就在这份文件里,
  不需要用户来问我们要。**没有 `--build-sh` 时脚本会明确报"占位符"并退出码 3**,不允许静默发布一份来源不明的包。
* 一个**真实的坑**(脚本已处理):Termux 的许可文件经常是**符号链接**,
  例如 `share/doc/openjdk-17/copyright -> ../../LICENSES/GPL-2.0.txt`,而 `share/LICENSES/*.txt` 在
  **另一个包 `termux-licenses`** 里。只解 openjdk 的 deb,链接是**断的**。所以:打包时把 `termux-licenses`
  一起 `--input`(脚本按链接目标的 basename 去 `share/LICENSES/` 找正文);找不到就记一条问题 → 退出码 3。

### 7.3 为什么不是"往包里塞一份 LICENSE"

* 包里 `legal/` 已经是权威全文,再塞一份只是重复;
* 只塞一份 GPLv2 说明不了"还有 7 个第三方共享库",那种"看起来合规"最危险;
* 包旁方案让**每一个**被重发布的文件都能追溯到它的许可原文与来源包。

## 8. 上传清单

**怎么用**:把 `<out>`(本次是 `D:\SilentStudio\_termux_jre\out`)**整个目录**原样传到你的托管根目录;
**本地相对路径 = 远端相对路径**。逐文件的"路径 + 字节 + sha256 前 16 位"在 `<out>/UPLOAD.txt` 里
(完整 sha256/sha1 在 `index.json` 的 `packages`/`manifest.files`/`notice` 里,逐文件都有)。
不要只传 tar.xz 而漏掉 `NOTICE/` 和 `index.json`:前者是许可义务,后者是客户端判断"要不要重装"的依据。

### 8.1 本次真产出的三个组件(ABI `arm64-v8a`,index.json `generated=2026-09-21T12:25:01Z`)

| 组件 | Java | universal.tar.xz | bin-arm64.tar.xz | version | 文件数(解开后) | index `id` |
|---|---|---|---|---|---|---|
| `jre17` | 17.0.20 | 31,132,644 B / sha256 `aa11db5ff7f38101…` | 4,630,460 B / `cd6a527c2373c7a4…` | 429 B | 339(158,165,753 B) | `jre17/17.0.20/arm64-v8a/9f87f8de24c52431` |
| `jre21` | 21.0.12 | 33,087,332 B / `167b4c3e69acb632…` | 5,902,036 B / `f3d487bb83a1d970…` | 429 B | 339(182,371,510 B) | `jre21/21.0.12/arm64-v8a/360013a3aa4469e5` |
| `jre25` | 25.0.4 | 38,491,816 B / `2c72818d43c883e2…` | 6,417,916 B / `1c64a3b473d61ac5…` | 427 B | 349(187,976,134 B) | `jre25/25.0.4/arm64-v8a/d17e0c15bdc705da` |

* `NOTICE/` 每个组件 287~298 个文件 / 约 2.5 MB(JDK 自带 `legal/` 的全量副本 + 10 个来源包的许可原文 + `termux-licenses` 的通用文本)。
* 额外搬进 `<jre>/lib` 的依赖库:`jre17`/`jre21` = 7 个(`libandroid-shmem.so`、`libandroid-spawn.so`、`libasound.so`、`libiconv.so`、`libjpeg.so.8`、`liblcms2.so`、`libz.so.1`);`jre25` = 6 个(它的 ELF 不 NEEDED `libandroid-spawn.so`)—— 闭包是**逐组件算**出来的,不是照抄别的组件。
* 上传总量 ≈ **121.3 MiB**;逐文件清单 `UPLOAD.txt`,全量 sha256/sha1 在 `index.json`。
* 独立校验(和打包脚本不是同一段代码):`python android/scripts/verify_jre_index.py --out <out>` ——
  `index.json` → 每个包的 size/sha256/sha1 → 解开两个 tar 逐文件复算 → 与 `manifest.files` 一条条对
  → `NOTICE`/`version` 逐文件对 → `UPLOAD.txt` 每行复算。退出码 0=全对,1=有对不上,2=缺文件。
  **本次实跑:exit 0,三个组件 + UPLOAD.txt 共 2790 个校验点全部一致。** 它也正是抓出
  "version 第一行与 index 记的 first_line 不符"的那个脚本(修法:把 id 挪到 version 的第一行,见 §9)。

## 9. 跑过 / 没跑(如实)

**跑过**
* apt 索引解析(3006 stanza)、GitHub 历史查询(openjdk-8/11 从未存在)、termux-packages 的
  build.sh / openjdk-17-x.subpackage.sh / termux_step_install_license.sh / properties.sh 原文核对;
* **下载**了 openjdk-17/21/25(+ -x)与依赖包的公开 .deb(记录 URL/size/sha256 在 `sources.json`);
* **解包+取证**:目录布局、体积表、每个 ELF 的 NEEDED/SONAME/RUNPATH/DT_RUNPATH/interp、
  `/proc/self/exe`、`$ORIGIN`、`lib/modules` 里 61 处前缀的逐条上下文、主包 lib/ 43 项点名;
* **打包**脚本端到端(合成夹具 + 真 Termux deb),含:确定性复跑 sha256 一致、自检逐文件复算、
  NOTICE 生成、index.json 合并更新、UPLOAD.txt 生成;
* 脚本自身的两个坑是**实测踩出来的**并已修:符号链接链(`libjpeg.so -> libjpeg.so.8 -> libjpeg.so.8.3.2`)、
  以及"半截 .deb 被当成下好了"(现在按索引里的期望大小校验,小的续传、大的重下)。

* **第二轮又踩出一个,而且是最危险的那个:断链许可被安静漏掉。** `openjdk-17`/`libiconv`/`alsa-lib` 的
  `share/doc/<包>/copyright` 是指向 `termux-licenses` 的**断链** —— 盘上**没有文件**,于是"遍历目录找许可"
  这一步根本看不见它们,第一版真的少收了这三份许可(而且不报错)。现在许可收集会连着"只有链接、没有文件"
  的那些一起算,`termux-licenses` 里能查到正文就抄正文,**查不到就报问题**(退出码 3),不再静默。

**没跑**
* **没有在真机/真 aarch64 上跑过 JVM**(本机 Windows x64,无 aarch64 环境、无 WSL;不许碰设备)→
  §3 的"挪到私有目录能不能跑"是静态证据 + 推断,真机验证留给 docs/18 §7.1 的 dlopen 探针;
* **没有**做 Java 8 的任何构建/打包(§1.3 只给候选与红线);
* **没有**改 `java_runtime.c` 去消费这份 index.json(下一步);
* **没有**从 FCL 资产里产出任何发布物(FCL 的 jre8/jre25 只被读来做对照与取证)。

## 10. 下一步

1. Java 8:定来源(§1.3 ①自建 或 ②用户点头用 FCL 的 jre8),然后照本文同一套流程打 `jre8`;
   注意 jre8 形状的 `shim_dir` 可能是 `lib/aarch64`(脚本会记录并警告"launcher 现在只认 jre8 的
   `<home>/jre/lib` 或 `<home>/lib`",两边要对齐)。
2. 客户端接线:`down tar.xz(校验 sha256)→ sxcl_xz → sxcl_tar → 逐文件 sha1/sha256 复校 → patch libs`,
   然后才是"首启/按需下载"。
3. `LD_LIBRARY_PATH` 与 `libc++_shared.so`:进程内 dlopen 路线在 `dlopen("libjli.so")` 之前就要把
   `<jre>/lib` 备好(本轮已经把依赖库搬进去了,客户端只要设环境变量)。
4. 可选:`openjdk-*-x` + X11 闭包(要 AWT 字体/图片时);`--keep-all`(要 jmods/javac 全功能时)。
