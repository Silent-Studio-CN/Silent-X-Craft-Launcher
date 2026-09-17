# 本目录的源文件由这里追加到 sxcl 目标(见根 CMakeLists 的说明)。
# 子代理请只改这个文件,不要动根 CMakeLists。

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/install.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 的 enable_testing() 在 add_subdirectory 之后才调用,所以子目录里
  # **必须自己再调一次**:否则本目录的 CTestTestfile.cmake 根本不会生成,add_test 静默丢失
  # (实测:不调这一行,ctest 里看不到 sxcl_install_test)。
  enable_testing()

  add_executable(sxcl_install_test ${CMAKE_SOURCE_DIR}/tests/install_test.c)
  target_link_libraries(sxcl_install_test PRIVATE sxcl)
  sxcl_warnings(sxcl_install_test)
  add_test(NAME sxcl_install_test COMMAND sxcl_install_test)
endif()
