/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/version.h"

int main(void)
{
    const char *v = sxcl_version_string();
    if (v == NULL || strlen(v) == 0) {
        fprintf(stderr, "version string empty\n");
        return 1;
    }
    if ((sxcl_version_features() & SXCL_FEATURE_DOWNLOAD) == 0u) {
        fprintf(stderr, "download feature missing\n");
        return 1;
    }
    printf("sxcl-c %s ok\n", v);
    return 0;
}
