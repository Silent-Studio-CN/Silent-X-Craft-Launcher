# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

# 模组中文名表（随包 + 云端可更新）。自己的 CMakeLists，不去抢根 CMakeLists
# （根那份的注释写着"多个并行改动不再抢同一份 CMakeLists"）。
target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modnames.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 是在 add_subdirectory 之后才 enable_testing() 的，所以本目录（在它之前
  # 被加进来）必须自己再开一次，否则 add_test 进不了 CTestTestfile.cmake（ctest 看不到）。
  enable_testing()

  # 中文名表：不联网。夹具是内嵌的表文本 + 假 transport（真文本喂给真解析/真校验），
  # 临时文件都写在构建目录下（仓库里不留夹具）。
  add_executable(sxcl_modnames_test tests/modnames_test.c)
  target_link_libraries(sxcl_modnames_test PRIVATE sxcl)
  sxcl_warnings(sxcl_modnames_test)
  add_test(NAME sxcl_modnames_test COMMAND sxcl_modnames_test)
  set_tests_properties(sxcl_modnames_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # 随包那张**真表**（存在才钉进去）：单测会把真的 assets/data/modnames.tsv 读一遍，
  # 校验表头 sha256/条数，再抽查几个真名字。表不在（有人按 NOTICE 的退路删了它）就跳过这一节，
  # 测试照样过 —— 这正是"缺表不许阻塞"那条口径在测试里的样子。
  if(EXISTS "${CMAKE_SOURCE_DIR}/assets/data/modnames.tsv")
    target_compile_definitions(sxcl_modnames_test PRIVATE
      SXCL_MODNAMES_BUNDLED_FIXTURE="${CMAKE_SOURCE_DIR}/assets/data/modnames.tsv")
  endif()
endif()
