# JRE 侧共享库的源码(android/jni/jre-libs/)—— 来源、许可、怎么用

这里是**vendor 进来的第三方源码**,不是我们写的。用途只有一个:交叉编译出两个 JRE 侧
共享库,让安卓上的 JVM 能起来。产物不签入仓库,构建时由
`android/scripts/build_jre_libs.ps1` 生成到 gitignore 的
`build/_android/jre-libs/<abi>/`,再由 `sync_to_build.ps1` 通过
`android-extra-libs` 打进 APK 的 `lib/<abi>/`(= `nativeLibraryDir`)。

| 产物 | 源码 | 为什么必须随包发布 |
|---|---|---|
| `libawt_xawt.so` | `awt_xawt/xawt_fake.c` | java.awt / sun.awt 的**无操作桩**。安卓 arm64 的 JRE 资产(jre8/17/21/25)里没有(或带的是链 X11 的真实现),而 JVM 会按**文件名**从 `<jre>/lib` dlopen 它;缺了就起不来。 |
| `libjsound.so` | `jsound/*.c` | OpenJDK `javax.sound` 的原生核心(DAUDIO 后端)+ OpenAL 平台后端。jre17/21/25 资产里没有这个文件;jre8 自带的是 ALSA 版、安卓上没有后端。 |

装完 JRE 之后由启动器从 `nativeLibraryDir` 拷进 `<jre>/lib`(jre8 是 `<jre>/jre/lib`),
语义与 FCL 的 `RuntimeUtils.patchJava()` 完全一致(见
`include/sxcl/android.h` 的 `sxcl_android_jre_patch_libs`)。

## 来源

vendor 自 **Fold Craft Launcher (FCL)** 的 JNI 源码树
(<https://github.com/FCL-Team/FoldCraftLauncher>,本机参考检出 `D:\SilentStudio\_ref\FCL`):

| 本目录 | FCL 里的路径 |
|---|---|
| `awt_xawt/xawt_fake.c` | `FCL/src/main/jni/awt_xawt/xawt_fake.c` |
| `jsound/*` | `FCL/src/main/jni/jsound/*`(全部 .c/.h,一个不少) |

编译参数也照抄 FCL 的 `FCL/src/main/jni/CMakeLists.txt`(jsound 目标):
`X_PLATFORM=X_LINUX`、`_LITTLE_ENDIAN`、`USE_DAUDIO=TRUE`、`USE_PORTS=FALSE`、
`USE_PLATFORM_MIDI_OUT=FALSE`、`USE_PLATFORM_MIDI_IN=FALSE`(PORT/MIDI 子系统编掉、
音频走 `jsound_openal.c`、JDK8 缺的三个 native 由 `jdk8_compat.c` 补齐)。

## 许可(逐文件已核对)

| 文件组 | 上游 | 许可 | 版权头 |
|---|---|---|---|
| `jsound/{Utilities,Platform,DirectAudioDevice,DirectAudioDeviceProvider,PortMixer,PortMixerProvider,MidiInDevice,MidiInDeviceProvider,MidiOutDevice,MidiOutDeviceProvider,PlatformMidi}.c`、`jsound/{Configure,DirectAudio,PlatformMidi,Ports,SoundDefs,Utilities}.h` | OpenJDK(`jdk17u` 的 `src/java.desktop/share/native/libjsound`,`jdk8u` 该目录字节级一致) | **GPL-2.0 only + Classpath 例外** | **原样保留 Oracle 头,不许删改** |
| `jsound/{jsound_openal.c,jdk8_compat.c,jni_util.h}`、`jsound/com_sun_media_sound_*.h`(9 个,`javah` 产物的替代品) | FCL 自己写的 | **GPL-3.0**(FCL 仓库的 LICENSE) | FCL 的注释头,原样保留 |
| `awt_xawt/xawt_fake.c` | FCL(其上游为 PojavLauncher 系) | 按 FCL 的 **GPL-3.0** 口径使用 | **上游就没有版权头**(首行即 `#include <jni.h>`),我们**不添加**自己的版权头,避免错误主张权利 |

与本项目(AGPL-3.0 + 附加条款,`LICENSE`)的兼容性:

* GPL-3.0 → AGPL-3.0:GPLv3 §13 **明文允许**与 AGPLv3 作品链接/合并成一个组合作品。
* GPL-2.0 only + Classpath 例外 → 该例外**明文允许**把这些"独立模块"与任意许可的代码
  链接,并按**你选择的条款**分发组合后的可执行文件;前提是库本身仍按 GPLv2 履行义务:
  **保留版权头、随包提供对应源码、标明许可**。
  本仓库做法:源码就在 `android/jni/jre-libs/`(随仓库分发)、版权头一字未改、
  产物是**独立的 .so**(JVM 用 `dlopen` 按名字加载,动态链接),docs/18 里写明来源与许可。
  **不要**把这两个库静态链进我们自己的二进制 —— 那会把整个组合卷进 GPLv2 的传染范围。

## 复现 / 校验

    # 交叉编译(NDK clang,arm64-v8a,minSdk 28;缺任何一个都算失败)
    pwsh -File android/scripts/build_jre_libs.ps1
    # 产物
    build/_android/jre-libs/arm64-v8a/libawt_xawt.so
    build/_android/jre-libs/arm64-v8a/libjsound.so
    # 打进 APK(android-extra-libs),构建脚本在缺文件时直接 abort
    pwsh -File android/scripts/build_apk.ps1

vendor 版本固定(SHA-256 前 16 位,和 FCL 检出逐一核对过):

```
awt_xawt/xawt_fake.c                             704ECDAB24174E9D   3976
jsound/com_sun_media_sound_DirectAudioDevice.h   D9EB4412F10B7D55   2717
jsound/com_sun_media_sound_DirectAudioDeviceProvider.h 6C65E0C6BC660E8D   598
jsound/com_sun_media_sound_MidiInDevice.h        C53F5ACCC1055F61   1022
jsound/com_sun_media_sound_MidiInDeviceProvider.h 3006AE151BA689DD    943
jsound/com_sun_media_sound_MidiOutDevice.h       A35791440CFD01E8   1006
jsound/com_sun_media_sound_MidiOutDeviceProvider.h 6AE57AC1AB0EB102   950
jsound/com_sun_media_sound_Platform.h            F85BFCFECD43AB18    403
jsound/com_sun_media_sound_PortMixer.h           1D8B3BCF393FEB9D   1534
jsound/com_sun_media_sound_PortMixerProvider.h   655A58B03B39F3F3    558
jsound/Configure.h                               68405CF337C9BD38   1858
jsound/DirectAudio.h                             8D3DD5671D72CFFC   3898
jsound/DirectAudioDevice.c                       81737DB80D68C26F  30608
jsound/DirectAudioDeviceProvider.c               F60EB68A476C57D5   5061
jsound/jdk8_compat.c                             5D366FB387DD02A5   1156
jsound/jni_util.h                                9203C9B711323630    628
jsound/jsound_openal.c                           18ACBF7E01B6A3D8  21015
jsound/MidiInDevice.c                            5A40AEA7F50B8076  10428
jsound/MidiInDeviceProvider.c                    A9DD5E731DA3F18B   4572
jsound/MidiOutDevice.c                           74C84E4C22EB2589   5395
jsound/MidiOutDeviceProvider.c                   5F35CCFB51F97289   4606
jsound/Platform.c                                BE2F934272654773   1679
jsound/PlatformMidi.c                            9E9BF014BF276148   6942
jsound/PlatformMidi.h                            A1393DEF91345E95  10197
jsound/PortMixer.c                               3BBAC4DF5CB259A2  15205
jsound/PortMixerProvider.c                       FC092FBBEFCED6E4   4412
jsound/Ports.h                                   3060F12FE99EB43D   5709
jsound/SoundDefs.h                               C4FF803D58EAAC2A   3458
jsound/Utilities.c                               C155F741E27EC57B   1801
jsound/Utilities.h                               3F94FFFA7AABBA59   3855
```
