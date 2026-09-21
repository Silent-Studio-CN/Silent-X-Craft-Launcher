#pragma once
// ui_paths —— 界面层共用的"运行期取值"解析(工作线程层;**不碰任何控件**)
//
// 为什么单独一份:下载配置页、下载进度页、启动页、安装/启动 worker 都要问同样几个问题 ——
// 游戏目录在哪、我们自己的设置文件在哪、内存给多少、要不要打进度追踪。
// 以前只有主页有这一份(home_page.cpp:129-192),几处各写一遍必然漂移。
//
// 取值口径与主页、CLI 完全一致:**环境变量 > sxcl_settings > 核心库平台默认**。
//   SXCL_UI_GAME_DIR   验收/取证通路:钉死游戏目录(不动用户配置)
//   SXCL_UI_SETTINGS   验收/取证通路:钉死设置文件
//   SXCL_UI_TRACE=1    把核心库回调里的真实数字打到 stderr(验收要的"进度回调真实日志")
#include <QString>

namespace sxcl::ui {

// 我们自己的设置文件(与 settings_page.cpp / home_page.cpp 同口径);
// SXCL_UI_SETTINGS 可覆盖(取证时用临时文件,不污染用户配置)。
QString uiSettingsFilePath();

// 启动器自己的**数据根**(配置/缓存都挂在它下面),**保证非空**。
// 为什么单独一个函数:版本页以前把清单缓存路径直接拼 %APPDATA%,APPDATA 一空
// (Android 上必然为空)缓存路径就整个消失 —— 远端不通时连本地缓存兜底都没有,
// 用户看到的就是"永远拿不到版本列表"。平台口径:
//   SXCL_UI_DATA_DIR 覆盖(取证) > Windows: %APPDATA%/SilentXCraftLauncher(与 Python 版共用)
//   > Android: App 私有数据目录 > 其它: ~/.config/SilentXCraftLauncher
QString uiLauncherDataRoot();

// 游戏目录:SXCL_UI_GAME_DIR > sxcl_settings 的 game.default_dir(含一次 Python 旧配置迁移)
// > 核心库平台默认(sxcl_paths_default_game_dir)。
QString uiGameDirectory();

// java 可执行文件:SXCL_UI_JAVA_PATH > sxcl_settings 的 game.java_path(设置页写的就是它)
// > Python 旧配置的 Game.javaPath。**空串 = 交给核心库自己探测**(那才是正常路径)。
QString uiJavaPath();

// 最大内存(MB);<=0 = 交给核心库按位数取默认(与 CLI 不给 --memory 时一致)。
int uiMemoryMb();

// 进度追踪开关(SXCL_UI_TRACE=1)。默认关闭,免得正常使用时把 stderr 刷满。
bool uiTraceEnabled();
// 打一行追踪(stderr)。未开启时什么都不做。
void uiTrace(const QString &line);

} // namespace sxcl::ui
