# SXCL 安卓端 —— 按键助手（比 FCL 更好的按键帮助）

这个模块解决一件事：**手机上玩 Java 版 Minecraft 时，按键太难受**。

FCL 提供了可拖拽的虚拟按键，但它只给你一堆按钮：键抢了不会说、不知道先按哪个、
换个手机分辨率全乱。SXCL 安卓端在这些地方补上：

| 能力 | FCL | SXCL 安卓端 |
|:--|:--|:--|
| 布局数据 | 各自私有格式 | `sxcl.keymap.v1`，和桌面启动器**同一份 JSON** |
| 坐标 | 像素（换机要重排） | 0~1 归一化（换机/横竖屏都不动） |
| 冲突检测 | 无 | 抢键 / 重叠 / 缺关键动作 三类都报 |
| 教学 | 无 | `meta.guide` 分步教学 + 每个键的 `hint` |
| 找键 | 肉眼找 | 搜"潜行 / jump / KEY_SPACE"直接高亮 |
| 跨端编辑 | 只能手机改 | 桌面端 `按键映射` 页编好再推过去 |
| FCL 迁移 | — | 导入 FCL 导出布局（像素→归一化），也能导出回 FCL |

## 目录

```
android/
├── app/src/main/java/com/silentstudio/sxcl/
│   ├── keymap/      # 纯 Java：JSON 解析 / 布局模型 / 校验 / 冲突 / 教学读取
│   ├── overlay/     # 悬浮按键层 + InputSink（按键注入接口）+ 前台服务
│   ├── data/        # assets 布局读取；用户手改过的布局存 filesDir（优先级更高）
│   └── ui/          # MainActivity（选/预览/开关悬浮层）、GuideActivity（教学+搜索）
├── app/src/main/assets/keymaps/   # 由桌面端导出，不要手改
├── app/src/test/java/...          # 纯 Java 单测，电脑上就能跑
└── app/build.gradle.kts           # Android Studio 路线的工程文件
```

## 编译

### 路线 A：不用 Gradle（本机已验证）

```bash
python ../scripts/export_keymap_assets.py     # 1. 从桌面端预设导出布局+教学
python ../scripts/build_android_apk.py        # 2. aapt2 -> javac -> d8 -> zipalign -> apksigner
python ../scripts/build_android_apk.py --install   # 3. 打完直接 adb install -r
```

需要：Android SDK（`ANDROID_HOME`）+ build-tools（aapt2/d8/zipalign/apksigner）+ JDK 17。
产物：`build/android/SXCL-Keymap-<版本>.apk`（debug 签名，可直装）。

### 路线 B：Android Studio / Gradle

```bash
cd android && ./gradlew :app:assembleDebug
```

两条路的包名都是 `com.silentstudio.sxcl`，产物等价（Gradle 路线额外方便调试与打 release）。

### 纯 Java 核心测试（不需要手机、不需要模拟器）

```bash
javac -encoding UTF-8 -d out app/src/main/java/com/silentstudio/sxcl/keymap/*.java \
      app/src/test/java/com/silentstudio/sxcl/keymap/KeymapCoreTest.java
java -cp out com.silentstudio.sxcl.keymap.KeymapCoreTest app/src/main/assets/keymaps
```

覆盖：JSON 解析、清单/布局一致性、搜索、教学步骤、序列化往返、冲突检测、摇杆方向与死区、
FCL 导出坐标 —— 共 40 项断言。

## 使用

1. 装 APK → 打开「SXCL 按键」→ 选一套布局（横屏/竖屏各有预设，"单手"固定竖屏）。
2. 预览区直接显示每个键的教学提示；点「冲突检查」看有没有抢键/重叠。
3. 需要微调就勾「编辑位置」，拖动按键，点「保存」（存在手机私有目录，悬浮层优先读它）。
4. 点「开始悬浮按键」，首次会跳系统设置要「显示在其他应用上层」权限，回来再点一次。
5. 进游戏，按键就铺在画面上了；通知栏的「停止」可以随时关掉。

## 接游戏（按键注入）

现在默认注入器是 `InputSink.LogSink`（只打日志），所以界面和教学可以脱离游戏先跑通。
接真实注入时实现 `InputSink`：

```java
public interface InputSink {
    boolean isConnected();          // 游戏进程是否就绪
    void keyDown(String key);       // "KEY_SPACE" / "MOUSE_LEFT" ...
    void keyUp(String key);
    default void keyTap(String key) { keyDown(key); keyUp(key); }
}
```

再把它塞给 `KeymapOverlayService`（`setInputSink`）即可 —— 覆盖层完全不知道底层是 JNI、
scrcpy 还是别的方案。

## 数据契约（`sxcl.keymap.v1`）

```json
{
  "schema": "sxcl.keymap.v1",
  "name": "生存", "screen": "landscape",
  "meta": { "guide": [ { "order": 1, "title": "会走路", "instruction": "...", "control_id": "move" } ] },
  "directions": [ { "id": "move", "style": "rocker", "keys": {"up": "KEY_W"}, "dead_zone": 0.18 } ],
  "buttons": [ { "id": "jump", "label": "跳", "x": 0.86, "y": 0.62, "w": 0.09, "h": 0.16,
                 "events": { "press": { "action": "jump", "keys": ["KEY_SPACE"], "behavior": "hold" } } } ]
}
```

* 四种事件：`press / long_press / click / double_click`（与 FCL 的四种一致）
* 三种行为：`hold`（按住保持）/ `toggle`（点一下切开关）/ `tap`（发一次按下+抬起）
* `hint` 是教学提示；`alias_of` 表示"这个按钮和某动作等价"（比如盾=右键），冲突检测不会误报

改文案请改桌面端 `src/core/keymap/guide.py` / `presets.py`，然后重新导出 —— 不要手改 assets。
