# 本目录的源文件由这里追加到 sxcl 目标(见根 CMakeLists 的说明)。
# 子代理请只改这个文件,不要动根 CMakeLists。

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/io_internal.c
  ${CMAKE_CURRENT_SOURCE_DIR}/paths.c
  ${CMAKE_CURRENT_SOURCE_DIR}/http.c
  ${CMAKE_CURRENT_SOURCE_DIR}/instance.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sysinfo.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 是在 add_subdirectory 之后才 enable_testing() 的,所以本目录(在它之前
  # 被加进来)必须自己再开一次,否则 add_test 进不了 CTestTestfile.cmake(实测:ctest 看不到)。
  enable_testing()
  # 夹具写在构建目录下(WORKING_DIRECTORY 就是本目录的构建目录,已被 .gitignore 忽略:
  # 仓库里不留任何二进制/临时夹具)。
  add_executable(sxcl_instance_test tests/instance_test.c)
  target_include_directories(sxcl_instance_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sxcl_instance_test PRIVATE sxcl)
  sxcl_warnings(sxcl_instance_test)
  add_test(NAME sxcl_instance_test COMMAND sxcl_instance_test)
  set_tests_properties(sxcl_instance_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
