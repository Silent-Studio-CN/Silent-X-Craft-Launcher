/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本隔离的目录计算与建目录（声明与理由见 sxcl/isolation.h）。

#include "sxcl/isolation.h"

#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"

/* 游戏自己要写的那些目录。清单照 PCL / HMCL 的"版本隔离"一套来：
 * 模组、存档、配置、资源包、光影、日志、崩溃报告、截图、服务器资源包。 */
static const char *const kSubdirs[] = {
    "mods",          "saves",       "config",  "resourcepacks", "shaderpacks",
    "logs",          "crash-reports", "screenshots", "server-resource-packs",
};

static int join_child(char *out, size_t cap, const char *parent, const char *child)
{
    const size_t plen = parent ? strlen(parent) : 0;
    const int need_sep = plen > 0 && parent[plen - 1] != '/' && parent[plen - 1] != '\\';
    const int wrote = snprintf(out, cap, "%s%s%s", parent ? parent : "", need_sep ? "/" : "",
                               child ? child : "");
    return (wrote <= 0 || (size_t)wrote >= cap) ? -1 : 0;
}

size_t sxcl_launch_isolated_subdirs(const char **out, size_t cap)
{
    const size_t total = sizeof(kSubdirs) / sizeof(kSubdirs[0]);
    if (out) {
        for (size_t i = 0; i < total && i < cap; ++i) {
            out[i] = kSubdirs[i];
        }
    }
    return total;
}

int sxcl_launch_isolated_dir(const char *game_dir, const char *version_name, char *out,
                             size_t out_len)
{
    if (!game_dir || !*game_dir || !version_name || !*version_name || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    char versions[4096];
    if (join_child(versions, sizeof(versions), game_dir, "versions") != 0) {
        return -1;
    }
    if (join_child(out, out_len, versions, version_name) != 0) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int sxcl_launch_prepare_isolated(const char *game_dir, const char *version_name, char *err,
                                 size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    char dir[4096];
    if (sxcl_launch_isolated_dir(game_dir, version_name, dir, sizeof(dir)) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "隔离目录拼不出来（游戏目录或版本名不合法）");
        }
        return -1;
    }
    if (sxcl_fs_mkdirs(dir) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "建不了隔离目录：%s", dir);
        }
        return -1;
    }
    for (size_t i = 0; i < sizeof(kSubdirs) / sizeof(kSubdirs[0]); ++i) {
        char sub[4200];
        if (join_child(sub, sizeof(sub), dir, kSubdirs[i]) != 0) {
            if (err && err_len) {
                (void)snprintf(err, err_len, "隔离子目录路径太长：%s", kSubdirs[i]);
            }
            return -1;
        }
        if (sxcl_fs_mkdirs(sub) != 0) {
            if (err && err_len) {
                (void)snprintf(err, err_len, "建不了隔离子目录：%s", sub);
            }
            return -1;
        }
    }
    return 0;
}
