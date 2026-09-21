# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

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

    # 事故夹具(三件套):目标已存在必须拒装(绝不覆盖)/ .part 到最终大小时续传起点只认
  # .part.json 记录的字节数(不是文件大小)/ size=0 无哈希的 loader 库残留不许算"已存在且校验通过"。
  # 全部在临时目录里跑(仓库不留产物),不联网、不碰任何真实游戏目录。
  add_executable(sxcl_install_target_test ${CMAKE_SOURCE_DIR}/tests/install_target_test.c)
  target_link_libraries(sxcl_install_target_test PRIVATE sxcl)
  sxcl_warnings(sxcl_install_target_test)
  add_test(NAME sxcl_install_target_test COMMAND sxcl_install_target_test)

  # R3 源级熔断夹具(引擎的源健康表):黑洞源只撞一次 / BMCLAPI 403 不算失败 / 判死的源排最后。
  # 内存假传输,不联网;夹具目录在临时目录里。
  add_executable(sxcl_source_health_test ${CMAKE_SOURCE_DIR}/tests/engine_source_health_test.c)
  target_link_libraries(sxcl_source_health_test PRIVATE sxcl)
  sxcl_warnings(sxcl_source_health_test)
  add_test(NAME sxcl_source_health_test COMMAND sxcl_source_health_test)
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

  # Java 检测(②:候选表 -> **真的执行** java -version -> 分类)。
  # 为什么注册在这里:被测代码在 src/services/launch/java.c,而 src/services/launch 还没有
  # 自己的 CMakeLists(与 java_runtime.c 放在本目录同一个理由),根 CMakeLists 不归本轮改。
  # 夹具是一个"冒充 java"的极小可执行文件(按自己的文件名决定打什么),所以单测既不联网、
  # 也不要求机器上装了 JDK —— 但**真的起了进程**,这正是这一轮要证明的事。
  add_executable(sxcl_fake_java ${CMAKE_SOURCE_DIR}/tests/java_finder_fixture.c)
  sxcl_warnings(sxcl_fake_java)
  add_executable(sxcl_java_finder_test ${CMAKE_SOURCE_DIR}/tests/java_finder_test.c)
  target_link_libraries(sxcl_java_finder_test PRIVATE sxcl)
  sxcl_warnings(sxcl_java_finder_test)
  add_test(NAME sxcl_java_finder_test
           COMMAND sxcl_java_finder_test "$<TARGET_FILE:sxcl_fake_java>")
  set_tests_properties(sxcl_java_finder_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
