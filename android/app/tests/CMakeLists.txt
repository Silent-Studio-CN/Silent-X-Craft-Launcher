# SXCL C - 安卓参数装配的离线夹具(独立小工程,不碰两棵树的 CMakeLists)
#
# 为什么单独一个工程:
#   * 它要在**桌面**上跑(设备不在也能验),所以不能进 android/ 的安卓构建;
#   * 它要读到核心库(sxcl)与我的安卓装配层(sxcl_android_game_args.c),把两者连起来断言;
#   * 两棵树的根 CMakeLists 是共享文件(并行改动多),不去动它 —— 这里用 add_subdirectory 引用。
#
# 用法:
#   cmake -S android/app/tests -B <build> -DSXCL_REPO_ROOT=<仓库根> -A x64
#   cmake --build <build> --config Release
#   ctest --test-dir <build> -C Release --output-on-failure
cmake_minimum_required(VERSION 3.24)

project(sxcl_android_args_fixture LANGUAGES C)

if(NOT SXCL_REPO_ROOT)
  message(FATAL_ERROR "需要 -DSXCL_REPO_ROOT=<仓库根>")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 只编核心库:测试/CLI/UI/Qt 传输全关(夹具自己就是测试)
set(SXCL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SXCL_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(SXCL_BUILD_UI OFF CACHE BOOL "" FORCE)
set(SXCL_BUILD_QT_TRANSPORT OFF CACHE BOOL "" FORCE)
set(SXCL_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory("${SXCL_REPO_ROOT}" "${CMAKE_BINARY_DIR}/sxcl_core")

enable_testing()
add_executable(sxcl_android_args_fixture
  sxcl_android_game_args_fixture.c
  ${SXCL_REPO_ROOT}/android/app/sxcl_android_game_args.c)
target_include_directories(sxcl_android_args_fixture PRIVATE
  ${SXCL_REPO_ROOT}/include
  ${SXCL_REPO_ROOT}/android/app)
target_link_libraries(sxcl_android_args_fixture PRIVATE sxcl)

# 源码含中文注释与中文字符串:/utf-8 必须显式给(否则 MSVC 按 936 代码页读 UTF-8 字节,
# 会出现"字符串字面量中的换行符"这种假错 —— 源文件本身的字节完全正确)。
if(MSVC)
  target_compile_options(sxcl_android_args_fixture PRIVATE /utf-8)
endif()
add_test(NAME sxcl_android_args_fixture COMMAND sxcl_android_args_fixture)
