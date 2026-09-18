# 本目录的源文件由这里追加到 sxcl 目标(见根 CMakeLists 的说明)。
# 子代理请只改这个文件,不要动根 CMakeLists。

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/install.c
  # 官方 Java 运行时(JRE)安装:逻辑上属于 src/services/java/**,为了不依赖"往根 CMakeLists
  # 加一句 add_subdirectory"先放这里(与 modloader 目录里放 keymap* 同一个理由);
  # 等 src/services/java/CMakeLists.txt 建好,把这一行与 java_runtime.c 一起搬过去即可。
  ${CMAKE_CURRENT_SOURCE_DIR}/java_runtime.c
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

  # 版本 JSON 落盘(下载路径与启动路径必须写在同一个地方;见 install.h 的说明)
  add_executable(sxcl_version_json_test tests/version_json_test.c)
  target_link_libraries(sxcl_version_json_test PRIVATE sxcl)
  sxcl_warnings(sxcl_version_json_test)
  add_test(NAME sxcl_version_json_test COMMAND sxcl_version_json_test)
  set_tests_properties(sxcl_version_json_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 官方 JRE 安装:**不联网**(清单 JSON 当夹具 + 内存假传输),覆盖正常安装 / 校验失败 /
  # 取消 / 已存在跳过。夹具写在构建目录下,仓库里不留产物。
  add_executable(sxcl_java_runtime_test ${CMAKE_SOURCE_DIR}/tests/java_runtime_test.c)
  target_link_libraries(sxcl_java_runtime_test PRIVATE sxcl)
  sxcl_warnings(sxcl_java_runtime_test)
  add_test(NAME sxcl_java_runtime_test COMMAND sxcl_java_runtime_test)
  set_tests_properties(sxcl_java_runtime_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
