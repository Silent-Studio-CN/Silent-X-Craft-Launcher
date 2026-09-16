plugins {
    id("com.android.application")
}

android {
    namespace = "com.silentstudio.sxcl"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.silentstudio.sxcl"
        minSdk = 26                 // 悬浮窗 + Java 8 语法都够用（安卓 8.0 起）
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // 按键核心是纯 Java，可以直接在电脑上跑单元测试：
    //   javac -d out app/src/main/java/com/silentstudio/sxcl/keymap/*.java \
    //         app/src/test/java/com/silentstudio/sxcl/keymap/KeymapCoreTest.java
    //   java -cp out com.silentstudio.sxcl.keymap.KeymapCoreTest app/src/main/assets/keymaps
    sourceSets["main"].assets.srcDir("src/main/assets")
}

dependencies {
    // 故意不引第三方库：悬浮层和教学界面用系统 API 就够，
    // 依赖越少，APK 越小，将来接注入器也不容易被版本冲突卡住。
}
