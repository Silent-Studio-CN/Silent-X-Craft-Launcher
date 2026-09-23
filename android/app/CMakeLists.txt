# SXCL C - Android packaging layer (SilentStudio JQt pipeline).
#
# This project lives OUTSIDE the shared source tree. It only decides WHICH
# translation units go into the Android shared library and how they link;
# every file under src/** is used verbatim (no Android forks of UI code).
#
# Layout expected next to this file (assembled by the staging script):
#   sxcl/    copy of the SXCL C source tree (root CMakeLists + src/)
#   pytoc/   copy of the libqf source tree ("PyQf to C")
#   include/ copy of the SXCL C include tree (repo root CMakeLists uses
#            ${CMAKE_SOURCE_DIR}/include; CMAKE_SOURCE_DIR is this directory)
#
cmake_minimum_required(VERSION 3.24)

project(sxcl_android VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT ANDROID)
  message(FATAL_ERROR "Android-only project: configure with the Qt android toolchain file.")
endif()

set(SXCL_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/sxcl"  CACHE PATH "SXCL C source tree copy")
set(SXCL_PYQFTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pytoc" CACHE PATH "libqf source tree copy")

# Options the two sub-projects read. Plain variables win over option() (CMP0077).
set(SXCL_BUILD_UI OFF)      # src/ui is added explicitly below (same sources, our entry)
set(SXCL_BUILD_TESTS OFF)
set(SXCL_BUILD_CLI OFF)
set(SXCL_WERROR OFF)        # keep -Wall/-Wextra, drop -Werror on the Android toolchain
set(PYQFTO_BUILD_DEMO OFF)
set(PYQFTO_MULTIMEDIA OFF)
set(PYQFTO_BUILD_TESTS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# ---- core library (pure C) -------------------------------------------------
add_subdirectory(${SXCL_SRC_DIR} ${CMAKE_BINARY_DIR}/sxcl)

# The desktop build gets AUTOMOC from the root CMakeLists (SXCL_BUILD_QT_TRANSPORT
# sets CMAKE_AUTOMOC ON before src/ui is added). We add src/ui from THIS scope, so
# we must turn it on here or tasks_page.cpp's "#include \"tasks_page.moc\"" fails.
set(CMAKE_AUTOMOC ON)

# ---- UI layer: the very same CMakeLists the desktop build uses -------------
add_subdirectory(${SXCL_SRC_DIR}/src/ui ${CMAKE_BINARY_DIR}/sxcl_ui)
# the desktop executable target is not part of the APK; it is not built
set_target_properties(sxcl-ui PROPERTIES EXCLUDE_FROM_ALL TRUE)
# ...and force AUTOMOC on the library itself (src/ui/CMakeLists.txt relies on the
# variable being set by the root CMakeLists, which only happens in its own scope).
set_target_properties(sxcl_ui_core PROPERTIES AUTOMOC ON)

# ---- 共享的"进程内起 JVM"自举层 -------------------------------------------
# 探针(sxcl_jre_probe.c)与 :game 游戏进程(sxcl_game.c)**共用同一份实现**:
# JLI_Launch 的调用约定 / LD_LIBRARY_PATH 的补法 / 崩溃兜底只要有一份走偏,
# 另一份就会以"JVM 起不来"的形式表现出来,而且极难查。
set(SXCL_JRE_BOOTSTRAP_C "${CMAKE_CURRENT_SOURCE_DIR}/sxcl_jre_bootstrap.c")

# ---- Android shared library ------------------------------------------------
# sxcl_android_main.cpp #includes src/ui/main.cpp under a renamed symbol, so the
# desktop entry point is reused unchanged instead of being copied.
# sxcl_jre_probe.c: 应用域内的"进程内起 JVM"自举探针(dlopen(libjli.so) + JLI_Launch)。
# 只在 boot 文件里有 jreprobe= 时跑,正常启动不受影响;见 docs/18-安卓起JVM的最小落地.md §4。
qt_add_executable(sxclui sxcl_android_main.cpp sxcl_jre_probe.c ${SXCL_JRE_BOOTSTRAP_C})
target_include_directories(sxclui PRIVATE ${SXCL_SRC_DIR}/src/ui)
# log: 探针用 __android_log_print 留痕(Qt 通常已经带上,这里显式写上)
target_link_libraries(sxclui PRIVATE sxcl_ui_core log)
set_target_properties(sxclui PROPERTIES OUTPUT_NAME "sxclui_${ANDROID_ABI}")

# ---- 游戏进程(:game)的原生库 ----------------------------------------------
# GameActivity(System.loadLibrary("sxclgame"))用到的那一份。
# 它**不链接 Qt、也不链接 sxcl_ui_core**:游戏进程只做四件事 —— 起本进程的 JVM、
# 走本地 socket 与主进程通信、把 stdout/stderr 接成日志、崩溃/结束时留退出记录。
# 详见 android/app/sxcl_game.c 与 docs/21-安卓游戏独立进程与画中画共存.md。
# sxcl_android_game_args.c:安卓上的"起游戏主类"参数装配(版本 JSON -> 游戏命令行)。
# 规则全在核心库(instance/natives/launch),它只补安卓布局与 -D;桌面离线夹具也编这一份
# (android/app/tests/,见 docs/21 §10)。
add_library(sxclgame SHARED sxcl_game.c ${SXCL_JRE_BOOTSTRAP_C}
  ${CMAKE_CURRENT_SOURCE_DIR}/sxcl_android_game_args.c)
target_include_directories(sxclgame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SXCL_SRC_DIR}/include)
# sxcl = 核心库静态库:环境变量与 JVM 参数的拼法只有它那一份(include/sxcl/jvm.h),
#        安卓这层只做"信号兜底 + 日志出口 + 收尾"(见 sxcl_jre_bootstrap.h 的分工说明)。
# log     = __android_log_print
# android = ANativeWindow_fromSurface / ANativeWindow_release(渲染器桥:Surface -> ANativeWindow);
#           少了它链接会报 "undefined symbol: ANativeWindow_fromSurface"(实测踩过一次)
# dl      = dlopen/dlsym(NDK 上 dl 并在 libc 里,CMAKE_DL_LIBS 求出来就是 dl)
target_link_libraries(sxclgame PRIVATE sxcl log android ${CMAKE_DL_LIBS})
set_property(TARGET sxclgame PROPERTY C_STANDARD 11)
set_target_properties(sxclgame PROPERTIES OUTPUT_NAME "sxclgame")

message(STATUS "sxclui -> libsxclui_${ANDROID_ABI}.so (Qt ${Qt6Widgets_VERSION})")
message(STATUS "sxclgame -> libsxclgame.so (:game 进程的原生层,无 Qt 依赖)")
