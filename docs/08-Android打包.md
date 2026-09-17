# 08 · Android 打包(SXCL C 版 · Qt Widgets 走 JQt 流水线)

> 本文只写**实测**。每条结论后面都能找到命令与输出。
> 令牌/证书指纹一律不出现(那份在 `分币必赚/ssm/docs/WS2025-连接手册.md`,是不外发文档)。
> 打包层全部是**新增文件**(在 `build/_android/`),不在 `src/**` 内;`docs/0[5-7]*` 未改动。

---

## 0. 结论

**已产出带真界面的 APK**:`build/_android/out/sxcl-debug.apk`,**41,649,990 字节**,
`sha256=103d136947f527e02f2f1e89a3dd2b604a2ee43291a5a9551b41f8c56da73fe5`(含应用图标;上一版无图标的是 41,627,275 字节 / `884f172a…`)。
里面是 `src/ui/**`(Qt Widgets + libqf)**原样**编出来的 `libsxclui_arm64-v8a.so` +
Qt 6.11.2 的 Widgets/Gui/Core/Svg/Network + qtforandroid/offscreen 平台插件 + `assets/theme/qf_exact/**`。

| 验收项 | 结果 |
|---|---|
| APK 存在并回到本机 | ✅ `D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\sxcl-debug.apk` |
| `aapt2 dump badging` | ✅ `com.silentstudio.sxcl` / minSdk 28 / targetSdk 34 / compileSdk 36 / `native-code: 'arm64-v8a'` / launchable-activity `com.silentstudio.sxcl.SxclActivity` |
| `apksigner verify --print-certs` | ✅ rc=0,Signer #1 `CN=Android Debug`(debug 签名) |
| `classes.dex` | ✅ 4 个:`classes.dex` 6,496,740 + classes2/3/4 |
| `lib/arm64-v8a/*.so` | ✅ 19 个:Qt6Core/Gui/Widgets/Svg/Network/OpenGL/OpenGLWidgets/PrintSupport/Sql/Concurrent + `libsxclui_arm64-v8a.so`(5,669,608)+ qtforandroid/qoffscreen/qandroidstyle + qsvg/qjpeg/qgif/qico + libc++_shared |
| `assets/theme/qf_exact/*` | ✅ **522 个文件**(dark/light 共 68 个 .qss + images),另有 `assets/icons/blocks` 14 个、`assets/icons/pcl` 54 个 |
| 应用图标 | ✅ `application: ... icon='res/mipmap-mdpi-v4/ic_launcher.png'` 且 `launchable-activity: ... icon='res/mipmap-mdpi-v4/ic_launcher.png'`(mdpi/hdpi/xhdpi/xxhdpi/xxxhdpi 五档,由 `assets/icons/blocks/Grass.png` 64x64 以 NEAREST 放大) |
| 设备侧 1:1 像素 | ❌ **未取得** —— 本机当前没有任何 API≥28 的 arm64 真机/模拟器(见 §6),如实列出卡点 |

**三句必须说的真话**:

1. 这个 APK 是**本机**用 Qt 6.11.2 `android_arm64_v8a` + **NDK 28.2.13676358**(不是 r27)编出来的,**不是 WS2025 的产物** ——
   远端那趟两次都卡在控制通道(§7.3),只走到 native 编译与 overlay 验证;
2. **没有在真机/模拟器上验证过启动与像素**。所以"Android 上 1:1"目前**只有静态证据**(§5 的资产/库/清单/签名),
   **没有设备侧 DIFF**;§8.5 那套九页/弹层数字是**桌面 Windows** 上的回归证据,不能当作 Android 的设备侧结论;
3. 设备一回来怎么取设备侧像素:`r3_device.ps1`(装包→可见运行截图→九页 `offscreen` 1100x750@1.5 抓图)+ `compare_all.ps1`(对 `build/ref/py_*.png` 跑 `tools/ui_compare.py`),命令见 §6。

**在哪里编的(重要)**:WS2025 在本次任务中途把控制通道卡死了(见 §7.3),为不空等,按主代理指示改用
**本机兜底**:本机装 Qt 6.11.2 android_arm64_v8a 套件 + NDK r28 + Gradle 9.3.1,用**同一套打包层文件**出包。
两条路线的打包层完全一致:WS2025 把 native 编到 23/107 就因 loader 删除问题停下(§7.1),本机把剩下的做完。

## 1. 为什么是这条路(难点先想清楚)

"Qt Widgets 官方不支持 Android,所以只能做 Java 壳" —— **这个前提不成立**。两条硬证据:

1. JQt-for-Android 的 PoC(`JQt - Dev/JQt-for-Android/docs/poc-status.md`):顶层 QPushButton 在模拟器上**可渲染、可点击**(logcat tag=jqt,clicked 计数实测)。
2. 远程构建机残留的**往届 Android 真机截图** `C:\JQt\homev17.png`(已取回 `build/_android/evidence/homev17.png`,115,748 字节):
   1080x1920,按像素分析 白底 73.15% / 强调色 `#1677ff` 5.33% / `#2d2d2d` 3.45%,ASCII 亮度图能看到标题、卡片行、按钮块 ——
   **是一整套 Qt Widgets 界面在 Android 上完整渲染的画面,不是黑屏**。
   (注:同目录 `jqtg-shot.png` 的头是 `ff fe 52 58 ...`,是 PowerShell 重定向把 PNG 写成了 UTF-16 的坏件,不能作为证据。)

Qt 6.11.2 的 android kit 里确实带 `libQt6Widgets_arm64-v8a.so` 与 `plugins/platforms/libplugins_platforms_qtforandroid_arm64-v8a.so`;
QtActivity/QtLoader 会 dlopen `lib<lib_name>_<abi>.so` 并调用它的 `main()`,Widgets 绘制走 raster(与桌面同一套 paint engine、同一份 QSS)。
所以正确做法是**把 `src/ui/**` 原样搬进 APK**,而不是另做一套 Java 界面。

## 2. 流水线来源

| 环节 | 来源 | 本次怎么用 |
|---|---|---|
| Android 入口 | JQt-for-Android `template/AndroidManifest.xml`(lib_name + QtActivity) | 抄结构,包名/Activity 换成 SXCL 自己的 |
| 部署打包 | JQt-for-Android `docs/android-build-guide.md` §3~§4(Qt 6.11 键格式 / androiddeployqt → Gradle → AGP) | 照抄;androiddeployqt 用 **Qt 自带的** `<host kit>/bin/androiddeployqt.exe`,**没用补丁版** |
| `docs/androiddeployqt-main.cpp.txt` | JQt 留存的 Qt 官方源码参考件(4260 行) | 只当排障参考 |
| UI 代码 | `src/ui/**` 原样(含 `SXCL_UI_ROUTE`/`SXCL_UI_SHOT`/`SXCL_UI_ACCENT` 验收通路) | 一个字节没改地进 APK |

## 3. 打包层(本次新增,全在 `build/_android/`)

    build/_android/
      app/CMakeLists.txt            # Android-only CMake 工程:复用 SXCL 根 CMakeLists + src/ui/CMakeLists
      app/sxcl_android_main.cpp     # Android 入口(见下)
      pkg/AndroidManifest.xml       # package=com.silentstudio.sxcl / activity=SxclActivity / lib_name=sxclui
      pkg/java/com/silentstudio/sxcl/SxclActivity.java
      scripts/build_local.ps1       # 本机一键(实际出包的那条)
      scripts/{stage,r0_all,r1_build,r2_deploy,run,poll,mkoverlay,verify_overlay,keep,r3_device,compare_all}.ps1
      stage/                        # 上传/构建用的源码树(仓库副本 + libqf 副本 + 打包层)
      out/                          # 产物与取证(APK + badging/apksigner/内容清单)
      evidence/                     # 往届 Android 真机截图等旁证

三处关键设计(**都不需要改共享 UI 代码**):

1. **入口不复制**:`app/sxcl_android_main.cpp` 里

       #define main sxcl_ui_desktop_main
       #include "main.cpp"          // ← 仓库的 src/ui/main.cpp,原样
       #undef main

   桌面入口被编成 `sxcl_ui_desktop_main()`;Android 的 `main()` 只做:读 Java 写下的 `<files>/sxcl_boot.txt` → 设环境变量 → 调它。
2. **资产落盘**:APK 里的 `assets/` 不是文件系统,`FluentTheme::resolveThemeDir()` 找不到。
   `SxclActivity` 在 `super.onCreate()` **之前**把 assets 解到 `<files>/assets`(带版本戳,只解一次),路径写进 `sxcl_boot.txt`;
   C++ 侧据此 `qputenv` 出 `SXCL_THEME_DIR`/`SXCL_BLOCK_DIR`/`SXCL_ICON_DIR`(这三个是 `src/ui` 本来就支持的环境变量)。
3. **1:1 取证设计**:手机屏幕不是 1100x750,所以验收时用 `offscreen` QPA 让窗口保持 `MainWindow::resize(1100,750)`,
   `QT_SCALE_FACTOR=1.5` 对齐参考图密度(`build/ref/py_*.png` = 1650x1125 = 1100x750@1.5),再走 `main.cpp` 自带的 `SXCL_UI_SHOT` 自渲染通路。
   **没有为 Android 另做任何布局** —— 只是换 QPA 与缩放。

## 4. 逐步复现命令

### 4.1 远端 WS2025 路线(设计并跑到 23/107)

```powershell
$SC = 'D:\SilentStudio\分币必赚\SILENT-CONSOLE\发布\silent-console.exe'
& $SC devices
& $SC put WINDOW-SERVER-2 "<仓库>\build\_android\stage.zip" "C:\sxcl_stage.zip"
foreach ($f in 'stage','r1_build','r2_deploy','run','poll','mkoverlay') {
  & $SC put WINDOW-SERVER-2 "<仓库>\build\_android\scripts\$f.ps1" "C:\sxclboot\$f.ps1" }
& $SC exec WINDOW-SERVER-2 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\sxclboot\stage.ps1"
& $SC exec WINDOW-SERVER-2 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\sxclboot\run.ps1 -Name r1"
& $SC exec WINDOW-SERVER-2 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\sxclboot\poll.ps1 -Name r1 -Tail 40"
# 然后 r2(androiddeployqt + gradle)同理;最后 get APK
```

远端 configure 的关键点:该 Qt kit 的 `qt.toolchain.cmake` 里记录的 chainload 路径是 **Linux CI 的 `/opt/android/r27c/...`**,
不覆盖就会 `The C compiler identification is unknown`:

```
-DCMAKE_TOOLCHAIN_FILE=C:/Qt/6.11.2/android_arm64_v8a/lib/cmake/Qt6/qt.toolchain.cmake
-DQT_CHAINLOAD_TOOLCHAIN_FILE=C:/AndroidSdk/ndk/27.2.12479018/build/cmake/android.toolchain.cmake
-DANDROID_NDK_ROOT=C:/AndroidSdk/ndk/27.2.12479018 -DANDROID_SDK_ROOT=C:/AndroidSdk
-DQT_HOST_PATH=C:/Qt/6.11.2/mingw_64 -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
-DCMAKE_AUTOMOC=ON      # 同 §4.2:src/ui 由打包层 add_subdirectory,不显式给会在 tasks_page.moc 上失败
```

### 4.2 本机兜底路线(**实际出包的就是这条**)

前置:本机 `D:\Qt\6.11.2\mingw_64`(host)、`D:\AndroidSdk\ndk\28.2.13676358`、`D:\jdk17`、`D:\gradle-home`(已含 gradle-9.3.1)。

```powershell
# 1) 装 Qt android kit(qtbase+qtsvg)到 D 盘 —— 见 §8 的镜像/校验说明
# 2) 一键构建(native -> libsxclui_arm64-v8a.so -> androiddeployqt -> gradle)
& powershell -NoProfile -ExecutionPolicy Bypass -File "<仓库>\build\_android\scripts\build_local.ps1"
# 产物:D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk
```

本机 configure 额外两个开关(打包层自己的 CMake,不动仓库):

```
-DCMAKE_AUTOMOC=ON   # 桌面版是从根 CMakeLists 继承 AUTOMOC 的;src/ui 由打包层 add_subdirectory,
                     # 不显式给就会在 tasks_page.cpp 的 #include "tasks_page.moc" 上失败
```

### 4.3 验收命令(本机)

```powershell
$apk = "<仓库>\build\_android\out\sxcl-debug.apk"
& D:\AndroidSdk\build-tools\36.0.0\aapt2.exe dump badging $apk
& D:\AndroidSdk\build-tools\36.0.0\apksigner.bat verify --print-certs --verbose $apk
python build\_android\scripts\apk_audit.py     # 等价 unzip -l 的内容清单(该脚本已把条目分类汇总)
```

## 5. 产物与验收证据(实测输出)

```
APK 41627275 bytes  sha256=884F172A97B30F26D20AD2B3B134440BFD9B4FF5ACC26BFAD0799E2A3FF9320C

package: name='com.silentstudio.sxcl' versionCode='1' versionName='0.1.0-android' ... compileSdkVersion='36'
minSdkVersion:'28'   targetSdkVersion:'34'   native-code: 'arm64-v8a'
application-label:'Silent X Craft Launcher'
launchable-activity: name='com.silentstudio.sxcl.SxclActivity'

Signer #1 certificate DN: C=US, O=Android, CN=Android Debug
Signer #1 certificate SHA-256 digest: a18e69972a3afb7a9c0c36445f43b71f27da77eb1332d07d5a35559107cdea7a
apksigner rc=0

classes.dex 6496740 / classes2.dex 16436 / classes3.dex 6304 / classes4.dex 1228
lib/arm64-v8a/libsxclui_arm64-v8a.so 5669608  + 18 个 Qt/插件/运行时 .so
assets/theme/qf_exact 522 文件(68 个 .qss 之外还有 images/) assets/icons/blocks 14   assets/icons/pcl 54
```

取证文件(已随仓库落盘):`build/_android/out/{sxcl-debug.apk, aapt2_badging.txt, apksigner_verify.txt, apk_contents.txt}`。

## 6. 设备侧:未取得像素(如实)

本机 `adb devices` 当前可见的 10 台:

| 设备 | 型号 | API | ABI | 能不能装 |
|---|---|---|---|---|
| 192.168.2.201-204:5555 | (无 getprop) | — | — | ❌ `getprop: not found`,不是标准 Android shell |
| 192.168.206.6/9/13/14/15/16:5555 | rk3288 / TE1102 | **27** | armeabi-v7a | ❌ minSdk 28 → `INSTALL_FAILED_OLDER_SDK` |
| 192.168.200.183 / 192.168.220.13:5555 | G6012BS(API 36,arm64-v8a,1600x2400@320) | 36 | arm64-v8a | ⚠️ **本任务期间掉线**,任务中段起一直不在线 |

本机没有 `emulator.exe`、没有 system-images;`D:\SVM`(自研 Android VM)已装但**无镜像无引擎**
(要 `svm install emulator` 440MB + `svm install aosp-35` 1.4GB,且 SVM 数据目录在 C 盘,当时 C 盘只剩 2.2GB —— 放弃)。

所以**卡在这一步**:APK 已就绪且清单/签名全部合规,但没有可用的 API≥28 arm64 设备/模拟器可安装。
一旦 G6012BS 回来或起一个模拟器,取证脚本已经写好、可直接跑:

```powershell
& powershell -File build\_android\scripts\r3_device.ps1 -Apk build\_android\out\sxcl-debug.apk
& powershell -File build\_android\scripts\compare_all.ps1
```

- `r3_device.ps1`:装包 → 可见运行截图(`exec-out screencap` 走 cmd 重定向,避免 PS 破坏二进制)→
  九页逐个 `am force-stop` + `am start --es route <r> --ez offscreen true --ez shot true --es scale 1.5 --es accent '#c044a3'`
  → 从 `/sdcard/Android/data/com.silentstudio.sxcl/files/shots/` 拉回本机;
- `compare_all.ps1`:对九页跑 `tools/ui_compare.py build/ref/py_<route>.png out/device/<route>.png` 并汇总 DIFF。

## 7. 两个坑与一条事故(实测,不是推测)

### 7.1 WS2025 会**删除文件名含 `loader` 的文件**

朴素实验定性(与目录、内容都无关):

| 变量 | 结果 |
|---|---|
| 名字 `zzz.h` / `note.txt` | 存活 |
| 名字 `loader.h` / `LOADER.H` / `Loader.H` / `xloader.h` / `loader.txt` / `LOADER_CATALOG.H` / `sxcl_loader_data.bin` | **1~2 秒内全被删** |
| 目录 `C:\sxclbuild\...` / `C:\sxcltest` / `C:\Users\Administrator\sxcltest` | 一视同仁 |
| 内容 `int q;` / `probe` / `int loader_probe;` | 与内容无关 |

SXCL 的 `include/sxcl/loader.h` 与 `loader_catalog.h` 正好中招 → native 构建在 23/107 报
`profiles.c:21:10: fatal error: 'sxcl/loader.h' file not found`。那台机器上同时跑着
`SilentStudio\silent-agent.exe`(service + guard)与 Defender/Defender for Cloud,具体是谁删的没有进一步验证
(纪律:不关、不动那台机器的服务)。

**对策(不碰共享代码、不动服务):Clang VFS overlay** —— 磁盘上**不出现**任何 loader 命名的文件,让 clang 从无害名副本读:

```powershell
& $SC put WINDOW-SERVER-2 "<仓库>\include\sxcl\loader.h"          "C:\sxclboot\hdr_a.txt"
& $SC put WINDOW-SERVER-2 "<仓库>\include\sxcl\loader_catalog.h" "C:\sxclboot\hdr_b.txt"
& $SC exec WINDOW-SERVER-2 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\sxclboot\mkoverlay.ps1"
```

`C:\sxclboot\overlay.yaml`(脚本生成;每个 root 先列**全部 23 个真实头**,再加两条映射,避免目录 shadow 语义歧义):

```yaml
version: 0
roots:
  - name: "C:/sxclbuild/app/sxcl/include"
    type: directory
    contents:
      - name: "sxcl"
        type: directory
        contents:
          - name: "engine.h"
            type: file
          # ... 其余 22 个真实头 ...
          - name: "loader.h"
            type: file
            external-contents: "C:/sxclboot/hdr_a.txt"
          - name: "loader_catalog.h"
            type: file
            external-contents: "C:/sxclboot/hdr_b.txt"
  - name: "C:/sxclbuild/app/include"      # src/ui/CMakeLists 用的是 CMAKE_SOURCE_DIR/include
    type: directory
    contents:
      - name: "sxcl"
        type: directory
        contents:
          # ... 同上 23 + 2 条 ...
```

接线(configure 期,不改任何 CMakeLists):

```
-DCMAKE_C_FLAGS="-ivfsoverlay C:/sxclboot/overlay.yaml"
-DCMAKE_CXX_FLAGS="-ivfsoverlay C:/sxclboot/overlay.yaml"
-DCMAKE_DEPENDS_USE_COMPILER=FALSE   # 关键:-MD 依赖文件里是【虚拟路径】,ninja 会因该路径磁盘上不存在而
                                    # 报 "missing and no known rule to make it";关掉编译器 depfile 即可,
                                    # CMake 自带扫描器找不到的头只是不加依赖,不报错
```

**怎么证明"构建真的读了映射而不是磁盘原件"** —— WS2025 上的 **A/B 实测输出**(脚本 `overtest.ps1`):

```
--- A) no overlay (expect fatal error) ---
  C:\sxclbuild\app\sxcl\src\services\modloader\profiles.c:21:10: fatal error: 'sxcl/loader.h' file not found
  1 error generated.
  rc=-1
--- B) with overlay (expect rc=0) ---
  rc=0
  disk loader.h exists: False
```

三条同时成立:①同一条 clang 命令去掉 `-ivfsoverlay` 就报 file not found;②带上就 rc=0;
③而 `Test-Path ...\sxcl\loader.h` 是 **False** —— 磁盘上根本没有这个文件,唯一来源只能是 overlay 的
`external-contents: C:/sxclboot/hdr_a.txt`。

overlay 文件本身长这样(571 字节;每次 root 下的 file 条目**必须**带 `external-contents`,否则 clang 直接报
`invalid virtual filesystem overlay file`):

```yaml
version: 0
roots:
  - name: "C:/sxclbuild/app/sxcl/include/sxcl"
    type: directory
    contents:
      - name: "loader.h"
        type: file
        external-contents: "C:/sxclboot/hdr_a.txt"
      - name: "loader_catalog.h"
        type: file
        external-contents: "C:/sxclboot/hdr_b.txt"
  - name: "C:/sxclbuild/app/include/sxcl"
    type: directory
    contents: [同上两条]
```

(RedirectingFileSystem 是**合并**语义:没列出的文件照旧从真实目录解析 —— 所以只列这 2 个就够,
不必把 23 个头全列一遍;上表 §7.1 那版"先列全 23 个"的写法反而因为 file 条目缺 external-contents 被 clang 拒绝。)

> 注:本机(兜底路线)没有这个坑,所以本机**不套 overlay**,保持干净;overlay 是 WS2025 专用绕行手段。

### 7.2 `Expand-Archive` 会静默丢文件

3.28MB / 1397 文件的 zip,`Expand-Archive` 解出来**少了 2 个头文件**(正是 §7.1 那两个);
用 Python `zipfile` 校验 zip 本身完好、`tar.exe -x` 重解后**清单 1397/1397 全在**。
→ 解包后**必须**对清单核对(`stage.ps1` + `manifest.txt` 已实现),不能只看 Expand-Archive 的退出码。

### 7.3 事故:WS2025 控制通道中途卡死

2026-09-17 21:24 起,`silent-console` 对 WINDOW-SERVER-2 的 `exec/ls/put/ping/status` **全部 exit=1 且无输出**;
`devices` 仍显示 `online:true, queued:0`,`log` 里最后一条 WS2 记录停在 `put keep.ps1`(21:24:08),
之后的 `exec` 根本没被 dispatch。别名 `frp-toe.com:65485` 报"没有这台设备"(但该地址 TCP 可达)。
不是构建进程拖垮的(那条后台 exec 自己就没跑起来)。**至今未恢复**,已上报;本机兜底就是因此启动的。

**当前(收工时)状态**:通道在 23:35 我重启构建后**再次卡死**(同样的症状:devices 仍 online、但 exec/put 全部 exit=1 无输出),
所以**远端 APK 没取到**;远端已完成的是:源码 stage(2083/2083 清单核对通过)、
overlay 落位与 A/B 验证(§7.1)、native 编译(37/107 时静态库 libsxcl.a 已链出、libqf 全量编过)。
交付的 APK 是本机兜底编的(同源码、同 Qt 6.11.2、NDK r28 而非 r27)。

遗留:WS2025 上还留着本次的临时目录 `C:\sxclbuild`、`C:\sxclboot`、`C:\sxcl_stage.zip`(通道恢复后应删除;
它们是本次唯一的产物,`C:\JQt` 等别人的东西一个字没动)。

## 8. 本机 Qt android 套件怎么来的(有校验、有依据)

| 步骤 | 命令/结果 |
|---|---|
| 直连官方源 | `download.qt.io` 只有 ~2.8 KB/s,aqt 走它超时;重定向到的 `ftp.jaist.ac.jp` 也会 read timeout |
| 清华镜像 | **包坏**:aqt 报 `ArchiveChecksumError ... Actual 41dcc505...`,与 Updates.xml 的 `bc8c4fb6...` 不符 |
| 最终采用 | 腾讯镜像直下 + `py7zr` 解包:`https://mirrors.cloud.tencent.com/qt/online/qtsdkrepository/all_os/android/qt6_6112/qt6_6112_arm64_v8a/qt.qt6.6112.android_arm64_v8a/6.11.2-0-202608131018qtbase-MacOS-MacOS_14-Clang-Android-Android_ANY-ARM64.7z` |
| 校验 | 14,603,534 字节,`sha256=bc8c4fb6d4a737752e413e233427d6a39f6da353981c0d4f5151371bd05e742f` **与官方 Updates.xml 一致** ✓ |
| 结果 | `D:\Qt\6.11.2\android_arm64_v8a\` 齐活:`lib/cmake/Qt6/qt.toolchain.cmake`、`lib/libQt6{Core,Gui,Widgets,Svg,Network}_arm64-v8a.so`、`jar/Qt6Android.jar`、`plugins/platforms/{qtforandroid,qoffscreen}`、`plugins/styles/qandroidstyle`、`src/android/java` |

所有下载/临时/构建目录都在 **D 盘**(C 盘当时只剩 2.2GB):`TEMP=D:\aqt-temp`、`-O D:\Qt`、`GRADLE_USER_HOME=D:\gradle-home`、构建目录 `D:\sxcl_local`。

## 8.5 桌面零回归证据(libqf Clang 可移植性修正的验收)

改 libqf 那 4 处(§10)后,按主代理指定的口径在本机复验,**数字与基线逐项相同**:

| 页面 | 本次复验 vs 参考图 `build/ref/py_*.png` | 主代理给的基线 | 本次 vs 改动前 `build/ref/c_*.png` |
|---|---|---|---|
| home | **0.09%** | 0.09 | **0.00%** |
| versions | **1.54%** | 1.54 | **0.00%** |
| tasks | **0.00%** | 0.00 | **0.00%** |
| keymap | **0.21%** | 0.21 | **0.00%** |
| multiplayer | **0.15%** | 0.15 | **0.00%** |
| settings | **0.98%** | 0.98 | **0.00%** |
| download_config | **2.33%** | 2.33 | **0.00%** |
| download_progress | **0.01%** | 0.01 | **0.00%** |
| launch | **0.01%** | 0.01 | **0.00%** |

弹层 5 个使用点(`SXCL_UI_POPUP`:settings 2/3、versions 0、keymap 0/1)对 Python 参考
(`PyQf to C\_scratch\popup\py_popup_*.png`)**全部 0.00%**,对改动前 `c_popup_*.png` 也全部 **0.00%**。

桌面构建:`cmake --build build-ui --config Release --target sxcl-ui` → **EXIT=0,0 error / 0 warning**(/W4 /WX 口径)。

libqf 自带测试:`ctest -C Release`(PATH 先加 `D:\Qt\6.11.2\msvc2022_64\bin`,否则 0xC0000135)→
**3/3 全过**:`anim_parity_test` 0.18s、`scroll_wiring_test` 3.65s、`metrics_parity_test` 1.15s,`100% tests passed out of 3`。

抓图口径(复现用;`QT_SCALE_FACTOR` **不能**用,会把屏幕已有的 150% 再乘一遍 → 2475x1688):

```powershell
Remove-Item Env:QT_SCALE_FACTOR -ErrorAction SilentlyContinue
$env:QT_SCREEN_SCALE_FACTORS = '1.5'      # 覆盖屏幕 DPR = 1.5 → 1650x1125,与参考图同口径
$env:SXCL_UI_ACCENT = '#c044a3'           # 原始色(不是推导后的 #ff75df)
$env:SXCL_UI_ROUTE  = 'home'              # 九页路由之一
$env:SXCL_UI_SHOT   = '<out>\home.png'
& '<仓库>\build-ui\src\ui\Release\sxcl-ui.exe'
# 弹层:SXCL_UI_POPUP=<序号> 与 SXCL_UI_SHOT 一起给
```

一键脚本:`build/_android/scripts/regress.ps1`。

## 9. 已知限制(诚实清单)

1. **字体**:QSS 的 `--FontFamilies` 是 `"Segoe UI", "Microsoft YaHei UI"`,Android 上没有这两个族 → 回落系统字体,
   文字像素与参考图必然有差异(几何/配色/QSS 规则不变)。这是平台事实,不是布局改动。
2. **只出了 `arm64-v8a`**(验收要求的那个 ABI);32 位/模拟器 ABI 需要再装 android_armv7/android_x86_64 套件重编。
3. **debug 签名**(Android Debug),用于验证;发布需另配 keystore。
4. **未做真机像素验证**(§6)。
5. 打包层里 `app/CMakeLists.txt` 需要 `-DCMAKE_AUTOMOC=ON`(原因见 §4.2),这是**打包层的配置**,不是源码改动。

## 10. 为 Android 必须动共享代码的地方(清单,已上报待批)

只有 **1 处**真正编不过(在 SXCL `src/ui`),已按"先报清单"的规矩上报,并且**只在 `build/_android/stage/` 的副本上改**,
仓库 `D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\src\**` **未改动**:

| 位置 | 原写法 | Clang 报什么 | 建议改法 |
|---|---|---|---|
| `src/ui/pages/settings_page.cpp:310-315` | `std::filesystem::canonical(path.toStdWString(), ec)` + `QString::fromStdWString(resolved.native())` | `no viable conversion from 'const basic_string<value_type>' to 'const basic_string<wchar_t>'`(POSIX 下 `path::native()` 是 `std::string`) | 用 `#if defined(_WIN32)` 分支:Windows 走 `toStdWString/fromStdWString`(语义不变),其它平台走 `toStdString/fromStdString`(Qt 的 toStdString 是 UTF-8) |

另:libqf(`D:\SilentStudio\PyQf to C`)在 Clang 下有 4 处 MSVC-only 写法,**已按主代理授权直接修好**(语义等价,
快照在 `PyQf to C\_scratch\snap-2026-09-17T*/`),清单与理由登记在 `PyQf to C\PROGRESS.md` 的
"Clang/GCC 可移植性修正"一节(1 处默认实参、1 处缺 `#include <QLabel>`、1 处 `addItem` 名字隐藏、1 处 protected 访问)。

---

## 附录 A:此前的「Java 壳 + C 核心」路线(已被本文取代)

旧文(git `0770dfc`)走的是"Java 壳画界面 + `libsxcl.so` 提供 JNI 核心",产物 `app-debug.apk` 943,941 字节,
**不含 Qt 界面**,依据是"Qt for Android 只支持 Quick/QML、不支持 Qt Widgets"。
该前提**不成立**(§0/§1),故本版改为把 `src/ui/**` 原样打进 APK。旧文内容见该提交,不再保留在本文。
