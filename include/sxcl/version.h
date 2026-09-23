/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_VERSION_H
#define SXCL_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_VERSION_MAJOR 0
#define SXCL_VERSION_MINOR 2
#define SXCL_VERSION_PATCH 0

/** 返回形如 "0.2.0" 的静态字符串，调用方不得释放。 */
const char *sxcl_version_string(void);

/** 返回编译期特性位掩码，供 UI 判断可选能力是否编译进来。 */
unsigned sxcl_version_features(void);

#define SXCL_FEATURE_DOWNLOAD 0x1u
#define SXCL_FEATURE_UI       0x2u
#define SXCL_FEATURE_NET      0x4u

#ifdef __cplusplus
}
#endif
#endif /* SXCL_VERSION_H */
