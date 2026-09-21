/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
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
