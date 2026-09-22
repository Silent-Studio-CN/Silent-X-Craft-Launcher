/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_controls.h"      // PushButton / InfoBar
#include "fluent/fluent_dialog.h"        // MessageBox(注销二次确认;账户功能新增)
#include "fluent/fluent_labels.h"        // TitleLabel / SubtitleLabel / CaptionLabel
#include "fluent/fluent_menu.h"          // RoundMenu(「下载 Java」的组件菜单,对应 qf RoundMenu)
#include "fluent/fluent_scroll.h"        // ScrollArea(= Python qf ScrollArea)
#include "fluent/fluent_setting_cards.h" // SettingCard 家族 / ComboBox / SettingCardGroup
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"
#include "theme_bridge.h"

// 账户(正版登录)—— Python 版无此功能,新增。实现见 dialogs/account.* 与 dialogs/auth_dialog.*
#include "workers/ui_error.h"  // 统一错误出口:完整上下文 + 自动复制剪贴板
#include "workers/ui_paths.h"  // uiSettingsFilePath():设置文件路径的唯一权威(见 settingsFilePath)

#include "dialogs/account.h"
#include "dialogs/auth_dialog.h"

// 核心库(纯 C):UI 层已链 sxcl 并挂了 include/
#include "sxcl/launch.h"        // Java 运行时探测(替代 Python services/java/finder.py)
#include "sxcl/limiter.h"       // sxcl_limiter_parse_rate("512K" 这类文本 → 字节/秒)
#include "sxcl/android.h"       // Android:"能不能读"的分类(沙箱拒绝/noexec)
#include "sxcl/fs.h"            // sxcl_fs_mkdirs(哈希缓存目录)/ sxcl_fs_exists
#include "sxcl/java_runtime.h"  // 官方 JRE 安装(替代 Python services/java/mojang_runtime.py)
#include "sxcl/jre_hosted.h"    // **自托管 JRE**(我们自己的 index.json;安卓那一档官方清单里没有)
#include "sxcl/json.h"          // 读 <运行时目录>/jre.json 标记(拿组件名,别拿目录名当权威)
#include "sxcl/lang.h"          // 语言表(键 -> 文案;.lang 与 Python 版逐字段兼容)
#include "sxcl/net.h"           // sxcl_transport_qt_create(JRE 下载的传输后端)
#include "sxcl/paths.h"         // 平台默认游戏目录 + 安卓候选扫描
#include "sxcl/settings.h"      // 设置读写(key=value,UTF-8)+ 跨平台设置路径
#include "sxcl/sysinfo.h"       // 物理内存(设置页内存滑块的数据源)

namespace sxcl::ui {
namespace {

// ─────────────────────────── 常量:全部是 Python 侧的字面量,不发明 ───────────────────────────

// src/core/constants.py:34-35
const char *const kAppName = "Silent X Craft Launcher";
const char *const kAppVersion = "0.1.0";
const char *const kAppRepoUrl = "https://github.com/"; // src/core/constants.py:37 APP_REPO_URL

// settings_page.py:215/273/339/365/398 —— 五个分组标题
const char *const kGroupGeneral = "通用设置";
const char *const kGroupGame = "游戏设置";
const char *const kGroupDownload = "下载设置";
const char *const kGroupAdvanced = "高级设置";
const char *const kGroupAbout = "关于";
// **Python 版没有「账户」组**(Python 版只有离线启动):这是新增功能,不是移植。
// 只加内容,控件/令牌/字号一律用既有口径(SettingCard 家族 + docs/05 §2/§3/§4/§5)。
const char *const kGroupAccount = "账户";

// 主题模式:src/app/theme.py:59 THEME_LABELS = ["浅色","深色","跟随系统"]
// 值用字符串(核心库 sxcl_settings_ui_theme 的口径是文本:auto/light/dark)。
const char *const kThemeValues[] = {"light", "dark", "auto"};
const char *const kThemeTexts[] = {"浅色", "深色", "跟随系统"};

// 语言(launcher_config.py:51-60 LauncherLanguage)
const char *const kLanguageValues[] = {"zh-CN", "en-US"};
const char *const kLanguageTexts[] = {"简体中文", "English"};

// 下载源(src/core/constants.py:51-68 DownloadSource:成员顺序 = 下拉顺序)
const char *const kSourceValues[] = {"auto", "mojang", "bmclapi"};
const char *const kSourceTexts[] = {"智能（自动选更快的源）", "Mojang 官方源", "BMCLAPI 镜像源"};

// 窗口大小(launcher_config.py:63-68 WindowSizePreset)
const char *const kWindowValues[] = {"854x480", "1280x720", "1600x900", "1920x1080", "全屏"};

// 版本列表刷新频率(launcher_config.py:71-93 RefreshInterval:值 = 秒,文案 = label)
const int kRefreshValues[] = {30, 60, 120, 300, 600, 1800};
const char *const kRefreshTexts[] = {"30秒", "1分钟", "2分钟", "5分钟", "10分钟", "30分钟"};

const int kRefreshCount = 6;

// 设置键名。注释里"核"= include/sxcl/settings.h 已有便捷读取函数(默认值以核心库为准);
// 其余键是本页新增,默认值与建议签名写进交付报告。
const char *const kKeyTheme = "ui.theme";                 // 核(默认 auto)
const char *const kKeyAccent = "ui.accent";               // 新增,#rrggbb(语义见报告"强调色")
const char *const kKeyLanguage = "ui.language";           // 核(默认 zh-CN)
const char *const kKeyAutoCheckUpdate = "general.auto_check_update";        // 新增,默认开
const char *const kKeyRefreshInterval = "general.version_refresh_interval"; // 新增,默认 120
const char *const kKeyVersionIsolation = "general.version_isolation";       // 新增,默认关
const char *const kKeyGameDir = "game.default_dir";       // 核(默认空 = 平台默认)
const char *const kKeyJavaPath = "game.java_path";        // 新增,默认空
const char *const kKeyMaxMemory = "game.max_memory_mb";   // 新增,默认 4096
const char *const kKeyWindowSize = "game.window_size";    // 新增,默认 1280x720
const char *const kKeyDownloadSource = "download.source"; // 新增,默认 auto
// 「结束后关闭」(电脑端):安装/下载**成功**之后自动退出启动器。默认**关**。
// 只有成功才关:失败/取消必须留在界面上(自动退出等于把原因藏掉)。启动游戏那条路不碰它。
const char *const kKeyCloseAfterInstall = "ui.close_after_install";
const char *const kKeyMaxConn = "download.max_conn";      // 核(便捷读取默认 1;冲突见报告)
const char *const kKeyRate = "download.rate";             // 核(字节/秒,默认 0 = 不限速)
const char *const kKeyVerifySha1 = "download.verify_sha1"; // 新增,默认开
const char *const kKeyDebugMode = "advanced.debug_mode";  // 新增,默认关
const char *const kKeyDownloadEngine = "advanced.use_download_engine"; // 新增,默认开
// docs/22 §14:CurseForge 官方 API 的 key(用户自己在 curseforge.com 申请)。
// **三处必须用同一个字面量**:本页 / 模组页(mods_page.cpp)/ CLI(sxcl-dl mods --key 的兜底来源)。
const char *const kKeyCfApiKey = "mods.curseforge_api_key";

// SettingsPage.__init__ = BasePage(title="设置", subtitle="")(settings_page.py:203-209)
const char *const kPageTitle = "设置";

QStringList textList(const char *const *items, int count) {
    QStringList out;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out.append(QString::fromUtf8(items[i]));
    return out;
}

QStringList stringList(const char *const *items, int count) {
    return textList(items, count); // 值也是字符串表
}

QStringList numberList(const int *items, int count) {
    QStringList out;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out.append(QString::number(items[i]));
    return out;
}

// 下拉"当前项"的通用做法:找不到就用 fallback(Python 的 setCurrentText 找不到时保持 0 号项)
int valueIndex(const QStringList &values, const QString &value, int fallback) {
    const int index = static_cast<int>(values.indexOf(value));
    return index >= 0 ? index : fallback;
}

// ─────────────────────────── 设置文件路径 ───────────────────────────
//
// 唯一权威在核心库:include/sxcl/settings.h 的 sxcl_settings_default_path()。
// 为什么不再自己拼:这段三分支代码以前在本页 / home_page / versions_page / dialogs/account.cpp
// 各有一份,而**安卓那一支全是错的** —— 安卓的 $HOME 是 "/",于是 "~/.config/SilentXCraftLauncher"
// 拼出 "/.config/..."(只读根文件系统,写不进去):设置能改、看着也成功,重启就没了。
// 核心库那份按平台走,安卓落到应用私有目录($SXCL_ANDROID_FILES/SilentXCraftLauncher),
// 另外支持 SXCL_CONFIG_DIR 覆盖(便携版/测试)。
QString settingsFilePath() {
    // **不要再自己拼路径**:交给 ui_paths.cpp 的 uiSettingsFilePath()(内部就是核心库的
    // sxcl_settings_default_path(),并支持 SXCL_UI_SETTINGS 取证覆盖)。
    // 以前这里手拼一份、ui_paths/account 又各手拼一份 —— Android 上"写的文件"和"读的文件"
    // 不是同一个,于是设置页改完重启就回默认值(用户报的"设置关闭重开直接打回原形")。
    return uiSettingsFilePath();
}

// ─────────────────────────── 语言(i18n)───────────────────────────
//
// 文案从核心库的语言表取(include/sxcl/lang.h):内置中英两份,磁盘上的 .lang 可覆盖,
// 找不到的键回落中文。这里只绑**与 Python 版 .lang 逐字一致**的键 —— 见下。
// 取不到就用 fallback(C 侧字面量),所以没初始化语言表时界面与从前一模一样。
QString trText(const char *key, const char *fallback) {
    return QString::fromUtf8(sxcl_lang_tr(key, fallback));
}

// 带 {name} 占位符的文案(java.downloading / java.success / java.failed 就是这种键)。
// 语言表没初始化时用 fallback(中文),行为与从前一致。
QString trName(const char *key, const char *fallback, const QString &name) {
    const char *names[1] = { "name" };
    const QByteArray value = name.toUtf8();
    const char *values[1] = { value.constData() };
    char out[256];
    out[0] = '\0';
    (void)sxcl_lang_format(sxcl_lang_default(), key, fallback, names, values, 1, out, sizeof(out));
    return QString::fromUtf8(out); // 截断也照样显示(format 的返回值是"截断了",不是"没内容")
}

// 设置存储:一个核心库句柄 + 每次改动落盘(对应 Python launcher_config.save_config())
class ConfigStore {
public:
    ConfigStore() {
        m_path = settingsFilePath();
        m_settings = sxcl_settings_open(m_path.toUtf8().constData());
    }
    ~ConfigStore() {
        if (m_settings)
            sxcl_settings_free(m_settings);
    }
    ConfigStore(const ConfigStore &) = delete;
    ConfigStore &operator=(const ConfigStore &) = delete;

    bool valid() const { return m_settings != nullptr; }
    sxcl_settings *handle() const { return m_settings; }

    QString text(const char *key, const QString &fallback = QString()) const {
        if (!m_settings)
            return fallback;
        const char *value = sxcl_settings_get(m_settings, key, nullptr);
        return value ? QString::fromUtf8(value) : fallback;
    }
    int number(const char *key, int fallback) const {
        if (!m_settings)
            return fallback;
        return static_cast<int>(sxcl_settings_get_int(m_settings, key, fallback));
    }
    bool flag(const char *key, bool fallback) const {
        if (!m_settings)
            return fallback;
        return sxcl_settings_get_bool(m_settings, key, fallback ? 1 : 0) != 0;
    }
    void set(const char *key, const QString &value) {
        if (!m_settings)
            return;
        sxcl_settings_set(m_settings, key, value.toUtf8().constData());
        save();
    }
    void set(const char *key, int value) { set(key, QString::number(value)); }
    void set(const char *key, bool value) {
        set(key, value ? QStringLiteral("1") : QStringLiteral("0"));
    }
    void remove(const char *key) {
        if (!m_settings)
            return;
        sxcl_settings_remove(m_settings, key);
        save();
    }
    void save() {
        if (m_settings)
            sxcl_settings_save(m_settings, m_path.toUtf8().constData());
    }

private:
    QString m_path;
    sxcl_settings *m_settings = nullptr;
};

// ─────────────────────────── 主题模式 ───────────────────────────

bool systemPrefersDark() {
    // Python 侧 Theme.AUTO 由 darkdetect.theme() 解析(qf common/config.py:400-402);
    // Qt6 的 QStyleHints::colorScheme() 是同一份平台信息(macOS 外观 / Linux 门户 / Windows 注册表)。
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark)
        return true;
    if (scheme == Qt::ColorScheme::Light)
        return false;
    return FluentTheme::instance().isDark(); // 拿不到就保持现状(darkdetect 返回 None 时 qf 同样回落)
}

// "主题模式"下拉的选中项:优先用存储值(核心库兜底 "auto");没存过就按当前实际明暗如实显示。
QString currentThemeMode(const ConfigStore &store) {
    // 注意:这里**不能**给 text() 传 "auto" 当兜底 —— 那样"没存过"与"存的是 auto"
    // 就分不开了(默认值一律显示成"跟随系统",而实际界面可能是深色)。
    const QString stored = store.text(kKeyTheme);
    if (stored == QLatin1String("light") || stored == QLatin1String("dark") ||
        stored == QLatin1String("auto"))
        return stored;
    return FluentTheme::instance().isDark() ? QStringLiteral("dark") : QStringLiteral("light");
}

// settings_page.py:511-514 _on_theme_changed → apply_theme(qconfig.themeMode.value)
void applyThemeMode(const QString &mode) {
    if (mode == QLatin1String("light"))
        ThemeBridge::instance().setMode(fluent::Theme::Light);
    else if (mode == QLatin1String("dark"))
        ThemeBridge::instance().setMode(fluent::Theme::Dark);
    else
        ThemeBridge::instance().setMode(systemPrefersDark() ? fluent::Theme::Dark
                                                           : fluent::Theme::Light);
}

// ─────────────────────────── Java 运行时(finder.py 的 C 版对应物)───────────────────────────

// finder.py:57-73 JavaInstallation 的展示字段
struct JavaEntry {
    QString path;    // 归一化后的 java 可执行文件全路径(对应 Python Path.resolve())
    QString version; // 完整版本串,如 "26.0.2"
    int major = 0;
    bool compatible = false;
    QString compatibilityLabel;

    // ── 实测结论(include/sxcl/launch.h 的 1c 节)──
    // 能不能用**不是看目录名**,而是真的执行过 <path> -version 才知道。
    // 检测到但不可用的(沙箱拒绝 / 共享存储 noexec / 没有执行位 / 不是 Java / 架构不符 /
    // 跑不起来)照样列出来,附上原因 —— 用户有权知道"为什么没检出来"。
    bool usable = true;      // false = 检测到但用不了
    QString verdictText;     // "可用" / "沙箱拒绝" / "共享存储不能执行" …
    QString reason;          // 原始原因(带路径与 errno/退出码原话)
    QString hint;            // 下一步建议(核心库给的人话)

    // finder.py:66-68 display_name
    QString displayName() const {
        return QStringLiteral("Java %1 - %2").arg(version, QDir::toNativeSeparators(path));
    }
};

// finder.py:86-94 compatibility_for_major
void fillCompatibility(JavaEntry &entry) {
    if (entry.major < 8) {
        entry.compatible = false;
        entry.compatibilityLabel = QStringLiteral("Java %1 - 版本过低").arg(entry.major);
    } else if (entry.major >= 17) {
        entry.compatible = true;
        entry.compatibilityLabel = QStringLiteral("Java %1 - 兼容").arg(entry.major);
    } else {
        entry.compatible = true;
        entry.compatibilityLabel = QStringLiteral("Java %1 - 可用于旧版").arg(entry.major);
    }
}

// Python 侧所有 Java 路径都会先过 platform.py:134 normalize_path → Path.resolve():
// 符号链接**与 Windows 联接点(junction)**都要解到真实路径,否则显示出来的不是同一个目录
// (实测:C:\Program Files\Java\latest\jdk-26 是联接点 → D:\ProgramData\JAVA\Jdk26.0.2)。
// Qt 的 QFileInfo::canonicalFilePath() **不解联接点**(实测返回原路径),所以这里用
// C++17 的 std::filesystem::canonical(MSVC 走 GetFinalPathNameByHandle,联接点一并解掉)。
QString resolveJavaPath(const QString &path) {
    if (path.isEmpty())
        return path;
    std::error_code ec;
#if defined(_WIN32)
    // Windows:宽字符路径 -> canonical() 走 GetFinalPathNameByHandle,联接点一并解掉
    const std::filesystem::path resolved =
        std::filesystem::canonical(path.toStdWString(), ec);
#else
    // POSIX/Android:没有联接点;Qt 的 toStdString() 是 UTF-8,fs::path 直接吃
    const std::filesystem::path resolved =
        std::filesystem::canonical(path.toStdString(), ec);
#endif
    if (ec || resolved.empty())
        return path;
#if defined(_WIN32)
    return QDir::fromNativeSeparators(
        QString::fromStdWString(resolved.native())); // 返回 Qt 口径('/'),显示时再转回本地分隔符
#else
    return QDir::fromNativeSeparators(QString::fromStdString(resolved.native()));
#endif
}

// java_setting_card.py:176-184 _norm:路径比较要归一化(Windows 短名 / 符号链接 / 大小写)
QString normalizeJavaPath(const QString &path) {
    if (path.isEmpty())
        return QString();
    return resolveJavaPath(path).toLower();
}

// finder.py:249-281 discover_java_installations 的 C 版:候选路径 + 扫描根交给核心库
// (sxcl_java_discover,include/sxcl/launch.h:124),这里补 Python 侧的另外两步:
//   1) finder.py:269 的符号链接/联接点解析(Windows 的 "C:\Program Files\Java\latest"
//      这类联接点必须解到真实目录,否则显示的路径与 Python 版不是同一个);
//   2) finder.py:150-159 _dedupe:按 (-major, path) 排序后按路径去重。
QVector<JavaEntry> discoverJavaInstallations() {
    QVector<JavaEntry> result;
    sxcl_java_env_store envStore;
    sxcl_java_env_capture(&envStore);

    constexpr size_t kCapacity = 8;
    sxcl_java_info raw[kCapacity];
    const size_t count =
        sxcl_java_discover(&envStore.env, sxcl_java_current_os(), raw, kCapacity);
    if (count == 0)
        return result;

    QVector<JavaEntry> items;
    const size_t limit = std::min(count, kCapacity);
    for (size_t i = 0; i < limit; ++i) {
        if (raw[i].major <= 0) // 解析不出主版本的候选不要(= Python inspect_java 返回 None)
            continue;
        JavaEntry entry;
        const QString scanned = QString::fromUtf8(raw[i].path);
        entry.path = resolveJavaPath(scanned);
        entry.version = QString::fromUtf8(raw[i].version);
        entry.major = raw[i].major;
        fillCompatibility(entry);
        items.append(entry);
    }
    if (items.isEmpty())
        return result;

    std::sort(items.begin(), items.end(), [](const JavaEntry &a, const JavaEntry &b) {
        if (a.major != b.major)
            return a.major > b.major; // finder.py:153 key = (-x.major, str(x.path))
        return a.path < b.path;
    });

    // 实测很贵(每个候选起一次进程,几百毫秒),而 refresh() 在"主题/语言变了"时会被连着
    // 调好几次 —— 用候选集合当键缓存一次结果(集合没变就不重复起进程)。
    auto detectCached = [](const sxcl_java_env *env,
                           const QVector<JavaEntry> &list) -> const sxcl_java_installations * {
        static sxcl_java_installations cache;
        static QString cacheKey;
        QString key;
        for (const JavaEntry &entry : list)
            key += entry.path + QLatin1Char('|') + QString::number(entry.major) + QLatin1Char('\n');
        if (key == cacheKey)
            return &cache;
        (void)sxcl_java_detect(env, sxcl_java_current_os(), 4000, &cache);
        cacheKey = key;
        return &cache;
    };

    // ── 实测:真的执行一次 <java> -version,把"能用/不能用 + 原因"贴上 ──
    // 桌面平台上 sxcl_java_discover 只读了 release 文本;这一步才回答"它到底跑不跑得起来"。
    // 安卓上不在这里跑(那里由 androidJavaProbeText/sxcl_java_probe_android 说明原因,
    // 而且别人的私有目录注定起不了进程)；安卓的"本应用私有目录"那份会被 detect 扫到。
    {
        const sxcl_java_installations &detected = *detectCached(&envStore.env, items);
        for (size_t i = 0; i < detected.count; ++i) {
            const sxcl_java_installation &d = detected.items[i];
            const QString dpath = normalizeJavaPath(QString::fromUtf8(d.info.path));
            const QString dhome = normalizeJavaPath(QString::fromUtf8(d.info.home));
            const bool ok = (d.verdict == SXCL_JAVA_RUN_OK);
            bool hit = false;
            for (JavaEntry &entry : items) {
                // 路径对不上时看 JAVA_HOME:联接点/符号链接会让"候选里写的路径"与
                // "解析后的真实路径"不同(实测 C:\Program Files\Java\latest\jdk-26 ->
                // D:\ProgramData\JAVA\Jdk26.0.2),只比 path 会漏掉已装好的 JDK。
                const QString ehome =
                    normalizeJavaPath(QFileInfo(entry.path).absolutePath());
                if (dpath == normalizeJavaPath(entry.path) ||
                    (!dhome.isEmpty() && ehome == dhome + QStringLiteral("/bin"))) {
                    entry.usable = ok;
                    entry.verdictText = QString::fromUtf8(sxcl_java_run_verdict_name(d.verdict));
                    entry.reason = QString::fromUtf8(d.reason);
                    entry.hint = QString::fromUtf8(sxcl_java_run_verdict_hint(d.verdict));
                    hit = true;
                    break;
                }
            }
            if (hit || ok)
                continue;
            // "不在"是最没信息量的一档(PATH 里每个目录都会产生一条),不进界面;
            // 真正"检测到了但用不了"的(沙箱拒绝/共享存储 noexec/没有执行位/架构不符/
            // 不是 Java/跑不起来)才列出来并附原因。
            if (d.verdict == SXCL_JAVA_RUN_MISSING)
                continue;
            bool already = false;
            for (const JavaEntry &entry : items) {
                if (normalizeJavaPath(entry.path) == dpath) {
                    already = true;
                    break;
                }
            }
            if (already)
                continue;
            // 检测到但不在"可用清单"里(路径解析不出 release / 用不了):照样列出来
            JavaEntry extra;
            extra.path = QString::fromUtf8(d.info.path);
            extra.version = QString::fromUtf8(d.info.version);
            extra.major = d.info.major;
            extra.usable = false;
            extra.verdictText = QString::fromUtf8(sxcl_java_run_verdict_name(d.verdict));
            extra.reason = QString::fromUtf8(d.reason);
            extra.hint = QString::fromUtf8(sxcl_java_run_verdict_hint(d.verdict));
            if (extra.major > 0)
                fillCompatibility(extra);
            else
                extra.compatibilityLabel = QStringLiteral("版本未知");
            items.append(extra);
        }
        // 用不了的排在最后:能用的那份列表本身按主版本降序
        std::stable_sort(items.begin(), items.end(),
                         [](const JavaEntry &a, const JavaEntry &b) {
                             return (a.usable ? 0 : 1) < (b.usable ? 0 : 1);
                         });
    }

    // 取证(安卓真机看 logcat):每个候选一行 —— 路径 / 来源 / 是否**真的执行过** /
    // 结论 / 原因。桌面验收同样看这几行(截图看不出 tooltip)。
    {
        static QString loggedKey;
        QString key;
        for (const JavaEntry &entry : items)
            key += entry.path + QLatin1Char('|') + entry.verdictText + QLatin1Char('\n');
        if (key != loggedKey) { // 同一份候选集只打一次(设置页刷新会连着调好几回)
            loggedKey = key;
            for (const JavaEntry &entry : items) {
                std::fprintf(stderr,
                             "[sxcl-ui] java-discover: %s major=%d usable=%d verdict=%s reason=%s\n",
                             entry.path.toUtf8().constData(), entry.major, entry.usable ? 1 : 0,
                             entry.verdictText.toUtf8().constData(),
                             entry.reason.isEmpty() ? "-" : entry.reason.toUtf8().constData());
            }
        }
    }

    QSet<QString> seen;
    for (const JavaEntry &entry : items) {
        if (seen.contains(entry.path))
            continue;
        seen.insert(entry.path);
        result.append(entry);
    }
    return result;
}

// finder.py:294-299 best_java_installation(最高版本的兼容 Java)。
// 只在**实测可用**的那些里挑:没真的跑起来过的(沙箱拒绝/架构不符/不是 Java)不当默认值,
// 否则会自动选中一个注定启动失败的 java(用户会看到"选中的 Java 起不来")。
const JavaEntry *bestJavaInstallation(const QVector<JavaEntry> &list) {
    const JavaEntry *best = nullptr;
    for (const JavaEntry &entry : list) {
        if (!entry.usable || !entry.compatible)
            continue;
        if (!best || entry.major > best->major)
            best = &entry;
    }
    if (best == nullptr) {
        for (const JavaEntry &entry : list) { // 兜底:一个可用的都没有时仍给个默认值
            if (!entry.compatible)
                continue;
            if (!best || entry.major > best->major)
                best = &entry;
        }
    }
    return best;
}

// ─────────────────────── 卡片:SpinSettingCard(settings_page.py:155-199)───────────────────────
//
// 单数值设置卡片(右侧 QSpinBox),用于并发数 / 限速这类数值项。
class SpinSettingCard : public SettingCard {
public:
    SpinSettingCard(const QIcon &icon, const QString &title, const QString &content, int minimum,
                    int maximum, int step, const QString &suffix, int value,
                    std::function<void(int)> onChanged, QWidget *parent = nullptr)
        : SettingCard(icon, title, content, parent), m_onChanged(std::move(onChanged)) {
        m_spin = new QSpinBox(this);        // :175
        m_spin->setRange(minimum, maximum); // :176
        m_spin->setSingleStep(step);        // :177
        m_spin->setSuffix(suffix);          // :178
        m_spin->setFixedWidth(130);         // :179
        m_spin->setValue(value);            // :181(cfg 值取不出来时 Python 退到 minimum)

        hBox()->addStretch(1);              // :185
        hBox()->addWidget(m_spin);          // :186
        hBox()->addSpacing(20);             // :187
        connect(m_spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int changed) { onValueChanged(changed); }); // :188
    }

    // :196-199 set_value(外部复位时用,屏蔽信号)
    void setValue(int value) {
        const QSignalBlocker blocker(m_spin);
        m_spin->setValue(value);
    }
    int value() const { return m_spin->value(); }
    QSpinBox *spinBox() const { return m_spin; }

private:
    void onValueChanged(int value) { // :190-194 _apply
        if (m_onChanged)
            m_onChanged(value);
    }

    QSpinBox *m_spin = nullptr;
    std::function<void(int)> m_onChanged;
};

// ─────────────────────── 卡片:MemorySettingCard(settings_page.py:63-152)───────────────────────
//
// 内存设置卡片 —— 滑块 + 输入框右对齐。右侧控件是**原生 QSlider / QSpinBox**
//(Python 里就是从 PySide6.QtWidgets 导的,不是 qf 的 Slider/SpinBox)。
class MemorySettingCard : public SettingCard {
public:
    MemorySettingCard(const QIcon &icon, const QString &title, const QString &content, int minimum,
                      int maximum, int step, int value, QWidget *parent = nullptr)
        : SettingCard(icon, title, content, parent), m_step(step), m_minimum(minimum),
          m_maximum(maximum) {
        auto *right = new QHBoxLayout();       // :84
        right->setSpacing(8);                  // :85
        right->setContentsMargins(0, 0, 20, 0); // :86

        m_slider = new QSlider(Qt::Horizontal, this); // :88
        m_slider->setRange(minimum, maximum);         // :90
        m_slider->setSingleStep(step);                // :91
        m_slider->setPageStep(step);                  // :92
        m_slider->setFixedWidth(180);                 // :93
        m_slider->setValue(value);                    // :94

        m_spin = new QSpinBox(this);                  // :96
        m_spin->setRange(minimum, maximum);           // :97
        m_spin->setSingleStep(step);                  // :98
        m_spin->setSuffix(QStringLiteral(" MB"));     // :99
        m_spin->setFixedWidth(110);                   // :100
        m_spin->setValue(value);                      // :101

        right->addWidget(m_slider);                   // :103
        right->addWidget(m_spin);                     // :104

        hBox()->addStretch(1);                        // :106
        hBox()->addLayout(right);                     // :107

        connect(m_slider, &QSlider::valueChanged, this,
                [this](int changed) { onSliderChanged(changed); }); // :109
        connect(m_spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int changed) { onSpinChanged(changed); });   // :110
    }

    void setValue(int value) { // :145-152 _on_config_changed(屏蔽信号,只同步显示)
        {
            const QSignalBlocker blocker(m_slider);
            m_slider->setValue(value);
        }
        {
            const QSignalBlocker blocker(m_spin);
            m_spin->setValue(value);
        }
    }
    int value() const { return m_spin->value(); }
    void setChangeHandler(std::function<void(int)> handler) { m_onChanged = std::move(handler); }

private:
    int aligned(int value) const { // :114-115 / :130-131 对齐到 step 并夹到 [min,max]
        int result = ((value + m_step / 2) / m_step) * m_step;
        result = std::max(m_minimum, std::min(result, m_maximum));
        return result;
    }

    void onSliderChanged(int value) { // :113-127
        const int current = aligned(value);
        if (current != value) {
            const QSignalBlocker blocker(m_slider);
            m_slider->setValue(current);
            value = current;
        }
        {
            const QSignalBlocker blocker(m_spin);
            m_spin->setValue(value);
        }
        if (m_onChanged)
            m_onChanged(value);
    }

    void onSpinChanged(int value) { // :129-143
        const int current = aligned(value);
        if (current != value) {
            const QSignalBlocker blocker(m_spin);
            m_spin->setValue(current);
            value = current;
        }
        {
            const QSignalBlocker blocker(m_slider);
            m_slider->setValue(value);
        }
        if (m_onChanged)
            m_onChanged(value);
    }

    QSlider *m_slider = nullptr;
    QSpinBox *m_spin = nullptr;
    int m_step;
    int m_minimum;
    int m_maximum;
    std::function<void(int)> m_onChanged;
};


// ── Android:共享存储权限("所有文件访问权限")──
//
// 设备实测(192.168.220.33,Android 16,targetSdk 34):APK 只声明 INTERNET +
// ACCESS_NETWORK_STATE 时,以本应用 uid 去 stat /storage/emulated/0/FCL/.minecraft
// 直接 Permission denied —— 也就是说**没有这个权限,自动扫描在共享存储上一无所获**,
// 而用户的 .minecraft/存档/模组全在那儿。所以:
//   1) 打包层 manifest 声明 MANAGE_EXTERNAL_STORAGE(见 build/_android/pkg/AndroidManifest.xml);
//   2) 这里负责"检查 + 引导 + 授权后立刻重扫",不依赖用户重启。
#if defined(__ANDROID__)
#include <QJniObject>
namespace {
bool androidHasAllFilesAccess() {
    return QJniObject::callStaticMethod<jboolean>("com/silentstudio/sxcl/SxclActivity",
                                                "hasAllFilesAccess", "()Z");
}
void androidRequestAllFilesAccess() {
    QJniObject::callStaticMethod<void>("com/silentstudio/sxcl/SxclActivity",
                                       "requestAllFilesAccess", "()V");
}
} // namespace
#else
namespace {
bool androidHasAllFilesAccess() { return true; } // 桌面没有这回事,恒当"有权限"
void androidRequestAllFilesAccess() {}
} // namespace
#endif
// ── Android:Java 到底去哪儿找了、为什么没用上(用户反馈原文:”我平板有 HMCL
//    不可能没有 JAVA…… 成功检出游戏,但是没检出 JAVA“)──
//
// 这一层只把核心库已经算好的结论(sxcl_java_probe_android)转成人看的两行文字:
//   * shortText —— 塞进卡片上**已经有**的那行 CaptionLabel,不改页面结构、不加控件;
//   * detail    —— 塞进 tooltip(悬停/长按可见),逐条列出每个候选与原始原因。
// 逻辑放这里而不是核心库,是因为”显示成什么样“属于界面口径;结论本身在 core 里单测过。
// 桌面平台也会走这段:候选表是安卓专用的,桌面上找不到就自然落回原来的空态文案。
struct JavaProbeText {
    QString shortText;  // 一行,给 CaptionLabel
    QString detail;     // 全文,给 tooltip
};

JavaProbeText androidJavaProbeText() {
    JavaProbeText out;
    const QByteArray files = qEnvironmentVariable("SXCL_ANDROID_FILES").toUtf8();
    sxcl_java_report report;
    const size_t count = sxcl_java_probe_android(files.isEmpty() ? nullptr : files.constData(),
                                                 nullptr, &report);
    if (count == 0)
        return out;

    // 先说最值钱的那条:DENIED 意味着”有 Java,但安卓不让用“,这正是用户的疑问所在。
    const sxcl_java_probe *lead = nullptr;
    for (size_t i = 0; i < report.count && lead == nullptr; ++i) {
        if (report.items[i].verdict == SXCL_JAVA_VERDICT_DENIED)
            lead = &report.items[i];
    }
    for (size_t i = 0; i < report.count && lead == nullptr; ++i) {
        if (report.items[i].verdict != SXCL_JAVA_VERDICT_MISSING &&
            report.items[i].verdict != SXCL_JAVA_VERDICT_USABLE)
            lead = &report.items[i];
    }
    if (lead != nullptr) {
        out.shortText = QStringLiteral("未找到可用的 Java:检测到 %1 的 Java,但%2")
                            .arg(QString::fromUtf8(lead->owner),
                                 QString::fromUtf8(sxcl_java_verdict_name(lead->verdict)));
    } else {
        out.shortText = QStringLiteral("未找到可用的 Java:这台机器上还没有装(可点下载 Java)");
    }

    QStringList lines;
    lines << QStringLiteral("我们找过这 %1 个地方:").arg(static_cast<int>(report.count));
    for (size_t i = 0; i < report.count; ++i) {
        const sxcl_java_probe &p = report.items[i];
        lines << QStringLiteral("[%1] %2  %3\n    %4\n    %5")
                     .arg(QString::fromUtf8(sxcl_java_verdict_name(p.verdict)),
                          QString::fromUtf8(p.owner), QString::fromUtf8(p.path),
                          QString::fromUtf8(p.reason),
                          QString::fromUtf8(sxcl_java_verdict_hint(p.verdict)));
    }
    out.detail = lines.join(QStringLiteral("\n"));
    return out;
}

// ──────────────────── 卡片:JavaSettingCard(java_setting_card.py:94-272)────────────────────
class JavaSettingCard : public SettingCard {
public:
    using SelectionHandler = std::function<void(const QString &)>;

    explicit JavaSettingCard(QWidget *parent = nullptr)
        : SettingCard(FluentIcon::qicon(FluentIcon::DEVELOPER_TOOLS),
                      QStringLiteral("Java 运行路径"),
                      QStringLiteral("选择用于启动 Minecraft 的 Java 运行时"), parent) {
        setFixedHeight(96); // :104

        m_combo = new ComboBox(this);      // :107
        m_combo->setMinimumWidth(320);     // :108
        m_importButton = new PushButton(QStringLiteral("导入"), this);        // :109
        m_downloadButton = new PushButton(QStringLiteral("下载 Java"), this); // :110
        m_statusLabel = new CaptionLabel(QString(), this);                    // :112
        m_statusLabel->setTextColor(QColor(0x52, 0xc4, 0x1a), QColor(0x73, 0xd1, 0x3d)); // :113

        auto *rightLayout = new QVBoxLayout();        // :115
        rightLayout->setSpacing(6);                   // :116
        rightLayout->setContentsMargins(0, 0, 0, 0);  // :117

        auto *topRow = new QHBoxLayout();             // :119
        topRow->setSpacing(8);                        // :120
        topRow->addWidget(m_combo);                   // :121
        topRow->addWidget(m_downloadButton);          // :122
        topRow->addWidget(m_importButton);            // :123
        topRow->setAlignment(Qt::AlignRight);         // :124

        rightLayout->addLayout(topRow);               // :126
        rightLayout->addWidget(m_statusLabel, 0, Qt::AlignRight); // :127

        hBox()->addLayout(rightLayout, 0);            // :129
        hBox()->addSpacing(16);                       // :130

        connect(m_importButton, &QPushButton::clicked, this, [this] { importJava(); });   // :132
        connect(m_downloadButton, &QPushButton::clicked, this, [this] {                // :133
            // 下载中再点一次 = 取消(核心库的取消是异步的,会在文件边界停下)
            if (m_worker.joinable()) {
                cancelDownload();
                return;
            }
            showDownloadMenu();
        });
        connect(m_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int index) { onSelectionChanged(index); });                        // :134

        refresh(); // :136
    }

    void setSelectionHandler(SelectionHandler handler) { m_onSelection = std::move(handler); }

    // java_setting_card.py:138-174 refresh
    void refresh(const QString &preferredPath = QString()) {
        m_installations = discoverJavaInstallations(); // :139
        {
            const QSignalBlocker blocker(m_combo);     // :140 blockSignals(True)
            m_combo->clear();                          // :141

            if (m_installations.isEmpty()) {           // :143-148
                // 用户反馈的正是这一支:"没检出 JAVA"。不再只写"未找到",而是**如实说为什么**
                // (别的启动器的 Java 在它自己的私有目录里,安卓不允许我们读;共享存储是 noexec)。
                // 一行结论放回原来那行 CaptionLabel(不加控件、不改布局),逐条明细放 tooltip。
                m_combo->addItem(QStringLiteral("未检测到 Java，请手动导入"));
                m_statusLabel->setText(QStringLiteral("未找到可用的 Java 运行时"));
                m_statusLabel->setTextColor(QColor(0xfa, 0x8c, 0x16), QColor(0xff, 0xa9, 0x40));
                const JavaProbeText probe = androidJavaProbeText();
                if (!probe.shortText.isEmpty())
                    m_statusLabel->setText(probe.shortText); // 仍然是那一行 CaptionLabel(12px)
                if (!probe.detail.isEmpty()) {
                    m_statusLabel->setToolTip(probe.detail);
                    m_combo->setToolTip(probe.detail);
                    m_downloadButton->setToolTip(probe.detail);
                }
                return;
            }

            for (const JavaEntry &entry : m_installations) { // :150-151
                // 检测到但用不了的照样列出来,并且在条目上直接写清为什么(② 的要求:
                // 分类给出不可用原因,别静默丢弃)
                const QString text =
                    entry.usable
                        ? entry.displayName()
                        : QStringLiteral("%1（不可用：%2）").arg(entry.displayName(),
                                                                 entry.verdictText);
                m_combo->addItem(text, entry.path);
                const int row = m_combo->count() - 1;
                if (!entry.usable) {
                    QString tip = entry.reason;
                    if (!entry.hint.isEmpty())
                        tip += QStringLiteral("\n") + entry.hint;
                    m_combo->setItemData(row, tip, Qt::ToolTipRole);
                }
            }
            // 一条能用的都没有、但确实"检测到了"时,把原因写到状态行(而不是只说"未找到")
            {
                bool anyUsable = false;
                for (const JavaEntry &entry : m_installations) {
                    if (entry.usable) {
                        anyUsable = true;
                        break;
                    }
                }
                if (!anyUsable && !m_installations.isEmpty()) {
                    const JavaEntry &first = m_installations.first();
                    m_statusLabel->setText(QStringLiteral("✗ 检测到 Java 但都用不了：%1")
                                               .arg(first.verdictText));
                    m_statusLabel->setTextColor(QColor(0xfa, 0x8c, 0x16),
                                                QColor(0xff, 0xa9, 0x40));
                    QStringList tips;
                    for (const JavaEntry &entry : m_installations)
                        tips << QStringLiteral("%1\n  %2").arg(entry.path, entry.reason);
                    m_statusLabel->setToolTip(tips.join(QStringLiteral("\n\n")));
                    m_downloadButton->setToolTip(tips.join(QStringLiteral("\n\n")));
                }
            }

            int selected = 0;                              // :153
            if (!preferredPath.isEmpty()) {                // :154-160 按归一化路径找
                const QString wanted = normalizeJavaPath(preferredPath);
                for (int index = 0; index < m_combo->count(); ++index) {
                    if (normalizeJavaPath(m_combo->itemData(index).toString()) == wanted) {
                        selected = index;
                        break;
                    }
                }
            } else if (const JavaEntry *best = bestJavaInstallation(m_installations)) { // :161-169
                for (int index = 0; index < m_installations.size(); ++index) {
                    if (m_installations.at(index).path == best->path) {
                        selected = index;
                        break;
                    }
                }
            }

            m_combo->setCurrentIndex(selected);            // :171
            updateStatus(m_installations.at(selected));    // :173-174
        }
    }

    // java_setting_card.py:186-190 selected_path
    QString selectedPath() const {
        if (m_combo->count() == 0)
            return QString();
        return m_combo->currentData().toString();
    }

private:
    // java_setting_card.py:199-207 _update_status
    void updateStatus(const JavaEntry &entry) {
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        if (!entry.usable) { // 检测到但用不了:状态行说清结论 + 原因,别显示"✓ 兼容"骗人
            m_statusLabel->setText(QStringLiteral("✗ %1：%2").arg(entry.verdictText, entry.reason));
            m_statusLabel->setTextColor(tokens.danger);
            m_statusLabel->setToolTip(entry.hint);
            return;
        }
        if (entry.compatible) {
            m_statusLabel->setText(QStringLiteral("✓ %1").arg(entry.compatibilityLabel));
            m_statusLabel->setTextColor(tokens.success);
        } else {
            m_statusLabel->setText(QStringLiteral("✗ %1").arg(entry.compatibilityLabel));
            m_statusLabel->setTextColor(tokens.danger);
        }
    }

    void onSelectionChanged(int index) { // :192-197
        if (index < 0 || index >= m_installations.size())
            return;
        const JavaEntry &entry = m_installations.at(index);
        updateStatus(entry);
        if (m_onSelection)
            m_onSelection(entry.path);
    }

    // java_setting_card.py:250-271 _import_java
    void importJava() {
#if defined(Q_OS_WIN)
        const QString filter = QStringLiteral("Java 可执行文件 (java.exe)");
#else
        const QString filter = QStringLiteral("Java 可执行文件 (java)");
#endif
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 Java 可执行文件"), QString(), filter);
        if (path.isEmpty())
            return;

        sxcl_java_info info{};
        if (sxcl_java_inspect(path.toUtf8().constData(), &info) != 0 || info.major <= 0) {
            m_statusLabel->setText(QStringLiteral("无法识别所选 Java 运行时")); // :263
            m_statusLabel->setTextColor(QColor(0xff, 0x4d, 0x4f), QColor(0xff, 0x78, 0x75));
            return;
        }

        JavaEntry entry;
        entry.path = resolveJavaPath(path);
        entry.version = QString::fromUtf8(info.version);
        entry.major = info.major;
        fillCompatibility(entry);

        bool exists = false;
        for (const JavaEntry &item : m_installations) {
            if (item.path == entry.path) {
                exists = true;
                break;
            }
        }
        if (!exists)
            m_installations.prepend(entry); // :269 insert(0, install)

        refresh(entry.path);                // :271
        if (m_onSelection)
            m_onSelection(entry.path);      // :272
    }

    // ── 官方 JRE 下载(java_setting_card.py:211-248 的 C 版)──
    //
    // 核心库接口:include/sxcl/java_runtime.h 的 sxcl_java_runtime_install ——
    // 组件清单(all.json -> 组件清单)从 Mojang 取、**每个文件按 downloads.raw.sha1 强校验**、
    // 文件交给下载引擎(断点续传/换源/已存在且校验通过就跳过)、装完写 .sxcl_runtime.json。
    // 线程模型:安装在**工作线程**里跑(核心库的进度回调来自引擎的工作线程),这里只把
    // 文案/百分比用 Qt 队列投递回界面线程;取消用一个原子标志 + 核心库的 is_cancelled 回调。
    // java_setting_card.py:211-219 _show_download_menu(候选表来自核心库,与 Python COMPONENT_PREVIEW 同源)
    void showDownloadMenu() {
        if (m_worker.joinable()) {
            return; // 正在下载:按钮此刻是"取消下载",菜单不该弹出来
        }
        auto *menu = new RoundMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(FluentIcon::qicon(FluentIcon::DOWNLOAD),
                        QStringLiteral("自动（按最新正式版选择）"),
                        [this] { startDownload(QString()); });
        menu->addSeparator();
        for (size_t i = 0; i < sxcl_java_runtime_preset_count(); ++i) {
            const sxcl_java_runtime_preset *preset = sxcl_java_runtime_preset_at(i);
            if (preset == nullptr)
                continue;
            const QString component = QString::fromUtf8(preset->component);
            menu->addAction(FluentIcon::qicon(FluentIcon::DOWNLOAD),
                            QString::fromUtf8(preset->label),
                            [this, component] { startDownload(component); });
        }
        // 与 Python 同一处:菜单从按钮左下角弹出(exec 会开嵌套事件循环,这里用 popupAt)
        menu->popupAt(m_downloadButton->mapToGlobal(m_downloadButton->rect().bottomLeft()));
    }

    // java_setting_card.py:221-232 _start_download
    void startDownload(const QString &component) {
        if (m_worker.joinable())
            return;
#if !defined(SXCL_UI_HAVE_QT_TRANSPORT)
        // 没有传输后端就如实说 —— 不做"假进度条"这种事(界面层不许假装成功)
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("下载 Java 需要网络后端"),
                      QStringLiteral("本次构建没有链接 Qt Network 传输后端(sxcl_net_qt)，无法下载官方 JRE。"),
                      window(), 6000);
#else
        char root[SXCL_JAVA_RUNTIME_PATH_MAX];
        char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
        err[0] = '\0';
        if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
            SXCL_JAVA_RUNTIME_OK) {
            UiErrorContext ctx;
            ctx.page = QStringLiteral("设置页 / settings");
            ctx.action = QStringLiteral("下载 Java %1").arg(component);
            ctx.reason = QString::fromUtf8(err);
            ctx.title = QStringLiteral("找不到 Java 安装目录");
            pushUiError(window(), ctx, 8000);
            return;
        }
        m_cancelRequested.store(false);
        m_downloadButton->setText(QStringLiteral("取消下载"));
        m_statusLabel->setText(QStringLiteral("正在获取官方 JRE 清单…"));
        m_statusLabel->setTextColor(QColor(0x00, 0x78, 0xd4), QColor(0x00, 0xbc, 0xf2));

        // 这两份 QByteArray 必须活到 install 调用结束(绝不能写成 xxx.toUtf8().constData():
        // 那是临时对象,语句一结束就失效 —— request 里存的是裸指针)
        const QByteArray componentUtf8 = component.toUtf8();
        const QByteArray rootUtf8 = QByteArray(root);
        m_worker = std::thread([this, componentUtf8, rootUtf8] {
            // 下载参数从设置读(环境变量优先:SXCL_DL_*),与其它下载路径同一口径
            sxcl_settings_download dl;
            memset(&dl, 0, sizeof(dl));
            // 同一份设置文件(与设置页写的是同一个;见 settingsFilePath 的说明)
            const QByteArray cfg = settingsFilePath().toUtf8();
            if (!cfg.isEmpty()) {
                if (sxcl_settings *settings = sxcl_settings_open(cfg.constData())) {
                    sxcl_settings_resolve_download(settings, &dl);
                    sxcl_settings_free(settings);
                }
            }
            char cacheFile[600];
            cacheFile[0] = '\0';
            if (dl.cache_dir[0] != '\0' && sxcl_fs_mkdirs(dl.cache_dir) == 0) {
                // 与命令行前端同口径:<缓存目录>/hashes.txt(sxcl-dl main.c:447)
                snprintf(cacheFile, sizeof(cacheFile), "%s/hashes.txt", dl.cache_dir);
            }

            sxcl_engine_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.workers = dl.workers;
            opts.rate_bps = dl.rate_bps;
            opts.max_conn_per_file = dl.max_conn_per_file;
            opts.cache_path = cacheFile[0] ? cacheFile : nullptr;

            sxcl_java_runtime_request request;
            memset(&request, 0, sizeof(request));
            request.component = componentUtf8.isEmpty() ? nullptr : componentUtf8.constData();
            request.required_major = 0; // 0 = 核心库自己定(没有 MC 版本时为 21,与 Python 一致)
            request.target_root = rootUtf8.constData();
            request.use_mirror = 1;     // 官方失败就换 BMCLAPI(与 Python 的两条路一致)
            // java_runtime.h 的字段是 sxcl_transport *(*)(void *ud),而 net.h 的 Qt 工厂是无参的
            // sxcl_transport_qt_create(void) —— 用无捕获 lambda 适配(避免为了一个签名去改公共头)
            request.transport_factory = [](void *) -> sxcl_transport * { return sxcl_transport_qt_create(); };
            request.engine_opts = &opts;
            request.on_progress = &JavaSettingCard::progressTrampoline;
            request.is_cancelled = &JavaSettingCard::cancelTrampoline;
            request.ud = this;

            sxcl_java_runtime_result result;
            const int rc = sxcl_java_runtime_install(&request, &result);

            const bool ok = (rc == SXCL_JAVA_RUNTIME_OK);
            const QString detail = ok ? QString::fromUtf8(result.version)
                                      : QString::fromUtf8(result.error);
            const QString javaPath =
                ok ? QDir::fromNativeSeparators(QString::fromUtf8(result.java_path)) : QString();
            const bool cancelled = (rc == SXCL_JAVA_RUNTIME_ERR_CANCELLED);
            QMetaObject::invokeMethod(
                this,
                [this, ok, cancelled, detail, javaPath] {
                    onInstallFinished(ok, cancelled, detail, javaPath);
                },
                Qt::QueuedConnection);
        });
#endif
    }

    // 下载中再点一次 = 取消(请求是异步的,核心库会在文件边界上停下来)
    void cancelDownload() {
        m_cancelRequested.store(true);
        m_statusLabel->setText(QStringLiteral("正在取消…"));
    }

    bool cancelRequested() const { return m_cancelRequested.load(); }
    bool installing() const { return m_worker.joinable(); }

    // 工作线程 -> 界面线程的进度投递(核心库的进度回调在工作线程里)
    void postProgress(const QString &message, int percent) {
        QMetaObject::invokeMethod(
            this, [this, message, percent] { onInstallProgress(message, percent); },
            Qt::QueuedConnection);
    }

    ~JavaSettingCard() override {
        // 页面被销毁时先请工作线程收工:取消是异步的,join 等它真的退出(不 detach,
        // 否则线程会拿着已经析构的 this 去回调 —— 那是崩溃,不是"偶发")
        if (m_worker.joinable()) {
            m_cancelRequested.store(true);
            m_worker.join();
        }
    }

    ComboBox *m_combo = nullptr;
    PushButton *m_importButton = nullptr;
    PushButton *m_downloadButton = nullptr;
    CaptionLabel *m_statusLabel = nullptr;
    QVector<JavaEntry> m_installations;
    SelectionHandler m_onSelection;

    // 核心库的进度回调(**工作线程**):只做投递,不碰控件
    static void progressTrampoline(void *ud, const sxcl_java_runtime_progress *progress) {
        if (ud == nullptr || progress == nullptr)
            return;
        static_cast<JavaSettingCard *>(ud)->postProgress(
            QString::fromUtf8(progress->message ? progress->message : ""), progress->percent);
    }
    static int cancelTrampoline(void *ud) {
        return (ud != nullptr && static_cast<JavaSettingCard *>(ud)->cancelRequested()) ? 1 : 0;
    }

    // 界面线程:一行状态 + 百分比(进度条不新加控件,复用卡片里那行 CaptionLabel)
    void onInstallProgress(const QString &message, int percent) {
        m_statusLabel->setText(QStringLiteral("%1（%2%）").arg(message, QString::number(percent)));
    }

    // java_setting_card.py:237-248 _on_download_finished
    void onInstallFinished(bool ok, bool cancelled, const QString &detail,
                           const QString &javaPath) {
        m_downloadButton->setText(QStringLiteral("下载 Java"));
        if (ok) {
            m_statusLabel->setText(QStringLiteral("✅ 已安装官方 JRE"));
            m_statusLabel->setTextColor(QColor(0x52, 0xc4, 0x1a), QColor(0x73, 0xd1, 0x3d));
            refresh(javaPath); // :244 装完立刻选中它
            if (m_onSelection)
                m_onSelection(javaPath); // :245 selectionChanged -> 设置页落盘 game.java_path
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("Java 安装完成"),
                          trName("java.success", "{name} 安装完成", detail), window(), 6000);
        } else if (cancelled) {
            m_statusLabel->setText(QStringLiteral("已取消下载"));
            m_statusLabel->setTextColor(QColor(0xfa, 0x8c, 0x16), QColor(0xff, 0xa9, 0x40));
        } else {
            m_statusLabel->setText(QStringLiteral("❌ %1").arg(detail.left(80)));
            m_statusLabel->setTextColor(QColor(0xff, 0x4d, 0x4f), QColor(0xff, 0x78, 0x75));
            UiErrorContext ctx;
            ctx.page = QStringLiteral("设置页 / settings");
            ctx.action = QStringLiteral("安装官方 JRE");
            ctx.reason = detail; // 核心库/下载器给的真实原因(状态码/校验/网络),原样进剪贴板
            ctx.detail = QStringLiteral("版本状态行:%1").arg(m_statusLabel->text());
            ctx.title = QStringLiteral("Java 安装失败");
            pushUiError(window(), ctx, 10000);
        }
    }

    std::thread m_worker;
    std::atomic<bool> m_cancelRequested{false};
};

// ─────────────── 卡片:HostedJreCard(自托管 JRE;核心接口 include/sxcl/jre_hosted.h)───────────────
//
// 与上面 JavaSettingCard 的**官方 JRE** 是两条来源完全不同的链路,不能混:
//   * 官方(java_runtime.h):Mojang all.json 的**桌面**构建(安卓 arm64 那一档清单里根本没有),
//     逐文件走 downloads.raw.sha1;
//   * 自托管(jre_hosted.h):**我们自己的** index.json(用户自己托管,SXCL/jre 落位已定),
//     包是 .tar.xz,逐文件 sha256,装完写 <目录>/jre.json(见 docs/19 §2.1/§3)。
//
// 这一层是**纯 UI 接线**(用户要求):取清单 / 下载 / 校验 / 解包 / 落标记全在核心库
// sxcl_jre_install(),界面一行下载逻辑都不重写。界面只做三件事:
//   1) 显示**来源**(三级可配,优先级由核心库 sxcl_jre_resolve_index_url 定,界面不另发明顺序);
//   2) 显示**已装组件**(逐个读 <运行时根>/<目录>/jre.json 标记);
//   3) 在工作线程里跑安装,把阶段/百分比/速度/剩余与错误搬回界面线程。
//
// 来源三级(高 -> 低),卡片上把"设置里存的"与"这次真正会用的"分开显示,两者不同时一眼可见:
//   1) 环境变量 SXCL_JAVA_JRE_INDEX_URL        (打包层 / 运维)
//   2) 设置键   java.jre_index_url             (就是下面这个输入框)
//   3) 编译期默认 SXCL_JRE_INDEX_URL_DEFAULT    (GitHub raw)
// 注意:安装请求传的是 **index_setting**,不是 index_url(explicit)。显式值优先级**高于**环境变量,
// 那样子命令行的"照着界面走"会盖掉运维注入的来源 —— 与 §来源三级 的口径不符。
namespace {

// 组件候选:index.json 的组件名是 jre<主版本>(见 docs/19 §2.1 的真清单:jre17/jre21/jre25)。
// 这里只列**主版本**,由核心库去清单里精确挑(找不到直接报错,不"顺手换一个")。
const int kHostedMajors[] = {8, 17, 21, 25};
const int kHostedMajorCount = static_cast<int>(sizeof(kHostedMajors) / sizeof(kHostedMajors[0]));

QString humanBytes(int64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 2);
    return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
}

// 剩余时间:算不出来就明说"算不出来",不编一个数字(与核心库 eta_seconds < 0 的口径一致)。
QString humanEta(int64_t seconds) {
    if (seconds < 0)
        return QStringLiteral("剩余 算不出");
    if (seconds < 60)
        return QStringLiteral("剩余 %1 秒").arg(seconds);
    return QStringLiteral("剩余 %1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
}

struct HostedJre {
    QString dir;
    QString component; // <dir>/jre.json 里的 component(index 的 id 才是权威;读不到才用目录名)
    QString version;
};

/** 一个装好的自托管运行时:读它目录里的 jre.json 补出组件名与版本。 */
HostedJre readHostedJre(const QDir &dir) {
    HostedJre item;
    item.dir = QDir::fromNativeSeparators(dir.absolutePath());
    item.component = dir.dirName();
    const QByteArray native = QDir::toNativeSeparators(item.dir).toUtf8();
    char version[SXCL_JRE_VERSION_MAX];
    version[0] = '\0';
    (void)sxcl_jre_read_marker(native.constData(), version, sizeof(version), nullptr, 0);
    item.version = QString::fromUtf8(version);
    char jerr[160];
    jerr[0] = '\0';
    const QString marker = dir.absoluteFilePath(QString::fromUtf8(SXCL_JRE_MARKER));
    if (sxcl_json *doc = sxcl_json_parse_file(marker.toUtf8().constData(), jerr, sizeof(jerr))) {
        // 注意:文档句柄(sxcl_json*)与节点(sxcl_json_value*)是**两个类型**,
        // 取字段要先过 sxcl_json_root()(json.h:39/72)。
        const char *component = sxcl_json_get_string(sxcl_json_root(doc), "component", nullptr);
        if (component != nullptr && component[0] != '\0')
            item.component = QString::fromUtf8(component);
        sxcl_json_free(doc);
    }
    return item;
}

/** 递归找"已经装好的自托管运行时"。
 *  **必须递归**:装到哪由清单里的 dir_key 定,而它是 <组件>/<版本>/<ABI>
 *  三段(jre_hosted.c:655 -> install 用 dir_key 落盘,见 jre_hosted.c:1676)——
 *  只扫一层是**找不到**的(验收真跑时当场踩到)。判据始终是核心库的
 *  sxcl_jre_is_installed(<dir>/bin/java + <dir>/jre.json 都在),界面不自己拍一个规则。 */
void collectHostedJres(const QDir &dir, int depth, QVector<HostedJre> *out) {
    if (out == nullptr || depth > 4)
        return;
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QDir sub(dir.absoluteFilePath(name));
        const QByteArray native = QDir::toNativeSeparators(sub.absolutePath()).toUtf8();
        if (sxcl_jre_is_installed(native.constData(), nullptr) == 1) {
            out->append(readHostedJre(sub));
            continue; // 这一层已经是一份运行时,不再往里走(里面还有 bin/ lib/ 等)
        }
        collectHostedJres(sub, depth + 1, out);
    }
}

QVector<HostedJre> scanHostedJres(const QString &root) {
    QVector<HostedJre> out;
    const QDir dir(root);
    if (root.isEmpty() || !dir.exists())
        return out;
    collectHostedJres(dir, 0, &out);
    return out;
}

} // namespace

class HostedJreCard : public SettingCard {
public:
    explicit HostedJreCard(QWidget *parent = nullptr)
        : SettingCard(FluentIcon::qicon(FluentIcon::DEVELOPER_TOOLS),
                      QStringLiteral("内置 JRE（自托管）"),
                      QStringLiteral("从我们自己的 index.json 下载随包分发的运行时"), parent) {
        setFixedHeight(132); // 四行:来源输入 / 生效来源 / 已装组件 / 状态

        m_urlEdit = new QLineEdit(this);
        m_urlEdit->setMinimumWidth(330);
        m_urlEdit->setPlaceholderText(
            QStringLiteral("index.json 地址（留空 = 用环境变量或编译期默认）"));
        m_urlEdit->setToolTip(QStringLiteral("设置键：%1\n留空表示不覆盖——环境变量 %2 优先，"
                                             "其次是编译期默认的 GitHub raw 地址。")
                                  .arg(QString::fromUtf8(SXCL_JRE_INDEX_URL_SETTING),
                                       QString::fromUtf8(SXCL_JRE_INDEX_URL_ENV)));

        m_component = new ComboBox(this);
        for (int i = 0; i < kHostedMajorCount; ++i) {
            m_component->addItem(QStringLiteral("Java %1").arg(kHostedMajors[i]), kHostedMajors[i]);
        }
        m_component->setCurrentIndex(1); // 默认 Java 17(与"装 1.17+ 用 17"的口径一致)
        m_component->setToolTip(QStringLiteral("要装哪一个主版本（清单里的组件名是 jre<主版本>）"));

        m_downloadButton = new PushButton(QStringLiteral("开始下载"), this);
        // 稳定的 objectName:验收钩子(SXCL_UI_JRE_HOSTED)靠它**点真的按钮**走产品路径,
        // 而不是在 main.cpp 里另起一条"自己调核心库"的旁路(那样验不到接线本身)。
        m_downloadButton->setObjectName(QStringLiteral("hostedJreDownloadButton"));
        m_component->setObjectName(QStringLiteral("hostedJreComponent"));
        m_urlEdit->setObjectName(QStringLiteral("hostedJreSourceUrl"));
        m_statusLabel = new CaptionLabel(QString(), this);
        m_sourceLabel = new CaptionLabel(QString(), this);
        m_installedLabel = new CaptionLabel(QString(), this);

        auto *rightLayout = new QVBoxLayout();
        rightLayout->setSpacing(4);
        rightLayout->setContentsMargins(0, 0, 0, 0);

        auto *topRow = new QHBoxLayout();
        topRow->setSpacing(8);
        topRow->addWidget(m_urlEdit);
        topRow->addWidget(m_component);
        topRow->addWidget(m_downloadButton);
        topRow->setAlignment(Qt::AlignRight);

        rightLayout->addLayout(topRow);
        rightLayout->addWidget(m_sourceLabel, 0, Qt::AlignRight);
        rightLayout->addWidget(m_installedLabel, 0, Qt::AlignRight);
        rightLayout->addWidget(m_statusLabel, 0, Qt::AlignRight);

        hBox()->addLayout(rightLayout, 0);
        hBox()->addSpacing(16);

        connect(m_urlEdit, &QLineEdit::editingFinished, this, [this] { saveSourceUrl(); });
        connect(m_downloadButton, &QPushButton::clicked, this, [this] {
            // 下载中再点一次 = 取消(核心库在文件边界上停,已装好的组件保留)
            if (m_worker.joinable()) {
                cancelDownload();
                return;
            }
            startDownload();
        });

        refresh();
    }

    ~HostedJreCard() override {
        // 页面销毁时先请工作线程收工:取消是异步的,join 等它真的退出
        // (不 detach —— 否则线程会拿着已经析构的 this 回调,那是崩溃不是"偶发")
        if (m_worker.joinable()) {
            m_cancelRequested.store(true);
            m_worker.join();
        }
    }

    /** 初始值由页面从设置里读好传进来(卡片不自己开第二份设置句柄去读,免得两份读数分叉)。 */
    void setStoredSource(const QString &value) {
        m_sourceSaved = value;
        refresh();
    }

    /** 落盘出口由页面给(设置页的 ConfigStore 才是设置的唯一写入口)。 */
    void setSaveHandler(std::function<void(const QString &)> handler) {
        m_onSave = std::move(handler);
    }

    /** 刷新"来源"与"已装组件"两块读数(装完、切页、设置变更后都调它)。 */
    void refresh() {
        refreshSource();
        refreshInstalled();
        traceState();
    }

    bool installing() const { return m_worker.joinable(); }
    /** 供验收钩子直接读(界面读数):卡片上三行的原文。 */
    QString sourceText() const { return m_sourceLabel->text(); }
    QString installedText() const { return m_installedLabel->text(); }
    QString statusText() const { return m_statusLabel->text(); }

private:
    // ── 来源:设置里存的(可编辑)与这次真正会用的,分开显示 ──
    QString storedSourceUrl() const { return m_urlEdit->text().trimmed(); }

    void saveSourceUrl() {
        const QString text = storedSourceUrl();
        // 空 = 不覆盖(回落环境变量/默认)。写空串而不是 remove():语义一致,
        // 而且后台线程读同一份文件时不会出现"键一会儿在、一会儿不在"。
        m_sourceSaved = text;
        if (m_onSave)
            m_onSave(text);
        refreshSource();
        traceState();
    }

    void refreshSource() {
        m_urlEdit->setText(m_sourceSaved);
        const QByteArray env = qgetenv(SXCL_JRE_INDEX_URL_ENV);
        const QByteArray setting = m_sourceSaved.toUtf8();
        char resolved[SXCL_JRE_URL_MAX];
        resolved[0] = '\0';
        const int rc = sxcl_jre_resolve_index_url(nullptr, setting.isEmpty() ? nullptr : setting.constData(),
                                                  env.isEmpty() ? nullptr : env.constData(), resolved,
                                                  sizeof(resolved));
        QString level;
        if (!env.isEmpty())
            level = QStringLiteral("环境变量 %1").arg(QString::fromUtf8(SXCL_JRE_INDEX_URL_ENV));
        else if (!setting.isEmpty())
            level = QStringLiteral("设置项");
        else
            level = QStringLiteral("编译期默认");

        if (rc != SXCL_JRE_OK) {
            m_sourceLabel->setText(QStringLiteral("生效来源：没有可用的 index.json 地址"));
            m_sourceLabel->setTextColor(QColor(0xff, 0x4d, 0x4f), QColor(0xff, 0x78, 0x75));
            m_sourceLabel->setToolTip(QStringLiteral(
                "三级来源都为空,且编译期默认地址是 <REPO> 占位 —— 请在上面填一个 index.json 地址。"));
            return;
        }
        const QString url = QString::fromUtf8(resolved);
        m_sourceLabel->setText(QStringLiteral("生效来源（%1）：%2").arg(level, url));
        m_sourceLabel->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xa0, 0xa0, 0xa0));
        m_sourceLabel->setToolTip(url);
    }

    void refreshInstalled() {
        const QString root = runtimeRoot();
        m_installed = root.isEmpty() ? QVector<HostedJre>() : scanHostedJres(root);
        if (m_installed.isEmpty()) {
            m_installedLabel->setText(QStringLiteral("已装组件：无"));
            m_installedLabel->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xa0, 0xa0, 0xa0));
            m_installedLabel->setToolTip(root.isEmpty() ? QString()
                                                        : QStringLiteral("运行时根目录：%1").arg(root));
            return;
        }
        QStringList parts;
        QStringList tips;
        for (const HostedJre &item : m_installed) {
            parts << QStringLiteral("%1 %2").arg(item.component,
                                                 item.version.isEmpty() ? QStringLiteral("(版本未知)")
                                                                        : item.version);
            tips << item.dir;
        }
        m_installedLabel->setText(QStringLiteral("已装 %1 个：%2").arg(m_installed.size())
                                      .arg(parts.join(QStringLiteral(" · "))));
        m_installedLabel->setTextColor(QColor(0x52, 0xc4, 0x1a), QColor(0x73, 0xd1, 0x3d));
        m_installedLabel->setToolTip(tips.join(QStringLiteral("\n")));
    }

    QString runtimeRoot() const {
        char root[SXCL_JAVA_RUNTIME_PATH_MAX];
        char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
        err[0] = '\0';
        if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
            SXCL_JAVA_RUNTIME_OK) {
            return QString();
        }
        return QDir::fromNativeSeparators(QString::fromUtf8(root));
    }

    // 验收用的机器可读读数(与版本页/Java 那两行同一个套路;只在 SXCL_UI_TRACE=1 时进 stderr)
    void traceState() {
        QStringList parts;
        for (const HostedJre &item : m_installed)
            parts << QStringLiteral("%1=%2").arg(item.component, item.version);
        uiTrace(QStringLiteral("jre-hosted | 来源=%1 设置值=%2 已装=%3 [%4] 根=%5")
                    .arg(m_sourceLabel->text(), m_sourceSaved.isEmpty() ? QStringLiteral("(空)")
                                                                        : m_sourceSaved)
                    .arg(m_installed.size())
                    .arg(parts.join(QStringLiteral(",")), runtimeRoot()));
    }

    // ── 安装:工作线程 + 核心库 sxcl_jre_install() ──
    void startDownload() {
        if (m_worker.joinable())
            return;
#if !defined(SXCL_UI_HAVE_QT_TRANSPORT)
        // 没有传输后端就如实说 —— 不做"假进度条"这种事(界面层不许假装成功)
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("下载内置 JRE 需要网络后端"),
                      QStringLiteral("本次构建没有链接 Qt Network 传输后端(sxcl_net_qt)，"
                                     "无法下载自托管 JRE。"),
                      window(), 6000);
#else
        const QString root = runtimeRoot();
        if (root.isEmpty()) {
            UiErrorContext ctx;
            ctx.page = QStringLiteral("设置页 / settings");
            ctx.action = QStringLiteral("下载内置 JRE（自托管）");
            ctx.reason = QStringLiteral("拿不到运行时根目录(sxcl_java_runtime_default_root 失败)");
            ctx.title = QStringLiteral("找不到运行时目录");
            pushUiError(window(), ctx, 8000);
            return;
        }
        saveSourceUrl(); // 保证这次跑的就是界面上看到的那个地址
        const QByteArray rootUtf8 = root.toUtf8();
        const int major = m_component->currentData().toInt();

        m_cancelRequested.store(false);
        m_downloadButton->setText(QStringLiteral("取消下载"));
        m_statusLabel->setText(QStringLiteral("正在取 index.json…"));
        m_statusLabel->setTextColor(QColor(0x00, 0x78, 0xd4), QColor(0x00, 0xbc, 0xf2));

        m_worker = std::thread([this, rootUtf8, major] {
            // 来源从**同一份设置文件**读(与设置页写的是同一个;见 settingsFilePath 的说明)。
            // 传 index_setting 而不是 index_url:显式值的优先级高于环境变量,会把运维注入的
            // 来源盖掉 —— 与"环境变量 > 设置项 > 默认"的三级口径不符(核心库按这个顺序解析)。
            QByteArray settingValue;
            {
                const QByteArray cfg = settingsFilePath().toUtf8();
                if (!cfg.isEmpty()) {
                    if (sxcl_settings *handle = sxcl_settings_open(cfg.constData())) {
                        const char *value = sxcl_settings_get(handle, SXCL_JRE_INDEX_URL_SETTING, nullptr);
                        if (value != nullptr)
                            settingValue = QByteArray(value);
                        sxcl_settings_free(handle);
                    }
                }
            }

            // 下载参数从设置读(环境变量优先:SXCL_DL_*),与官方 JRE / CLI 同一口径
            sxcl_settings_download dl;
            memset(&dl, 0, sizeof(dl));
            const QByteArray cfg = settingsFilePath().toUtf8();
            if (!cfg.isEmpty()) {
                if (sxcl_settings *handle = sxcl_settings_open(cfg.constData())) {
                    sxcl_settings_resolve_download(handle, &dl);
                    sxcl_settings_free(handle);
                }
            }
            char cacheFile[600];
            cacheFile[0] = '\0';
            if (dl.cache_dir[0] != '\0' && sxcl_fs_mkdirs(dl.cache_dir) == 0) {
                snprintf(cacheFile, sizeof(cacheFile), "%s/hashes.txt", dl.cache_dir);
            }

            sxcl_engine_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.workers = dl.workers;
            opts.rate_bps = dl.rate_bps;
            opts.max_conn_per_file = dl.max_conn_per_file;
            opts.cache_path = cacheFile[0] ? cacheFile : nullptr;

            sxcl_jre_request request;
            memset(&request, 0, sizeof(request));
            request.java_major = major;   // >0 且没给 component_id = 按主版本从清单里挑
            request.component_id = nullptr;
            request.target_root = rootUtf8.constData();
            request.index_setting = settingValue.isEmpty() ? nullptr : settingValue.constData();
            request.skip_if_installed = 1; // 同 version 已装好 -> 立刻成功返回(不联网、不算哈希)
            request.transport_factory = [](void *) -> sxcl_transport * {
                return sxcl_transport_qt_create();
            };
            request.engine_opts = &opts;
            request.on_progress = &HostedJreCard::progressTrampoline;
            request.is_cancelled = &HostedJreCard::cancelTrampoline;
            request.ud = this;

            sxcl_jre_result result;
            const int rc = sxcl_jre_install(&request, &result);

            const bool ok = (rc == SXCL_JRE_OK);
            const bool cancelled = (rc == SXCL_JRE_ERR_CANCELLED);
            const QString detail = ok ? QString::fromUtf8(result.version)
                                      : QString::fromUtf8(result.error);
            const QString home =
                ok ? QDir::fromNativeSeparators(QString::fromUtf8(result.java_home)) : QString();
            const QString stage = QString::fromUtf8(
                result.fail_stage_id != nullptr ? result.fail_stage_id : sxcl_jre_code_name(rc));
            const QString indexUrl = QString::fromUtf8(result.index_url);
            QMetaObject::invokeMethod(
                this,
                [this, ok, cancelled, detail, home, stage, indexUrl, rc] {
                    onInstallFinished(ok, cancelled, detail, home, stage, indexUrl, rc);
                },
                Qt::QueuedConnection);
        });
#endif
    }

    void cancelDownload() {
        m_cancelRequested.store(true);
        m_statusLabel->setText(QStringLiteral("正在取消…"));
    }

    bool cancelRequested() const { return m_cancelRequested.load(); }

    // 工作线程 -> 界面线程的进度投递(核心库的进度回调在**工作线程**里)
    void postProgress(const sxcl_jre_progress &p) {
        // 量不出来就写"未知",不编数字(与核心库 bytes_total=0 / speed_bps=0 / eta<0 的口径一致)
        const QString totalText =
            p.bytes_total > 0 ? humanBytes(p.bytes_total) : QStringLiteral("总量未知");
        const QString speedText =
            p.speed_bps > 0.0
                ? QStringLiteral("%1/s").arg(humanBytes(static_cast<int64_t>(p.speed_bps)))
                : QStringLiteral("速度未知");
        const QString line = QStringLiteral("%1 %2%  %3/%4  %5  %6")
                                 .arg(QString::fromUtf8(p.stage_name != nullptr ? p.stage_name : "?"))
                                 .arg(p.percent)
                                 .arg(humanBytes(p.bytes_done), totalText, speedText,
                                      humanEta(p.eta_seconds));
        const QString trace =
            QStringLiteral("progress stage=%1 percent=%2 bytes=%3/%4 speed=%5 eta=%6 files=%7/%8 "
                           "skipped=%9 failed=%10 component=%11 version=%12 current=%13 msg=%14")
                .arg(QString::fromUtf8(p.stage_id != nullptr ? p.stage_id : "?"))
                .arg(p.percent)
                .arg(p.bytes_done)
                .arg(p.bytes_total)
                .arg(static_cast<int64_t>(p.speed_bps))
                .arg(p.eta_seconds)
                .arg(p.files_done)
                .arg(p.files_total)
                .arg(p.files_skipped)
                .arg(p.files_failed)
                .arg(QString::fromUtf8(p.component != nullptr ? p.component : ""),
                     QString::fromUtf8(p.version != nullptr ? p.version : ""),
                     QString::fromUtf8(p.current != nullptr ? p.current : ""),
                     QString::fromUtf8(p.message));
        QMetaObject::invokeMethod(
            this,
            [this, line, trace] {
                m_statusLabel->setText(line);
                m_statusLabel->setTextColor(QColor(0x00, 0x78, 0xd4), QColor(0x00, 0xbc, 0xf2));
                // 核心库回调逐条留痕:验收直接拿这一段核对阶段/百分比/速度/剩余
                uiTrace(QStringLiteral("jre-hosted | ") + trace);
            },
            Qt::QueuedConnection);
    }

    // 核心库的进度回调(**工作线程**):只做投递,不碰控件
    static void progressTrampoline(void *ud, const sxcl_jre_progress *progress) {
        if (ud == nullptr || progress == nullptr)
            return;
        static_cast<HostedJreCard *>(ud)->postProgress(*progress);
    }
    static int cancelTrampoline(void *ud) {
        return (ud != nullptr && static_cast<HostedJreCard *>(ud)->cancelRequested()) ? 1 : 0;
    }

    // 界面线程:收尾(成功 / 取消 / 失败)
    void onInstallFinished(bool ok, bool cancelled, const QString &detail, const QString &home,
                           const QString &stage, const QString &indexUrl, int code) {
        m_downloadButton->setText(QStringLiteral("开始下载"));
        if (ok) {
            m_statusLabel->setText(QStringLiteral("✅ 已装好：%1").arg(home));
            m_statusLabel->setTextColor(QColor(0x52, 0xc4, 0x1a), QColor(0x73, 0xd1, 0x3d));
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("内置 JRE 安装完成"),
                          QStringLiteral("%1（版本 %2）").arg(home, detail), window(), 6000);
        } else if (cancelled) {
            m_statusLabel->setText(QStringLiteral("已取消下载"));
            m_statusLabel->setTextColor(QColor(0xfa, 0x8c, 0x16), QColor(0xff, 0xa9, 0x40));
        } else {
            m_statusLabel->setText(QStringLiteral("❌ %1").arg(detail.left(80)));
            m_statusLabel->setTextColor(QColor(0xff, 0x4d, 0x4f), QColor(0xff, 0x78, 0x75));
            // 统一错误出口:完整上下文进剪贴板 + 进运行日志(界面只显示原因的前 300 字)
            UiErrorContext ctx;
            ctx.page = QStringLiteral("设置页 / settings");
            ctx.action = QStringLiteral("下载内置 JRE（自托管）");
            ctx.reason = detail; // 核心库给的真实原因(状态码/校验/解包),原样进剪贴板
            ctx.detail = QStringLiteral("阶段=%1 返回码=%2(%3) index=%4 运行时根=%5")
                             .arg(stage, QString::number(code),
                                  QString::fromUtf8(sxcl_jre_code_name(code)), indexUrl,
                                  runtimeRoot());
            ctx.title = QStringLiteral("内置 JRE 安装失败");
            pushUiError(window(), ctx, 10000);
        }
        refresh();
        uiTrace(QStringLiteral("jre-hosted | 结果 ok=%1 cancelled=%2 code=%3 阶段=%4 版本=%5 家=%6")
                    .arg(ok ? 1 : 0)
                    .arg(cancelled ? 1 : 0)
                    .arg(code)
                    .arg(stage, detail, home));
    }

    QLineEdit *m_urlEdit = nullptr;
    ComboBox *m_component = nullptr;
    PushButton *m_downloadButton = nullptr;
    CaptionLabel *m_statusLabel = nullptr;
    CaptionLabel *m_sourceLabel = nullptr;
    CaptionLabel *m_installedLabel = nullptr;
    QVector<HostedJre> m_installed;
    QString m_sourceSaved;
    std::function<void(const QString &)> m_onSave;
    std::thread m_worker;
    std::atomic<bool> m_cancelRequested{false};
};

// ─────────────── 内存范围(settings_page.py:288-307 的整数运算,逐行照抄)───────────────

struct MemoryRange {
    int total = 0;
    int minimum = 0;
    int maximum = 0;
    int defaultMb = 0;
};

MemoryRange computeMemoryRange() {
    MemoryRange range;
    const int total = static_cast<int>(sxcl_sysinfo_total_mb()); // psutil.virtual_memory().total
    range.total = std::max(total, 0);
    if (range.total <= 0) {
        // 核心库查不到物理内存(容器/受限环境):范围退回 sxcl/sysinfo.h 的推荐区间,
        // 默认值用 sxcl_sysinfo_recommended_heap_mb()(同头文件:"拿不到内存时给 2048")。
        range.minimum = static_cast<int>(SXCL_SYSINFO_HEAP_MIN_MB);
        range.maximum = static_cast<int>(SXCL_SYSINFO_HEAP_MAX_MB);
        range.defaultMb = static_cast<int>(sxcl_sysinfo_recommended_heap_mb());
        return range;
    }

    int minimum = std::min(2048, static_cast<int>(range.total * 0.2)); // :291
    minimum = ((minimum + 2047) / 2048) * 2048;                        // :292
    minimum = std::max(minimum, 1024);                                 // :293

    int maximum = static_cast<int>(range.total * 0.75);                // :295
    maximum = (maximum / 2048) * 2048;                                 // :296
    maximum = std::max(maximum, 4096);                                 // :297

    int defaultValue = static_cast<int>(range.total * 0.5);            // :299
    defaultValue = (defaultValue / 2048) * 2048;                       // :300
    defaultValue = std::max(minimum, std::min(defaultValue, maximum)); // :301

    range.minimum = minimum;
    range.maximum = maximum;
    range.defaultMb = defaultValue;
    return range;
}

// ─────────────── 卡片:AccountStatusCard(**新增**;Python 版没有账户功能) ───────────────
//
// 与 qf 的 SettingCard 同构(定高 70/图标 16x16/标题 14px/说明 11px,见文件头),
// 右侧多一枚状态标签:样式用既有的 FluentTheme::chipQss()(对应 Python styles.py 的
// chip_qss,docs/05 §5),颜色只取令牌 —— 不发明任何色值。
// 展示内容就是交付要求的三件事:未登录 / 已登录 + 玩家名 / Java 版是否有权益。
class AccountStatusCard : public SettingCard {
public:
    AccountStatusCard(const QIcon &icon, const QString &title, QWidget *parent)
        : SettingCard(icon, title, QString(), parent) {
        m_chip = new BodyLabel(QString(), this);
        m_chip->setAlignment(Qt::AlignCenter);
        hBox()->addStretch(1);
        hBox()->addWidget(m_chip);
        hBox()->addSpacing(20);
    }

    void updateState(const AccountSnapshot &snapshot) {
        if (!snapshot.error.isEmpty()) {
            setChip(QStringLiteral("读取失败"), QStringLiteral("danger"));
            setContent(QStringLiteral("读取登录凭据失败：%1").arg(snapshot.error));
            return;
        }
        if (!snapshot.loggedIn) {
            setChip(QStringLiteral("未登录"), QStringLiteral("danger"));
            setContent(QStringLiteral("还没有登录过。点「登录」用设备码方式登录 Microsoft 账户。"));
            return;
        }

        // 已登录:玩家名 + Java 版权益分开说(核心库把这两件事分成两条接口,
        // 详见 docs/09 §1 的 6a/6b;"没查" 与 "没有" 不许混为一谈)。
        const bool entitled = snapshot.javaEntitled();
        QString chipText;
        QString chipToken;
        if (!snapshot.hasMcToken || snapshot.mcExpired) {
            chipText = QStringLiteral("凭据已过期");
            chipToken = QStringLiteral("warning");
        } else if (entitled) {
            chipText = QStringLiteral("已登录");
            chipToken = QStringLiteral("success");
        } else {
            chipText = QStringLiteral("已登录");
            chipToken = QStringLiteral("warning");
        }
        setChip(chipText, chipToken);

        QString player;
        if (snapshot.playerName.isEmpty()) {
            player = QStringLiteral("未取到 Java 版档案（这个账号可能没有 Java 版）");
        } else {
            player = QStringLiteral("%1（uuid %2）").arg(snapshot.playerName, snapshot.uuid);
        }
        QString entitlement;
        if (!snapshot.entitlementChecked)
            entitlement = QStringLiteral("权益：未查过");
        else if (entitled)
            entitlement = QStringLiteral("权益：拥有 Java 版（mcstore 条目 %1）")
                              .arg(snapshot.entitlementCount);
        else
            entitlement = QStringLiteral("权益：商店里没有条目");
        const QString expiry =
            snapshot.mcExpiresAt > 0
                ? QStringLiteral("；MC 令牌到 %1")
                      .arg(QDateTime::fromSecsSinceEpoch(snapshot.mcExpiresAt)
                               .toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                : QString();
        const QString account = snapshot.accountName.isEmpty()
                                    ? QString()
                                    : QStringLiteral("；账户 %1").arg(snapshot.accountName);
        setContent(QStringLiteral("玩家名 %1；%2%3%4").arg(player, entitlement, expiry, account));
    }

private:
    void setChip(const QString &text, const QString &colorToken) {
        m_chip->setText(text);
        m_chip->setStyleSheet(FluentTheme::instance().chipQss(colorToken));
    }

    BodyLabel *m_chip = nullptr;
};

// ──────────────────── 卡片:CurseForge API Key(新增;docs/22 §14)────────────────────
//
// 这一栏**不是装饰**:CurseForge 的官方 API 不带 x-api-key 就不回数据。没填的含义是
// "CurseForge 这一源不发请求"(模组页会如实说缺什么,绝不会改用 Modrinth 假装是 CF 的结果)。
// key 只存在本地设置文件里,请求时放进 **请求头**,绝不进 URL(URL 会进日志/错误消息)。
class CurseForgeKeyCard : public SettingCard {
public:
    explicit CurseForgeKeyCard(QWidget *parent = nullptr)
        : SettingCard(FluentIcon::qicon(FluentIcon::CERTIFICATE),
                      QStringLiteral("CurseForge API Key"),
                      QStringLiteral("官方 API 必须带 x-api-key；只存本地设置，"
                                     "请求时放进请求头，绝不写进 URL"), parent) {
        setFixedHeight(100);

        m_edit = new QLineEdit(this);
        m_edit->setMinimumWidth(300);
        m_edit->setFixedHeight(34);
        m_edit->setEchoMode(QLineEdit::Password);   // 密钥不回显
        m_edit->setPlaceholderText(QStringLiteral("粘贴 key（留空/清除 = 不配）"));
        m_edit->setObjectName(QStringLiteral("curseForgeKeyEdit"));
        m_save = new PushButton(QStringLiteral("保存"), this);
        m_save->setFixedHeight(34);
        m_save->setObjectName(QStringLiteral("curseForgeKeySave"));
        m_clear = new PushButton(QStringLiteral("清除"), this);
        m_clear->setFixedHeight(34);
        m_state = new CaptionLabel(QString(), this);

        auto *right = new QVBoxLayout();
        right->setSpacing(4);
        right->setContentsMargins(0, 0, 0, 0);
        auto *top = new QHBoxLayout();
        top->setSpacing(8);
        top->addWidget(m_edit);
        top->addWidget(m_save);
        top->addWidget(m_clear);
        right->addLayout(top);
        right->addWidget(m_state, 0, Qt::AlignRight);
        hBox()->addLayout(right, 0);
        hBox()->addSpacing(16);

        connect(m_save, &QAbstractButton::clicked, this, [this] { commit(m_edit->text()); });
        connect(m_clear, &QAbstractButton::clicked, this, [this] {
            m_edit->clear();
            commit(QString());
        });
        connect(m_edit, &QLineEdit::returnPressed, this, [this] { commit(m_edit->text()); });
        refresh();
    }

    /** 初始值由页面从设置里读好传进来(卡片不自己开第二份设置句柄去读)。 */
    void setStored(const QString &value) {
        m_value = value.trimmed();
        refresh();
    }
    /** 落盘出口由页面给(设置页的 ConfigStore 才是设置的唯一写入口)。 */
    void setSaveHandler(std::function<void(const QString &)> handler) {
        m_onSave = std::move(handler);
    }
    /** 供验收钩子直接读的界面读数。 */
    QString stateText() const { return m_state->text(); }

private:
    void commit(const QString &value) {
        m_value = value.trimmed();
        if (m_onSave) {
            m_onSave(m_value);
        }
        m_edit->clear();
        refresh();
    }
    void refresh() {
        const bool has = !m_value.isEmpty();
        m_clear->setEnabled(has);
        m_state->setText(has ? QStringLiteral("已配置（%1…，共 %2 个字符）")
                                   .arg(m_value.left(4))
                                   .arg(m_value.size())
                             : QStringLiteral("未配置 —— 模组页的 CurseForge 源不会发请求"));
    }

    QLineEdit *m_edit = nullptr;
    PushButton *m_save = nullptr;
    PushButton *m_clear = nullptr;
    CaptionLabel *m_state = nullptr;
    QString m_value;
    std::function<void(const QString &)> m_onSave;
};

// ─────────────────────────── 页面本体 ───────────────────────────

class SettingsPage : public ScrollArea {
public:
    explicit SettingsPage(QWidget *parent = nullptr);

private:
    void buildContent(); // settings_page.py:213-426 _build_content
    void bindEvents();   // settings_page.py:428-435 _bind_events

    QString gameDirectory() const;               // :330/:474 游戏目录(存储 → 核心库平台默认)
    int speedLimitKbps() const;                  // :381 Download.speedLimitKbps(核心库存字节/秒)
    void refreshPageBackground();
    void onThemeChanged();                        // :511-514
    void onLanguageChanged(const QString &value); // :494-509
    // 语言热切换:把绑了语言键的文案重新取一遍(不重建窗口、不重启)
    void applyLanguageTexts();
    void onDownloadSourceChanged(const QString &value); // :525-528
    void pickGameDirectory();                     // :441-450
    void resetSettings();                         // :452-492
    void applySpeedLimit(int kbps);                // :516-523

    // ---- 账户(**新增**;Python 版没有账户功能,这一组从零加) ----
    void refreshAccountCard();                                        // 读令牌文件 → 刷状态卡
    void onLoginClicked();                                            // 开设备码登录对话框
    void onRefreshClicked();                                          // 免密续期(后台线程)
    void onLogoutClicked();                                           // 确认后删凭据(后台线程)
    void runAccountAction(AccountTask::Operation operation, const QString &title);

    ConfigStore m_store;
    MemoryRange m_memoryRange;

    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;

    ComboBoxSettingCard *m_refreshIntervalCard = nullptr;
    SwitchSettingCard *m_updateCard = nullptr;
    SwitchSettingCard *m_closeAfterCard = nullptr; // 「结束后关闭」(电脑端;安卓不建)
    ComboBoxSettingCard *m_themeCard = nullptr;
    ColorSettingCard *m_themeColorCard = nullptr;
    ComboBoxSettingCard *m_languageCard = nullptr;
    ComboBoxSettingCard *m_sourceCard = nullptr;

    SwitchSettingCard *m_isolationCard = nullptr;
    JavaSettingCard *m_javaCard = nullptr;
    HostedJreCard *m_hostedJreCard = nullptr; // 自托管 JRE(新增;官方 JRE 之外的**第二条来源**)
    MemorySettingCard *m_memoryCard = nullptr;
    ComboBoxSettingCard *m_windowCard = nullptr;
    PushSettingCard *m_gameDirCard = nullptr;

    SpinSettingCard *m_connCard = nullptr;
    SpinSettingCard *m_limitCard = nullptr;
    SwitchSettingCard *m_verifyCard = nullptr;
    CurseForgeKeyCard *m_cfKeyCard = nullptr; // 新增:CurseForge 的 key(没它 CF 那一源不查)

    SwitchSettingCard *m_debugCard = nullptr;
    SwitchSettingCard *m_downloadEngineCard = nullptr;
    PushSettingCard *m_resetCard = nullptr;

    SettingCard *m_aboutCard = nullptr;
    HyperlinkCard *m_websiteCard = nullptr;
    SettingCardGroup *m_aboutGroup = nullptr; // 语言热切换要改它的组标题(绑 page.settings.about_group)

    // 账户(新增):状态卡 + 登录/刷新/注销三张动作卡;同一时刻只跑一个后台任务。
    AccountStatusCard *m_accountCard = nullptr;
    PushSettingCard *m_loginCard = nullptr;
    PushSettingCard *m_refreshCard = nullptr;
    PushSettingCard *m_logoutCard = nullptr;
    AccountTask *m_accountTask = nullptr;
};

SettingsPage::SettingsPage(QWidget *parent) : ScrollArea(parent) {
    // ---- BasePage(src/app/common/base_page.py:41-60)----
    setObjectName(QStringLiteral("SettingsPage"));        // :42
    setWidgetResizable(true);                             // :43
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // :44
    // 竖直方向也要关掉**原生**滚动条:qf 的 SmoothScrollBar 构造里会替父级设
    // AlwaysOff(components/widgets/scroll_bar.py:SmoothScrollBar.__init__ → 经 delegate 换掉的
    // setVerticalScrollBarPolicy 落到 super(QAbstractScrollArea).setVerticalScrollBarPolicy
    // (Qt.ScrollBarAlwaysOff)),所以 Python 的页面视口是**整宽**的。
    // libqf 的 SmoothScrollDelegate 只在"区域原本没有自绘条"时才设策略
    // (fluent_scroll.cpp:424-433),而 ScrollArea 家族在 initArea() 里已经塞好自绘条 →
    // 策略停在默认的 AsNeeded,竖直方向会多占 12px(实测内容区 1051 → 1039、
    // 卡片右缘 1069 → 1057)。这里按 qf 的行为钉死(见交付报告:libqf 侧也该修)。
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Python 的 BasePage 继承 qf ScrollArea,没改 frameShape → 走 QFrame 默认的 StyledPanel(1px
    // 边框);libqf 的 ScrollArea::initArea() 也不设它(见 fluent_scroll.cpp:204-221 的取证),
    // 这里显式写出来,免得将来被改动时页面整体偏移 1px。
    setFrameShape(QFrame::StyledPanel);
    setLineWidth(1);

    m_view = new QWidget(this);                                         // :46
    m_view->setStyleSheet(QStringLiteral("background: transparent;"));  // :47
    setWidget(m_view);                                                  // :48

    m_vBox = new QVBoxLayout(m_view);             // :50
    m_vBox->setContentsMargins(28, 24, 28, 24);   // :51
    m_vBox->setSpacing(16);                       // :52
    m_vBox->setAlignment(Qt::AlignTop);           // :53

    // 页面标题:绑核心库语言表(键与 Python .lang 里 page.settings.title 逐字一致 ——
    // zh-cn 那份的值就是"设置",所以中文下的显示与 1:1 规格完全一致)
    // 语言表:按设置里的 ui.language 初始化(幂等 —— main.cpp 启动时已经设过一次,
    // 这里再设一次是为了"直接建设置页"的入口(UI 冒烟测试 / 后续单页预览)也能拿到正确文案)。
    // 找不到 .lang 不算失败:核心库有内置中英两份(见 include/sxcl/lang.h)。
    {
        char lerr[SXCL_LANG_ERR_MAX];
        lerr[0] = '\0';
        const QByteArray code = m_store.text(kKeyLanguage, QStringLiteral("zh-CN")).toUtf8();
        (void)sxcl_lang_set_default(code.constData(), nullptr, lerr, sizeof(lerr));
    }

    m_title = new TitleLabel(trText("page.settings.title", kPageTitle), m_view); // :55
    m_subtitle = new SubtitleLabel(QString(), m_view);               // :56
    m_subtitle->hide();                                             // :209 subtitleLabel.hide()
    m_vBox->addWidget(m_title);                                     // :59
    m_vBox->addWidget(m_subtitle);                                  // :60

    // 页面底色 = 令牌 bg(#202020 / 浅色 #f3f3f3)。依据 docs/05-UI-1to1规格.md §10.2
    // (内容区设计值 = 窗口底)与 §11.2(抓图时页面自己画底色,否则半透明内容栈会透出桌面)。
    refreshPageBackground();
    connect(&FluentTheme::instance(), &FluentTheme::changed, this,
            [this] { refreshPageBackground(); });
    // Android:用户去"所有文件访问权限"授权后回到本应用,要**立刻**重扫并刷新
    // (父任务明确要求,不能让用户重启)。应用状态变回 Active 就是那个时刻。
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive)
                    return;
                static bool announced = false;
                const bool granted = androidHasAllFilesAccess();
                if (granted && !announced) {
                    announced = true;
                    // 没配过游戏目录时把刚扫到"确实有版本"的目录落进设置,
                    // 这样卡片显示的值与各页面真正使用的目录一致。
                    if (m_store.text(kKeyGameDir).isEmpty()) {
                        const QByteArray files = qEnvironmentVariable("SXCL_ANDROID_FILES").toUtf8();
                        sxcl_game_folders folders;
                        char derr[SXCL_PATHS_ERROR_MAX];
                        derr[0] = '\0';
                        (void)sxcl_paths_detect_android(files.isEmpty() ? nullptr : files.constData(),
                                                        nullptr, &folders, nullptr, derr, sizeof(derr));
                        const sxcl_game_folder *best = sxcl_paths_best(&folders);
                        if (best != nullptr && best->versions > 0) {
                            m_store.set(kKeyGameDir,
                                        QDir::fromNativeSeparators(QString::fromUtf8(best->path)));
                        }
                    }
                    InfoBar::push(InfoBar::Type::Success, QStringLiteral("已获得共享存储权限"),
                                  QStringLiteral("现在可以扫描共享存储上的 .minecraft 了"), window(),
                                  4000);
                }
                if (m_gameDirCard)
                    m_gameDirCard->setContent(gameDirectory());
                if (m_javaCard)
                    m_javaCard->refresh();
                if (m_hostedJreCard)
                    m_hostedJreCard->refresh();
            });

    buildContent();
    bindEvents();
}

void SettingsPage::refreshPageBackground() {
    setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                      .arg(FluentTheme::instance().tokens().bg.name()));
}

// settings_page.py:326-332 / :474 —— 游戏目录显示值
QString SettingsPage::gameDirectory() const {
    const QString stored = m_store.text(kKeyGameDir);
    if (!stored.isEmpty())
        return stored;
    char out[SXCL_PATHS_PATH_MAX] = {0};
    char err[SXCL_PATHS_ERROR_MAX] = {0};
    if (sxcl_paths_default_game_dir(out, sizeof(out), err, sizeof(err)) == SXCL_PATHS_OK)
        return QString::fromUtf8(out);
    return QDir::homePath() + QStringLiteral("/.minecraft"); // 平台默认都拼不出来时的最后兜底
}

// settings_page.py:516-523 —— 核心库把限速存成"字节/秒"(download.rate),界面是 KB/s
int SettingsPage::speedLimitKbps() const {
    const char *text = sxcl_settings_download_rate_text(m_store.handle());
    const double bytesPerSecond = sxcl_limiter_parse_rate(text ? text : "0");
    if (bytesPerSecond <= 0)
        return 0;
    return static_cast<int>(std::lround(bytesPerSecond / 1024.0));
}

// 取证:把"自动检测到的游戏目录"逐条打到 stderr(安卓真机看 logcat)。
// 设置页的"游戏目录"卡就是拿这份数据让用户选的 —— 截图看不出每条的判据与优先级,
// 所以留一行机器可读的输出给验收脚本(与版本页/Java 那两行同一个套路)。
void logGameDirDetection() {
#if defined(__ANDROID__)
    const QByteArray files = qEnvironmentVariable("SXCL_ANDROID_FILES").toUtf8();
    sxcl_game_folders folders;
    char err[SXCL_PATHS_ERROR_MAX];
    err[0] = '\0';
    (void)sxcl_paths_detect_android(files.isEmpty() ? nullptr : files.constData(), nullptr, &folders,
                                    nullptr, err, sizeof(err));
    std::fprintf(stderr, "[sxcl-ui] gamedir-detect: 平台=android 候选=%zu 原因=%s\n",
                 folders.count, err[0] ? err : "-");
#else
    sxcl_paths_env_store envStore;
    sxcl_paths_env_capture(&envStore);
    sxcl_game_folders folders;
    char err[SXCL_PATHS_ERROR_MAX];
    err[0] = '\0';
    (void)sxcl_paths_detect_ex(&envStore.env, sxcl_paths_current_os(), &folders, nullptr, err,
                               sizeof(err));
    std::fprintf(stderr, "[sxcl-ui] gamedir-detect: 平台=%s 候选=%zu 原因=%s\n",
                 sxcl_paths_os_name(sxcl_paths_current_os()), folders.count,
                 err[0] ? err : "-");
#endif
    for (size_t i = 0; i < folders.count; ++i) {
        const sxcl_game_folder &f = folders.items[i];
        char marksText[SXCL_PATHS_DESC_MAX];
        marksText[0] = '\0';
        (void)sxcl_paths_marks_text(sxcl_paths_marks(f.path), f.versions, marksText,
                                    sizeof(marksText));
        std::fprintf(stderr,
                     "[sxcl-ui] gamedir-probe: #%zu %s owner=%s label=%s versions=%d "
                     "priority=%d marks=%d(%s)\n",
                     i, f.path, f.owner[0] ? f.owner : "-", f.label, f.versions, f.priority,
                     sxcl_paths_marks(f.path), marksText);
    }
}

void SettingsPage::buildContent() {
    logGameDirDetection();
    // ── 通用设置(settings_page.py:215-270)──
    auto *generalGroup = new SettingCardGroup(QString::fromUtf8(kGroupGeneral), m_view);

    m_updateCard = new SwitchSettingCard( // :217-222
        FluentIcon::qicon(FluentIcon::UPDATE), QStringLiteral("启动时自动检查更新"), QString(),
        m_store.flag(kKeyAutoCheckUpdate, true), generalGroup);

    const QStringList themeTexts = textList(kThemeTexts, 3);   // :228 texts=THEME_LABELS
    const QStringList themeValues = stringList(kThemeValues, 3);
    m_themeCard = new ComboBoxSettingCard( // :223-230
        FluentIcon::qicon(FluentIcon::BRUSH), QStringLiteral("主题模式"),
        QStringLiteral("浅色 / 深色 / 跟随系统"), themeTexts, themeValues,
        valueIndex(themeValues, currentThemeMode(m_store), 2), generalGroup);

    m_themeColorCard = new ColorSettingCard( // :231-237
        FluentIcon::qicon(FluentIcon::PALETTE), QStringLiteral("主题色"),
        QStringLiteral("强调色：按钮、选中态、进度条都会跟着变"),
        FluentTheme::instance().accent(), generalGroup);

    const QStringList languageTexts = textList(kLanguageTexts, 2); // :242
    const QStringList languageValues = stringList(kLanguageValues, 2);
    m_languageCard = new ComboBoxSettingCard( // :238-244
        FluentIcon::qicon(FluentIcon::LANGUAGE), trText("page.settings.language", "语言"), QString(),
        languageTexts, languageValues,
        valueIndex(languageValues, m_store.text(kKeyLanguage, QStringLiteral("zh-CN")), 0),
        generalGroup);

    QStringList sourceTexts = textList(kSourceTexts, 3); // :252
    QStringList sourceValues = stringList(kSourceValues, 3);
    // 这里原来有一个验收夹具:设 SXCL_UI_TALLMENU=N 就给「版本下载源」追加 N 个"测试源"。
    // 用户明确要求删掉("把测试源删掉")—— 一个用户看得见的设置项里混进测试条目,
    // 不管开关默认关不关,都是不该留的东西。弹层内部滚动的验收另有别的通路(超高弹层用例)。
    m_sourceCard = new ComboBoxSettingCard( // :246-253
        FluentIcon::qicon(FluentIcon::DOWNLOAD), QStringLiteral("版本下载源"),
        QStringLiteral("选择版本清单与资源文件的下载源"), sourceTexts, sourceValues,
        // 默认 **BMCLAPI**(用户指定):Python 版"重置设置"落地的也是 bmclapi;
        // 运行期真读这个键的是 ui_paths.cpp 的 uiDownloadSource()(版本页清单与下载都按它排序)。
        // 用 uiDownloadSource()(清洗过的合法值)而不是原始存储值:历史遗留的 testsrcN
        // 会被纠成 bmclapi,下拉显示的也就与实际行为一致了。
        valueIndex(sourceValues, uiDownloadSource(), 0),
        generalGroup);

    const QStringList refreshTexts = textList(kRefreshTexts, kRefreshCount); // :261
    const QStringList refreshValues = numberList(kRefreshValues, kRefreshCount);
    m_refreshIntervalCard = new ComboBoxSettingCard( // :255-263
        FluentIcon::qicon(FluentIcon::UPDATE), QStringLiteral("版本列表刷新频率"),
        QStringLiteral("版本清单自动刷新的间隔时间"), refreshTexts, refreshValues,
        valueIndex(refreshValues, QString::number(m_store.number(kKeyRefreshInterval, 120)), 2),
        generalGroup);

    // 「结束后关闭」:**本版新增**(Python 版没有这个设置项),所以排在照抄顺序之后。
    // 电脑端语义,安卓上不出现在界面上(安卓是系统回收进程,没有"退出启动器"这一说)。
#if !defined(Q_OS_ANDROID)
    m_closeAfterCard = new SwitchSettingCard(
        FluentIcon::qicon(FluentIcon::POWER_BUTTON), QStringLiteral("结束后关闭"),
        QStringLiteral("安装/下载成功之后自动退出启动器(失败或取消时不退,原因要看得见)"),
        m_store.flag(kKeyCloseAfterInstall, false), generalGroup);
    connect(m_closeAfterCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) { m_store.set(kKeyCloseAfterInstall, checked); });
#endif

    // 入组顺序照抄 Python(:264 先入组 refresh_interval,再 update/theme/theme_color/language/source)
    generalGroup->addSettingCard(m_refreshIntervalCard);
    generalGroup->addSettingCard(m_updateCard);
    generalGroup->addSettingCard(m_themeCard);
    generalGroup->addSettingCard(m_themeColorCard);
    generalGroup->addSettingCard(m_languageCard);
    generalGroup->addSettingCard(m_sourceCard);
#if !defined(Q_OS_ANDROID)
    generalGroup->addSettingCard(m_closeAfterCard); // 新增项,排在照抄项之后
#endif

    // ── 游戏设置(settings_page.py:272-336)──
    auto *gameGroup = new SettingCardGroup(QString::fromUtf8(kGroupGame), m_view);

    m_isolationCard = new SwitchSettingCard( // :276-282
        FluentIcon::qicon(FluentIcon::FOLDER), trText("page.settings.version_isolation", "版本隔离"),
        // 2026-09-22 晚:A2 真落地了 —— 说清"独立的是什么、共用的又是什么",
        QStringLiteral("每个版本各用一套 mods / saves / config / options.txt"
                       "（assets 与 libraries 仍然共用；默认关）"),
        m_store.flag(kKeyVersionIsolation, false), gameGroup);
    gameGroup->addSettingCard(m_isolationCard); // :283

    m_javaCard = new JavaSettingCard(gameGroup); // :285
    gameGroup->addSettingCard(m_javaCard);       // :286

    // 自托管 JRE(**新增**;Python 版没有这条来源,见 docs/19 §2.1)。
    // 紧跟在"Java 运行路径"之后:两条来源是同一件事的两个答案(官方桌面构建 / 我们自己的安卓包)。
    m_hostedJreCard = new HostedJreCard(gameGroup);
    m_hostedJreCard->setSaveHandler([this](const QString &value) {
        // 空 = 不覆盖(回落环境变量/编译期默认):写空串而不是 remove(),
        // 语义一样,但后台线程读同一份文件时不会看到"键一会儿在、一会儿不在"。
        m_store.set(SXCL_JRE_INDEX_URL_SETTING, value);
    });
    m_hostedJreCard->setStoredSource(m_store.text(SXCL_JRE_INDEX_URL_SETTING));
    gameGroup->addSettingCard(m_hostedJreCard);

    m_memoryRange = computeMemoryRange(); // :289-301

    int memory = m_store.number(kKeyMaxMemory, 4096); // cfg.maxMemoryMb 默认值(launcher_config.py:134)
    if (memory < m_memoryRange.minimum || memory > m_memoryRange.maximum) {
        memory = m_memoryRange.defaultMb; // :305-307 超出范围 → 回到默认值并落盘
        m_store.set(kKeyMaxMemory, memory);
    }

    // :313 f"系统总内存 {total_memory_mb}MB，步进 2GB"
    const QString memoryContent = QStringLiteral("系统总内存 %1MB，步进 2GB").arg(m_memoryRange.total);
    m_memoryCard = new MemorySettingCard( // :309-318
        FluentIcon::qicon(FluentIcon::SPEED_OFF), QStringLiteral("最大内存分配"), memoryContent,
        m_memoryRange.minimum, m_memoryRange.maximum, 2048, memory, gameGroup);

    const QStringList windowValues = stringList(kWindowValues, 5); // :323
    m_windowCard = new ComboBoxSettingCard( // :319-325
        FluentIcon::qicon(FluentIcon::FULL_SCREEN), QStringLiteral("游戏窗口大小"), QString(),
        windowValues, windowValues,
        valueIndex(windowValues, m_store.text(kKeyWindowSize, QStringLiteral("1280x720")), 1),
        gameGroup);

    m_gameDirCard = new PushSettingCard( // :326-332
        QStringLiteral("选择目录"), FluentIcon::qicon(FluentIcon::FOLDER),
        trText("page.settings.game_dir", "游戏目录"), gameDirectory(), gameGroup);

    gameGroup->addSettingCard(m_memoryCard);  // :334
    gameGroup->addSettingCard(m_windowCard);  // :335
    gameGroup->addSettingCard(m_gameDirCard); // :336

    // ── 高级设置(settings_page.py:338-362)──
    auto *advancedGroup = new SettingCardGroup(QString::fromUtf8(kGroupAdvanced), m_view);
    m_debugCard = new SwitchSettingCard( // :340-345
        FluentIcon::qicon(FluentIcon::CODE), QStringLiteral("调试模式"), QString(),
        m_store.flag(kKeyDebugMode, false), advancedGroup);
    m_downloadEngineCard = new SwitchSettingCard( // :346-352
        FluentIcon::qicon(FluentIcon::SPEED_OFF), QStringLiteral("多线程下载引擎-测试"),
        QStringLiteral("启用后使用多线程分片下载，可大幅提升下载速度；如果遇到安装问题可关闭此开关"),
        m_store.flag(kKeyDownloadEngine, true), advancedGroup);
    m_resetCard = new PushSettingCard( // :353-359
        QStringLiteral("重置所有设置"), FluentIcon::qicon(FluentIcon::CANCEL),
        QStringLiteral("重置配置"), QStringLiteral("将所有设置恢复为默认值"), advancedGroup);
    advancedGroup->addSettingCard(m_debugCard);          // :360
    advancedGroup->addSettingCard(m_downloadEngineCard); // :361
    advancedGroup->addSettingCard(m_resetCard);          // :362

    // ── 下载设置(settings_page.py:364-395)──
    auto *downloadGroup = new SettingCardGroup(QString::fromUtf8(kGroupDownload), m_view);
    m_connCard = new SpinSettingCard( // :367-374
        FluentIcon::qicon(FluentIcon::SPEED_HIGH), QStringLiteral("并发连接数"),
        QStringLiteral("同时进行的下载连接数；带宽跑不满时可以调高（4 - 128）"), 4, 128, 4,
        QString(),
        // 核心库便捷读取(sxcl_settings_download_max_conn)的兜底是 1(= 不分片),而 Python 设置页
        // 默认 32、范围 4-128 —— 范围下限都够不着 1,所以这里按 Python 的 UI 口径取 32。
        // 冲突写进交付报告,请主代理在核心库侧统一默认值。
        m_store.number(kKeyMaxConn, 32),
        [this](int value) { m_store.set(kKeyMaxConn, value); }, downloadGroup);
    m_limitCard = new SpinSettingCard( // :375-383
        FluentIcon::qicon(FluentIcon::SPEED_OFF), QStringLiteral("下载限速"),
        QStringLiteral("0 = 不限速；单位 KB/s（1024KB/s = 1MB/s），对所有下载连接全局生效"),
        0, 1048576, 256, QStringLiteral(" KB/s"), speedLimitKbps(),
        [this](int value) { applySpeedLimit(value); }, downloadGroup);
    m_verifyCard = new SwitchSettingCard( // :384-391
        // Python: _verify_icon = getattr(FIF, "CERTIFICATE", None) or getattr(FIF, "ACCEPT", FIF.INFO)
        // (qf 的 FluentIcon 有 CERTIFICATE;libqf 同样有,所以取 CERTIFICATE)
        FluentIcon::qicon(FluentIcon::CERTIFICATE), QStringLiteral("校验文件完整性（SHA1）"),
        QStringLiteral("强制校验 Mojang 提供哈希的全部资源；校验失败会自动换源重下"),
        m_store.flag(kKeyVerifySha1, true), downloadGroup);
    m_cfKeyCard = new CurseForgeKeyCard(downloadGroup); // 新增(模组页的 CurseForge 源靠它)
    m_cfKeyCard->setStored(m_store.text(kKeyCfApiKey));
    m_cfKeyCard->setSaveHandler([this](const QString &key) { m_store.set(kKeyCfApiKey, key); });
    downloadGroup->addSettingCard(m_connCard);   // :393
    downloadGroup->addSettingCard(m_limitCard);  // :394
    downloadGroup->addSettingCard(m_verifyCard); // :395
    downloadGroup->addSettingCard(m_cfKeyCard);  // 新增项,排在照抄项之后

    // ── 账户(**新增**;Python 版没有这一组)──
    //
    // 位置:按交付要求放在「关于」之前。控件全部复用既有 SettingCard 家族
    // (状态卡 = SettingCard + chip_qss;动作卡 = PushSettingCard),
    // 因此外观与其它分组逐像素同源,不存在"自创一套账户界面"。
    auto *accountGroup = new SettingCardGroup(QString::fromUtf8(kGroupAccount), m_view);
    m_accountCard = new AccountStatusCard(FluentIcon::qicon(FluentIcon::PEOPLE),
                                          QStringLiteral("登录状态"), accountGroup);
    m_loginCard = new PushSettingCard( // 设备码流为主(核心库 --device-code 的那条路)
        QStringLiteral("登录"), FluentIcon::qicon(FluentIcon::ACCEPT),
        QStringLiteral("登录 Microsoft 账户"),
        QStringLiteral("设备码登录：用浏览器输入 8 位代码即可，不需要本地监听端口"),
        accountGroup);
    m_refreshCard = new PushSettingCard( // 免密续期:refresh token → 重跑后半条链
        QStringLiteral("刷新"), FluentIcon::qicon(FluentIcon::SYNC),
        QStringLiteral("刷新登录状态"),
        QStringLiteral("用已保存的 refresh token 免密续期并重查权益，不用再输一次设备码"),
        accountGroup);
    m_logoutCard = new PushSettingCard( // 删掉本机加密保存的凭据(幂等)
        QStringLiteral("注销"), FluentIcon::qicon(FluentIcon::CANCEL),
        QStringLiteral("退出登录"), QStringLiteral("删除本机加密保存的登录凭据"), accountGroup);
    accountGroup->addSettingCard(m_accountCard);
    accountGroup->addSettingCard(m_loginCard);
    accountGroup->addSettingCard(m_refreshCard);
    accountGroup->addSettingCard(m_logoutCard);

    // ── 关于(settings_page.py:397-413)──
    m_aboutGroup = new SettingCardGroup(trText("page.settings.about_group", kGroupAbout), m_view);
    m_aboutCard = new SettingCard( // :399-404
        FluentIcon::qicon(FluentIcon::INFO),
        QStringLiteral("%1 %2").arg(QString::fromUtf8(kAppName), QString::fromUtf8(kAppVersion)),
        // 注意:Python 原文是"基于 PySide6 与 QFluentWidgets 构建,支持 Windows / macOS / Linux",
        // C 版必须写自己的技术栈 —— 照抄会谎报实现。JQt 是 SilentStudio 自有的 Qt 框架,
        // 也是 C 版出 Android 安装包所依赖的流水线(JQt-for-Android)。
        QStringLiteral("基于 Qt6 · libqf · JQt 构建，支持 Windows / macOS / Linux / Android"),
        m_aboutGroup);
    m_websiteCard = new HyperlinkCard( // :405-411
        QString::fromUtf8(kAppRepoUrl), QStringLiteral("访问官网"),
        FluentIcon::qicon(FluentIcon::LINK), QStringLiteral("项目主页"), QString(), m_aboutGroup);
    m_aboutGroup->addSettingCard(m_aboutCard);   // :412
    m_aboutGroup->addSettingCard(m_websiteCard); // :413

    m_vBox->addWidget(generalGroup);  // :415
    m_vBox->addWidget(gameGroup);     // :416
    m_vBox->addWidget(downloadGroup); // :417
    m_vBox->addWidget(advancedGroup); // :418
    m_vBox->addWidget(accountGroup);  // 新增:「账户」组,按交付要求放在「关于」之前
    m_vBox->addWidget(m_aboutGroup);  // :419
    m_vBox->addStretch(1);            // :420

    // :422-426 配过 Java 就按它选中,否则自动推荐
    const QString preferredJava = m_store.text(kKeyJavaPath);
    if (!preferredJava.isEmpty())
        m_javaCard->refresh(preferredJava);
    else
        m_javaCard->refresh();

    refreshAccountCard(); // 新增:进设置页就把账户状态读出来(读的是加密令牌文件,不联网)
}

void SettingsPage::bindEvents() { // settings_page.py:428-435
    m_javaCard->setSelectionHandler([this](const QString &path) { // :429-430 → :437-439
        // Python 存 str(install.path)(Windows 本地分隔符);界面里的 path 是 Qt 口径('/'),
        // 落盘时转回本地分隔符,免得配置里两套写法混着来。
        m_store.set(kKeyJavaPath, QDir::toNativeSeparators(path));
    });
    connect(m_gameDirCard, &PushSettingCard::clicked, this,
            [this] { pickGameDirectory(); }); // :430
    connect(m_resetCard, &PushSettingCard::clicked, this,
            [this] { resetSettings(); });     // :431
    connect(m_themeCard, &ComboBoxSettingCard::indexChanged, this,
            [this](int, const QString &value) { // :432-433
                m_store.set(kKeyTheme, value);
                onThemeChanged();
            });
    connect(m_themeColorCard, &ColorSettingCard::colorChanged, this,
            [this](const QColor &color) {       // :433(qconfig.themeColor → setThemeColor → 重套样式)
                m_store.set(kKeyAccent, color.name());
                FluentTheme::instance().setAccent(color); // C 版口径:accent 就是**渲染色**(见报告)
                onThemeChanged();
            });
    connect(m_sourceCard, &ComboBoxSettingCard::indexChanged, this,
            [this](int, const QString &value) { // :434
                m_store.set(kKeyDownloadSource, value);
                onDownloadSourceChanged(value);
            });
    connect(m_isolationCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) {              // :435
                m_store.set(kKeyVersionIsolation, checked);
            });
    connect(m_updateCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) { m_store.set(kKeyAutoCheckUpdate, checked); });
    connect(m_refreshIntervalCard, &ComboBoxSettingCard::indexChanged, this,
            [this](int, const QString &value) { m_store.set(kKeyRefreshInterval, value.toInt()); });
    connect(m_languageCard, &ComboBoxSettingCard::indexChanged, this,
            [this](int, const QString &value) { onLanguageChanged(value); });
    connect(m_windowCard, &ComboBoxSettingCard::indexChanged, this,
            [this](int, const QString &value) { m_store.set(kKeyWindowSize, value); });
    m_memoryCard->setChangeHandler([this](int value) { m_store.set(kKeyMaxMemory, value); });
    connect(m_verifyCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) { m_store.set(kKeyVerifySha1, checked); });
    connect(m_debugCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) { m_store.set(kKeyDebugMode, checked); });
    connect(m_downloadEngineCard, &SwitchSettingCard::checkedChanged, this,
            [this](bool checked) { m_store.set(kKeyDownloadEngine, checked); });

    // ---- 账户(新增)----
    connect(m_loginCard, &PushSettingCard::clicked, this, [this] { onLoginClicked(); });
    connect(m_refreshCard, &PushSettingCard::clicked, this, [this] { onRefreshClicked(); });
    connect(m_logoutCard, &PushSettingCard::clicked, this, [this] { onLogoutClicked(); });
}

// ─────────────────────────── 账户(新增;Python 版无此功能) ───────────────────────────
//
// 三条硬规矩:
//   1) **主线程绝不阻塞** —— 登录/续期/注销全部交给 AccountTask 在 work 线程里跑
//      (核心库是同步阻塞的,见 docs/09 与 dialogs/account.h 的线程纪律);
//   2) 错误**原样**转达 —— InfoBar 里既有人话,也有核心库 err 的原文(第 6 跳 403 就是这条路);
//   3) 不做假象 —— 没有真 token 就绝不说"已登录/启动成功"。

void SettingsPage::refreshAccountCard() {
    if (m_accountCard == nullptr)
        return;
    const AccountSnapshot snapshot = loadAccountSnapshot();
    m_accountCard->updateState(snapshot);
    // 没登录过就没有可刷新的凭据、也没有可注销的东西(两个动作都置灰,免得点了没反应)。
    if (m_refreshCard != nullptr)
        m_refreshCard->setEnabled(snapshot.tokenFileExists);
    if (m_logoutCard != nullptr)
        m_logoutCard->setEnabled(snapshot.tokenFileExists);
}

void SettingsPage::onLoginClicked() {
    QWidget *host = window() != nullptr ? window() : this;
    // 对话框自己管后台任务;这里只接"登录成功"的通知去刷账户卡(见 dialogs/auth_dialog.cpp)。
    AuthLoginDialog *dialog = AuthLoginDialog::open(host);
    connect(dialog, &AuthLoginDialog::accountChanged, this, [this] {
        refreshAccountCard();
        InfoBar::push(InfoBar::Type::Success, QStringLiteral("登录成功"),
                      QStringLiteral("账户状态已刷新。"), window(), 5000);
    });
}

void SettingsPage::onRefreshClicked() {
    runAccountAction(AccountTask::Operation::Refresh, QStringLiteral("刷新登录状态"));
}

void SettingsPage::onLogoutClicked() {
    // 注销会删掉 refresh token(下次要重新输一次设备码),所以先确认一次。
    auto *box = new MessageBox(QStringLiteral("退出登录"),
                               QStringLiteral("将删除本机加密保存的登录凭据；下次登录要重新输一次"
                                              "设备码。确定退出吗？"),
                               window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (box->yesButton() != nullptr)
        box->yesButton()->setText(QStringLiteral("退出登录"));
    if (box->cancelButton() != nullptr)
        box->cancelButton()->setText(QStringLiteral("取消"));
    connect(box, &MessageBox::yesSignal, this, [this] {
        runAccountAction(AccountTask::Operation::Logout, QStringLiteral("退出登录"));
    });
    box->show();
}

void SettingsPage::runAccountAction(AccountTask::Operation operation, const QString &title) {
    if (m_accountTask != nullptr && m_accountTask->running()) {
        InfoBar::push(InfoBar::Type::Info, title,
                      QStringLiteral("上一次账户操作还在进行中，请稍候…"), window(), 3000);
        return;
    }
    auto *task = new AccountTask(operation, this);
    m_accountTask = task;
    connect(task, &AccountTask::finished, this,
            [this, task, title](bool ok, const QString &message, const QString &rawError) {
                QString detail = message;
                if (!rawError.isEmpty())
                    detail += QStringLiteral("\n核心库原文（原样）：") + rawError;
                InfoBar::push(ok ? InfoBar::Type::Success : InfoBar::Type::Error, title, detail,
                              window(), ok ? 6000 : 12000);
                refreshAccountCard();
                if (m_accountTask == task)
                    m_accountTask = nullptr;
                task->deleteLater();
            });
    InfoBar::push(InfoBar::Type::Info, title,
                  QStringLiteral("正在后台进行（界面不会卡住）…"), window(), 2500);
    task->start();
}

// settings_page.py:511-514 _on_theme_changed:
//   from src.app.theme import apply_theme; apply_theme(qconfig.themeMode.value)
// —— Python 每次都按**配置里的主题模式**重套样式(含"跟随系统"的系统判定),这里照做;
// 主题色则是调用方先 setAccent 再进来重套(等价 qconfig.themeColor 的 valueChanged)。
void SettingsPage::onThemeChanged() {
    applyThemeMode(currentThemeMode(m_store));
    FluentTheme::instance().apply(qApp);
}

// settings_page.py:494-509 _on_language_changed → init_language(code)
//
// C 版的对应物是核心库的语言表(include/sxcl/lang.h):**立即生效**,不重启。
// 三步:落盘 -> 换进程级语言表(核心库热切换)-> 本页文案重取一遍。
// 如实说明生效范围:本页绑了键的文案 + Java 下载提示(java.* 键)立刻变;
// 导航与其它页面的文案还是 C 侧字面量(它们没有对应的 Python .lang 键,硬绑会破坏 1:1 文案),
// 这一条写进 docs/10-Java安装与国际化.md,不糊弄用户。
void SettingsPage::onLanguageChanged(const QString &value) {
    m_store.set(kKeyLanguage, value);

    char err[SXCL_LANG_ERR_MAX];
    err[0] = '\0';
    const QByteArray code = value.toUtf8();
    if (sxcl_lang_set_default(code.constData(), nullptr, err, sizeof(err)) != SXCL_LANG_OK) {
        UiErrorContext ctx;
        ctx.page = QStringLiteral("设置页 / settings");
        ctx.action = QStringLiteral("切换语言 -> %1").arg(value);
        ctx.reason = QString::fromUtf8(err);
        ctx.title = QStringLiteral("语言切换失败");
        pushUiError(this, ctx, 8000);
        return;
    }
    applyLanguageTexts();

    const QString display = value == QLatin1String("en-US") ? QStringLiteral("English")
                                                           : QStringLiteral("简体中文");
    InfoBar::push(InfoBar::Type::Success, QStringLiteral("语言已切换 / Language switched"),
                  QStringLiteral("已切换至 %1：设置页文案与 Java 下载提示已立即生效；"
                                 "其余页面文案尚未绑定语言键（见 docs/10）。")
                      .arg(display),
                  this, 6000);
}

// 把绑了语言键的文案重新取一遍(语言热切换;不重建窗口、不重启)
void SettingsPage::applyLanguageTexts() {
    if (m_title != nullptr)
        m_title->setText(trText("page.settings.title", kPageTitle));
    if (m_languageCard != nullptr)
        m_languageCard->setTitle(trText("page.settings.language", "语言"));
    if (m_isolationCard != nullptr)
        m_isolationCard->setTitle(trText("page.settings.version_isolation", "版本隔离"));
    if (m_gameDirCard != nullptr)
        m_gameDirCard->setTitle(trText("page.settings.game_dir", "游戏目录"));
    if (m_aboutGroup != nullptr)
        m_aboutGroup->setTitle(trText("page.settings.about_group", kGroupAbout));
}

// settings_page.py:525-528 _on_download_source_changed → window().on_download_source_changed(source)
void SettingsPage::onDownloadSourceChanged(const QString &value) {
    QWidget *host = window();
    if (!host)
        return;
    // Python 用 hasattr(window, "on_download_source_changed") 判断;C 版主窗口还没有这个钩子,
    // 用一次动态调用等价地"有就调、没有就算了"(报告:主窗口需要 onDownloadSourceChanged)。
    (void)QMetaObject::invokeMethod(host, "onDownloadSourceChanged", Qt::DirectConnection,
                                    Q_ARG(QString, value));
}

// Android 追加(用户反馈:”我设置为 HMCL 游戏目录成功检出游戏“,说明自动扫描当年没覆盖到)。
// 先跑一遍安卓候选扫描(sxcl_paths_detect_android),把**真的存在**的目录连同
// ”这是谁留的 / 有几个版本“列出来让用户点一下;想去别的地方就选最后一项走文件对话框。
// 找不到任何候选时返回空串,调用方落回原行为 —— 桌面平台上这张表恒为空,行为完全不变。
QString pickAndroidGameDir(QWidget *parent) {
    // 没有共享存储权限时,扫描结果一定是空的 —— 这时候"列候选"没有意义,
    // 直接说清原因并把用户送去授权页(而不是让他以为"我有游戏却说没有")。
    if (!androidHasAllFilesAccess()) {
        QMessageBox box(parent);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(QStringLiteral("需要开启「所有文件访问权限」"));
        box.setText(QStringLiteral(
            "安卓限制了本应用读取共享存储,所以扫不到你的 .minecraft(HMCL/FCL/PojavLauncher "
            "的游戏目录通常也在那里)。\n\n请到:\n系统设置 → 应用 → 特殊应用权限 → 所有文件访问权限\n"
            "给「Silent X Craft Launcher」打开开关。\n\n"
            "打开后回到本应用会自动重新扫描,不用重启。"));
        QPushButton *go = box.addButton(QStringLiteral("去授权"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == go)
            androidRequestAllFilesAccess();
        return QString();
    }
    const QByteArray files = qEnvironmentVariable("SXCL_ANDROID_FILES").toUtf8();
    sxcl_game_folders folders;
    char err[SXCL_PATHS_ERROR_MAX];
    err[0] = '\0';
    (void)sxcl_paths_detect_android(files.isEmpty() ? nullptr : files.constData(), nullptr, &folders,
                                     nullptr, err, sizeof(err));
    if (folders.count == 0) {
        // 一个都自动找不到时**不再静默**:把"我们找过哪些地方、为什么没用上"如实摊开,
        // 然后照旧让用户手动指定(用户反馈就是"自动扫描没覆盖到我的目录")。
        sxcl_game_probes probes;
        (void)sxcl_paths_probe_android(files.isEmpty() ? nullptr : files.constData(), nullptr, nullptr,
                                       &probes);
        QStringList lines;
        for (size_t i = 0; i < probes.count; ++i) {
            const sxcl_game_probe &g = probes.items[i];
            if (g.access == SXCL_ANDROID_MISSING)
                continue; // 不存在的那些不用占篇幅
            lines << QStringLiteral("%1（%2）\n    %3\n    %4")
                         .arg(QString::fromUtf8(g.path), QString::fromUtf8(g.owner),
                              QString::fromUtf8(g.reason), QString::fromUtf8(g.hint));
        }
        QString text = lines.isEmpty()
                           ? QStringLiteral("常见位置都没有游戏数据。\n请手动指定你的 .minecraft 目录。")
                           : (QStringLiteral("自动扫描到的可用目录:%1 个。以下是看着像、但用不了的位置:\n\n")
                                  .arg(static_cast<int>(probes.usable)) +
                              lines.join(QStringLiteral("\n\n")));
        QMessageBox::information(parent, QStringLiteral("没找到游戏目录"), text);
        return QString();
    }

    QStringList items;
    for (size_t i = 0; i < folders.count; ++i) {
        const sxcl_game_folder &folder = folders.items[i];
        const QString owner = QString::fromUtf8(folder.owner);
        const QString label = QString::fromUtf8(folder.label);
        if (owner.isEmpty()) {
            items << QStringLiteral("%1（%2，%3 个版本）")
                         .arg(QString::fromUtf8(folder.path), label)
                         .arg(folder.versions);
        } else {
            items << QStringLiteral("%1（%2 / %3，%4 个版本）")
                         .arg(QString::fromUtf8(folder.path), owner, label)
                         .arg(folder.versions);
        }
    }
    const QString manual = QStringLiteral("手动选择其它目录…");
    items << manual;

    bool ok = false;
    const QString chosen = QInputDialog::getItem(parent, QStringLiteral("选择 Minecraft 游戏目录"),
                                                QStringLiteral("自动找到这些，选一个："), items, 0,
                                                false, &ok);
    if (!ok || chosen.isEmpty() || chosen == manual)
        return QString();
    const int index = items.indexOf(chosen);
    if (index < 0 || static_cast<size_t>(index) >= folders.count)
        return QString();
    return QDir::fromNativeSeparators(QString::fromUtf8(folders.items[index].path));
}

// 桌面平台的"自动检测出来的游戏目录"选择器(①:别让用户自己填路径)。
// 与安卓那路同一个形状:列出**真实存在**的候选(带来源标签 / 谁留的 / 版本数 / 判据),
// 最后一项才是"手动选择其它目录…"。核心库 sxcl_paths_detect_ex 一次覆盖
// 便携目录 / 官方启动器 / HMCL / MultiMC / Prism / CurseForge / ATLauncher / Flatpak /
// 桌面,并且把 instances/<名字> 这种容器根展开成真正的实例目录。
QString pickDetectedGameDir(QWidget *parent) {
    sxcl_paths_env_store envStore;
    sxcl_paths_env_capture(&envStore);
    sxcl_game_folders folders;
    char err[SXCL_PATHS_ERROR_MAX];
    err[0] = '\0';
    (void)sxcl_paths_detect_ex(&envStore.env, sxcl_paths_current_os(), &folders, nullptr, err,
                               sizeof(err));
    if (folders.count == 0)
        return QString(); // 一个都没扫到:落回文件对话框,不打扰用户

    QStringList items;
    for (size_t i = 0; i < folders.count; ++i) {
        const sxcl_game_folder &folder = folders.items[i];
        char marksText[SXCL_PATHS_DESC_MAX];
        marksText[0] = '\0';
        (void)sxcl_paths_marks_text(sxcl_paths_marks(folder.path), folder.versions, marksText,
                                    sizeof(marksText));
        const QString owner = QString::fromUtf8(folder.owner);
        const QString label = QString::fromUtf8(folder.label);
        const QString source = owner.isEmpty() ? label : QStringLiteral("%1 / %2").arg(owner, label);
        items << QStringLiteral("%1\n    %2 · %3")
                     .arg(QDir::toNativeSeparators(QString::fromUtf8(folder.path)), source,
                          QString::fromUtf8(marksText));
    }
    const QString manual = QStringLiteral("手动选择其它目录…");
    items << manual;

    bool ok = false;
    const QString chosen =
        QInputDialog::getItem(parent, QStringLiteral("选择 Minecraft 游戏目录"),
                              QStringLiteral("自动检测到这些（选一个即生效）："), items, 0, false, &ok);
    if (!ok || chosen.isEmpty() || chosen == manual)
        return QString();
    const int index = items.indexOf(chosen);
    if (index < 0 || static_cast<size_t>(index) >= folders.count)
        return QString();
    return QDir::fromNativeSeparators(QString::fromUtf8(folders.items[index].path));
}

// settings_page.py:441-450 _pick_game_directory
void SettingsPage::pickGameDirectory() {
    // 先给候选:安卓走共享存储/各家启动器那张表;桌面走 ① 的候选根表。
    QString path = pickAndroidGameDir(this);
    if (path.isEmpty())
        path = pickDetectedGameDir(this);
    if (path.isEmpty()) {
        path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 Minecraft 游戏目录"),
                                                 gameDirectory());
    }
    if (path.isEmpty())
        return;
    const QString stored = QDir::fromNativeSeparators(path); // Python 存 Qt 原样字符串(正斜杠)
    m_store.set(kKeyGameDir, stored);
    m_gameDirCard->setContent(stored);
}

// settings_page.py:452-492 _reset_settings
void SettingsPage::resetSettings() {
    m_store.set(kKeyAutoCheckUpdate, true);                     // :453
    m_store.set(kKeyCloseAfterInstall, false);                  // 本版新增项:重置 = 回到默认(关)
    m_store.set(kKeyTheme, QStringLiteral("auto"));             // :454-455 Theme.AUTO
    m_store.set(kKeyLanguage, QStringLiteral("zh-CN"));         // :456
    m_store.set(kKeyDownloadSource, QStringLiteral("bmclapi")); // :457(注意:Python 重置就是 BMCLAPI)
    m_store.set(kKeyJavaPath, QString());                       // :458
    m_store.set(kKeyVersionIsolation, false);                   // :459
    m_store.set(kKeyDownloadEngine, true);                      // :460
    m_store.set(kKeyMaxMemory, m_memoryRange.defaultMb);        // :462-471
    m_store.set(kKeyWindowSize, QStringLiteral("1280x720"));    // :473
    // :474 qconfig.set(cfg.gameDirectory, str(default_game_directory())) —— Python 会把
    // 平台默认目录**显式写进配置**;核心库的 game.default_dir 约定是"空 = 用平台默认",
    // 所以这里先删掉旧值,再从核心库取一次平台默认并写下去(显示与 Python 完全一致)。
    m_store.remove(kKeyGameDir);
    const QString defaultGameDir = gameDirectory();
    if (!defaultGameDir.isEmpty())
        m_store.set(kKeyGameDir, defaultGameDir);
    m_store.set(kKeyDebugMode, false);                          // :475
    m_store.set(kKeyMaxConn, 32);                               // :476
    m_store.set(kKeyRate, 0);                                   // :477 speedLimitKbps = 0
    m_store.set(kKeyVerifySha1, true);                          // :478
    // Python 的重置**没有**重置"版本列表刷新频率",也没有重置主题色(照抄,不"顺手修正")
    m_store.save();

    m_connCard->setValue(32);                        // :482
    m_limitCard->setValue(0);                        // :483
    applySpeedLimit(0);                              // :484

    m_gameDirCard->setContent(gameDirectory());      // :486
    m_javaCard->refresh();                           // :487
    m_hostedJreCard->setStoredSource(m_store.text(SXCL_JRE_INDEX_URL_SETTING)); // 来源也被重置了
    m_themeCard->comboBox()->setCurrentIndex(2);     // :488 themeMode = AUTO(索引 2)
    m_languageCard->comboBox()->setCurrentIndex(0);  // :489 语言 = ZH_CN
    m_sourceCard->comboBox()->setCurrentIndex(2);    // :490 下载源 = BMCLAPI
    m_windowCard->comboBox()->setCurrentIndex(1);    // :491 窗口 = 1280x720
    onThemeChanged();                                // :492 _on_theme_changed
}

// settings_page.py:516-523 _apply_speed_limit
void SettingsPage::applySpeedLimit(int kbps) {
    // Python:limiter().set_rate(kbps * 1024) —— 直接改**全局**限速器实例。
    // C 核心库的限速器由调用方自己创建(include/sxcl/limiter.h:26),没有全局单例,
    // 所以这里只落盘 download.rate(核心库口径:字节/秒),由下载侧读取生效。
    // 缺口(全局 limiter 单例)写进交付报告。
    const qint64 bytesPerSecond = static_cast<qint64>(kbps) * 1024;
    m_store.set(kKeyRate, QString::number(bytesPerSecond));
}

} // namespace

QWidget *createSettingsPage(QWidget *parent) { return new SettingsPage(parent); }

} // namespace sxcl::ui
