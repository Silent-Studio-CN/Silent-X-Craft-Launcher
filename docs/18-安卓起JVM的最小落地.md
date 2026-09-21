# 18. 安卓起 JVM 的最小落地(extractNativeLibs + JRE 侧两个 .so + exec 诊断 + **进程内起 JVM 跑通**)

> 设备:192.168.200.164:5555(G6012BS / Android 16 / arm64-v8a / targetSdk 34)。
> 本轮**没有**安装任何 Minecraft 版本,没有改任何系统设置(wm / density / 旋转 / stayon 一律没动),
> **没有**读写 `/storage/emulated/0/FCL` 或任何游戏目录;设备上只做:装/卸我们自己的 APK、
> 看 `nativeLibraryDir`、在我们自己的私有目录里跑诊断。所有输出都是原文粘贴。

## 0. 结论

| # | 结论 | 证据 |
|---|---|---|
| 1 | `legacyPackaging=false -> true` **必须改**:不然 `nativeLibraryDir` 是**空目录**,FCL 那套"从 nativeLibraryDir 拷 JRE 侧 .so"根本没有源文件 | §2 aapt2 + §3 真机 before/after |
| 2 | 两个 JRE 侧 .so(`libawt_xawt.so` / `libjsound.so`)已能从 FCL 源码交叉编译并随 APK 发布,真机上确实落在 `nativeLibraryDir` 里 | §5 + §3 |
| 3 | **fork+exec 起 JVM 这条路,在我们自己的应用里是死的**:SELinux 直接拒 `execute_no_trans`(`permissive=0`),JRE 装在私有目录也 exec 不了 | §4.2 avc 原文 |
| 4 | 上一轮"我们私有目录里的可执行能跑"的结论**是 run-as 假象**:`run-as` 进的是 `runas_app` 域(允许),应用进程进的是 `untrusted_app` 域(拒绝) | §4.1 vs §4.2 |
| 5 | 同一份 JRE,在允许 exec 的域里**真的跑起来了**:`openjdk version "25.0.5-internal"`(exit 0),前提是 `LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir>` | §4.1 |
| 6 | **最终答案:进程内 `dlopen(libjli.so)` + `JLI_Launch` 在应用自己的域里跑通了。** JVM 真的起来:打印出 `java.version = 25.0.5-internal`、`java.home = <jre>`、VM 横幅,`JLI_Launch` 正常返回 rc=0,**进程没有崩**(没有 SIGABRT),启动器接着把 UI 跑起来 | §4.4 原始 logcat |
| 7 | 安卓 arm64 的 JRE **我们还没有来源**:官方清单是桌面 JRE;这一步改成"自有托管 + 运行时可配下载"(来源未定,链条已可配) | §6 |

**由此定的路线(已验证)**:JVM 只能**进程内**起 —— `dlopen("<jre>/lib/libjli.so")` + `dlsym("JLI_Launch")` + 调用,
不是 fork+exec。打包层的两件事(真实 .so 文件 + 两个 JRE 侧 .so)是这条路线的前置条件,照做。
§4.4 是这条路线在真机、真应用域里跑通的原始输出。

## 1. 改了什么

| 文件 | 改动 |
|---|---|
| `android/gradle/gradle.properties.template` | 第 27 行 `legacyPackaging=false` -> `true`,并留了四行注释说明为什么(源码树里没有 `gradle.properties`,真正被读的是 androiddeployqt 生成的 `build/_android/pkg/gradle/gradle.properties`;`build.gradle:81` 只是读变量,没动) |
| `android/jni/jre-libs/**` | **新增**:从 FCL vendor 进来的 `libawt_xawt.so` / `libjsound.so` 源码(`awt_xawt/xawt_fake.c` + `jsound/*` 全部 .c/.h),版权头原样保留;来源与许可见该目录的 `README.md` |
| `android/scripts/build_jre_libs.ps1` | **新增**:NDK clang 交叉编译这两个 .so,产物进 gitignore 的 `build/_android/jre-libs/<abi>/` |
| `android/scripts/sync_to_build.ps1` | 把两个 `.so` 追加进 `android-extra-libs`(缺文件只警告,不硬失败——独立跑 sync 也要能用) |
| `android/scripts/build_apk.ps1` | 步骤 0 先编 JRE 侧 .so;打包后再核一次"它们真的进了 `libs/arm64-v8a`",缺则 **exit 4**(与 TLS 那条同规矩,绝不静默发布) |
| `include/sxcl/android.h` + `src/core/instance/android.c` | **新增** `sxcl_android_jre_patch_libs()`:FCL `RuntimeUtils.patchJava()` 语义——把 `nativeLibraryDir` 里的两个库拷进 `<jre>/lib`(jre8 是 `<jre>/jre/lib`);源缺一个就**硬失败**;"先全量体检再拷",失败不留半个文件 |
| `src/core/instance/tests/android_test.c` | 新增第 5 组断言(JRE 补齐:jre17/jre8 布局、幂等覆盖、150KB 分块拷贝逐字节一致、缺源硬失败、路径过长、out 太小不越界……) |
| `src/services/install/java_runtime.c` | 装完 JRE 的 FINISH 阶段:打包层给了 `SXCL_ANDROID_NATIVE_LIB_DIR` 就调上面的补齐函数(补不进去 = 安装失败,不写标记文件);没给就一个字节都不动(桌面零影响) |
| `include/sxcl/java_runtime.h` + `java_runtime.c` + `tests/java_runtime_test.c` | **来源可配**:`sxcl_java_runtime_resolve_all_url()`(显式 URL > 环境变量 `SXCL_JAVA_RUNTIME_MANIFEST_URL` > 编译期默认)+ 单测钉住优先级 |
| `android/app/sxcl_jre_probe.c` | **新增**:应用域内的**进程内 JVM 自举探针**(我们自己写的):`dlopen(libjli.so)` -> `dlsym("JLI_Launch")` -> 调用;内建 FCL 式崩溃兜底(信号 -> 看门线程打日志 + `_exit(128+sig)`),并把 `LD_LIBRARY_PATH` 的追加与实际生效路径打进日志 |
| `android/app/CMakeLists.txt` / `android/app/sxcl_android_main.cpp` / `android/java/.../SxclActivity.java` | 探针的接线:编译进 `libsxclui`;boot 文件支持 `jreprobe=`/`nativelib=`(由 `am start ... --es jreprobe <jre home>` 触发,默认关,正常启动零影响) |
| `android/scripts/build_apk.ps1`(第二次改动) | 打包层文件改成 **robocopy 整个 `build/_android/app`**:原来那句只 Copy-Item 两个文件的写法把新增的 `sxcl_jre_probe.c` 落在 stage 外,cmake 直接 `Cannot find source file` 挂掉(实测踩到并修掉) |
| `include/sxcl/launch.h` + `src/services/launch/java.c` + `tests/java_finder_test.c` | **失败分类认 SELinux**:exit 127 时,安卓且"文件可读可执行、挂载也没问题"= 新结论 `app_data_exec_denied`(名"私有目录不能执行"),文案带 `avc: denied { execute_no_trans }` 关键字段并指向"只能进程内 dlopen";纯函数 `sxcl_java_exec_failure_text()` + 单测钉住两条分支 |
| 本文件 / `android/jni/jre-libs/README.md` / docs/08 / docs/11 | 证据、来源许可、以及对上一轮错误结论的更正 |

本轮**没动**:`build.gradle`(只读变量)、`java_runtime.c` 的清单结构、任何系统设置、任何别人的目录。

## 2. 证据一:`extractNativeLibs=true`(aapt2 原文)

    $ adb -s 192.168.200.164:5555 install -r D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\sxcl-debug.apk
    Success                              # 旧包(上一轮产物)= 对照样本

    $ aapt2 dump xmltree --file AndroidManifest.xml <旧包 sxcl-debug.apk>
      E: application (line=21)
        A: http://schemas.android.com/apk/res/android:extractNativeLibs(0x010104ea)=false

    $ pwsh -File android/scripts/build_apk.ps1        # exit 0
    [apk] jre lib ok    : libawt_xawt.so  10416
    [apk] jre lib ok    : libjsound.so  67800
    [apk] gradle rc=0
    [apk] APK D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk 24070255
    [apk] DONE

    $ aapt2 dump xmltree --file AndroidManifest.xml <新包 sxcl-debug.apk>
      E: application (line=51)
        A: http://schemas.android.com/apk/res/android:debuggable(0x0101000f)=true
        A: http://schemas.android.com/apk/res/android:extractNativeLibs(0x010104ea)=true      <- 要的就是这一行

    $ aapt2 dump badging <新包>
    package: name='com.silentstudio.sxcl' versionCode='1' versionName='0.1.0-android' ...
    minSdkVersion:'28'  targetSdkVersion:'34'  native-code: 'arm64-v8a'

新包 `lib/arm64-v8a/` 24 个条目(节选,raw/comp 是 APK 里的字节数):

    lib/arm64-v8a/libawt_xawt.so                raw=   7168  comp=   2455     <- 本轮新增
    lib/arm64-v8a/libjsound.so                  raw=  57384  comp=  24510     <- 本轮新增
    lib/arm64-v8a/libc++_shared.so              raw=1253544  comp= 440495
    lib/arm64-v8a/libsxclui_arm64-v8a.so        raw=6638080  comp=2716132
    ...(共 24 个)

顺带后果:APK 体积 **47,583,273 -> 24,070,255 字节**(.so 在包里被压缩,安装时解出来)。

## 3. 证据二:`nativeLibraryDir` 之前是**空目录**,之后是真实文件

`dumpsys` 给的 `legacyNativeLibraryDir` 是 `<codePath>/lib`;应用真正拿到的
`ApplicationInfo.nativeLibraryDir` 是 `<codePath>/lib/arm64`。

**之前(extractNativeLibs=false 的旧包,装完立刻看):**

    $ adb -s 192.168.200.164:5555 shell "ls -la /data/app/~~ngkTHlz7hI8dFFMqxKHclw==/com.silentstudio.sxcl--Y2APNo2lT97-p7653qndg==/lib/arm64"
    total 6
    drwxr-xr-x 2 system system 3452 2026-09-21 19:55 .
    drwxr-xr-x 3 system system 3452 2026-09-21 19:55 ..
    $ adb ... shell "readlink .../lib/arm64/libQt6Core_arm64-v8a.so; readlink .../lib/arm64/libjsound.so"
    (无输出)

→ 不是"符号链接指向 APK",是这个目录**压根是空的**。任何"从 nativeLibraryDir 拷东西"的代码
在这一档上都是 `ENOENT`;`libawt_xawt.so` / `libjsound.so` 也根本不在设备上。

**之后(legacyPackaging=true 的新包):**

    $ adb -s 192.168.200.164:5555 shell "ls -la /data/app/~~lyGvZxeknoxk9Zz9TFjEYQ==/com.silentstudio.sxcl-2leHM2-JPsrva1N35msiqg==/lib/arm64"
    total 38794
    -rwxr-xr-x 1 system system    7168 1981-01-01 01:01 libawt_xawt.so
    -rwxr-xr-x 1 system system   57384 1981-01-01 01:01 libjsound.so
    -rwxr-xr-x 1 system system 1253544 1981-01-01 01:01 libc++_shared.so
    -rwxr-xr-x 1 system system 6638080 1981-01-01 01:01 libsxclui_arm64-v8a.so
    -rwxr-xr-x 1 system system 6367240 1981-01-01 01:01 libQt6Core_arm64-v8a.so
    ...(共 24 个,全部真实文件、带 x 位)

    $ adb ... shell "ls -ldZ .../lib/arm64; ls -lZ .../lib/arm64/libjsound.so"
    drwxr-xr-x 2 system system u:object_r:apk_data_file:s0  3452 ... lib/arm64
    -rwxr-xr-x 1 system system u:object_r:apk_data_file:s0  57384 ... libjsound.so

→ 打包层这一档符合 FCL 的前提(`useLegacyPackaging = true`,`FCL/build.gradle.kts:119`),
"从 nativeLibraryDir 拷两个 .so"从"注定 ENOENT"变成"文件就在那儿"。

## 4. 证据三:exec 诊断(这一轮最关键的一条)

诊断用的 JRE 是本机 FCL 参考检出里现成的 arm64 资产
(`D:\SilentStudio\_ref\FCL\FCL\src\main\jreAssets\app_runtime\java\jre25\{universal,bin-arm64}.tar.xz`,
解出 198 个文件 / 158.1 MB),`adb push` 到 `/data/local/tmp/jre25`,再由 **run-as 以应用 uid**
拷进我们自己的私有目录 `files/runtime/jre25`。**这不是我们的来源,只是诊断夹具**(见 §6)。

### 4.1 run-as 会话里:exec **成功**(但这个域不是应用域)

    $ adb -s 192.168.200.164:5555 shell "run-as com.silentstudio.sxcl sh /data/local/tmp/exec_probe3.sh <nativeLibraryDir>"
    [nativeLibraryDir] /data/app/~~lyGvZxeknoxk9Zz9TFjEYQ==/com.silentstudio.sxcl-2leHM2-JPsrva1N35msiqg==/lib/arm64
    -rwxr-xr-x 1 system system 1253544 ... /lib/arm64/libc++_shared.so

    === A) LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir>  ./bin/java -version ===
    openjdk version "25.0.5-internal" 2026-10-20
    OpenJDK Android Runtime Environment (build 25.0.5-internal-adhoc.runner.openjdk)
    OpenJDK 64-Bit Server VM (build 25.0.5-internal-adhoc.runner.openjdk, mixed mode)
    java_exit=0

    === B) LD_LIBRARY_PATH=<jre>/lib only ===
    CANNOT LINK EXECUTABLE "./bin/java": library "libc++_shared.so" not found: needed by main executable
    java_exit=1

    [ctx] u:r:runas_app:s0:c170,c256,c512,c768        # <- run-as 自己的域

对照组(同一个目录里的系统二进制)照旧能跑:`files/exec_probe/toybox echo EXEC_PRIVATE_OK -> EXEC_PRIVATE_OK`。

**这一条不能当成"应用能 exec"**:`run-as` 进的是 `runas_app` 域,不是应用进程的
`untrusted_app` 域(下一节)。它证明的是:JRE 本身在这台设备上能跑、而且
`bin/java` 需要 `LD_LIBRARY_PATH`(见 4.3)。

### 4.2 应用进程自己的域:被 SELinux **拒绝**(原文)

把 JRE 放在私有目录后启动我们的 APK,应用自己的 Java 探测扫到了它,并且**真的去 exec** 了:

    09-21 19:57:02.436  4537  4587 I sxcl : java-discover[0] major=25 version=25.0.5 src=AndroidPrivate path=/data/user/0/com.silentstudio.sxcl/files/runtime/jre25/bin/java
    09-21 19:57:03.341  4537  4594 I sxcl : [sxcl-ui] java-discover: /data/data/com.silentstudio.sxcl/files/runtime/jre25/bin/java major=25 usable=0 verdict=跑不起来 reason=进程起不来(退出码 127:多半是没有执行位、noexec 挂载或架构不符):/data/user/0/com.silentstudio.sxcl/files/runtime/jre25/bin/java

内核审计记录(SELinux Enforcing):

    09-21 19:57:03.328  4622  4622 W qtMainLoopThrea: type=1400 audit(0.0:49073): avc:  denied  { execute_no_trans } for  path="/data/data/com.silentstudio.sxcl/files/runtime/jre25/bin/java" dev="dm-74" ino=122649 scontext=u:r:untrusted_app:s0:c170,c256,c512,c768 tcontext=u:object_r:app_data_file:s0:c170,c256,c512,c768 tclass=file permissive=0 app=com.silentstudio.sxcl

    $ adb -s 192.168.200.164:5555 shell getenforce
    Enforcing

读法:
* `scontext=u:r:untrusted_app:...` —— **应用进程自己的域**(不是 `runas_app`);
* `tcontext=u:object_r:app_data_file:...` —— 私有目录里的文件就是 `app_data_file`;
* 被拒的是 `execute_no_trans` —— 这正是 `execve()` 要的那一项;它是 Android 10 起
  "应用不能执行自己 home 目录里的文件"W^X 规则的落点;
* `permissive=0` —— 不是"只记一笔",是真的不让跑;退出码 127 与上面那条日志对得上。

**注意拒绝的只是 `execute_no_trans`(execve)。** `dlopen()` 走的是 mmap(`file:execute`),
不是这一项 —— 这也正是 FCL/PojavLauncher 都用 `dlopen("libjli.so")` + `JLI_Launch` 而不是
fork+exec 的原因。应用域里 dlopen 这条路**已经补测:跑通了**,原始输出见 §4.4。

### 4.3 附带发现:`bin/java` 必须配 `LD_LIBRARY_PATH`

    $ readelf -d jre25/bin/java
     (NEEDED)  Shared library: [libc++_shared.so]
     (NEEDED)  Shared library: [libjli.so]
     (NEEDED)  Shared library: [libdl.so]
     (NEEDED)  Shared library: [libc.so]
     (没有 RPATH / RUNPATH)
    $ readelf -l jre25/bin/java | grep interpreter
     [Requesting program interpreter: /system/bin/linker64]

也就是说这份安卓 JRE 的 `bin/java` **没有 RUNPATH**,靠 `LD_LIBRARY_PATH` 找 `libjli.so`,
而 `libc++_shared.so` 只能从 APK 的 `nativeLibraryDir` 拿到(FCL 也是这么干的:
`FCLauncher` 把 `LD_LIBRARY_PATH` 指到 JRE 的 lib + `nativeLibraryDir`)。
**进程内 dlopen 路线同样要吃这一条**(dlopen 之前要先把依赖备齐)。

### 4.4 【最终答案】应用域内的进程内 `dlopen` + `JLI_Launch`:跑通了(原始 logcat)

探针是我们**自己写的**(`android/app/sxcl_jre_probe.c`,只借鉴 FCL 的思路),接在安卓入口原有的
boot 文件机制上,只有显式要求才跑(正常启动一个字节都不动):

    $ adb -s 192.168.200.164:5555 install -r D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk
    Success
    # 诊断用 JRE 仍由 run-as 拷进我们自己的私有目录(不碰任何别人的目录)
    $ adb -s 192.168.200.164:5555 shell "am start -n com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity \
          --es jreprobe /data/data/com.silentstudio.sxcl/files/runtime/jre25"
    Starting: Intent { cmp=com.silentstudio.sxcl/.SxclActivity (has extras) }

    $ adb -s 192.168.200.164:5555 logcat -d -s sxcl | findstr /c:"jre-probe" /c:"openjdk version" /c:"java.home =" /c:"java.version ="
    20:09:04.130 6044 6068 I sxcl : jre-probe: requested jre=/data/data/com.silentstudio.sxcl/files/runtime/jre25 nativeLib=/data/app/~~WtW_rvzx8KBs-pxPzE7JZg==/com.silentstudio.sxcl-0KjG0PCV1PVAgrziLLO9OA==/lib/arm64 report=/data/user/0/com.silentstudio.sxcl/files/jre_probe.txt
    20:09:04.130 6044 6068 I sxcl : [jre-probe] === 进程内 JVM 自举探针开始 ===
    20:09:04.130 6044 6068 I sxcl : [jre-probe] java_home      = /data/data/com.silentstudio.sxcl/files/runtime/jre25
    20:09:04.130 6044 6068 I sxcl : [jre-probe] nativeLibraryDir = /data/app/~~WtW.../lib/arm64
    20:09:04.130 6044 6068 I sxcl : [jre-probe] libjli         = /data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/libjli.so
    20:09:04.130 6044 6068 I sxcl : [jre-probe] LD 追加路径    = /data/data/com.silentstudio.sxcl/files/runtime/jre25/lib:/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/server:/data/app/~~WtW.../lib/arm64
    20:09:04.130 6044 6068 I sxcl : [jre-probe] 崩溃兜底就绪(跟踪 SIGABRT/SIGBUS/SIGILL/SIGFPE;SIGSEGV 留给 JVM)
    20:09:04.131 6044 6068 I sxcl : [jre-probe] LD_LIBRARY_PATH 已更新(经 __loader_android_update_LD_LIBRARY_PATH):/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib:/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/server:/data/app/~~WtW.../lib/arm64
    20:09:04.131 6044 6068 I sxcl : [jre-probe] 预加载 libc++_shared.so -> 成功
    20:09:04.132 6044 6068 I sxcl : [jre-probe] dlopen 成功: /data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/libjli.so
    20:09:04.132 6044 6068 I sxcl : [jre-probe] dlsym 成功: JLI_Launch @ 0x78ad413ed8
    20:09:04.132 6044 6068 I sxcl : [jre-probe] 调用 JLI_Launch(argc=4): /data/.../jre25/bin/java | -Djava.home=/data/.../jre25 | -XshowSettings:properties | -version
    20:09:04.315 6044 6075 I sxcl :     java.home = /data/data/com.silentstudio.sxcl/files/runtime/jre25
    20:09:04.317 6044 6075 I sxcl :     java.version = 25.0.5-internal
    20:09:04.317 6044 6075 I sxcl :     java.vm.name = OpenJDK 64-Bit Server VM
    20:09:04.318 6044 6075 I sxcl : openjdk version "25.0.5-internal" 2026-10-20
    20:09:04.325 6044 6068 I sxcl : [jre-probe] JLI_Launch 正常返回,rc=0(0 = 版本/属性已打印并退出;进程没有崩)
    20:09:04.325 6044 6068 I sxcl : [jre-probe] === 进程内 JVM 自举探针结束(启动器继续跑 UI)===
    20:09:04.326 6044 6068 I sxcl : jre-probe: returned rc=0 (启动器继续跑 UI)
    $ adb -s 192.168.200.164:5555 shell "pidof com.silentstudio.sxcl"
    6044                                  # 进程还活着(没有 SIGABRT、没有静默死亡)

探针自己落盘的报告(run-as 读回来,同一台设备):

    $ adb -s 192.168.200.164:5555 shell "run-as com.silentstudio.sxcl cat files/jre_probe.txt"
    stage=done
    result=ok
    rc=0
    java_home=/data/data/com.silentstudio.sxcl/files/runtime/jre25
    native_lib_dir=/data/app/~~WtW.../lib/arm64
    ld_paths=/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib:/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/server:/data/app/~~WtW.../lib/arm64
    libjli=/data/data/com.silentstudio.sxcl/files/runtime/jre25/lib/libjli.so

**同一份 logcat 里**旧的 exec 路仍然被拒(两条路 A/B 同框,一秒钟之内):

    20:09:04.764  6098  6098 W qtMainLoopThrea: type=1400 audit(0.0:49278): avc:  denied  { execute_no_trans } for
      path="/data/data/com.silentstudio.sxcl/files/runtime/jre25/bin/java" dev="dm-74" ino=118021
      scontext=u:r:untrusted_app:s0:c170,c256,c512,c768 tcontext=u:object_r:app_data_file:s0:c170,c256,c512,c768
      tclass=file permissive=0 app=com.silentstudio.sxcl

读法(逐条回答要问的问题):

* **SELinux 会不会**也**拦 `dlopen`?不会。** `dlopen` 成功、`dlsym` 成功 —— 被拒的从来只是 `execve` 要的
  `execute_no_trans`,不是 mmap 要的 `execute`;整份 logcat 里没有任何一条针对 dlopen 的 avc 拒绝。
* **JVM 版本**:`java.version = 25.0.5-internal` / `openjdk version "25.0.5-internal" 2026-10-20`。
* **java.home**:`/data/data/com.silentstudio.sxcl/files/runtime/jre25` —— 由我们**显式传的 `-Djava.home=`**
  定住(进程内时 `/proc/self/exe` 是 app_process,推不出 JRE 在哪)。
* **退出方式**:`JLI_Launch` **正常返回 rc=0**,不是 SIGABRT;进程继续活着并把 Qt UI 跑起来。
  崩溃兜底只在真崩时才接手(打日志 + `_exit(128+sig)`),本轮没有触发。
* `LD_LIBRARY_PATH`:这一份 JRE 的 `bin/java` / `libjli.so` **都没有 RUNPATH**,必须显式补
  `<jre>/lib`、`<jre>/lib/server`、`nativeLibraryDir`;两个 linker 私有符号里本机实测
  **只有 `__loader_android_update_LD_LIBRARY_PATH` 拿得到**(`android_update_LD_LIBRARY_PATH` 为 NULL),
  另外 RTLD_GLOBAL 预加载 `libc++_shared.so` 兜底 —— 实际生效的路径与走了哪条都打进了日志。
* JVM 自己还试了两次被拒的读(`cgroup.controllers`、`mmap_min_addr`,`permissive=0`),它自己降级处理,
  不影响启动(原文在同一份 logcat 里)。

同一份 logcat 里,应用自己的 Java 探测现在把这条 exec 失败**单独归成一类**(本轮改的失败分类):

    $ adb -s 192.168.200.164:5555 logcat -d -s sxcl | findstr java-discover
    20:12:41.460 6306 6337 I sxcl : [sxcl-ui] java-discover: /data/data/com.silentstudio.sxcl/files/runtime/jre25/bin/java major=25 usable=0 verdict=私有目录不能执行 reason=退出码 127:avc: denied { execute_no_trans } —— 不让 exec 应用私有目录,只能进程内 dlopen 起 JVM:/data/user/0/com.silentstudio.sxcl/files/runtime/jre25/bin/java

(改之前这条是 `verdict=跑不起来 reason=进程起不来(退出码 127:多半是没有执行位、noexec 挂载或架构不符)` ——
把"系统策略不让 exec"错报成"文件权限 / 架构不对",查错方向会跑偏。)

JVM 返回之后启动器**照常把 UI 跑起来**(不是"起来就死"):

    20:12:41.982 6306 6331 I sxcl : scrollarea ScrollArea [KeymapPage] rect=(49,49 638x478) viewport_h=478 content_h=601 scrollable=1
    $ adb -s 192.168.200.164:5555 shell "dumpsys window | grep -m1 mCurrentFocus"
    mCurrentFocus=Window{4e034d8 u0 com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity}

**探针的崩溃兜底**照 FCL 的写法:`pipe()` + 看门线程;`SIGABRT/SIGBUS/SIGILL/SIGFPE` 的处理器只往管道写
信号号然后挂住,看门线程负责打日志并 `_exit(128+sig)`;起 JVM 之前把全部信号处理器清一遍
(`SIGSEGV` 设 `SIG_IGN`,安卓对 `SIG_DFL` 有意见),`SIGSEGV` 留给 JVM 自己。**这样进程内 JVM 崩了
也是"有日志的退出",不是启动器静默消失。**

## 5. JRE 侧两个 .so:为什么、怎么编、来源与许可

* 为什么必须有:JVM 启动时按**文件名**从 `<jre>/lib` dlopen `libawt_xawt.so` / `libjsound.so`;
  安卓 arm64 的 JRE 资产里没有(或带的是链 X11 / ALSA 的版本),**运行时也补不回来**——组件清单里
  没有这两个文件。FCL 的 `RuntimeUtils.patchJava()`(其 239-249 行)就是干这个的。
* 怎么编:`android/scripts/build_jre_libs.ps1`,NDK clang `--target=aarch64-linux-android28 -shared -fPIC -O2`,
  jsound 的编译宏照抄 FCL(`X_PLATFORM=X_LINUX`、`USE_DAUDIO=TRUE`、PORT/MIDI 关掉)。实测产物:
  `libawt_xawt.so` 10416 字节 / `libjsound.so` 67800 字节,ELF64 AArch64,LOAD 对齐 0x4000(16KB 页安全),
  导出 `Java_com_sun_media_sound_*` 54 个符号。
* 运行时拷贝:`sxcl_android_jre_patch_libs()`(`include/sxcl/android.h`),已接进安装器的 FINISH 阶段;
  单测在 `src/core/instance/tests/android_test.c` 第 5 组(136 项断言全过)。
* 来源与许可:**全部写在 `android/jni/jre-libs/README.md`**——vendor 自
  <https://github.com/FCL-Team/FoldCraftLauncher>(GPL-3.0);其中 OpenJDK 那些文件是
  **GPL-2.0 only + Classpath 例外**(Oracle 版权头逐字保留);动态链接成独立 .so + 随仓库提供源码,
  满足 Classpath 例外的条件,**不要**静态链进我们的二进制。

## 6. 安卓 arm64 的 JRE 从哪来(本轮**:没有来源**,但链条已改成可配)

事实盘点:

| 路径 | 结果 |
|---|---|
| 我们现有的 `java_runtime.c` | 走 **Mojang 官方清单**(桌面 JRE:windows-x64 / mac-os / linux…)。安卓上 `sxcl_java_runtime_platform_key_for(SXCL_JAVA_OS_ANDROID, …)` 给的是 `"linux"`,清单里**没有 arm64 的安卓 JRE** —— 这条线在安卓上取不到东西 |
| 别的启动器装在它们私有目录里的 JRE | 沙箱挡住(且本轮明确不许碰);`/sdcard` 上是 noexec |
| FCL 自带在 APK assets 里的 arm64 JRE | 本机参考检出里就有:jre8/17/21/25 各一份 `universal.tar.xz + bin-arm64.tar.xz`,压缩态合计约 97 MB(jre25 一份 38.3 MB,解开 158.1 MB / 198 文件)。**只用于本轮诊断**,不随我们的包发布、也不写成来源 |
| 用户的服务 | 用户已明确:**国内托管可用,运行时下载可行** |

因此本轮把"下载 / 校验 / 安装"这条链路做成**来源可配**:

    # 来源三级(显式入参 > 环境变量 > 编译期默认),见 include/sxcl/java_runtime.h
    sxcl_java_runtime_resolve_all_url(explicit_url, env_value, out, out_len)
    sxcl_java_runtime_query.all_json_url        # 调用方直接给的 URL
    SXCL_JAVA_RUNTIME_MANIFEST_URL              # 环境变量:换我们自己的托管,不用重编
    SXCL_RUNTIME_DIR                            # 装到哪里(已有)
    # 校验/断点续传沿用引擎既有能力:清单逐文件 sha1 强校验 + 分片续传(sxcl_file_write_at)

仍**未定**的是:那份 all.json 与 JRE 包由谁产出、arm64 的 JRE 用哪个构建(以及它的许可与
NOTICE 怎么随包给)。这是**下一步的阻塞项**,不猜、不去找野包。

## 7. 还没做 / 下一步(按优先级)

1. ~~应用域 dlopen 探针~~ **已完成,§4.4 真机跑通**。下一步是把它从"探针"变成"产品路径":
   真正的启动要 `JLI_Launch` 起游戏主类(带 `-cp`、`-Djava.library.path=$NATIVEDIR`、LWJGL/JNA 的
   `-D` 参数等),并且要和 UI/下载线程共存:JVM 会装自己的信号处理器、接管 `exit`/`System.exit`,
   `DestroyJavaVM` 之后再回 Qt 事件循环这条路也要实测(本轮只跑到 `-version` 就返回)。
2. **JRE 来源落地(阻塞项,来源未定)**:自有托管上的 all.json + arm64 JRE 包(含由启动器补的
   `libawt_xawt.so`/`libjsound.so`)、许可与 NOTICE、sha1 清单;然后才是"首启/按需下载"。
   代码侧已把来源做成可配(`sxcl_java_runtime_resolve_all_url`),不会再写死 URL。
3. JRE 侧两个 .so 的**装入**还没有端到端真机验证:`sxcl_android_jre_patch_libs()` 有 136 项单测,
   但要等"我们自己的 JRE"第一次装进私有目录时才算走完。
4. `docs/14-源码头注归档.md` 是自动生成的头注归档,本轮新增的函数还没进那份归档(下次生成时带上)。
5. 探针现在靠 boot 文件 + `am start --es jreprobe` 触发;产品化时应变成设置页里的诊断按钮或首启自检。

## 8. 复现命令(逐条,和本轮跑的一样)

    # 1) 打包层:一行开关 + 两个 .so
    pwsh -File android/scripts/build_jre_libs.ps1        # -> build/_android/jre-libs/arm64-v8a/*.so
    pwsh -File android/scripts/build_apk.ps1             # -> D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk
    aapt2 dump xmltree --file AndroidManifest.xml <apk>  # 必须看到 extractNativeLibs=true

    # 2) 真机:装包 + 看 nativeLibraryDir
    adb -s 192.168.200.164:5555 install -r <apk>
    adb -s 192.168.200.164:5555 shell "dumpsys package com.silentstudio.sxcl | grep -m1 codePath"
    adb -s 192.168.200.164:5555 shell "ls -la <codePath>/lib/arm64; ls -lZ <codePath>/lib/arm64/libjsound.so"

    # 3) exec 诊断(JRE 只放在我们自己的私有目录;夹具 = 本机 FCL 参考检出里的 arm64 资产)
    python android/scripts/extract_fcl_jre_assets.py \\
           D:/SilentStudio/_ref/FCL/FCL/src/main/jreAssets D:/sxcl_local/jre25 jre25
    adb -s 192.168.200.164:5555 push D:\sxcl_local\jre25 /data/local/tmp/jre25
    adb -s 192.168.200.164:5555 push android/scripts/jre_exec_probe.sh /data/local/tmp/
    adb -s 192.168.200.164:5555 shell "dumpsys package com.silentstudio.sxcl | grep -m1 codePath"
    adb -s 192.168.200.164:5555 shell "run-as com.silentstudio.sxcl sh /data/local/tmp/jre_exec_probe.sh '<codePath>/lib/arm64'"
    adb -s 192.168.200.164:5555 logcat -d | findstr /i "avc:  denied  java-discover"

    # 3b) 应用域的证据(不是 run-as):起一次 APK,让应用自己的 Java 探测去 exec 它
    adb -s 192.168.200.164:5555 shell "am start -n com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity"
    adb -s 192.168.200.164:5555 logcat -d -s sxcl | findstr java-discover
    adb -s 192.168.200.164:5555 logcat -d | findstr "execute_no_trans"

    # 3c) 【进程内起 JVM】把 JRE 留在私有目录,让 APK 自己 dlopen + JLI_Launch(§4.4)
    adb -s 192.168.200.164:5555 logcat -c
    adb -s 192.168.200.164:5555 shell "am force-stop com.silentstudio.sxcl"
    adb -s 192.168.200.164:5555 shell "am start -n com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity --es jreprobe /data/data/com.silentstudio.sxcl/files/runtime/jre25"
    adb -s 192.168.200.164:5555 logcat -d -s sxcl | findstr /c:"jre-probe" /c:"java.version =" /c:"openjdk version"
    adb -s 192.168.200.164:5555 shell "run-as com.silentstudio.sxcl cat files/jre_probe.txt"
    adb -s 192.168.200.164:5555 shell "pidof com.silentstudio.sxcl"        # 进程要还在

    # 3d) 收尾:把诊断用的 JRE 从设备上撤掉(只删我们自己私有目录里的那份)
    adb -s 192.168.200.164:5555 shell "run-as com.silentstudio.sxcl rm -rf files/runtime"
    adb -s 192.168.200.164:5555 shell "rm -rf /data/local/tmp/jre25 /data/local/tmp/jre_exec_probe.sh"

    # 4) 单测(新增的 JRE 补齐 + 来源解析)
    cmake -S . -B build-jre-diag -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 -DSXCL_BUILD_TESTS=ON -DSXCL_BUILD_UI=OFF -DSXCL_BUILD_CLI=OFF -DSXCL_WERROR=ON
    cmake --build build-jre-diag --config Release --target sxcl_android_test
    build-jre-diag/src/core/instance/Release/sxcl_android_test.exe     # 通过 136,失败 0

## 9. 对既有文档的更正

* `docs/08` §14.1 里"我们自己的私有目录是可执行的(所以装一份我们自己的 Java 这条路成立)"——
  这句话只有一半对:**文件能放**,但**不能 exec**(`run-as` 的 `EXEC_PRIVATE_OK` 是 `runas_app` 域的
  假象,应用自己的 `untrusted_app` 域被 avc 拒绝,见 §4.2)。Java 要用,只能在进程内 dlopen 起 JVM。
* `docs/08` §14.2 结尾"→ 共享存储上的 Java 一定起不来;`/data/data/<包名>/files/**` 可以" ——
  后半句同样要改成"可以**放**,不能 exec"。
* `docs/11` §2.3"所以安卓上只有我们自己私有目录里的可执行能跑" —— 同上更正。
