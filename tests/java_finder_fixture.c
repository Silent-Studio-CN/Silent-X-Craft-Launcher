/* 夹具:冒充 java 的一个极小可执行文件(②"必须真的执行 java -version"的测试用)。
 *
 * 为什么不直接用机器上装的 JDK:单测不能依赖"这台机器恰好装了 Temurin 21" —— 那测的是
 * 环境,不是代码。这里按**自己的文件名**决定行为,一份二进制覆盖多种情形:
 *   * 路径里有 "notjre" -> 打印一段不是 Java 的东西(退出码 0)—— 模拟"能跑但不是 JRE";
 *   * 路径里有 "silent" -> 什么都不打印、退出码 127 —— 模拟"进程起不来";
 *   * 其它              -> 打印 Temurin 21 的 java -version 横幅(**打到 stderr**,与真 java 一致)。
 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *self = (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "";
    (void)argc;
    if (strstr(self, "notjre") != NULL) {
        (void)printf("hello, I am definitely not a java runtime\n");
        return 0;
    }
    if (strstr(self, "silent") != NULL) {
        return 127;
    }
    (void)fprintf(stderr, "openjdk version \"21.0.3\" 2024-04-16\n");
    (void)fprintf(stderr,
                  "OpenJDK Runtime Environment Temurin-21.0.3+9 (build 21.0.3+9-LTS)\n");
    (void)fprintf(stderr,
                  "OpenJDK 64-Bit Server VM Temurin-21.0.3+9 (build 21.0.3+9-LTS, mixed mode)\n");
    return 0;
}
