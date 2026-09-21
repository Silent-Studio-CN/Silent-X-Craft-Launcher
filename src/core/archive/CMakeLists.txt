# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# 本目录(src/core/archive)的源文件由这里追加到 sxcl 目标(根 CMakeLists 的约定:各模块自带
# CMakeLists 用 target_sources 追加,免得多个并行改动抢同一份根文件)。
#
#   xz.c   自己实现的 .xz(LZMA2)解码器 —— JRE 自托管包是 tar.xz,仓库原本只有 DEFLATE/ZIP
#   tar.c  ustar/pax tar 解包器(安全路径、pax 扩展头、硬/软链接、可执行位)
target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/xz.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tar.c
)

if(SXCL_BUILD_TESTS)
  enable_testing()

  # 夹具是**签入仓库的**真 .xz 文件(用 Python 的 lzma/tarfile 生成,见 fixtures 目录里的
  # generate.py):测试不联网、不依赖 Python,解出来的字节与 sha256 在用例里钉死。
  add_executable(sxcl_xz_test ${CMAKE_SOURCE_DIR}/tests/xz_test.c)
  target_link_libraries(sxcl_xz_test PRIVATE sxcl)
  sxcl_warnings(sxcl_xz_test)
  add_test(NAME sxcl_xz_test COMMAND sxcl_xz_test)
  set_tests_properties(sxcl_xz_test PROPERTIES
    ENVIRONMENT "SXCL_XZ_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/xz")

  add_executable(sxcl_tar_test ${CMAKE_SOURCE_DIR}/tests/tar_test.c)
  target_link_libraries(sxcl_tar_test PRIVATE sxcl)
  sxcl_warnings(sxcl_tar_test)
  add_test(NAME sxcl_tar_test COMMAND sxcl_tar_test)
  set_tests_properties(sxcl_tar_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ENVIRONMENT "SXCL_TAR_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/tar;SXCL_XZ_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/xz")
endif()
