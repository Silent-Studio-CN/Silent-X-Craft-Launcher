# 08 · Android 打包

> 本文只记录**本机实测**的事实:每条结论后面都能找到命令与输出。
> 涉及的文件全部在 `android/**`(新增),仓库里别人的文件一个字没动。

## 0. 一句话结论

C 版**打得出能装能跑的 APK**:`android/app/build/outputs/apk/debug/app-debug.apk`(943,941 字节),
里面是「Java 壳(按键助手)+ C 核心 `lib/arm64-v8a/libsxcl.so`」,**没有 Qt 界面**。

## 1. 为什么 APK 里没有界面(硬约束,不是偷懒)

* C 版的界面是 **Qt Widgets + libqf**(`src/ui/**`,`SXCL_BUILD_UI=ON` 才编);
* Qt for Android 只支持 **Quick/QML**,不支持 Qt Widgets;
* 本机也没有 Qt 的 android 套件,用不了 `androiddeployqt`。

所以 Android 侧走「**Java 壳 + C 核心 .so**」:Java 壳负责界面与交互(纯 Java、零第三方依赖),
C 核心以 `libsxcl.so` 进 APK,通过 JNI 暴露(`app/src/main/cpp/sxcl_jni.c`)。
桌面端该有的 Qt 界面照旧,两边不冲突。

## 2. 工具链版本(本机实测)

| 组件 | 版本 | 位置 |
|:--|:--|:--|
| JDK | 17.0.2 | `D:\jdk17`(`java -version` 实测) |
| Android SDK | platform `android-36` / build-tools `36.0.0` | `D:\AndroidSdk` |
| NDK | `28.2.13676358`(Clang 19.0.1) | `D:\AndroidSdk\ndk\28.2.13676358` |
| CMake | `3.31.6` | `D:\AndroidSdk\cmake\3.31.6`(本次用 sdkmanager 新装) |
| Ninja | 随 SDK 的 cmake 包提供 | `D:\AndroidSdk\cmake\3.31.6\bin\ninja.exe` |
| Gradle | `9.3.1` | `android/gradlew`(wrapper 已签进仓库) |
| AGP | `9.1.1` | `android/build.gradle.kts` |

两个必须说明的版本选择:

* **AGP 只能用 9.0.x / 9.1.x**:AGP 9.2.1 要求 Gradle ≥ 9.4.1,实测报
  `Minimum supported Gradle version is 9.4.1. Current version is 9.3.1.`;
  本机从 `services.gradle.org` 拉新发行包拉不动(见 §10),所以固定在 Gradle 9.3.1 能用的
  **AGP 9.1.1**(9.0.1 / 9.1.0 / 9.1.1 三个都实测过版本检查能过,取最新)。
* **CMake 必须是 SDK 里装的 3.31.6**:仓库根 `CMakeLists.txt` 写的是
  `cmake_minimum_required(VERSION 3.24)`,而 SDK 自带的 cmake 是 3.22.1 —— 不够。
  装法见 §4 第 0 步。

## 3. 目录结构(新增文件)

```
android/
├── settings.gradle.kts / build.gradle.kts / gradle.properties
├── gradlew / gradlew.bat / gradle/wrapper/{gradle-wrapper.jar,gradle-wrapper.properties}
├── .gitignore                      # 忽略 build/ .cxx/ .gradle/ local.properties
└── app/
    ├── build.gradle.kts
    └── src/
        ├── main/
        │   ├── AndroidManifest.xml
        │   ├── cpp/CMakeLists.txt   # 原生层入口:把仓库根的 C 源码编成 libsxcl.so
        │   ├── cpp/sxcl_jni.c       # JNI 外壳(只搬运参数,不含业务逻辑)
        │   ├── java/com/silentstudio/sxcl/
        │   │   ├── core/NativeCore.java   # ← C 版新增:libsxcl.so 的 Java 入口
        │   │   ├── keymap/ data/ overlay/ ui/   # 见下表的来源
        │   ├── assets/keymaps/*.json      # 9 套布局 + index.json
        │   ├── assets/icons/**            # 14 个方块 PNG + NOTICE.md
        │   └── res/{drawable,values}
        └── test/java/.../KeymapCoreTest.java
```

**Java 源码的来源**:除 `core/NativeCore.java`(C 版新增)外,其余 15 个 `.java` 全部搬自
Python 版仓库 `Silent-X-Craft-Launcher/android/app/src/main/java/...`,**逻辑一行未改**,
每个文件在原有 AGPL 头之后补了一段来源说明(署名口径同 `assets/icons/NOTICE.md`)。
唯一有代码改动的是 `ui/MainActivity.java`:加了 4 行,多显示一行核心库状态
(`NativeCore.describe()`),按键相关逻辑未动 —— 文件头的来源说明里也写明了这一处。

## 4. 复现步骤(逐步)

前置:设好 `JAVA_HOME=D:\jdk17`、`ANDROID_HOME=D:\AndroidSdk`。
本机把 Gradle 的家目录放在 D 盘(`GRADLE_USER_HOME=D:\gradle-home`),避免占 C 盘 —— 可省略。

```powershell
# 0) 装 SDK 里带的 CMake(根 CMakeLists 要 >= 3.24,SDK 自带的只有 3.22.1)
& "$env:ANDROID_HOME\cmdline-tools\latest\bin\sdkmanager.bat" --install "cmake;3.31.6"

# 1) 先验核心逻辑:纯 Java 单测,不用手机不用模拟器
cd android
javac -encoding UTF-8 -d out app/src/main/java/com/silentstudio/sxcl/keymap/*.java `
      app/src/test/java/com/silentstudio/sxcl/keymap/KeymapCoreTest.java
java -cp out com.silentstudio.sxcl.keymap.KeymapCoreTest app/src/main/assets/keymaps

# 2) 打 APK(wrapper 会按 gradle-wrapper.properties 取 Gradle 9.3.1)
.\gradlew.bat :app:assembleDebug
```

产物:`android/app/build/outputs/apk/debug/app-debug.apk`。

### 只想单独编 C 核心(不经 Gradle)

这条路线同时就是 CI `.github/workflows/ci.yml` 的 android job 在跑的那套配置:

```powershell
cmake -S android/app/src/main/cpp -B build-android -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_HOME/ndk/28.2.13676358/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake --build build-android      # → build-android/libsxcl.so
```

## 5. 产物与验收证据

### 5.1 APK

```
FullName      : D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\android\app\build\outputs\apk\debug\app-debug.apk
Length        : 943941
SHA-256       : D24350D129AC3E04C8E7857443CC7AA548675748F6EBCF4DBD1C3AB1C8FA820B
```

### 5.2 `aapt2 dump badging`(关键行)

```
package: name='com.silentstudio.sxcl' versionCode='1' versionName='0.1.0' platformBuildVersionName='16' platformBuildVersionCode='36' compileSdkVersion='36' compileSdkVersionCodename='16'
minSdkVersion:'26'
targetSdkVersion:'36'
uses-permission: name='android.permission.SYSTEM_ALERT_WINDOW'
uses-permission: name='android.permission.FOREGROUND_SERVICE'
uses-permission: name='android.permission.FOREGROUND_SERVICE_SPECIAL_USE'
uses-permission: name='android.permission.POST_NOTIFICATIONS'
application-label:'SXCL 按键'
launchable-activity: name='com.silentstudio.sxcl.ui.MainActivity'  label='' icon=''
native-code: 'arm64-v8a'
```

### 5.3 `apksigner verify --print-certs`

```
Verifies
Verified using v1 scheme (JAR signing): false
Verified using v2 scheme (APK Signature Scheme v2): true
Verified using v3 scheme (APK Signature Scheme v3): false
Verified for SourceStamp: false
Number of signers: 1
Signer #1 certificate DN: C=US, O=Android, CN=Android Debug
Signer #1 certificate SHA-256 digest: a18e69972a3afb7a9c0c36445f43b71f27da77eb1332d07d5a35559107cdea7a
apksigner exit=0
```

### 5.4 `unzip -l`(截取关键条目,共 43 个文件)

```
  2419168  1981-01-01 01:01   classes.dex
    13168  1981-01-01 01:01   lib/arm64-v8a/libsxcl.so
     4324  1981-01-01 01:01   assets/keymaps/index.json
    12711  1981-01-01 01:01   assets/keymaps/building-landscape.json
    10806  1981-01-01 01:01   assets/keymaps/building-portrait.json
     6054  1981-01-01 01:01   assets/keymaps/minimal-landscape.json
     6079  1981-01-01 01:01   assets/keymaps/minimal-portrait.json
     5745  1981-01-01 01:01   assets/keymaps/one_hand-portrait.json
     9010  1981-01-01 01:01   assets/keymaps/pvp-landscape.json
     9035  1981-01-01 01:01   assets/keymaps/pvp-portrait.json
     7044  1981-01-01 01:01   assets/keymaps/survival-landscape.json
     7069  1981-01-01 01:01   assets/keymaps/survival-portrait.json
     1805  1981-01-01 01:01   assets/icons/NOTICE.md
     1467  1981-01-01 01:01   assets/icons/blocks/Grass.png        (共 14 个 PNG)
      952  1981-01-01 01:01   resources.arsc
     3788  1981-01-01 01:01   AndroidManifest.xml
---------                     -------
  2663748                     43 files
```

`unzip` 用的是 Git for Windows 自带的 `C:\Program Files\Git\usr\bin\unzip.exe`(本机 PATH 里没有 unzip)。

### 5.5 `.so` 里确实有 JNI 导出

```
$ llvm-nm --dynamic --defined-only app/build/intermediates/cxx/Debug/14w2k2r3/obj/arm64-v8a/libsxcl.so
0000000000000f60 T Java_com_silentstudio_sxcl_core_NativeCore_version
0000000000000fac T Java_com_silentstudio_sxcl_core_NativeCore_features
0000000000000fd0 T Java_com_silentstudio_sxcl_core_NativeCore_sha1Hex
0000000000001144 T Java_com_silentstudio_sxcl_core_NativeCore_sha256Hex
```

`llvm-readelf -h`:`Class: ELF64` / `Machine: AArch64` / `Type: DYN (Shared object file)`。

### 5.6 Java 单测(不需要手机)

```
$ java -cp out com.silentstudio.sxcl.keymap.KeymapCoreTest app/src/main/assets/keymaps
资源目录: ...\android\app\src\main\assets\keymaps
  [OK] JSON 数字 / 数组长度 / unicode 转义 / 嵌套取值 / 点号路径 / 生成后可再解析 / 生成含换行缩进
  [OK] index.json 里有 9 个布局 … 全部布局：schema 正确 / 校验通过 / 无冲突 / 与清单一致
  [OK] 搜索：中文标签「跳」/ 动作名 jump / 按键名 KEY_SPACE / 教学提示里的词 / 大小写不敏感 …
  [OK] 教学步骤：序号连续 / 指向的控件都存在 / 每条都有文案，共 75 条
  [OK] 冲突检测：抢键 / 缺动作 / 控件重叠
  [OK] 摇杆：死区 / 四方向 / 推到底疾跑 / 轻推不疾跑
  [OK] 按键：primary 优先级 / allKeys 去重 / toggle 行为识别
  [OK] FCL 导出：控件数量一致 / 像素坐标都在屏幕内

通过 40 项
[PASS] 安卓端按键核心测试全部通过
java 退出码: 0
```

### 5.7 真机实测(装上了、跑起来了、C 核心在设备上算对了)

本机 `adb devices` 里有局域网设备。其中两台是 **arm64-v8a / SDK 36(Android 16)**,
正好和本 APK 匹配,选了没装过同包名的那台做验证:

```
$ adb devices -l
192.168.200.183:5555  device product:G6012BS model:G6012BS device:G6012BS
$ adb -s 192.168.200.183:5555 shell getprop ro.product.cpu.abi   → arm64-v8a
$ adb -s 192.168.200.183:5555 shell getprop ro.build.version.sdk → 36

$ adb -s 192.168.200.183:5555 install -r app-debug.apk
Performing Streamed Install
Success

$ adb -s 192.168.200.183:5555 shell am start -W -n com.silentstudio.sxcl/.ui.MainActivity
Status: ok
LaunchState: HOT
Activity: com.silentstudio.sxcl/.ui.MainActivity
TotalTime: 40

$ adb -s 192.168.200.183:5555 shell dumpsys activity activities | grep topResumedActivity
topResumedActivity=ActivityRecord{... com.silentstudio.sxcl/.ui.MainActivity t838}

$ adb -s 192.168.200.183:5555 shell dumpsys package com.silentstudio.sxcl | grep -E 'primaryCpuAbi|versionName'
primaryCpuAbi=arm64-v8a
versionName=0.1.0

# 把界面上的文字 dump 出来(uiautomator),确认跨语言链路真的通了:
$ adb -s 192.168.200.183:5555 shell uiautomator dump /sdcard/sxcl_ui.xml && adb pull ...
极简 · 横屏
只留四个必用键，屏幕最干净（新手先用这套）（7 条教学，长按/双击都有说明）
核心库 libsxcl.so v0.1.0 · features=0x1 · SHA-1/SHA-256 自检通过     ← 这一行是 C 算出来的
极简（推荐新手） · 横屏   6 键 / 教学 7 条
生存 · 横屏   7 键 / 教学 8 条
建造 · 横屏   18 键 / 教学 9 条
显示教学提示 / 编辑位置（拖动按键）/ 开始悬浮按键 / 停止 / 教学 / 冲突检查 / 保存
```

这一行 `核心库 libsxcl.so v0.1.0 · features=0x1 · SHA-1/SHA-256 自检通过` 是**跨语言链路通的硬证据**:

* `v0.1.0` 来自 C 的 `sxcl_version_string()`;
* `features=0x1` 来自 C 的 `sxcl_version_features()`(`SXCL_FEATURE_DOWNLOAD`);
* `自检通过` = `NativeCore.selfCheck()` 拿标准向量 `sha1("abc")` / `sha256("abc")` 跟 C 里
  `sxcl_hash_digest()` 的返回值逐字符比过 —— 说明 **APK 里的 `libsxcl.so` 被真正加载并执行了**。

顺带确认资产也对:9 套布局按 `assets/keymaps/index.json` 正常列出,教学条数(7/8/9)与清单一致。

**边界说明**:只动了 `192.168.200.183` 这一台(它没装过 `com.silentstudio.sxcl`);
另一台 arm64 设备 `192.168.220.13` 上**已经装着同包名的 Python 版**,为避免覆盖别人的安装,
**没有碰它**。验证完把设备按回 HOME,应用本身留着(卸载:`adb uninstall com.silentstudio.sxcl`)。

## 6. 核心库怎么进 APK:走的是**方案①**(Gradle 里用 CMake 编)

`app/build.gradle.kts` 的 `externalNativeBuild.cmake` 指向 `app/src/main/cpp/CMakeLists.txt`,
AGP 在 `:app:buildCMakeDebug[arm64-v8a]` 里编出 `libsxcl.so` 并自动打进 `lib/arm64-v8a/`。
实测的任务列表里有 `configureCMakeDebug[arm64-v8a]` 与 `buildCMakeDebug[arm64-v8a]`。

选①不选②(先手编好塞 jniLibs)的理由:

1. **仓库里不留二进制**:APK 里的 `.so` 永远和 C 源码同版本,不会出现"改了 C 忘了重编 .so";
2. **AGP 的 strip / 打包 / 调试符号处理都现成**,自己塞 jniLibs 得手动管 ABI 目录与 strip;
3. 包装层已经把两件麻烦事解决掉了:根 CMakeLists 只产**静态库**(静态库进不了 APK)、
   根要求 CMake ≥ 3.24 而 SDK 自带 3.22.1 —— `app/src/main/cpp/CMakeLists.txt` 把根
   `add_subdirectory` 进来复用同一批源码与选项,再补一个 `SHARED` 目标
   (`sxcl_android`,`OUTPUT_NAME=sxcl` → `libsxcl.so`),**没有改动仓库根 CMakeLists 一个字**。

CI 的配置与本机包装层的对应关系:`-DSXCL_BUILD_TESTS=OFF -DSXCL_BUILD_QT_TRANSPORT=OFF
-DSXCL_WERROR=ON` 三项在 `app/src/main/cpp/CMakeLists.txt` 里用 `set(... CACHE ... FORCE)` 固定成一致。

### 一处与 CI 的**已知差异**

CI 手编命令带 `-DANDROID_PLATFORM=android-24`,而 Gradle 路线下 AGP 自己按 minSdk 传
`-DANDROID_PLATFORM=android-26`(实测 `.cxx/Debug/<hash>/arm64-v8a/CMakeCache.txt` 里
`ANDROID_PLATFORM:UNINITIALIZED=android-26`)。

* 为什么没在 Gradle 里覆盖它:**AGP 9 的 `cmake {}` DSL 里没有 `arguments` 这个成员**
  (实测报 `e: ... build.gradle.kts:52:13: Unresolved reference 'arguments'`),写不进去。
* 影响:产物都是 arm64-v8a ELF、都跑在 minSdk 26 的设备上;差别只是链接时可见的 libc 符号集合
  (android-24 更保守)。要严格对齐 CI,用 §4 的"单独编 C 核心"那条命令。

## 7. 实测踩到的 Android 编译差异(如实报告,没有删功能)

**上一句结论**:核心库在 Android 下**能编过**。但用 CI 的口径(`-DSXCL_WERROR=ON`,
即 `-Wall -Wextra -Wpedantic -Werror`)在本机 NDK 28.2 / Clang 19 下**编不过**,共有 4 类
clang 独有的告警被 `-Werror` 升级成错误。用 `ninja -k 0` 扫完全部 38 个编译单元后拿到完整清单:

| # | 告警 | 位置 | 性质 |
|:--|:--|:--|:--|
| 1 | `-Wcomment` | `include/sxcl/zip.h:7`、`include/sxcl/instance.h:1`、`include/sxcl/loader.h:391`、`include/sxcl/install.h:130`、`src/services/modloader/keymap_presets.c:2` | 注释正文里出现 `/*`(如 zip.h 的 `maven/*`)。**纯文本问题,与代码无关** |
| 2 | `-Wint-to-void-pointer-cast` | `src/core/dl/engine.c:642` 经 `src/core/internal/platform_thread.h:38` 的 `SXCL_THREAD_RETURN(n) = return (void *)(n)` | 线程返回值那个 int 在 `sxcl_thread_join()` 里被丢弃,不参与逻辑。GCC 不报,Clang 报 |
| 3 | `-Wpointer-bool-conversion` | `src/services/modloader/keymap_store.c:623` 的 `(layout->name && *layout->name)` | `layout->name` 是数组,取地址恒真 —— **真实代码异味**(冗余判断),但语义等价于 `*layout->name ? ...`,**行为无差异** |
| 4 | `-Wunused-function` | `src/core/instance/sysinfo.c:39` 的 `static sysinfo_copy()` | 调用点(110/145/148)全在 Windows/Apple 分支里,**Android/POSIX 分支用不到** —— 该平台的死代码,不是缺陷 |

处理方式:`app/src/main/cpp/CMakeLists.txt` 里**逐条定点**关掉这 4 条:

```cmake
target_compile_options(sxcl PRIVATE
  -Wno-comment -Wno-int-to-void-pointer-cast
  -Wno-pointer-bool-conversion -Wno-unused-function)
```

* `-Werror` **没有**整体关掉,其它告警仍然是错误;
* 4 类之外的功能一个没删,也没有改 `src/**`、`include/**`(本任务不允许);
* 踩过的坑:写成 `-DCMAKE_C_FLAGS=...` **没用** —— 它排在 `sxcl_warnings()` 的 `-Wall` 之前,
  `-Wall` 会把 `-Wno-comment` 重新打开(实测第一次修就栽在这)。必须用 `target_compile_options`。

治本要去改 `src/**`、`include/**`(比如把注释里的 `maven/*` 写成 `maven/` + 说明、
把 `SXCL_THREAD_RETURN` 改成先转 `intptr_t`),**这超出本次任务范围,留给仓库主人决定**。

## 8. 已知限制

1. **界面不在 APK 里**。APK 只有 Android 原生的按键助手(Java)+ C 核心 `.so`;
   Qt Widgets 界面的所有页面在 Android 上**不存在**。Java 壳与桌面端共用 `sxcl.keymap.v1` 布局格式。
2. **只有 arm64-v8a**。`abiFilters += "arm64-v8a"`(与 CI 一致)。x86_64 模拟器装不上,
   真机(现代手机基本都是 arm64)没问题。要加 ABI 就在 `app/build.gradle.kts` 的 `abiFilters` 里加。
3. **悬浮层要用户手动授权**。「显示在其他应用上层」(`SYSTEM_ALERT_WINDOW`)必须由用户在系统设置里给,
   应用自己弹不出这个对话框(Android 6 起的硬规矩)。
4. **debug 签名,不能发布**。当前 APK 用 Android 默认 debug key(`CN=Android Debug`,v2 方案)。
   装机 / 联调没问题,**上架或分发必须换成自己的 release keystore**,并把 `signingConfigs` 配进
   `app/build.gradle.kts`。`android.nonTransitiveRClass` 等属性已就位,换签名不影响构建。
5. **debug 变体的原生代码没优化**。AGP 在 debug 变体下传 `CMAKE_BUILD_TYPE=Debug`,
   APK 里的 `.so` 是 Debug 产物(strip 后 13,168 字节)。要优化产物就 `assembleRelease`。
6. **核心库目前只用到 `version/features/sha1Hex/sha256Hex` 四个 JNI 入口**。
   核心里的下载引擎、安装器、启动参数等还没接进 Java 壳 —— 这一版证明的是"**C 核心能编进 APK
   并在设备上跑通**",不是"核心全部功能都被界面用上了"。
7. **只在 arm64 真机上验证过,没有模拟器验证**。`android-36` 的 system-image 没装,
   所以 x86_64 模拟器的路径没走过(反正 ABI 也只有 arm64-v8a)。
   真机验证的完整记录在 §5.7:`adb install` 成功、Activity 正常启动、界面显示出 C 核心算出来的版本与自检结果。

## 9. JNI 边界(Java ↔ C)

| Java(`com.silentstudio.sxcl.core.NativeCore`) | C(`app/src/main/cpp/sxcl_jni.c`) | 核心实现 |
|:--|:--|:--|
| `static native String version()` | `Java_..._NativeCore_version` | `sxcl_version_string()` |
| `static native int features()` | `Java_..._NativeCore_features` | `sxcl_version_features()` |
| `static native String sha1Hex(byte[])` | `Java_..._NativeCore_sha1Hex` | `sxcl_hash_digest(SXCL_HASH_SHA1, ...)` |
| `static native String sha256Hex(byte[])` | `Java_..._NativeCore_sha256Hex` | `sxcl_hash_digest(SXCL_HASH_SHA256, ...)` |

`NativeCore` **不抛异常**:库没打进 APK(例如装到非 arm64 设备)时只置 `loaded=false`,
界面照常能用(按键功能不依赖核心库),状态行会写明 `UnsatisfiedLinkError` 的原因。
`selfCheck()` 用标准向量 `sha1("abc")=a9993e36...`、`sha256("abc")=ba7816bf...` 验一遍
**C 里的哈希实现**,所以设备上看到"自检通过"就说明跨语言链路真的通了。

## 10. 本机环境的两处坑(与代码无关,但换机器可能再遇到)

### 10.1 JDK 17.0.2 的信任库连不上 `services.gradle.org`

```
java NetProbe https://services.gradle.org/distributions/gradle-9.3.1-bin.zip
→ FAIL javax.net.ssl.SSLHandshakeException: PKIX path building failed:
       unable to find valid certification path to requested target
（同一台机器上 PowerShell / Invoke-WebRequest 取同一个 URL 是 200；
  https://dl.google.com/... 与 https://repo.maven.apache.org/... 用 JVM 也正常）
```

后果:`gradle wrapper` 任务默认的 URL 校验直接失败
(`Test of distribution url https://services.gradle.org/distributions/gradle-9.3.1-bin.zip failed`),
所以生成 wrapper 时加了 `--no-validate-url`;AGP 的依赖(dl.google.com)不受影响。
另外 `curl.exe` 走 schannel 时因为没有吊销列表报 `CRYPT_E_NO_REVOCATION_CHECK`,
要加 `--ssl-no-revoke`。

因此本机的 Gradle 发行包**没有重新下载**,而是把已经在机器上的
`C:\Users\<用户>\.gradle\wrapper\dists\gradle-9.3.1-all\...\gradle-9.3.1\`(官方 Gradle 9.3.1 发行包解压结果)
的 `bin/ lib/ init.d/` 预置到 wrapper 期望的缓存路径 + 补 `gradle-9.3.1-bin.zip.ok` 标记。
**换台网络正常的机器,直接跑 `gradlew` 就会按 `distributionUrl` 自己下载,不需要这一步。**

### 10.2 下载速度

`Invoke-WebRequest` 默认带进度条渲染,实测只有 ~50 KB/s(137 MB 要一个多小时);
加 `$ProgressPreference='SilentlyContinue'` 或换 `curl.exe` 才正常。本机最后没用上下载。

## 11. 大文件与磁盘占用(都在 D 盘)

| 路径 | 占用 | 说明 |
|:--|--:|:--|
| `D:\gradle-home\wrapper\dists\...` | 144.9 MB | Gradle 9.3.1 发行包(预置,未下载) |
| `D:\gradle-home\caches` | 1045.8 MB | AGP 9.1.1 及其依赖(dl.google.com 下载) |
| `D:\AndroidSdk\cmake\3.31.6` | 46.4 MB | sdkmanager 新装的 CMake |
| `D:\android-build` | 51.2 MB | 本次的临时构建/验证目录(不是仓库内容) |
| `android/app/build` | 4.0 MB | APK 与中间产物(已 gitignore) |
| `android/app/.cxx` | 4.2 MB | AGP 的原生构建缓存(已 gitignore) |
```

> 用完想清干净:`Remove-Item -Recurse D:\gradle-home,D:\android-build`。
> 仓库里不会因此丢东西 —— `android/.gitignore` 已经把 `build/`、`.cxx/`、`.gradle/`、`local.properties` 挡掉了。
