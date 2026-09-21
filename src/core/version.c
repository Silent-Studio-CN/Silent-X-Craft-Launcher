/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/version.h"

const char *sxcl_version_string(void)
{
    return "0.1.0";
}

unsigned sxcl_version_features(void)
{
    return SXCL_FEATURE_DOWNLOAD;
}
