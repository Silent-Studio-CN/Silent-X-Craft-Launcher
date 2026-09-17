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
  src/services/modloader/compat.c
  src/services/modloader/profiles.c
  src/services/modloader/installer.c
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
add_subdirectory(src/core/instance)
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
    # Qt 的 DLL 不在 PATH 时这个用例会以 STATUS_DLL_NOT_FOUND(0xC0000135) 失败 ——
    # 本地一直靠手工往 PATH 里塞 Qt bin 才过,CI 里换个环境就会红。测试自己带上 Qt 的 bin 目录。
    set_tests_properties(sxcl_net_qt_test PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Network>")
  endif()
endif()

if(SXCL_BUILD_UI)
  add_subdirectory(src/ui)
endif()