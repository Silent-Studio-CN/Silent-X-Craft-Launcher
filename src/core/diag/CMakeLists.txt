# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# 本目录(src/core/diag)的源文件由这里追加到 sxcl 目标(根 CMakeLists 的约定:各模块自带
# CMakeLists 用 target_sources 追加,免得多个并行改动抢同一份根文件)。
#
#   text.c    文本编码探测与转换(UTF-8 / UTF-16 / GBK)——游戏日志不保证是 UTF-8
#   dir.c     目录列举与路径拼接(本模块内部共用)
#   crash.c   崩溃取证:最近的非空 crash-report + logs/latest.log -> 逐行喂给调用方
#   export.c  一键导出日志包(zip store 打包 + token/uuid/用户名打码)
target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/text.c
  ${CMAKE_CURRENT_SOURCE_DIR}/dir.c
  ${CMAKE_CURRENT_SOURCE_DIR}/crash.c
  ${CMAKE_CURRENT_SOURCE_DIR}/export.c
)

if(SXCL_BUILD_TESTS)
  enable_testing()

  # 编码探测/转换:夹具是**构造出来的字节**(UTF-8 BOM / UTF-16LE/BE / GBK / 坏字节),
  # 期望值钉死在用例里 —— 不依赖本机代码页,Windows/Linux/macOS 结果一致。
  add_executable(sxcl_text_test ${CMAKE_SOURCE_DIR}/tests/text_test.c)
  target_link_libraries(sxcl_text_test PRIVATE sxcl)
  sxcl_warnings(sxcl_text_test)
  add_test(NAME sxcl_text_test COMMAND sxcl_text_test)
  set_tests_properties(sxcl_text_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 崩溃取证:夹具是构建目录下的一棵**假游戏目录**(空报告/非空报告/latest.log),
  # 仓库里不留产物,不联网。
  add_executable(sxcl_crash_test ${CMAKE_SOURCE_DIR}/tests/crash_test.c)
  target_link_libraries(sxcl_crash_test PRIVATE sxcl)
  sxcl_warnings(sxcl_crash_test)
  add_test(NAME sxcl_crash_test COMMAND sxcl_crash_test)
  set_tests_properties(sxcl_crash_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 一键导出:夹具同上;打完包用**我们自己的 zip 读侧**回读校验(条目/内容/打码)。
  add_executable(sxcl_logexport_test ${CMAKE_SOURCE_DIR}/tests/logexport_test.c)
  target_link_libraries(sxcl_logexport_test PRIVATE sxcl)
  sxcl_warnings(sxcl_logexport_test)
  add_test(NAME sxcl_logexport_test COMMAND sxcl_logexport_test)
  set_tests_properties(sxcl_logexport_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
