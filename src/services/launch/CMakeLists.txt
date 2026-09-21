# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# 本目录(src/services/launch)的**源文件**仍在根 CMakeLists 的明确列表里(它们是被并行改动
# 最少的那几个),这里只挂本模块自己的测试 —— 理由与别的模块一样:并行改动别抢同一份文件。

if(SXCL_BUILD_TESTS)
  enable_testing()

  # 原因键(第 3b 节):文案完备性 + 逐行判定 + 合成 crash-report(与 stdout 同一套分析)。
  # 夹具是**文本 + 一棵假游戏目录**,建在构建目录下,不联网、不留产物。
  add_executable(sxcl_crash_reason_test ${CMAKE_SOURCE_DIR}/tests/crash_reason_test.c)
  target_link_libraries(sxcl_crash_reason_test PRIVATE sxcl)
  sxcl_warnings(sxcl_crash_reason_test)
  add_test(NAME sxcl_crash_reason_test COMMAND sxcl_crash_reason_test)
  set_tests_properties(sxcl_crash_reason_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
