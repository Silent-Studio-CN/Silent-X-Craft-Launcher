# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# 本目录(src/core)的源文件由这里追加到 sxcl 目标(根 CMakeLists 的约定:各模块自带
# CMakeLists 用 target_sources 追加,免得多个并行改动抢同一份根文件)。
target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/log.c
)

# 安卓:log.c 的 logcat 输出要链 liblog(NDK 自带,不是第三方依赖)
if(ANDROID)
  target_link_libraries(sxcl PUBLIC log)
endif()

if(SXCL_BUILD_TESTS)
  enable_testing()
  # 运行日志模块的夹具:级别过滤 / 行格式 / 轮转与保留份数 / 多线程 / 静默降级 / URL 打码
  add_executable(sxcl_log_test ${CMAKE_SOURCE_DIR}/tests/log_test.c)
  target_link_libraries(sxcl_log_test PRIVATE sxcl)
  sxcl_warnings(sxcl_log_test)
  add_test(NAME sxcl_log_test COMMAND sxcl_log_test)

  # 游戏输出落盘通道(攒批 / 打码 / 开关 / 截断 / shutdown 冲刷):夹具写在构建目录下
  add_executable(sxcl_log_game_test ${CMAKE_SOURCE_DIR}/tests/log_game_test.c)
  target_link_libraries(sxcl_log_game_test PRIVATE sxcl)
  sxcl_warnings(sxcl_log_game_test)
  add_test(NAME sxcl_log_game_test COMMAND sxcl_log_game_test)
  set_tests_properties(sxcl_log_game_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
