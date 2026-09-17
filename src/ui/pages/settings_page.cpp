// settings_page.cpp —— 设置页(1:1 移植 Python 版 src/app/pages/settings_page.py)
//
// 依据(每一条都能指回出处;不许自己发明颜色/尺寸):
//   * 版面骨架 = Python src/app/common/base_page.py:32-66(BasePage:ScrollArea + view 的
//     QVBoxLayout margins(28,24,28,24)/spacing 16/AlignTop + TitleLabel + SubtitleLabel),
//     与 tasks_page.cpp / versions_page.cpp 用的是同一套外壳;
//   * 设置卡片族 = qfluentwidgets components/settings/*(SettingCard 定高 70/50、图标 16x16、
//     hBox margins(16,0,0,0)、图标后 16、vBox(标题 14px / 说明 11px)、右控件后 16;
//     SettingCardGroup = 组标题 20px + 12 间距 + 卡片列 spacing 2)。C 版直接用 libqf 的
//     SettingCard / SettingCardGroup / ComboBoxSettingCard / ColorSettingCard /
//     SwitchSettingCard / PushSettingCard / HyperlinkCard(libqf 的 fluent_setting_cards.cpp
//     与 qf 的 setting_card.py:36-114 逐条对齐);
//   * 页面结构/文字/尺寸 = Python src/app/pages/settings_page.py(逐段标注行号)
//     + src/app/widgets/java_setting_card.py + src/app/common/launcher_config.py;
//   * 颜色令牌/字号 = docs/05-UI-1to1规格.md §2/§3/§5/§6。
//
// 设置持久化:一律走 C 核心库 sxcl_settings(include/sxcl/settings.h),**不**去读 Python 版
// 的 %APPDATA%/SilentXCraftLauncher/config.json。键名、默认值、以及"核心库缺什么"见交付报告。
#include "page_factory.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
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
#include <cmath>
#include <filesystem>
#include <functional>
#include <system_error>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_controls.h"      // PushButton / InfoBar
#include "fluent/fluent_labels.h"        // TitleLabel / SubtitleLabel / CaptionLabel
#include "fluent/fluent_scroll.h"        // ScrollArea(= Python qf ScrollArea)
#include "fluent/fluent_setting_cards.h" // SettingCard 家族 / ComboBox / SettingCardGroup
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"
#include "theme_bridge.h"

// 核心库(纯 C):UI 层已链 sxcl 并挂了 include/
#include "sxcl/launch.h"   // Java 运行时探测(替代 Python services/java/finder.py)
#include "sxcl/limiter.h"  // sxcl_limiter_parse_rate("512K" 这类文本 → 字节/秒)
#include "sxcl/paths.h"    // 平台默认游戏目录
#include "sxcl/settings.h" // 设置读写(key=value,UTF-8)
#include "sxcl/sysinfo.h"  // 物理内存(设置页内存滑块的数据源)

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
const char *const kKeyMaxConn = "download.max_conn";      // 核(便捷读取默认 1;冲突见报告)
const char *const kKeyRate = "download.rate";             // 核(字节/秒,默认 0 = 不限速)
const char *const kKeyVerifySha1 = "download.verify_sha1"; // 新增,默认开
const char *const kKeyDebugMode = "advanced.debug_mode";  // 新增,默认关
const char *const kKeyDownloadEngine = "advanced.use_download_engine"; // 新增,默认开

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
// 与核心库 modloader/keymap_store.c:95-108 的"配置目录"口径一致
// (Python platform.py:default_config_directory("SilentXCraftLauncher"))。
// **缺口**:核心库没有公开的"sxcl_paths_config_dir",这里只能自己拼;见交付报告。
QString settingsFilePath() {
#if defined(Q_OS_WIN)
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/settings.conf");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() +
           QStringLiteral("/Library/Application Support/SilentXCraftLauncher/settings.conf");
#else
    return QDir::homePath() + QStringLiteral("/.config/SilentXCraftLauncher/settings.conf");
#endif
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
    const std::filesystem::path resolved =
        std::filesystem::canonical(path.toStdWString(), ec);
    if (ec || resolved.empty())
        return path;
    return QDir::fromNativeSeparators(
        QString::fromStdWString(resolved.native())); // 返回 Qt 口径('/'),显示时再转回本地分隔符
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

    QSet<QString> seen;
    for (const JavaEntry &entry : items) {
        if (seen.contains(entry.path))
            continue;
        seen.insert(entry.path);
        result.append(entry);
    }
    return result;
}

// finder.py:294-299 best_java_installation(最高版本的兼容 Java)
const JavaEntry *bestJavaInstallation(const QVector<JavaEntry> &list) {
    const JavaEntry *best = nullptr;
    for (const JavaEntry &entry : list) {
        if (!entry.compatible)
            continue;
        if (!best || entry.major > best->major)
            best = &entry;
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
        connect(m_downloadButton, &QPushButton::clicked, this,
                [this] { showDownloadMenu(); });                                          // :133
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
                m_combo->addItem(QStringLiteral("未检测到 Java，请手动导入"));
                m_statusLabel->setText(QStringLiteral("未找到可用的 Java 运行时"));
                m_statusLabel->setTextColor(QColor(0xfa, 0x8c, 0x16), QColor(0xff, 0xa9, 0x40));
                return;
            }

            for (const JavaEntry &entry : m_installations) // :150-151
                m_combo->addItem(entry.displayName(), entry.path);

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

    // java_setting_card.py:211-231 _show_download_menu / _start_download
    // 官方 JRE(java-runtime-*)要 Mojang 运行时清单 + 逐文件 SHA1 校验 + 解包,C 核心库目前
    // **没有**对应接口(mojang_runtime.py 的 install_runtime / find_installed_runtimes)。
    // 这里如实提示,不假装成功;接口建议见交付报告。
    void showDownloadMenu() {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("暂时无法下载官方 JRE"),
                      QStringLiteral("核心库还没有 Mojang 运行时安装接口（见交付报告）"),
                      window(), 5000);
    }

    ComboBox *m_combo = nullptr;
    PushButton *m_importButton = nullptr;
    PushButton *m_downloadButton = nullptr;
    CaptionLabel *m_statusLabel = nullptr;
    QVector<JavaEntry> m_installations;
    SelectionHandler m_onSelection;
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
    void onDownloadSourceChanged(const QString &value); // :525-528
    void pickGameDirectory();                     // :441-450
    void resetSettings();                         // :452-492
    void applySpeedLimit(int kbps);                // :516-523

    ConfigStore m_store;
    MemoryRange m_memoryRange;

    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;

    ComboBoxSettingCard *m_refreshIntervalCard = nullptr;
    SwitchSettingCard *m_updateCard = nullptr;
    ComboBoxSettingCard *m_themeCard = nullptr;
    ColorSettingCard *m_themeColorCard = nullptr;
    ComboBoxSettingCard *m_languageCard = nullptr;
    ComboBoxSettingCard *m_sourceCard = nullptr;

    SwitchSettingCard *m_isolationCard = nullptr;
    JavaSettingCard *m_javaCard = nullptr;
    MemorySettingCard *m_memoryCard = nullptr;
    ComboBoxSettingCard *m_windowCard = nullptr;
    PushSettingCard *m_gameDirCard = nullptr;

    SpinSettingCard *m_connCard = nullptr;
    SpinSettingCard *m_limitCard = nullptr;
    SwitchSettingCard *m_verifyCard = nullptr;

    SwitchSettingCard *m_debugCard = nullptr;
    SwitchSettingCard *m_downloadEngineCard = nullptr;
    PushSettingCard *m_resetCard = nullptr;

    SettingCard *m_aboutCard = nullptr;
    HyperlinkCard *m_websiteCard = nullptr;
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

    m_title = new TitleLabel(QString::fromUtf8(kPageTitle), m_view); // :55
    m_subtitle = new SubtitleLabel(QString(), m_view);               // :56
    m_subtitle->hide();                                             // :209 subtitleLabel.hide()
    m_vBox->addWidget(m_title);                                     // :59
    m_vBox->addWidget(m_subtitle);                                  // :60

    // 页面底色 = 令牌 bg(#202020 / 浅色 #f3f3f3)。依据 docs/05-UI-1to1规格.md §10.2
    // (内容区设计值 = 窗口底)与 §11.2(抓图时页面自己画底色,否则半透明内容栈会透出桌面)。
    refreshPageBackground();
    connect(&FluentTheme::instance(), &FluentTheme::changed, this,
            [this] { refreshPageBackground(); });

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

void SettingsPage::buildContent() {
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
        FluentIcon::qicon(FluentIcon::LANGUAGE), QStringLiteral("语言"), QString(), languageTexts,
        languageValues,
        valueIndex(languageValues, m_store.text(kKeyLanguage, QStringLiteral("zh-CN")), 0),
        generalGroup);

    const QStringList sourceTexts = textList(kSourceTexts, 3); // :252
    const QStringList sourceValues = stringList(kSourceValues, 3);
    m_sourceCard = new ComboBoxSettingCard( // :246-253
        FluentIcon::qicon(FluentIcon::DOWNLOAD), QStringLiteral("版本下载源"),
        QStringLiteral("选择版本清单与资源文件的下载源"), sourceTexts, sourceValues,
        valueIndex(sourceValues, m_store.text(kKeyDownloadSource, QStringLiteral("auto")), 0),
        generalGroup);

    const QStringList refreshTexts = textList(kRefreshTexts, kRefreshCount); // :261
    const QStringList refreshValues = numberList(kRefreshValues, kRefreshCount);
    m_refreshIntervalCard = new ComboBoxSettingCard( // :255-263
        FluentIcon::qicon(FluentIcon::UPDATE), QStringLiteral("版本列表刷新频率"),
        QStringLiteral("版本清单自动刷新的间隔时间"), refreshTexts, refreshValues,
        valueIndex(refreshValues, QString::number(m_store.number(kKeyRefreshInterval, 120)), 2),
        generalGroup);

    // 入组顺序照抄 Python(:264 先入组 refresh_interval,再 update/theme/theme_color/language/source)
    generalGroup->addSettingCard(m_refreshIntervalCard);
    generalGroup->addSettingCard(m_updateCard);
    generalGroup->addSettingCard(m_themeCard);
    generalGroup->addSettingCard(m_themeColorCard);
    generalGroup->addSettingCard(m_languageCard);
    generalGroup->addSettingCard(m_sourceCard);

    // ── 游戏设置(settings_page.py:272-336)──
    auto *gameGroup = new SettingCardGroup(QString::fromUtf8(kGroupGame), m_view);

    m_isolationCard = new SwitchSettingCard( // :276-282
        FluentIcon::qicon(FluentIcon::FOLDER), QStringLiteral("版本隔离"),
        QStringLiteral("每个版本使用独立的 .minecraft 目录"),
        m_store.flag(kKeyVersionIsolation, false), gameGroup);
    gameGroup->addSettingCard(m_isolationCard); // :283

    m_javaCard = new JavaSettingCard(gameGroup); // :285
    gameGroup->addSettingCard(m_javaCard);       // :286

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
        QStringLiteral("游戏目录"), gameDirectory(), gameGroup);

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
    downloadGroup->addSettingCard(m_connCard);   // :393
    downloadGroup->addSettingCard(m_limitCard);  // :394
    downloadGroup->addSettingCard(m_verifyCard); // :395

    // ── 关于(settings_page.py:397-413)──
    auto *aboutGroup = new SettingCardGroup(QString::fromUtf8(kGroupAbout), m_view);
    m_aboutCard = new SettingCard( // :399-404
        FluentIcon::qicon(FluentIcon::INFO),
        QStringLiteral("%1 %2").arg(QString::fromUtf8(kAppName), QString::fromUtf8(kAppVersion)),
        // 注意:Python 原文是"基于 PySide6 与 QFluentWidgets 构建,支持 Windows / macOS / Linux",
        // C 版必须写自己的技术栈 —— 照抄会谎报实现。JQt 是 SilentStudio 自有的 Qt 框架,
        // 也是 C 版出 Android 安装包所依赖的流水线(JQt-for-Android)。
        QStringLiteral("基于 Qt6 · libqf · JQt 构建，支持 Windows / macOS / Linux / Android"),
        aboutGroup);
    m_websiteCard = new HyperlinkCard( // :405-411
        QString::fromUtf8(kAppRepoUrl), QStringLiteral("访问官网"),
        FluentIcon::qicon(FluentIcon::LINK), QStringLiteral("项目主页"), QString(), aboutGroup);
    aboutGroup->addSettingCard(m_aboutCard);   // :412
    aboutGroup->addSettingCard(m_websiteCard); // :413

    m_vBox->addWidget(generalGroup);  // :415
    m_vBox->addWidget(gameGroup);     // :416
    m_vBox->addWidget(downloadGroup); // :417
    m_vBox->addWidget(advancedGroup); // :418
    m_vBox->addWidget(aboutGroup);    // :419
    m_vBox->addStretch(1);            // :420

    // :422-426 配过 Java 就按它选中,否则自动推荐
    const QString preferredJava = m_store.text(kKeyJavaPath);
    if (!preferredJava.isEmpty())
        m_javaCard->refresh(preferredJava);
    else
        m_javaCard->refresh();
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
}

// settings_page.py:511-514 _on_theme_changed:
//   from src.app.theme import apply_theme; apply_theme(qconfig.themeMode.value)
// —— Python 每次都按**配置里的主题模式**重套样式(含"跟随系统"的系统判定),这里照做;
// 主题色则是调用方先 setAccent 再进来重套(等价 qconfig.themeColor 的 valueChanged)。
void SettingsPage::onThemeChanged() {
    applyThemeMode(currentThemeMode(m_store));
    FluentTheme::instance().apply(qApp);
}

// settings_page.py:494-509 _on_language_changed
void SettingsPage::onLanguageChanged(const QString &value) {
    m_store.set(kKeyLanguage, value);
    const QString display = value == QLatin1String("en-US") ? QStringLiteral("English")
                                                           : QStringLiteral("简体中文");
    // Python 这里还会 init_language(code) 切 .qm 翻译;C 版界面层还没有 i18n(见交付报告)。
    InfoBar::push(InfoBar::Type::Success, QStringLiteral("语言已切换"),
                  QStringLiteral("已切换至 %1，部分界面需要重启应用后完全生效").arg(display), this,
                  5000);
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

// settings_page.py:441-450 _pick_game_directory
void SettingsPage::pickGameDirectory() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 Minecraft 游戏目录"), gameDirectory());
    if (path.isEmpty())
        return;
    const QString stored = QDir::fromNativeSeparators(path); // Python 存 Qt 原样字符串(正斜杠)
    m_store.set(kKeyGameDir, stored);
    m_gameDirCard->setContent(stored);
}

// settings_page.py:452-492 _reset_settings
void SettingsPage::resetSettings() {
    m_store.set(kKeyAutoCheckUpdate, true);                     // :453
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
