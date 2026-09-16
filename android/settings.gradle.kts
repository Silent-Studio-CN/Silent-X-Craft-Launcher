// 安卓端只做一件事：把桌面端编好的按键布局铺到游戏上，并提供更好的按键帮助。
// 布局数据（assets/keymaps/*.json）由桌面端 scripts/export_keymap_assets.py 生成，
// 两边共用 sxcl.keymap.v1 这套 schema，不存在"两套预设要同步"的问题。

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "SXCL-Keymap"
include(":app")
