# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

cmake_minimum_required(VERSION 3.24)
project(sxcl_c VERSION 0.1.0 LANGUAGES C CXX)

option(SXCL_BUILD_UI "构建 Qt 界面（需要 Qt6 与 libqf）" OFF)
option(SXCL_BUILD_TESTS "构建测试" ON)
option(SXCL_WERROR "警告视为错误" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 源码统一 UTF-8(含中文注释):MSVC 在非 936/1252 代码页下会报 C4819 且执行字符集漂移
if(MSVC)
  add_compile_options(/utf-8)
endif()

# ── Qt 运行时部署:把需要的 DLL 放到 exe 同目录 ──
# 为什么必须在构建里做(用户反馈"总是提示找不到 Qt*.dll"):靠 PATH 或手工拷贝都不稳,
# 换机器、换 shell、在别处双击都会失败。这里让构建产物自带运行时。
#
# **加新模块不该再改这个文件**:DLL 清单首选 CMake 3.21+ 的 $<TARGET_RUNTIME_DLLS:target> ——
# 它按目标的链接接口算出"运行时真正要 import 的 DLL",谁加了新的 Qt 模块(Widgets/Multimedia/
# MultimediaWidgets/Charts…),它的 DLL 都会自动跟着铺,不需要人维护清单。
# (实测踩过:老的轻量清单只有 Core/Gui/Network/Svg,漏了 **Widgets** —— 用 QWidget 的测试
#  目标一直靠"测试环境把 Qt 的 bin 塞进 PATH"才跑起来;现在它的 Qt6Widgets.dll 自己会拷。)
#
#   sxcl_deploy_qt(target)      —— 真实依赖的 DLL + 平台插件 + TLS 插件(轻,测试用)
#   sxcl_deploy_qt_full(target) —— 先 windeployqt(插件全家桶),再补离屏插件,最后跑摆放自检
#
# 摆放自检(tools/check_qt_deploy.ps1):构建后断言"该在的文件真的在 exe 同目录/子目录"。
# "DLL 在、插件不在"(tls/ 缺失 -> HTTPS 全废)这种最难查的情况会当场构建失败并指名道姓。
# 跳过:$env:SXCL_SKIP_DEPLOY_CHECK=1;整体关掉:-DSXCL_DEPLOY_CHECK=OFF(默认必须开)。
option(SXCL_DEPLOY_CHECK "构建后跑 Qt 部署摆放自检(缺文件直接构建失败)" ON)

# windeployqt 参数:部署与自检**必须**用同一套,否则自检会去要求"本来就不打算铺"的文件
# (翻译 / 软件 OpenGL / 系统 D3D 编译器 / 编译器运行时,我们都不带)。
# 存成空格分隔的字符串(人能直接 -D 改),用之前再拆成参数表。
set(SXCL_WINDEPLOYQT_FLAGS "--no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime"
    CACHE STRING "windeployqt 参数(部署与自检共用,空格分隔)")

function(sxcl_deploy_qt target)
  if(NOT WIN32)
    return()
  endif()

  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    # 首选:目标真正 import 的运行时 DLL —— 新加的 Qt 模块自动跟随,不用改这里。
    # 只在目标确实有链接依赖时才发这条命令:TARGET_RUNTIME_DLLS 展开为空会让
    # cmake -E copy_if_different 因参数不足而报错(那种目标本来也没有 Qt 运行时可铺)。
    get_target_property(_sxcl_link_libs ${target} LINK_LIBRARIES)
    if(_sxcl_link_libs)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Qt 运行时(按 $<TARGET_FILE_NAME:${target}> 的真实依赖自动判定)-> $<TARGET_FILE_DIR:${target}>")
    endif()
  else()
    # 兜底(CMake < 3.21:没有 TARGET_RUNTIME_DLLS):手写清单。
    # 这里是**兜底,不是首选** —— 谁加了新的 Qt 模块,这张表不会自动跟进,必须手工补。
    set(_dlls "")
    foreach(_qt_target Qt6::Core Qt6::Gui Qt6::Network Qt6::Svg)
      if(TARGET ${_qt_target})
        list(APPEND _dlls "$<TARGET_FILE:${_qt_target}>")
      endif()
    endforeach()
    if(_dlls)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Qt 运行时(兜底手写清单:CMake < 3.21 没有 TARGET_RUNTIME_DLLS)-> $<TARGET_FILE_DIR:${target}>")
    endif()
  endif()

  # 插件照旧铺开,一份都不能少(与上面的 DLL 是两回事,别合并):
  #   platforms/qwindows.dll   —— 起窗的最低要求,没有它连窗口都建不出来
  #   platforms/qoffscreen.dll —— **windeployqt 不铺**;离屏跑测试/截图要用
  #                               (踩过:只铺 DLL 不铺插件时,-platform offscreen 直接报
  #                                "Could not find the Qt platform plugin")
  #   tls/*.dll                —— HTTPS 的后端,缺了就是
  #                               "qt.network.ssl: No functional TLS backend was found"(下载全废)
  set(_qt_plugins "${Qt6_DIR}/../../../plugins")
  if(EXISTS "${_qt_plugins}/platforms")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_qt_plugins}/platforms/qwindows.dll" "${_qt_plugins}/platforms/qoffscreen.dll"
              "$<TARGET_FILE_DIR:${target}>/platforms/"
      COMMENT "Qt 平台插件(qwindows+qoffscreen)-> $<TARGET_FILE_DIR:${target}>/platforms")
  else()
    message(WARNING "找不到 Qt 平台插件目录:${_qt_plugins}/platforms —— 产物会缺 qwindows/qoffscreen")
  endif()
  file(GLOB _sxcl_tls_backends CONFIGURE_DEPENDS "${_qt_plugins}/tls/*.dll")
  if(_sxcl_tls_backends)
    # 插件文件名区分发布/调试(qschannelbackend.dll / qschannelbackendd.dll),按配置只拷对应的那份,
    # 别把 debug 插件混进 Release 目录(打包时要按 doc/07 剔除杂物)。
    set(_sxcl_tls_rel "")
    set(_sxcl_tls_dbg "")
    foreach(_tls IN LISTS _sxcl_tls_backends)
      get_filename_component(_tls_stem "${_tls}" NAME_WE)   # 去掉 .dll
      set(_tls_is_dbg FALSE)
      if(_tls_stem MATCHES "d$")
        # 调试版文件名 = 发布版名 + d(qschannelbackendd.dll)。**不能只判"以 d 结尾"** ——
        # 发布版自己就以 backend 的 d 结尾(qschannelbackend.dll),会被误判成调试版
        # (踩过:结果两种都拷,debug 插件混进 Release 目录)。所以要看"去掉尾字母 d 的那个
        # 文件是否也在":在 -> 本文件是调试版;不在 -> 本文件就是发布版。
        string(REGEX REPLACE "d$" "" _tls_base_stem "${_tls_stem}")
        get_filename_component(_tls_dir "${_tls}" DIRECTORY)
        if(EXISTS "${_tls_dir}/${_tls_base_stem}.dll")
          set(_tls_is_dbg TRUE)
        endif()
      endif()
      if(_tls_is_dbg)
        list(APPEND _sxcl_tls_dbg "${_tls}")
      else()
        list(APPEND _sxcl_tls_rel "${_tls}")
      endif()
    endforeach()
    if(NOT _sxcl_tls_dbg)
      set(_sxcl_tls_dbg "${_sxcl_tls_rel}")
    endif()
    if(NOT _sxcl_tls_rel)
      set(_sxcl_tls_rel "${_sxcl_tls_dbg}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/tls"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<IF:$<CONFIG:Debug>,${_sxcl_tls_dbg},${_sxcl_tls_rel}>"
              "$<TARGET_FILE_DIR:${target}>/tls/"
      COMMAND_EXPAND_LISTS
      COMMENT "Qt TLS 后端(qschannel/qopenssl…)-> $<TARGET_FILE_DIR:${target}>/tls(缺它 HTTPS 全废)")
  endif()
endfunction()

# 摆放自检:构建后跑 tools/check_qt_deploy.ps1,断言"该在的文件真的在 exe 同目录/子目录"。
# 缺任何一个 -> 脚本非零退出 -> 构建失败,并打印**缺的是哪个文件、应该在哪个目录**。
# 判定依据三路互补:windeployqt --list relative(权威清单)+ exe/同目录 Qt DLL 的 PE 导入表
# + 插件目录规则(所以 windeployqt 不可用时自检依然有效,不会退化成"什么都不查")。
# 想跳过:$env:SXCL_SKIP_DEPLOY_CHECK=1(默认必须跑,只有极特殊情况才用)。
function(sxcl_add_deploy_check target windeployqt_path)
  if(NOT WIN32 OR NOT SXCL_DEPLOY_CHECK)
    return()
  endif()
  find_program(SXCL_POWERSHELL_EXE NAMES pwsh powershell)
  if(NOT SXCL_POWERSHELL_EXE)
    message(WARNING "找不到 PowerShell,${target} 跳过 Qt 部署摆放自检(tools/check_qt_deploy.ps1)")
    return()
  endif()
  # 自检与部署必须用同一套 windeployqt 参数,否则会要求"本来就不打算铺"的文件
  string(REPLACE ";" " " _sxcl_wdq_str "${SXCL_WINDEPLOYQT_FLAGS}")
  set(_sxcl_wdq_args "")
  if(windeployqt_path)
    list(APPEND _sxcl_wdq_args -Windeployqt "${windeployqt_path}")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${SXCL_POWERSHELL_EXE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/check_qt_deploy.ps1"
            -Exe "$<TARGET_FILE:${target}>"
            -WindeployqtArgs "${_sxcl_wdq_str}"
            ${_sxcl_wdq_args}
    COMMENT "Qt 部署摆放自检:$<TARGET_FILE_NAME:${target}>(缺哪个文件就报哪个;SXCL_SKIP_DEPLOY_CHECK=1 跳过)"
    VERBATIM)
endfunction()

function(sxcl_deploy_qt_full target)
  if(NOT WIN32)
    return()
  endif()
  find_program(SXCL_WINDEPLOYQT NAMES windeployqt HINTS "${QT_BIN_DIR}" "${Qt6_DIR}/../../../bin")
  if(SXCL_WINDEPLOYQT)
    string(REPLACE ";" " " _sxcl_wdq_str "${SXCL_WINDEPLOYQT_FLAGS}")
    separate_arguments(_sxcl_wdq_list UNIX_COMMAND "${_sxcl_wdq_str}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${SXCL_WINDEPLOYQT}" ${_sxcl_wdq_list}
              --dir "$<TARGET_FILE_DIR:${target}>" "$<TARGET_FILE:${target}>"
      COMMENT "windeployqt -> $<TARGET_FILE_DIR:${target}>(含 platforms/tls 插件)")
  else()
    message(STATUS "没找到 windeployqt:${target} 退回轻量部署(真实依赖的 DLL + platforms/tls 插件)")
    sxcl_deploy_qt(${target})
  endif()
  # windeployqt 不铺离屏插件,但我们常要离屏起窗(截图/自动化):补上
  sxcl_deploy_qt(${target})
  # 最后一道:摆放自检。必须排在**所有**部署命令之后,否则会检到半成品。
  sxcl_add_deploy_check(${target} "${SXCL_WINDEPLOYQT}")
endfunction()

# 警告口径统一:库与测试用同一套,避免"库干净、测试脏"
function(sxcl_warnings target)
  if(SXCL_WERROR)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
endfunction()

# ---- libsxcl(核心静态库) ----
add_library(sxcl STATIC
  src/core/version.c
  src/core/json.c
  src/platform/platform_fopen.c
  src/platform/fs_tree.c
  src/core/inflate.c
  src/core/dl/hash.c
  src/core/dl/limiter.c
  src/core/dl/verify.c
  src/core/dl/engine.c
  src/core/dl/hashcache.c
  src/services/metadata/manifest.c
  src/services/archive/zip.c
  src/services/minecraft/options.c
  src/services/minecraft/natives.c
  src/core/settings.c
  src/services/launch/java.c
  src/services/launch/args.c
  src/services/launch/logscan.c
  src/services/launch/driver.c
  src/services/launch/isolation.c
  src/services/modloader/compat.c
  src/services/modloader/profiles.c
  src/services/modloader/installer.c
  src/services/mods/mods.c
)
target_include_directories(sxcl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 各模块自带的 CMakeLists(用 target_sources 追加到 sxcl)。
# 这样做是为了让多个并行改动**不再抢同一份 CMakeLists.txt**(已经因此出过两次中间态事故)。
#
# 注意:enable_testing() 必须在这批 add_subdirectory **之前**调用 ——
# 子目录里写的 add_test 只有在 enable_testing 已生效时才会被 ctest 看到,
# 否则测试**静默不注册**(cntest -N 少几个,还看不出来为什么)。子代理实测踩过。
if(SXCL_BUILD_TESTS)
  enable_testing()
endif()
add_subdirectory(src/services/minecraft)
add_subdirectory(src/services/modloader)
add_subdirectory(src/services/install)
add_subdirectory(src/core)          # 运行日志(include/sxcl/log.h + src/core/log.c);自己的 CMakeLists
add_subdirectory(src/core/instance)
add_subdirectory(src/core/auth)   # 正版登录(微软/Xbox Live);自己的 CMakeLists 追加源文件与测试
add_subdirectory(src/core/archive) # .xz(LZMA2)解码 + tar 解包:JRE 自托管包是 tar.xz;自己的 CMakeLists
add_subdirectory(src/core/diag)    # 崩溃取证与日志导出:编码探测/latest.log/crash-report/打包(zip store)
add_subdirectory(src/services/launch) # 启动层自己的测试(原因键夹具);源文件仍在下面的显式列表里
sxcl_warnings(sxcl)

# 平台实现二选一(WinHTTP/UTF-16 转换在 win32 侧,POSIX 侧用通用 API)
if(WIN32)
  target_sources(sxcl PRIVATE src/platform/platform_win32.c src/platform/process_win32.c)
elseif(ANDROID)
  # Android 的 pthread 就在 libc 里:FindThreads 的探测会失败,不要去找它
  target_sources(sxcl PRIVATE src/platform/platform_posix.c src/platform/process_posix.c)
else()
  target_sources(sxcl PRIVATE src/platform/platform_posix.c src/platform/process_posix.c)
  find_package(Threads REQUIRED)
  target_link_libraries(sxcl PUBLIC Threads::Threads)
endif()

# ---- 传输后端(可选,缺依赖不报错、只是不编译) ----
option(SXCL_BUILD_QT_TRANSPORT "构建 Qt Network 传输后端(四平台通用)" ON)
if(SXCL_BUILD_QT_TRANSPORT)
  set(CMAKE_AUTOMOC ON)
  find_package(Qt6 QUIET COMPONENTS Network)
  if(Qt6Network_FOUND)
    add_library(sxcl_net_qt STATIC src/net/transport_qt.cpp)
    target_link_libraries(sxcl_net_qt PUBLIC sxcl Qt6::Network)
    sxcl_warnings(sxcl_net_qt)
    message(STATUS "已启用 Qt Network 传输后端")
  else()
    message(STATUS "未找到 Qt6::Network,跳过 Qt 传输后端")
  endif()
endif()

# ---- 命令行前端(纯 C,无 Python) ----
option(SXCL_BUILD_CLI "构建 sxcl-dl 命令行下载器" ON)
if(SXCL_BUILD_CLI)
  add_executable(sxcl-dl tools/sxcl-dl/main.c)
  target_link_libraries(sxcl-dl PRIVATE sxcl)
  sxcl_warnings(sxcl-dl)
  if(TARGET sxcl_net_qt)
    target_link_libraries(sxcl-dl PRIVATE sxcl_net_qt)
    target_compile_definitions(sxcl-dl PRIVATE SXCL_HAVE_QT_TRANSPORT=1)
    # CLI 的 HTTPS 走 Qt Network —— **必须**把 Qt 运行时和插件(尤其 tls/)铺到同目录。
    # 踩过的坑:只靠"同目录恰好有别的测试目标拷来的 Qt6Core/Qt6Network.dll",
    # 在干净机器(或清空 PATH)上会直接报
    #   qt.network.ssl: No functional TLS backend was found
    # 于是下载全废。sxcl_deploy_qt_full 用 windeployqt,会带 platforms/tls 插件。
    sxcl_deploy_qt_full(sxcl-dl)
  endif()
  if(WIN32)
    # 崩溃处理器要解析符号:链 DbgHelp(WER 在这台机器上不记录我们的崩溃)
    target_link_libraries(sxcl-dl PRIVATE Dbghelp)
  endif()
endif()

# ---- 图标管线(开发工具,默认不编:只有重新抽图标时才需要) ----
option(SXCL_BUILD_ICON_TOOLS "构建 SVG 归一化工具" OFF)
if(SXCL_BUILD_ICON_TOOLS)
  find_package(Qt6 REQUIRED COMPONENTS Gui Svg)
  add_executable(sxcl-svgnorm tools/svg_normalize/main.cpp)
  target_link_libraries(sxcl-svgnorm PRIVATE Qt6::Gui Qt6::Svg)
endif()

# ---- 测试 ----
if(SXCL_BUILD_TESTS)
  enable_testing()
  set(SXCL_TESTS
    smoke        tests/smoke.c
    hash_test    tests/hash_test.c
    limiter_test tests/limiter_test.c
    verify_test  tests/verify_test.c
    json_test    tests/json_test.c
    manifest_test tests/manifest_test.c
    options_test tests/options_test.c
    process_test tests/process_test.c
    hashcache_test tests/hashcache_test.c
    settings_test tests/settings_test.c
    inflate_test tests/inflate_test.c
    zip_test     tests/zip_test.c
    launch_test  tests/launch_test.c
    launch_driver_test tests/launch_driver_test.c
    natives_test tests/natives_test.c
    loader_test  tests/loader_test.c
    processor_test tests/processor_test.c
    mods_test    tests/mods_test.c
  )
  list(LENGTH SXCL_TESTS _n)
  math(EXPR _last "${_n} - 1")
  foreach(_i RANGE 0 ${_last} 2)
    list(GET SXCL_TESTS ${_i} _name)
    math(EXPR _j "${_i} + 1")
    list(GET SXCL_TESTS ${_j} _src)
    add_executable(sxcl_${_name} ${_src})
    target_link_libraries(sxcl_${_name} PRIVATE sxcl)
    sxcl_warnings(sxcl_${_name})
    add_test(NAME sxcl_${_name} COMMAND sxcl_${_name})
  endforeach()

  # 传输后端测试只在后端存在时注册(Qt 是可选依赖)
  if(TARGET sxcl_net_qt)
    add_executable(sxcl_net_qt_test tests/net_qt_test.cpp)
    target_link_libraries(sxcl_net_qt_test PRIVATE sxcl_net_qt)
    sxcl_warnings(sxcl_net_qt_test)
    add_test(NAME sxcl_net_qt_test COMMAND sxcl_net_qt_test)
    sxcl_deploy_qt(sxcl_net_qt_test) # Qt6Core/Qt6Network 放到 exe 同目录(不再依赖 PATH)
    # Qt 的 DLL 不在 PATH 时这个用例会以 STATUS_DLL_NOT_FOUND(0xC0000135) 失败 ——
    # 本地一直靠手工往 PATH 里塞 Qt bin 才过,CI 里换个环境就会红。测试自己带上 Qt 的 bin 目录。
    set_tests_properties(sxcl_net_qt_test PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Network>")
  endif()
endif()

if(SXCL_BUILD_UI)
  # libqf(PyQf)那棵树 PYQFTO_MULTIMEDIA 默认 ON:会编出媒体模块,并带出 fluent_demo 目标,
  # 而 fluent_demo 链 Qt6::Multimedia + Qt6::MultimediaWidgets(背后一整套 FFmpeg 编解码栈:
  # 体积与再分发许可都得交代),部署时还得给 demo 目录单独铺 multimedia/ 后端插件。
  # 启动器一个都用不到 —— 在进它的 CMakeLists 之前就关掉(它自己的默认值不动)。
  # (CMP0077 下普通变量优先于 option(),所以 set 就够:不用改 PyQf 仓库,也不怕缓存里是 ON)
  set(PYQFTO_MULTIMEDIA OFF)
  set(PYQFTO_BUILD_DEMO OFF)
  set(PYQFTO_BUILD_TESTS OFF)   # 它自带的黄金文件测试不该混进我们的 ctest
  add_subdirectory(src/ui)
  # 兜底断言:哪天有人把 demo/多媒体放回来,这里当场配置失败 ——
  # 而不是等"干净机器上少了 Qt6Multimedia.dll / multimedia/ 插件"才发现。
  if(TARGET fluent_demo)
    message(FATAL_ERROR "fluent_demo 不该被构建(PYQFTO_BUILD_DEMO/PYQFTO_MULTIMEDIA 必须是 OFF):它会链 Qt6::Multimedia,把 ffmpeg 后端与 multimedia/ 插件拖进部署")
  endif()
  message(STATUS "libqf:PYQFTO_MULTIMEDIA=OFF / PYQFTO_BUILD_DEMO=OFF(不链 Qt6::Multimedia,不铺 multimedia/ 插件)")
endif()