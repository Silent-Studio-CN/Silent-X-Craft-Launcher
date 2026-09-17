# 本目录的源文件由这里追加到 sxcl 目标(见根 CMakeLists 的说明)。
# 子代理请只改这个文件,不要动根 CMakeLists。

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/install.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 现在把 enable_testing() 提到了 add_subdirectory 之前(必要的),
  # 这里再调一次是幂等的保险:万一顺序被人改回去,子目录里的 add_test 会被**静默丢掉**
  # (实测过一次:ctest 里根本看不到 sxcl_install_test,不报任何错)。
  enable_testing()

  add_executable(sxcl_install_test ${CMAKE_SOURCE_DIR}/tests/install_test.c)
  target_link_libraries(sxcl_install_test PRIVATE sxcl)
  sxcl_warnings(sxcl_install_test)
  add_test(NAME sxcl_install_test COMMAND sxcl_install_test)
endif()
