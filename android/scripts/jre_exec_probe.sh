#!/system/bin/sh
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# jre_exec_probe.sh - 真机 exec 诊断(诊断用,不随包发布,不进 APK)。
#
# 干什么:在**我们自己的私有目录**里,用真正的 JRE 跑一次 `bin/java -version`,把原始输出打出来。
# 结论(2026-09-21,见 docs/18-安卓起JVM的最小落地.md §4):
#   * run-as 会话(runas_app 域)-> 能 exec,JRE 真的跑起来;
#   * 应用进程自己的域(untrusted_app)-> SELinux 拒 execute_no_trans,execve 直接死;
#     所以 fork+exec 起 JVM 这条路在应用里是死的,只能进程内 dlopen(libjli.so)+JLI_Launch。
#   * 无论哪条路,`bin/java` 都要 LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir>(没有 RUNPATH,
#     而 libc++_shared.so 只在 APK 的 nativeLibraryDir 里)。
#
# 用法(设备上必须先有解开的 JRE 树,例如 /data/local/tmp/jre25;拷贝进私有目录由 run-as 做):
#   adb push <jre tree> /data/local/tmp/jre25
#   adb push android/scripts/jre_exec_probe.sh /data/local/tmp/
#   NATIVE=$(adb shell "dumpsys package com.silentstudio.sxcl | grep -m1 codePath" | sed 's/.*codePath=//')/lib/arm64
#   adb shell "run-as com.silentstudio.sxcl sh /data/local/tmp/jre_exec_probe.sh $NATIVE"
# 结果:<= 私有目录里的 java -version 原文 + app_data_file / apk_data_file 的 SELinux 标签对照。
#
# 纪律:只碰本应用私有目录(相对路径 files/...)与 /data/local/tmp 的只读输入,
# **不**读别的应用的目录,**不**动 /storage/emulated,不改任何系统设置。
JRE_REL="files/runtime/jre25"
NATIVE="$1"

if [ -z "$NATIVE" ]; then
  echo "用法: jre_exec_probe.sh <nativeLibraryDir(必须由 shell 侧 dumpsys 取,应用 uid 列不了 /data/app)>"
  exit 2
fi
echo "[uid] $(id)"
echo "[ctx] $(cat /proc/self/attr/current)"
echo "[nativeLibraryDir] $NATIVE"
ls -l "$NATIVE/libc++_shared.so" 2>&1
echo

echo "=== 0) 把 JRE 拷进私有目录(run-as = 应用 uid 自己拷)==="
mkdir -p files/runtime
rm -rf "$JRE_REL"
cp -r /data/local/tmp/jre25 "$JRE_REL" || exit 3
chmod -R 755 "$JRE_REL"
ls -l "$JRE_REL/bin/java" "$JRE_REL/lib/libjli.so"
echo

echo "=== 1) 阳性对照:/system/bin/toybox 拷进私有目录能不能跑 ==="
mkdir -p files/exec_probe
cp /system/bin/toybox files/exec_probe/toybox
chmod 755 files/exec_probe/toybox
./files/exec_probe/toybox echo EXEC_PRIVATE_OK
echo "toybox_exit=$?"
echo

echo "=== 2) exec 真 JRE:LD_LIBRARY_PATH=<jre>/lib:<nativeLibraryDir> ==="
cd "$JRE_REL" || exit 4
LD_LIBRARY_PATH="$PWD/lib:$NATIVE" ./bin/java -version
echo "java_exit=$?"
echo

echo "=== 3) 只给 <jre>/lib(少了 nativeLibraryDir 会怎样)==="
LD_LIBRARY_PATH="$PWD/lib" ./bin/java -version
echo "java_exit=$?"
echo

echo "=== 4) SELinux 标签:私有目录(app_data_file) vs APK 的 lib(apk_data_file)==="
ls -lZ bin/java lib/libjli.so
ls -lZ "$NATIVE/libc++_shared.so"
echo "=== done ==="
