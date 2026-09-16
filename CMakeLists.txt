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
  src/core/dl/hash.c
  src/core/dl/limiter.c
  src/core/dl/verify.c
  src/core/dl/engine.c
)
target_include_directories(sxcl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
sxcl_warnings(sxcl)

# 平台实现二选一(WinHTTP/UTF-16 转换在 win32 侧,POSIX 侧用通用 API)
if(WIN32)
  target_sources(sxcl PRIVATE src/platform/platform_win32.c)
else()
  target_sources(sxcl PRIVATE src/platform/platform_posix.c)
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
endif()

# ---- 测试 ----
if(SXCL_BUILD_TESTS)
  enable_testing()
  set(SXCL_TESTS
    smoke        tests/smoke.c
    hash_test    tests/hash_test.c
    limiter_test tests/limiter_test.c
    verify_test  tests/verify_test.c
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
  endif()
endif()

if(SXCL_BUILD_UI)
  add_subdirectory(src/ui)
endif()
