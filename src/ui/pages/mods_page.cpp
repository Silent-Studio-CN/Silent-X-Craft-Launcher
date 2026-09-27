/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 下载页的「拼图（MOD）」与「太阳（光影）」两栏（docs/22 的 A1）。
//
// 顶部三行：当前实例上下文 / 搜索框 / **来源勾选项**（两个源可同时勾上）；
// 下面    ：结果卡片（图标 / 标题 / 作者 / 下载量 / 简介 + 一键「装」）。
//
// 口径（与 sxcl/mods.h 完全一致）：
//   * **两个源是勾选项**（用户 2026-09-26：「模组下载的两个圆要作为勾选的选项，而不是搜一个模组
//     从两个下面儿选，这点你要模仿 PCL 的理念」）：勾上哪个就搜哪个，勾两个就两个都搜，
//     结果**合并成一份列表**，每一条自带真实来源；一个源都没勾时搜索按钮点不动。
//   * **CurseForge 的 key 是编译期内置的**（include/sxcl/mods_key.h；用户点名「不要让用户自己填写」）：
//     界面上没有任何填 key 的入口；这个构建没有内置 key 时，CF 那个勾选框**可见但点不动**
//     （tooltip 一句话），**不发注定失败的请求** —— 绝不改用另一个源假装是 CF 的结果；
//   * 筛选交给**服务端**（Modrinth 的 facets / CF 的 classId + modLoaderType），本地不过滤；
//   * **不自动换加载器**：实例是 Fabric 就只列/只挑 Fabric 的文件，挑不出来就如实说；
//   * 依赖**只展示不装**（装完把 required 的 id 打在提示里）；
//   * 装进**版本隔离**后的目录（<游戏>/versions/<实例>/mods），没开隔离就还是根 mods/；
//   * 网络全在工作线程（取 JSON / 下文件 / 下图标），界面线程一个字节都不等。

#include "page_factory.h"
#include "game_folders.h"
#include "page_shell.h"

#include "../workers/bg_task.h"        // 一次性后台任务(实例扫描是磁盘活,只许工作线程)
#include "../workers/instance_scan.h"  // 单个实例扫描 + **版本行那份展示口径**(versionRowInfo)
#include "../workers/mods_merge.h"     // 来源勾选列表的落盘/读回 + 两源结果合并(纯逻辑,单测在 tests/)
#include "../workers/mods_worker.h"
#include "../workers/ui_paths.h"

#include "../elided_label.h" // 单行标签：装不下就省略号（结果行的名字/介绍都用它）
#include "flow_layout.h" // 筛选区：放不下就换行，控件保证完整显示（docs/25 总则）
#include "mod_detail_page.h" // 点一行进去的那一页（介绍全文 + 一个「安装」动作）
#include "fluent_theme.h"
#include "libqf.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_input.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#include "fluent/fluent_selection.h"     // CheckBox:来源**勾选项**(两个源能同时勾上) + 勾选态自绘

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QStringListModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>
#include <vector>

#include "sxcl/fs.h"
#include "sxcl/mods.h"
#include "sxcl/mods_key.h"     // 编译期内置的 CurseForge key(SXCL_CURSEFORGE_API_KEY)
#include "sxcl/settings.h"

namespace sxcl::ui {
namespace {

/* 设置键：**勾选项的列表**（"modrinth,curseforge" / "modrinth" / 空 = 一个都不勾）。
 * 老设置文件里存过单值（"modrinth" / "curseforge"），读的时候自动升级成列表
 * —— 见 workers/mods_merge.h 的 parseModsSources()。*/
const char *const kKeyModsSource = "mods.source";

/** 光影那一栏要的加载器 slug：Modrinth 认的是 iris / optifine / canvas，
 *  拿实例的 fabric/forge 去筛会一条都搜不到（这是"不自动换加载器"在光影上的等价物：
 *  我们只按"这个实例理论上能装哪种光影加载器"去筛，搜不到就如实说搜不到）。
 *  CurseForge 那边光影不分加载器（modLoaderType 认不出 iris 就是不筛），返回空串。 */
QByteArray shaderLoaderSlug(const QByteArray &instanceLoader, bool curseforge) {
    if (curseforge || instanceLoader.isEmpty()) {
        return QByteArray();
    }
    if (instanceLoader == QByteArrayLiteral("optifine")) {
        return QByteArrayLiteral("optifine");
    }
    if (instanceLoader == QByteArrayLiteral("fabric") || instanceLoader == QByteArrayLiteral("quilt")) {
        return QByteArrayLiteral("iris");
    }
    return QByteArray();   /* forge/neoforge:光影加载器不统一,不筛(如实让用户自己看) */
}

/** 结果行里 logo 那个**固定方块**的边长（用户 2026-09-27：「强制放在模组栏里，不能上下延伸」）：
 *  图标按比例缩进它，行高由文字那 4 行定 —— 图片再大也撑不高一行。 */
const int kIconSide = 48;

/** 版本隔离开着吗（与启动层同一个键）。 */
bool versionIsolationOn() {
    const QByteArray path = uiSettingsFilePath().toUtf8();
    sxcl_settings *st = sxcl_settings_open(path.constData());
    if (st == nullptr) {
        return false;
    }
    const int on = (int)sxcl_settings_get_int(st, "general.version_isolation", 1);
    sxcl_settings_free(st);
    return on != 0;
}

class ModsPane : public QWidget {
public:
    ModsPane(QWidget *parent, bool shaders) : QWidget(parent), m_shaders(shaders) {
        build();
        reloadSettings();
        m_versionTask = new BgTask(this); // 上下文那一行的实例扫描(见 reloadContext)
        reloadContext();
    }

private:
    enum Phase { Idle, Searching, LoadingVersions, Downloading };

    /** 一次请求的候选：url + 它属于哪个源 + 走的是哪条路。
     *  （声明必须放在第一个用到它的成员函数**之前** —— 参数类型不参与"类内延迟解析"。） */
    struct PendingFetch {
        QString url;
        QString source;
        QString via; // "mirror" / "official" / "direct"
    };

    // ─────────────────────────── 搭界面 ───────────────────────────
    void build() {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);

        m_context = new BodyLabel(QString(), this);
        /* 稳定的 objectName:验收 dump 按它认"上下文那一行",直接读它的文字断言
         * "认不出时一个字都不写"(不用只信我们自己打的 trace)。 */
        m_context->setObjectName(QStringLiteral("modsContextLine"));
        m_context->setWordWrap(true);
        const QColor secondary = pageTokenColor("textSecondary");
        m_context->setTextColor(secondary, secondary);
        lay->addWidget(m_context);

        /* ── 筛选区：**正好两行**（用户 2026-09-27：「搜索输入框不动但变长，紧接着右侧是搜索；
         * 然后切第二行：选版本号和模组来源」）：
         *   第 1 行 = 搜索框（吃掉这一行剩下的宽，所以它变长了）+「搜索」；
         *   第 2 行 = 版本（**可手打**）+ 两个来源勾选。
         * 除这四件**不许加控件**。第二行还是那个流式布局（docs/25 的总则）：每个控件保证
         * 能**完整显示自己**，放不下就换行，绝不把勾选框压成 41px（用户点名的那个 bug）。 */
        auto *filterRow = new QWidget(this);
        filterRow->setObjectName(QStringLiteral("modsFilterRow"));
        auto *filterLay = new QVBoxLayout(filterRow);
        filterLay->setContentsMargins(0, 0, 0, 0);
        filterLay->setSpacing(8);

        auto *searchRow = new QWidget(filterRow);
        searchRow->setObjectName(QStringLiteral("modsSearchRow"));
        auto *searchLay = new QHBoxLayout(searchRow);
        searchLay->setContentsMargins(0, 0, 0, 0);
        searchLay->setSpacing(10);

        m_search = new SearchLineEdit(searchRow);
        m_search->setPlaceholderText(m_shaders ? QStringLiteral("搜光影包")
                                               : QStringLiteral("搜模组"));
        m_search->setFixedHeight(34);
        /* 稳定的 objectName:验收钩子(SXCL_UI_MODS_QUERY)靠它**往真控件里打字再点真的搜索按钮**,
         * 而不是在 main.cpp 里另起一条"自己调核心库"的旁路(那样验不到接线本身)。 */
        m_search->setObjectName(QStringLiteral("modsSearchBox"));
        /* 自己的自然宽就是下限；它是这一行里唯一带 Expanding 的项(吃掉剩下的宽度)。 */
        m_search->setMinimumWidth(qMax(200, m_search->sizeHint().width()));
        m_search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        searchLay->addWidget(m_search, 1); // 1 = 搜索框吃掉整行剩下的宽度（「不动但变长」）

        m_go = new PrimaryPushButton(QStringLiteral("搜索"), searchRow);
        applyButtonFont(m_go);
        m_go->setObjectName(QStringLiteral("modsSearchButton"));
        m_go->setFixedHeight(34);
        m_go->setMinimumWidth(m_go->sizeHint().width());
        m_go->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        searchLay->addWidget(m_go, 0); // 紧接着搜索框的右侧
        filterLay->addWidget(searchRow);

        /* ── 第 2 行：版本 + 模组来源 ── */
        auto *optionRow = new QWidget(filterRow);
        optionRow->setObjectName(QStringLiteral("modsOptionRow"));
        m_filterFlow = new FlowLayout(optionRow, 0, 10, 8);

        /* 版本筛选：**可输入的输入框**，不是只能挑的下拉（用户 2026-09-27：
         * 「那个版本号儿，换成输入框啊，MC 他妈好成千上万个版本儿，你全做成下拉菜单儿」）。
         *   * 直接手打版本号（1.21.11 这种也能打）；打错了就按"没有这个版本"如实返回，
         *     不猜、不自动改成别的版本；
         *   * 带候选提示（常用版本 + 当前实例的版本）——只是提示，**不限制**输入；
         *   * 留空 = "全部"（这一维不筛）；
         *   * 进页面时默认**自动填**当前实例的原版版本（他刚装的那个），用户可以改。 */
        m_versionEdit = new LineEdit(optionRow);
        m_versionEdit->setObjectName(QStringLiteral("modsVersionFilter"));
        m_versionEdit->setPlaceholderText(QStringLiteral("版本（留空 = 全部）"));
        m_versionEdit->setFixedHeight(28);
        m_versionEdit->setMinimumWidth(qMax(190, m_versionEdit->sizeHint().width()));
        m_versionEdit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        {
            auto *completer = new QCompleter(m_versionEdit);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            completer->setModel(new QStringListModel(commonVersionChoices(), completer));
            m_versionEdit->setCompleter(completer); // libqf 的 LineEdit 就是 QLineEdit,直接用 Qt 的补全
        }
        /* 打字/回车：条件变了，上一份结果作废（与勾选同一个口径）；回车直接搜。 */
        QObject::connect(m_versionEdit, &QLineEdit::textEdited, this, [this](const QString &) {
            m_versionFilterTouched = true;
            cancelAutoLoad(); // 他在挑版本了:默认榜不许再自动盖上来
            clearResults();
            printSearchFilterTrace(m_lastSort);
        });
        QObject::connect(m_versionEdit, &QLineEdit::returnPressed, this, [this] { startSearch(); });
        m_filterFlow->addWidget(m_versionEdit);

        // ── 来源**勾选项**（用户 2026-09-26：「模组下载的两个圆要作为勾选的选项」）──
        // 两个源**能同时勾上**：勾上哪个就搜哪个，勾两个就两个都搜，结果合并成一份列表
        // （每一条自带真实来源标记）。这不是"先选一个源、再在它下面搜"。
        struct SourceSpec {
            const char *key;
            const char *objectName;
        };
        const SourceSpec specs[] = {{"modrinth", "modsSourceModrinth"},
                                    {"curseforge", "modsSourceCurseForge"}};
        for (const SourceSpec &spec : specs) {
            const QString key = QString::fromLatin1(spec.key);
            auto *box = new CheckBox(modsSourceDisplayName(key), optionRow);
            /* 稳定 objectName：验收钩子(SXCL_UI_MODS_SOURCES)与 dump 都按它认这两个勾选框。 */
            box->setObjectName(QString::fromLatin1(spec.objectName));
            box->setTristate(false); // qf 的 CheckBox 默认三态(那份是给演示用的),这里只要勾/不勾
            box->setFixedHeight(28);
            box->setCursor(Qt::PointingHandCursor);
            /* 勾选框也**保证自己能完整显示**：自然宽是下限，布局只会把它换行，不会压它。 */
            box->setMinimumWidth(box->sizeHint().width());
            box->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            QObject::connect(box, &QAbstractButton::toggled, this,
                             [this, key] { onSourceToggled(key); });
            m_filterFlow->addWidget(box);
            m_sourceBoxes.insert(key, box);
        }
        filterLay->addWidget(optionRow);
        lay->addWidget(filterRow);
        /* 注意：这里**没有** key 输入栏 —— 官方 key 是编译期内置的(include/sxcl/mods_key.h)，
         * 用户 2026-09-26 点名「不要让用户自己填写」。没有内置 key 时 CurseForge 那个框点不动
         * （见 refreshSourceBoxes 的 tooltip），而不是摆个框让用户去申请。 */

        auto *scroll = new ScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *holder = new QWidget(scroll);
        holder->setStyleSheet(QStringLiteral("background: transparent;"));
        scroll->setWidget(holder);
        m_list = new QVBoxLayout(holder);
        m_list->setContentsMargins(0, 0, 0, 0);
        m_list->setSpacing(8);
        m_list->setAlignment(Qt::AlignTop);
        lay->addWidget(scroll, 1);

        /* 默认榜的静默期计时器（见 armAutoLoad / tryAutoLoad）。 */
        m_autoTimer = new QTimer(this);
        m_autoTimer->setSingleShot(true);
        QObject::connect(m_autoTimer, &QTimer::timeout, this, [this] { tryAutoLoad(); });

        QObject::connect(m_go, &QAbstractButton::clicked, this, [this] { startSearch(); });
        QObject::connect(m_search, &QLineEdit::returnPressed, this, [this] { startSearch(); });
        /* 搜索框里一有字就说明他在搜自己的东西了：默认榜取消（程序回填/钩子的 setText 同样算）。 */
        QObject::connect(m_search, &QLineEdit::textChanged, this,
                         [this](const QString &) { cancelAutoLoad(); });
    }

    // ─────────────────────────── 设置 ───────────────────────────
    /** 这个构建**内置**的 CurseForge key（include/sxcl/mods_key.h；空 = 这个构建没有这一源）。
     *  用户 2026-09-26 点名「不要让用户自己填写」，所以界面上没有任何填 key 的入口。 */
    static QString compiledCfKey() { return QString::fromUtf8(SXCL_CURSEFORGE_API_KEY).trimmed(); }

    /** 这一源能不能搜：**两个都能**。CurseForge 没有内置 key 时走**镜像**（PCL 同款，
     *  镜像这条路不要 key），所以不再"没有 key 就把勾选框置灰"；真连不上时由那一次请求如实报失败。
     *  内置 key 仍然保留：有 key 就自动升级成"官方优先 + 镜像兜底"。 */
    static bool sourceUsable(const QString &source) { return modsSourceValid(source); }

    void reloadSettings() {
        QStringList picked;
        const QByteArray path = uiSettingsFilePath().toUtf8();
        sxcl_settings *st = sxcl_settings_open(path.constData());
        if (st != nullptr) {
            /* **从来没写过这个键**(nullptr) = 默认勾 Modrinth；
             * 写过就按写过的样子（空 = 用户把两个都取消了，不许偷偷改回默认）。 */
            picked = modsSourcesFromStored(sxcl_settings_get(st, kKeyModsSource, nullptr));
            sxcl_settings_free(st);
        } else {
            picked = modsSourcesFromStored(nullptr);
        }
        for (const QString &source : modsAllSources()) {
            QCheckBox *box = m_sourceBoxes.value(source);
            if (box != nullptr) {
                QSignalBlocker blocker(box); // 回填不算"用户改了勾选",不重复落盘
                box->setChecked(picked.contains(source));
            }
        }
        refreshSourceBoxes();
    }

    void saveSetting(const char *key, const QString &value) {
        const QByteArray path = uiSettingsFilePath().toUtf8();
        sxcl_settings *st = sxcl_settings_open(path.constData());
        if (st == nullptr) {
            return;
        }
        sxcl_settings_set(st, key, value.toUtf8().constData());
        sxcl_settings_save(st, path.constData());   // **必须 save**：只 set 不 save 是"记住"的假象
        sxcl_settings_free(st);
    }

    /** 版本下拉里的"常用版本"（PCL 那个下拉也是这么给的）：常见的**模组基座**版本。
     *  它只是快捷选项，**不是**对实例的断言 —— 实例的真实版本由核心库扫描给出（见
     *  selectVersionFilterFromInstance），认不出就停在"全部"。 */
    static QStringList commonVersionChoices() {
        return QStringList{QStringLiteral("1.21.4"), QStringLiteral("1.21.1"),
                           QStringLiteral("1.20.6"), QStringLiteral("1.20.4"),
                           QStringLiteral("1.20.1"), QStringLiteral("1.19.4"),
                           QStringLiteral("1.19.2"), QStringLiteral("1.18.2"),
                           QStringLiteral("1.16.5"), QStringLiteral("1.12.2"),
                           QStringLiteral("1.7.10")};
    }

    /** 当前**筛选**用的版本：输入框里那个（去掉空白）。空 = "全部"（这一维不筛，不带版本 facets）。 */
    QString versionFilter() const {
        return m_versionEdit != nullptr ? m_versionEdit->text().trimmed() : QString();
    }

    /** 输入框的默认值 = 当前实例的**原版版本**（只认版本文件里的那个值；认不出 = 留空 = 全部）。
     *  用户在页面上动过输入框就**不再覆盖**他打的那个。候选里补上这个版本，方便他改回去。 */
    void selectVersionFilterFromInstance() {
        if (m_versionEdit == nullptr || m_versionFilterTouched) {
            return;
        }
        const QString want = m_mc;
        if (!want.isEmpty() && m_versionEdit->completer() != nullptr &&
            m_versionEdit->completer()->model() != nullptr) {
            auto *model = qobject_cast<QStringListModel *>(m_versionEdit->completer()->model());
            if (model != nullptr && !model->stringList().contains(want)) {
                QStringList items = model->stringList();
                items.prepend(want);
                model->setStringList(items);
            }
        }
        m_versionAuto = true;
        m_versionEdit->setText(want);
        m_versionAuto = false;
        std::fprintf(stderr, "[sxcl-ui] mods-version-default: %s\n",
                     want.isEmpty() ? "empty" : want.toUtf8().constData());
    }

    /** 取证行：这一轮搜索用的版本筛选（"全部"就写 all）、关键词、排序。
     *  排序这一项是"空搜索 = 下载量默认榜"的证据：Modrinth 那边就是 index=downloads
     *  （见紧随其后的 mods-fetch 行里的 URL），CF 那边是 sortField=2。 */
    void printSearchFilterTrace(const QByteArray &sort) const {
        const QString version = versionFilter();
        std::fprintf(stderr, "[sxcl-ui] mods-search: versionFilter=%s text=\"%s\" index=%s\n",
                     version.isEmpty() ? "all" : version.toUtf8().constData(),
                     m_search == nullptr ? "" : m_search->text().trimmed().toUtf8().constData(),
                     sort.isEmpty() ? "relevance" : sort.constData());
    }

    /** 眼前这几个勾选框里，勾上了哪几个（按固定顺序）。 */
    QStringList pickedSources() const {
        QStringList out;
        for (const QString &source : modsAllSources()) {
            QCheckBox *box = m_sourceBoxes.value(source);
            if (box != nullptr && box->isChecked()) {
                out << source;
            }
        }
        return out;
    }

    /** 勾选框的可用性：现在两个源都点得动（CF 有镜像兜底，不需要 key）。
     *  这里保留"用不了的源就取消勾选"的兜底逻辑，目前不会触发 —— 界面上也没有任何填 key 的入口。 */
    void refreshSourceBoxes() {
        for (const QString &source : modsAllSources()) {
            QCheckBox *box = m_sourceBoxes.value(source);
            if (box == nullptr) {
                continue;
            }
            const bool usable = sourceUsable(source);
            box->setEnabled(usable);
            box->setToolTip(QString());
            if (!usable && box->isChecked()) {
                QSignalBlocker blocker(box); // 我们替它取消,不算用户改勾选(不重复落盘)
                box->setChecked(false);
            }
        }
        m_sources = pickedSources();
        refreshGo();
        printSourcesTrace();
    }

    /** 用户动了勾选:落盘 + 上一份结果作废(它对应的是**另一组**源,留在屏幕上就是冒充)。 */
    void onSourceToggled(const QString &) {
        cancelAutoLoad(); // 他在挑源了:默认榜不许再自动盖上来
        m_sources = pickedSources();
        saveSetting(kKeyModsSource, formatModsSources(m_sources));
        clearResults();
        refreshGo();
        printSourcesTrace();
    }

    /** 一个源都没勾 = 搜索按钮点不动(不写"请至少选择一个源"那类废话)。
     *  **正在搜的时候照样点得动**：再点一次 = 用眼前的条件重搜一遍（见 startSearch 的 beginRun）——
     *  进门那趟"默认榜"绝不会把用户自己的搜索卡在后面。装的时候（取版本列表/下载）不给点：
     *  那一趟正在往磁盘上落东西，不该被一次搜索顶掉。 */
    void refreshGo() {
        if (m_go != nullptr) {
            const bool installing = (m_phase == LoadingVersions || m_phase == Downloading);
            m_go->setEnabled(!installing && !m_sources.isEmpty());
        }
    }

    /** 取证行:勾了哪几个源 / 这一轮真去搜哪几个 / 哪个源搜不了(为什么)。
     *  验收脚本按它断言"两个源是否**同时**被请求""没有 key 的那一源是不是如实被跳过"。 */
    void printSourcesTrace(const char *tag = "mods-sources-state") const {
        QStringList skipped;
        for (const QString &source : modsAllSources()) {
            if (!sourceUsable(source)) {
                skipped << (source + QStringLiteral(":no-key"));
            }
        }
        QStringList searchable;
        for (const QString &source : m_sources) {
            if (sourceUsable(source)) {
                searchable << source;
            }
        }
        std::fprintf(stderr, "[sxcl-ui] %s: picked=%s searchable=%s skipped=%s\n", tag,
                     m_sources.join(QLatin1Char(',')).toUtf8().constData(),
                     searchable.join(QLatin1Char(',')).toUtf8().constData(),
                     skipped.isEmpty() ? "none" : skipped.join(QLatin1Char(',')).toUtf8().constData());
    }

    // ─────────────────────────── 上下文 ───────────────────────────
    /* 这一行的"原版 x"**只认版本文件里写着的**(与版本行共用一份口径:instance_scan.h 的
     * versionRowInfo / 核心库 base_reliable)。以前是 `m_instance.section('-', 0, 0)`
     * —— 从**实例名**里抠一段当版本号,属于"猜的当事实写"(用户 2026-09-26 点名),
     * 而且那份读盘还压在界面线程上。现在:
     *   * 取数走 BgTask(mods-instance-version)在**工作线程**里调核心库单个实例扫描
     *     (instance_scan.h 的 scanInstalledInstance —— 它只许工作线程调);
     *   * 认不出就**一个字都不写**(m_mc 空 -> 那一栏不出现"原版"),也**不再拿它去筛版本**
     *     (筛版本交给服务端 facets 里的加载器,猜出来的版本号不许当筛选条件);
     *   * 结果回来才回填这一行 —— 界面线程一个字节都不等。 */
    void reloadContext() {
        m_gameDir = uiGameDirectory();
        m_instance = selectedVersionName();
        const QString tag = versionLoaderTag(m_gameDir, m_instance);
        m_loader = tag.isEmpty() ? QString() : tag.toLower();
        // 换实例 / 换目录:先把上一份结论清掉,免得拿旧结论去筛新实例
        m_versionScanned = false;
        m_versionFound = false;
        m_versionRow = VersionRowInfo();
        m_versionError.clear();
        m_mc.clear();
        applyContextText(); // 先把"当前实例 / 加载器 / 隔离"画出来(不等磁盘)
        startVersionScan();
    }

    /** 起一次实例扫描(磁盘活,**工作线程**)。期间又换过版本的话,这一趟回来会发现自己是旧的,
     *  自己再排一次 —— 不这样做的话 BgTask"已在跑就忽略"会让新实例永远等不到结论。 */
    void startVersionScan() {
        if (m_versionTask == nullptr)
            return;
        if (m_instance.isEmpty())
            return; // 还没选版本:不扫,也不写版本号
        if (m_versionTask->running())
            return;
        const QString id = m_instance;
        const QString gameDir = m_gameDir;
        m_versionTask->start(QStringLiteral("mods-instance-version"),
                             [this, id, gameDir] {
                                 InstalledInstance inst;
                                 QString err;
                                 m_versionFound = scanInstalledInstance(gameDir, id, &inst, &err);
                                 m_versionError = err;
                                 m_versionRow = m_versionFound ? versionRowInfo(inst, gameDir)
                                                               : VersionRowInfo();
                             },
                             [this, id] {
                                 if (id != m_instance) {
                                     startVersionScan(); // 期间换过版本:这一趟作废,重扫
                                     return;
                                 }
                                 m_versionScanned = true;
                                 // **只认版本文件里的**;认不出就是空 —— 什么都不写
                                 m_mc = m_versionRow.base;
                                 /* "他刚装的那个版本"就是这里的 m_mc（核心库扫出来的实例原版版本）:
                                  * 版本筛选默认跟着它走；认不出就停在"全部"。 */
                                 selectVersionFilterFromInstance();
                                 applyContextText();
                                 /* 默认榜的基准版本有了：条件都满足就让它排上。
                                  * 换了实例（他刚装的那个变了）且他什么筛选都没动过 -> 按新实例重拉一次。 */
                                 if (m_autoDone && !m_userTouched &&
                                     (m_search == nullptr || m_search->text().trimmed().isEmpty()) &&
                                     id != m_autoInstance) {
                                     m_autoDone = false;
                                 }
                                 m_autoInstance = id;
                                 armAutoLoad(200);
                             });
    }

    /** 拼出上下文那一行(有版本才写"原版 x"),并打一行取证。 */
    void applyContextText() {
        /* 用户 2026-09-26(文字纪律):「能推断出来的信息一个字都不写」。
         * 这一行原来还写 "· 版本隔离已开（装进实例自己的目录）· 源 Modrinth":
         *   * 源 —— 上面那个滑块本身就是答案(Modrinth / CurseForge 两个选项就在眼前),删;
         *   * "（装进实例自己的目录）" / "会装进根目录" —— 解释我们怎么实现,删(只留开/关);
         *   * "不筛" 这类空话删(没筛就不写)。
         * 留下的三件**不看就不知道**:装进哪个实例、它的原版与加载器(有才写)、隔离开关的状态
         * (它决定文件落到实例目录还是游戏根目录)。 */
        QStringList bits;
        bits << QStringLiteral("当前实例：%1")
                    .arg(m_instance.isEmpty() ? QStringLiteral("（还没选版本）") : m_instance);
        if (!m_mc.isEmpty())
            bits << QStringLiteral("原版 %1").arg(m_mc);
        if (!m_loader.isEmpty())
            bits << m_loader;
        bits << (versionIsolationOn() ? QStringLiteral("版本隔离：开") : QStringLiteral("版本隔离：关"));
        m_currentText = bits.join(QStringLiteral(" · "));
        m_context->setText(m_currentText);
        printContextTrace();
    }

    /* 逐行取证(stderr):这一行到底写了什么、那个版本号是从**哪个字段**认出来的、
     * 以及"猜没猜过实例名"。验收脚本按它断言"认不出时一个字都不写"。
     * baseFrom 的取值与版本行同一套:core:inheritsFrom / core:json-other / json:id / none。 */
    void printContextTrace() const {
        // 还没扫完时 baseFrom 是空串:打成 pending,与"扫完了但认不出"(none)区分开
        const QByteArray from = m_versionRow.baseFrom.isEmpty()
                                    ? QByteArray("pending")
                                    : m_versionRow.baseFrom.toUtf8();
        std::fprintf(stderr,
                     "[sxcl-ui] mods-context: instance=%s scanned=%d found=%d base=\"%s\" "
                     "baseFrom=%s coreReliable=%d loader=%s text=\"%s\" err=\"%s\"\n",
                     m_instance.toUtf8().constData(), m_versionScanned ? 1 : 0,
                     m_versionFound ? 1 : 0, m_versionRow.base.toUtf8().constData(),
                     from.constData(),
                     m_versionRow.coreReliable ? 1 : 0, m_loader.toUtf8().constData(),
                     m_currentText.toUtf8().constData(), m_versionError.toUtf8().constData());
    }

    /** 这一栏、**这一个源**实际拿去做筛选/挑文件的加载器：光影那一栏要换加载器自己的 slug
     *  （Modrinth 认 iris/optifine；CurseForge 那边光影不分加载器，见 shaderLoaderSlug）。 */
    QByteArray loaderFor(const QString &source) const {
        const QByteArray loader = m_loader.toUtf8();
        if (!m_shaders) {
            return loader;
        }
        return shaderLoaderSlug(loader, source == QLatin1String("curseforge"));
    }

    void clearResults() {
        while (QLayoutItem *item = m_list->takeAt(0)) {
            if (QWidget *w = item->widget()) {
                w->deleteLater();
            }
            delete item;
        }
    }

    void setBusy(bool busy, const QString &text) {
        m_busy = busy;
        m_go->setText(busy ? QStringLiteral("查询中…") : QStringLiteral("搜索"));
        refreshGo();
        m_context->setText(text.isEmpty() ? m_currentText : text);
    }

    // ─────────────────── 空搜索的默认榜（进门自动拉一次） ───────────────────
    /** 起一趟新的请求链：**编号 +1**、队列清空。上一趟（可能还在飞）的结果回来时对不上号，
     *  会被丢掉。用户点了搜索就是"我要新的"，不该被上一趟的慢请求卡住 —— 尤其是进门时
     *  自动拉的那趟默认榜（CF 官方那一路没有 key 时要等 8 秒才轮到镜像兜底）。 */
    void beginRun() {
        ++m_run;
        m_pending.clear();
        m_fallback.clear();
    }

    /** 用户只要动了任何一件筛选（打字 / 改版本 / 点来源 / 点搜索），默认榜就不再自动拉 ——
     *  他已经在搜自己的东西了，默认榜盖上去就是抢他的屏。 */
    void cancelAutoLoad() {
        m_userTouched = true;
        if (m_autoTimer != nullptr) {
            m_autoTimer->stop();
        }
    }

    /** 默认榜的"静默期"：页面显示之后先等一会儿再拉（见 cancelAutoLoad 的理由）。 */
    void armAutoLoad(int delayMs) {
        if (m_autoTimer == nullptr || m_autoDone || m_userTouched) {
            return;
        }
        m_autoTimer->start(delayMs);
    }

    /** 拉空搜索的默认榜：**下载量从高到低**（Modrinth index=downloads / CF sortField=2），
     *  版本按**眼前这个实例的版本**筛（版本框默认就填着它）。条件不满足就**不拉**：
     *  用户改过东西 / 搜索框里有字 / 一个源都没勾 / 实例还没扫出来（基准版本还没定，等它）。 */
    void tryAutoLoad() {
        if (m_autoDone || m_userTouched || m_busy || m_search == nullptr || m_go == nullptr) {
            return;
        }
        if (!m_search->text().trimmed().isEmpty()) {
            return;
        }
        if (!isVisible()) {
            return; // 隐藏着的那一栏（模组/光影两份）不许背着用户去拉榜
        }
        m_sources = pickedSources();
        if (m_sources.isEmpty() || !m_go->isEnabled()) {
            return; // 一个源都没勾 = 没有榜可拉
        }
        if (!m_instance.isEmpty() && !m_versionScanned) {
            return; // 实例扫描还没回来：等它（回调里会再 arm 一次）
        }
        m_autoDone = true;
        std::fprintf(stderr, "[sxcl-ui] mods-default-chart: instance=%s version=%s\n",
                     m_instance.toUtf8().constData(),
                     versionFilter().isEmpty() ? "all" : versionFilter().toUtf8().constData());
        startSearch();
    }

    /** 第一次显示这一页才排默认榜（模组/光影两栏各一次；隐藏着的那一栏不拉）。 */
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        if (m_shown) {
            return;
        }
        m_shown = true;
        armAutoLoad(1000);
    }

    // ─────────────────────────── 搜索 ───────────────────────────
    /** 点「搜索」：给**勾上的每一个源**各排一个请求（先后固定，与勾选顺序无关），
     *  回来的结果合并成一份列表。没勾/搜不了的源一个请求都不发。
     *
     *  空搜索 = **下载量默认榜**（用户 2026-09-27：「没搜索模组的时候，默认填充下载量最多的模组，
     *  不管它是什么；以他最新下载或最新启动的版本为基准来选」）：
     *    * Modrinth -> index=downloads；CurseForge -> sortField=2（Popularity，从高到低）；
     *    * 版本这一维照旧按**眼前这个实例的版本**筛（版本框默认就填着它）。 */
    void startSearch() {
        cancelAutoLoad(); // 手点/回车/默认榜自己进来:默认榜那一趟不用再等了
        beginRun();       // 新的一趟开始:上一趟（可能还在飞）的结果作废
        reloadContext();
        m_sources = pickedSources();
        if (m_sources.isEmpty()) {
            m_phase = Idle;
            return; // 按钮本来就是灰的
        }
        m_rows.clear();
        m_requested.clear();
        const QByteArray text = m_search->text().trimmed().toUtf8();
        /* 有关键词 = 按相关度（上游默认）；**空搜索 = 默认榜**，按下载量排序。 */
        m_lastSort = text.isEmpty() ? QByteArrayLiteral("downloads") : QByteArrayLiteral("relevance");
        /* 搜索用的版本 = 眼前那个版本框（不再是永远用实例版本）；留空时这一维不筛。 */
        const QByteArray mc = versionFilter().toUtf8();
        printSearchFilterTrace(m_lastSort);
        /* 资源类型:模组那一栏必须钉死 "mod"（Modrinth 默认不筛类型，不钉的话光影/资源包会混进来）；
         * 光影那一栏钉 "shader"。CF 那边走 classId（6 / 6552），同一个 project_type 进去。 */
        const QByteArray type = QByteArrayLiteral("mod");
        const QByteArray shaderType = QByteArrayLiteral("shader");
        for (const QString &source : m_sources) {
            if (!sourceUsable(source)) {
                continue;
            }
            const QByteArray loader = loaderFor(source);
            sxcl_mods_query q;
            std::memset(&q, 0, sizeof(q));
            q.text = text.constData();
            q.game_version = mc.constData();
            q.loader = loader.constData();
            q.project_type = m_shaders ? shaderType.constData() : type.constData();
            q.sort = m_lastSort.constData(); // "downloads"（默认榜）/ "relevance"（关键词搜索）
            q.limit = 20;
            char url[1200];
            const bool cf = (source == QLatin1String("curseforge"));
            const int rc = cf ? sxcl_mods_curseforge_search_url(&q, url, sizeof(url))
                              : sxcl_mods_modrinth_search_url(&q, url, sizeof(url));
            if (rc != 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("条件太长"),
                              QStringLiteral("搜索条件拼不成 URL，换个短点的关键词"), this, 4000);
                return;
            }
            enqueueCandidates(source, QString::fromUtf8(url));
            m_requested << source;
        }
        clearResults();
        m_phase = Searching;
        /* 搜索这一刻单独一个 tag：勾选/重载也会打同样的内容(state)，验收脚本要能分清
         * "这一刻真去搜了哪几个源"。 */
        printSourcesTrace("mods-sources");
        if (m_pending.isEmpty()) {
            m_phase = Idle;
            return;
        }
        /* 搜索这件事**不动上下文那一行**（它是"眼前是哪个实例"的信息，被"正在搜…"顶掉就丢了）；
         * 进行中的状态写在按钮上（"查询中…"），结果就是下面那份列表。 */
        setBusy(true, QString());
        startNextFetch();
    }

    /** 这一源可以试的候选，按先后（PCL 同款"逐条试、谁先返回 JSON 用谁"）：
     *   * CurseForge -> **官方优先**（带内置 key，key 只进 x-api-key 头），**官方失败才转镜像兜底**；
     *     没有内置 key 时官方那一次必 403，照样先试官方、失败再兜底 —— 用户不需要知道也不需要配；
     *   * Modrinth -> 直连，一条。 */
    QVector<PendingFetch> candidatesFor(const QString &source, const QString &officialUrl) const {
        QVector<PendingFetch> out;
        if (source != QLatin1String("curseforge")) {
            out.append(PendingFetch{officialUrl, source, QStringLiteral("direct")});
            return out;
        }
        char mirrored[1400];
        mirrored[0] = '\0';
        const bool haveMirror =
            sxcl_mods_mirror_url(officialUrl.toUtf8().constData(), mirrored, sizeof(mirrored)) == 1;
        /* **官方优先、镜像兜底**（用户 2026-09-27 的口径）。没有内置 key 时官方那一次会 403 ——
         * 顺序照旧（拿到 key 之前它是唯一"官方"的路），失败后自动转镜像，用户什么都不用管。 */
        out.append(PendingFetch{officialUrl, source, QStringLiteral("official")});
        (void)haveMirror;
        if (haveMirror) {
            out.append(PendingFetch{QString::fromUtf8(mirrored), source, QStringLiteral("mirror")});
        }
        if (out.isEmpty()) {
            /* 理论上不会走到(官方 URL 一定认得出来):仍发官方那一条,让失败如实报出来 */
            out.append(PendingFetch{officialUrl, source, QStringLiteral("official")});
        }
        return out;
    }

    /** 把一个源的候选排进队列：第一条马上发，其余留作兜底（失败才接着试）。 */
    void enqueueCandidates(const QString &source, const QString &officialUrl) {
        const QVector<PendingFetch> cands = candidatesFor(source, officialUrl);
        m_pending.append(cands.first());
        for (int i = 1; i < cands.size(); ++i) {
            m_fallback[source].append(cands.at(i));
        }
    }

    /** 队列里下一个请求（一次只飞一个：界面层的 worker 槽只有一个，串行也最省事）。 */
    void startNextFetch() {
        if (m_pending.isEmpty()) {
            finishSearch();
            return;
        }
        const PendingFetch job = m_pending.takeFirst();
        startFetch(job);
    }

    /** 队列清空：把**合并后**的列表摆出来，并打一行取证（每个被请求过的源各多少条，0 也写出来）。 */
    void finishSearch() {
        m_phase = Idle;
        showResults(m_rows);
        const QString counts = modsPerSourceCounts(m_rows, m_requested);
        /* 取证行里的 index= 是"空搜索按下载量拉"的证据（与 mods-search 那条同一个值）；
         * perSource 每个被请求过的源各多少条（0 也写出来）。 */
        std::fprintf(stderr, "[sxcl-ui] mods-merged: sources=%s rows=%d perSource=\"%s\" index=%s\n",
                     m_requested.join(QLatin1Char(',')).toUtf8().constData(), int(m_rows.size()),
                     counts.toUtf8().constData(),
                     m_lastSort.isEmpty() ? "relevance" : m_lastSort.constData());
        setBusy(false, QString()); // 上下文那一行还给"眼前是哪个实例"
    }

    void startFetch(const PendingFetch &job) {
        m_fetchSource = job.source;
        m_fetchVia = job.via;
        /* 取证行:这一轮真的往哪个源、**哪条路**发了请求(验收脚本按它断言
         * "两个源都发了""镜像兜底真的兜上了")。 */
        std::fprintf(stderr, "[sxcl-ui] mods-fetch: source=%s via=%s url=%s\n",
                     job.source.toUtf8().constData(), job.via.toUtf8().constData(),
                     job.url.toUtf8().constData());
        ModsWorker::Request req;
        req.op = ModsWorker::FetchText;
        req.url = job.url;
        req.settingsFile = uiSettingsFilePath();
        if (job.source == QLatin1String("curseforge") && job.via == QLatin1String("official")) {
            /* key 只走**官方**那一路的请求头(x-api-key,编译期内置,见 sxcl/mods_key.h)；
             * 镜像请求不带它。任何情况下 key 都不进 URL —— URL 会进日志/错误消息/历史。 */
            req.headers << QStringLiteral("Accept: application/json");
            req.headers << QStringLiteral("x-api-key: %1").arg(compiledCfKey());
        }
        auto *worker = new ModsWorker(req, this);
        m_worker = worker;
        /* 这一趟的**编号**兜住它自己:用户又点了一次搜索（或页面打开时那趟默认榜被顶掉）之后，
         * 旧请求回来时对不上号 -> 直接丢掉，不画、不写 trace、不改状态。 */
        const int run = m_run;
        QObject::connect(worker, &ModsWorker::finished, this,
                         [this, run](bool ok, const QString &error, const QString &text, qint64) {
                             if (run != m_run) {
                                 return; // 这一趟已经被新的一次搜索顶掉了
                             }
                             m_worker = nullptr;
                             onFetchDone(ok, error, text);
                         });
        worker->start();
    }

    void onFetchDone(bool ok, const QString &error, const QString &text) {
        const Phase phase = m_phase;
        const bool cf = (m_fetchSource == QLatin1String("curseforge"));
        if (!ok) {
            /* 这一源还有候选就接着试（官方优先 + 镜像兜底 / 没 key 时只有镜像）——
             * 逐条试、谁先返回 JSON 用谁。**不换源**：绝不拿 Modrinth 的结果顶 CF。 */
            const QVector<PendingFetch> rest = m_fallback.value(m_fetchSource);
            if (!rest.isEmpty()) {
                const PendingFetch nextJob = rest.first();
                QVector<PendingFetch> tail = rest;
                tail.removeFirst();
                m_fallback.insert(m_fetchSource, tail);
                m_pending.prepend(nextJob);
                std::fprintf(stderr, "[sxcl-ui] mods-fetch-retry: source=%s next=%s\n",
                             m_fetchSource.toUtf8().constData(), nextJob.via.toUtf8().constData());
                startNextFetch();
                return;
            }
            std::fprintf(stderr, "[sxcl-ui] mods-fetch-failed: source=%s via=%s err=\"%s\"\n",
                         m_fetchSource.toUtf8().constData(), m_fetchVia.toUtf8().constData(),
                         error.toUtf8().constData());
        } else {
            m_fallback.remove(m_fetchSource); // 这一源已经拿到结果,不再试兜底
        }
        if (phase == Searching) {
            /* 搜索是**队列**驱动的:一个源失败/为空不影响另一个源 —— 这一源这一轮就是 0 条,
             * 如实写 0,绝不拿另一个源的结果顶上来冒充它。 */
            if (!ok) {
                InfoBar::push(InfoBar::Type::Warning,
                              QStringLiteral("%1 取不到").arg(modsSourceDisplayName(m_fetchSource)),
                              error, this, 8000);
            } else {
                const QByteArray body = text.toUtf8();
                char err[192];
                err[0] = '\0';
                sxcl_mod_page page;
                std::memset(&page, 0, sizeof(page));
                const int prc =
                    cf ? sxcl_mods_curseforge_search_parse(body.constData(), (size_t)body.size(),
                                                           &page, err, sizeof(err))
                       : sxcl_mods_modrinth_search_parse(body.constData(), (size_t)body.size(), &page,
                                                         err, sizeof(err));
                if (prc != 0) {
                    InfoBar::push(InfoBar::Type::Warning,
                                  QStringLiteral("%1 解析失败").arg(modsSourceDisplayName(m_fetchSource)),
                                  QString::fromUtf8(err), this, 8000);
                } else {
                    appendModsHits(m_rows, page);
                    sxcl_mods_page_free(&page);
                }
            }
            startNextFetch();
            return;
        }
        m_phase = Idle;
        if (!ok) {
            endInstallWait(); // 「安装」这一趟到这儿就结束了（失败如实说）
            setBusy(false, QStringLiteral("查不到：%1").arg(error));
            return;
        }
        const QByteArray body = text.toUtf8();
        char err[192];
        err[0] = '\0';
        if (phase == LoadingVersions) {
            std::vector<sxcl_mod_file> files(64);
            size_t count = 0;
            const int prc = cf ? sxcl_mods_curseforge_versions_parse(body.constData(), (size_t)body.size(),
                                                                    files.data(), files.size(), &count,
                                                                    err, sizeof(err))
                               : sxcl_mods_modrinth_versions_parse(body.constData(), (size_t)body.size(),
                                                                  files.data(), files.size(), &count,
                                                                  err, sizeof(err));
            if (prc != 0) {
                endInstallWait();
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("取版本失败"),
                              QString::fromUtf8(err), this, 6000);
                return;
            }
            sxcl_mod_file picked;
            std::memset(&picked, 0, sizeof(picked));
            const QByteArray pickLoader = loaderFor(m_fetchSource);
            const int pickRc =
                count > 0 ? sxcl_mods_pick_file(files.data(), count, m_mc.toUtf8().constData(),
                                                pickLoader.constData(), &picked)
                          : -1;
            std::fprintf(stderr, "[sxcl-ui] mods-install: versions count=%d pick=%d file=\"%s\"\n",
                         (int)count, pickRc, picked.filename);
            if (count > 0 && pickRc != 0) {
                /* 挑不出**唯一**那个（好几个文件都说得过去）:不摆一排按钮，改成"点一项 = 我要它"。 */
                showFileChoices(files, count);
                return;
            }
            if (count == 0) {
                endInstallWait();
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没有能用的文件"),
                              QStringLiteral("「%1」里没有匹配这个实例（版本 %2 / 加载器 %3）的文件 —— "
                                             "**不自动换加载器**，要么换个包，要么装对应加载器的版本。")
                                  .arg(m_projectTitle, m_mc.isEmpty() ? QStringLiteral("任何") : m_mc,
                                       pickLoader.isEmpty() ? QStringLiteral("任何")
                                                            : QString::fromUtf8(pickLoader)),
                              this, 9000);
                return;
            }
            startDownload(picked);
        }
    }

    /** 点结果行 = **进详情页**（用户 2026-09-27：「介绍信息强制只有一行，后面的用 3 个点代替，
     *  单击进去可以看模组详细信息」）。
     *  详情页由**这一栏**创建并持有：只有它知道眼前是哪个实例、该装进 mods 还是 shaderpacks；
     *  外壳（main_window）只负责把这一页摆到眼前（与三个临时页同一套"会话页"机制）。 */
    void openDetail(const ModsHitRow &row) {
        if (m_detail == nullptr) {
            m_detail = new ModDetailPage(this);
            m_detail->setInstallHandler([this] {
                if (!m_detailRow.id.isEmpty()) {
                    install(m_detailRow);
                }
            });
            m_detail->setFilePickHandler([this](const sxcl_mod_file &file) { startDownload(file); });
            m_detail->setBackHandler([this] {
                /* 「返回」= 回下载页（模组那一栏还在原地，列表一行都没动）——走产品换页入口。 */
                QWidget *shell = window();
                if (shell != nullptr) {
                    QMetaObject::invokeMethod(shell, "switchToRoute", Qt::DirectConnection,
                                              Q_ARG(QString, QStringLiteral("download")));
                }
            });
        }
        m_detailRow = row;
        m_detail->setItem(row);
        if (m_detail->iconLabel() != nullptr && !row.iconUrl.isEmpty()) {
            /* 同一条缓存/工作线程：列表里已经下过的话这里直接命中；拉不到就一直空着。 */
            startIcon(row.iconUrl, row.source + QLatin1Char('-') + row.id, m_detail->iconLabel(),
                      kModDetailIconSide);
        }
        /* 点的是哪一条（取证行）：点完眼前是详情页，但"哪一条"只有这条日志说了算。 */
        std::fprintf(stderr, "[sxcl-ui] mods-detail-open: id=%s source=%s title=\"%s\"\n",
                     row.id.toUtf8().constData(), row.source.toUtf8().constData(),
                     row.title.toUtf8().constData());
        QWidget *shell = window();
        if (shell == nullptr) {
            std::fprintf(stderr, "[sxcl-ui] mods-detail: 找不到外壳窗口（页面没摆上去）\n");
            return;
        }
        const QString key = m_shaders ? QStringLiteral("mods_detail_shader")
                                      : QStringLiteral("mods_detail");
        if (!QMetaObject::invokeMethod(shell, "showModDetail", Qt::DirectConnection,
                                       Q_ARG(QWidget *, m_detail), Q_ARG(QString, key))) {
            std::fprintf(stderr, "[sxcl-ui] mods-detail: 外壳没有 showModDetail（换页被丢掉）\n");
        }
    }

    /** 一个包里"好几个文件都说得过去"时：**点一项 = 我要它**（不是一排按钮）。
     *  这些可选项摆在**详情页**里（用户点的就是详情页里那个「安装」，挑也得在他眼前挑）。 */
    void showFileChoices(const std::vector<sxcl_mod_file> &files, size_t count) {
        if (m_detail == nullptr) {
            std::fprintf(stderr, "[sxcl-ui] mods-detail: 没有详情页可摆可选项\n");
            return;
        }
        m_detail->setInstalling(false); // 轮到他挑了：按钮不锁着
        m_detail->setFileChoices(files, count);
    }

    /** 「安装」这一趟结束（成功/失败/要他自己挑）——把详情页那个按钮放开。 */
    void endInstallWait() {
        if (m_detail != nullptr) {
            m_detail->setInstalling(false);
        }
    }

    void startDownload(const sxcl_mod_file &picked) {
        const char *kind = m_shaders ? "shaderpacks" : "mods";
        /* 从"可选项里点了一项"进来的也要把按钮锁上（同一个出口 endInstallWait 放开）。 */
        if (m_detail != nullptr) {
            m_detail->setInstalling(true);
        }
        char dir[1200];
        if (sxcl_mods_dir(m_gameDir.toUtf8().constData(), m_instance.toUtf8().constData(), kind,
                          versionIsolationOn() ? 1 : 0, dir, sizeof(dir)) != 0) {
            endInstallWait();
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("路径太长"),
                          QStringLiteral("模组目录拼不出来"), this, 5000);
            return;
        }
        if (sxcl_fs_mkdirs(dir) != 0) {
            endInstallWait();
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("目录建不了"),
                          QString::fromUtf8(dir), this, 6000);
            return;
        }
        const QString dest = QString::fromUtf8(dir) + QLatin1Char('/') +
                             QString::fromUtf8(picked.filename);
        m_deps = QString::fromUtf8(picked.required_deps);
        ModsWorker::Request req;
        req.op = ModsWorker::DownloadFile;
        /* CurseForge 的文件链接：**官方在前、镜像兜底**（与查询同一条口径：官方在前镜像在后）。
         * 实测这条官方直链目前回 404，引擎自己换到镜像那一条（302 -> forgecdn，206 续传）。
         * Modrinth 直连，不动。 */
        req.url = QString::fromUtf8(picked.url);
        if (m_installSource == QLatin1String("curseforge")) {
            char mirrored[1400];
            const QString direct = QString::fromUtf8(picked.url);
            if (sxcl_mods_mirror_url(direct.toUtf8().constData(), mirrored, sizeof(mirrored)) == 1) {
                const QString viaMirror = QString::fromUtf8(mirrored);
                if (viaMirror != direct) {
                    req.altUrls << viaMirror;
                }
            }
        }
        req.dest = dest;
        req.sha1 = QString::fromUtf8(picked.sha1);
        req.size = picked.size;
        req.settingsFile = uiSettingsFilePath();
        std::fprintf(stderr, "[sxcl-ui] mods-install: download file=\"%s\" size=%lld dest=\"%s\"\n",
                     picked.filename, (long long)picked.size, dest.toUtf8().constData());
        m_phase = Downloading;
        setBusy(true, QStringLiteral("正在下 %1 …（官方 sha1 有就强校验）")
                          .arg(QString::fromUtf8(picked.filename)));
        auto *worker = new ModsWorker(req, this);
        m_worker = worker;
        /* 也是这一趟的编号：下载途中用户又搜了一次的话，这一趟的状态收尾**不许**动新那一趟的
         * （m_worker/m_phase 只有一个槽），但"下好了"这件事照样如实报出来。 */
        const int run = m_run;
        QObject::connect(worker, &ModsWorker::finished, this,
                         [this, dest, run](bool ok, const QString &error, const QString &, qint64 bytes) {
                             std::fprintf(stderr, "[sxcl-ui] mods-install: done ok=%d bytes=%lld\n",
                                          ok ? 1 : 0, (long long)bytes);
                             endInstallWait(); // 装完了（成功失败都算）:详情页那个按钮放开
                             const bool current = (run == m_run);
                             if (current) {
                                 m_worker = nullptr;
                                 m_phase = Idle;
                                 setBusy(false, QString());
                             }
                             if (!ok) {
                                 InfoBar::push(InfoBar::Type::Warning, QStringLiteral("装失败"), error,
                                               this, 9000);
                                 return;
                             }
                             QString extra;
                             if (!m_deps.isEmpty()) {
                                 const QStringList deps =
                                     m_deps.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                                 extra = QStringLiteral("；依赖（只展示不装）：%1")
                                             .arg(deps.join(QStringLiteral(", ")));
                             }
                             if (current) {
                                 setBusy(false, QStringLiteral("已装好：%1")
                                                    .arg(QDir::toNativeSeparators(dest)));
                             }
                             InfoBar::push(InfoBar::Type::Success, QStringLiteral("已就位"),
                                           QStringLiteral("%1（%2 字节）%3")
                                               .arg(QDir::toNativeSeparators(dest))
                                               .arg(bytes)
                                               .arg(extra),
                                           this, 9000);
                         });
        worker->start();
    }

    /** 详情页里那**一个**「安装」动作：按当前实例的版本 + 加载器挑最合适的文件装下去
     *  （用户 2026-09-27 的口径："点击代表我要它"落在这一步）。 */
    void install(const ModsHitRow &row) {
        m_projectTitle = row.title.isEmpty() ? row.slug : row.title;
        /* 用**这一条自己的** source:合并列表里两个源的条目混在一起,不能按"当前勾选/上一个源"去解析。 */
        const QString src = modsSourceValid(row.source) ? row.source : QStringLiteral("modrinth");
        const QByteArray loader = loaderFor(src);
        /* 取证行:点「安装」这一刻按的是**眼前这个实例**的版本 + 加载器（不是列表的筛选条件）。 */
        std::fprintf(stderr,
                     "[sxcl-ui] mods-install: start id=%s source=%s instance_version=%s loader=%s\n",
                     row.id.toUtf8().constData(), src.toUtf8().constData(),
                     m_mc.isEmpty() ? "all" : m_mc.toUtf8().constData(),
                     loader.isEmpty() ? "all" : loader.constData());
        /* 这一趟在跑的时候详情页那个按钮锁住（"正在装…"）——放开的每个出口见 endInstallWait。 */
        if (m_detail != nullptr) {
            m_detail->setInstalling(true);
        }
        char url[1200];
        if (src == QLatin1String("curseforge")) {
            bool numeric = false;
            const qlonglong id = row.id.toLongLong(&numeric);
            if (!numeric || id <= 0) {
                endInstallWait();
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("工程 id 拿不到"),
                              QStringLiteral("这一条没有可用的数字 id（%1）").arg(row.id), this, 5000);
                return;
            }
            if (sxcl_mods_curseforge_versions_url(id, m_mc.toUtf8().constData(), loader.constData(),
                                                  url, sizeof(url)) != 0) {
                endInstallWait();
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("条件太长"),
                              QStringLiteral("文件列表 URL 拼不出来"), this, 5000);
                return;
            }
        } else if (sxcl_mods_modrinth_versions_url(row.id.toUtf8().constData(),
                                                   m_mc.toUtf8().constData(), loader.constData(), url,
                                                   sizeof(url)) != 1) {
            endInstallWait();
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("工程 id 拿不到"),
                          QStringLiteral("这一条没有可用的 id"), this, 5000);
            return;
        }
        m_installSource = src;
        m_phase = LoadingVersions;
        beginRun(); // 装这一条:上一趟（可能还在飞）的搜索/取版本作废
        enqueueCandidates(src, QString::fromUtf8(url)); // 文件列表也走同一条兜底
        setBusy(true, QStringLiteral("正在取「%1」的版本列表…").arg(m_projectTitle));
        startNextFetch();
    }

    /** 结果行 = **固定 4 行**（用户 2026-09-27）：
     *    ① 名字 + 右侧来源   ② 开发商 + 版本范围（"1.20 – 1.21.4"）
     *    ③ 只占一行的介绍（超出就省略号）④ 留空
     *  行高**固定**（同一套字号量出来的数，每张卡一样高）；logo 是固定方块，不许撑高行；
     *  行上**一个按钮都没有**：点这一行 = **进详情页**（用户最新口径；装是详情页里那一个「安装」动作）。 */
    void showResults(const QVector<ModsHitRow> &rows) {
        if (rows.isEmpty()) {
            auto *empty = new BodyLabel(QStringLiteral("没找到。换个关键词再试。"),
                                       m_list->parentWidget());
            empty->setWordWrap(true);
            m_list->addWidget(empty);
            return;
        }
        const QColor secondary = pageTokenColor("textSecondary");
        QFont titleFont = font();
        titleFont.setPixelSize(15);
        titleFont.setWeight(QFont::DemiBold);
        QFont metaFont = font();
        metaFont.setPixelSize(12);
        const QFontMetrics fmTitle(titleFont);
        const QFontMetrics fmMeta(metaFont);
        for (const ModsHitRow &row : rows) {
            auto *card = new CardWidget(m_list->parentWidget());
            auto *rowLay = new QHBoxLayout(card);
            rowLay->setContentsMargins(16, 10, 16, 10);
            rowLay->setSpacing(12);

            /* logo：**固定方块**（用户 2026-09-27：「模组 logo 强制以我们的布局为主，获取到图片后
             * 强制放在模组栏里，不能上下延伸」）—— 图标按比例缩进这个方块，行高由文字那 4 行定。
             * 拉不到图标就**什么都不画**（连底框都不画），位置照样占着，行与行才对得齐。 */
            auto *icon = new QLabel(card);
            icon->setObjectName(QStringLiteral("modsRowIcon"));
            icon->setFixedSize(kIconSide, kIconSide);
            icon->setAlignment(Qt::AlignCenter);
            icon->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));
            rowLay->addWidget(icon, 0, Qt::AlignVCenter);
            if (!row.iconUrl.isEmpty()) {
                /* 缓存文件名带上源:两个源的 id 可能撞(CF 是数字、Modrinth 是短 id),
                 * 只按 id 存会把一个源的图标当成另一个源的。 */
                startIcon(row.iconUrl, row.source + QLatin1Char('-') + row.id, icon);
            }

            auto *textCol = new QVBoxLayout();
            textCol->setContentsMargins(0, 0, 0, 0);
            textCol->setSpacing(2);

            /* ① 名字（占满左边，装不下省略号）+ 右侧来源标记。 */
            auto *line1 = new QHBoxLayout();
            line1->setSpacing(8);
            auto *title = new BodyLabel(row.title, card);
            title->setObjectName(QStringLiteral("modsRowTitle"));
            title->setFont(titleFont);
            title->setToolTip(row.title);
            makeLabelElide(title, 60);
            line1->addWidget(title, 1);
            /* 来源标记:合并列表里"这条是谁家的"必须一眼看出来(不写句子,一个小标签)。
             * 稳定 objectName:验收脚本按它逐行断言"没有一行冒充别的源"。 */
            auto *tag = new BodyLabel(modsSourceDisplayName(row.source), card);
            tag->setObjectName(QStringLiteral("modsSourceTag"));
            tag->setWordWrap(false); // 行高是钉死的：这一行绝不许折成两行
            tag->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                   .arg(pageTokenText("textTertiary")));
            line1->addWidget(tag, 0, Qt::AlignRight | Qt::AlignVCenter);
            textCol->addLayout(line1);

            /* ② 开发商 + 版本范围（左）；下载量 + 更新时间（右，都是**单位化/相对时间**）。
             *  CF 的搜索响应里没有作者时如实少一段,不编一个名字出来；范围认不出就只写作者。 */
            auto *line2 = new QHBoxLayout();
            line2->setSpacing(8);
            QStringList meta;
            if (!row.author.isEmpty()) {
                meta << row.author;
            }
            if (!row.versionsMin.isEmpty()) {
                meta << (row.versionsMin == row.versionsMax
                             ? row.versionsMin
                             : QStringLiteral("%1 – %2").arg(row.versionsMin, row.versionsMax));
            }
            auto *dev = new BodyLabel(meta.join(QStringLiteral(" · ")), card);
            dev->setObjectName(QStringLiteral("modsRowDev"));
            dev->setFont(metaFont);
            dev->setTextColor(secondary, secondary);
            makeLabelElide(dev, 40);
            line2->addWidget(dev, 1);
            QStringList stats;
            stats << QStringLiteral("%1次下载").arg(modsDownloadText(row.downloads));
            const QString when = modsUpdatedText(row.updated, QDateTime::currentDateTimeUtc());
            if (!when.isEmpty()) {
                stats << when;
            }
            auto *numbers = new BodyLabel(stats.join(QStringLiteral(" · ")), card);
            numbers->setObjectName(QStringLiteral("modsRowStats"));
            numbers->setWordWrap(false); // 同上：下载量与更新时间就占一行
            numbers->setFont(metaFont);
            numbers->setTextColor(secondary, secondary);
            line2->addWidget(numbers, 0, Qt::AlignRight | Qt::AlignVCenter);
            textCol->addLayout(line2);

            /* ③ 介绍**只占一行**：超出省略号（单击整行这件事见下面那段，口径不变）。
             *  整份介绍挂在悬停提示里 —— 不占行、也不多一个控件。 */
            auto *desc = new BodyLabel(row.description, card);
            desc->setObjectName(QStringLiteral("modsRowDesc"));
            desc->setFont(metaFont);
            desc->setTextColor(secondary, secondary);
            makeLabelElide(desc, 40);
            if (!row.description.isEmpty()) {
                desc->setToolTip(row.description);
            }
            textCol->addWidget(desc);

            /* ④ 第 4 行先留空（占住行高，四行的行距与基线才固定）。 */
            auto *blank = new BodyLabel(QString(), card);
            blank->setObjectName(QStringLiteral("modsRowBlank"));
            blank->setWordWrap(false);
            blank->setFont(metaFont);
            textCol->addWidget(blank);

            /* 4 行的高度：按这套字号量（含标签自己的内边距），行高**钉死**，每张卡一样高。 */
            const int hTitle = qMax(fmTitle.height(), title->sizeHint().height());
            const int hMeta = qMax(qMax(fmMeta.height(), desc->sizeHint().height()),
                                   numbers->sizeHint().height());
            title->setFixedHeight(hTitle);
            dev->setFixedHeight(hMeta);
            desc->setFixedHeight(hMeta);
            blank->setFixedHeight(hMeta);
            numbers->setFixedHeight(hMeta);
            card->setFixedHeight(qMax(kIconSide + 2 * 10, 2 * 10 + hTitle + 3 * hMeta + 3 * 2));

            rowLay->addLayout(textCol, 1);

            /* 整行可点（用户 2026-09-27：「都做成他妈选项卡没有按钮，点击就是代表我要点它」）——
             * 但**点一下的落点**后来定了：「介绍只有一行 + 省略号，单击进去可以看模组详细信息」，
             * 所以点行 = **进详情页**（装是详情页里那**一个**「安装」动作）。
             * 用 CardWidget 自带的 clicked + hover/pressed 背景过渡(120ms) + 手型光标 ——
             * 小白一眼就知道整块能点。行上**一个按钮都没有**。 */
            card->setObjectName(QStringLiteral("modsResultCard"));
            card->setCursor(Qt::PointingHandCursor);
            const ModsHitRow copy = row; /* 值拷贝:卡片被清掉后回调还要用 */
            QObject::connect(card, &CardWidget::clicked, this, [this, copy] { openDetail(copy); });
            m_list->addWidget(card);
        }
    }

    /** 取一张图标（工作线程下到 <数据根>/icons/<id>.png，命中缓存就不再下）。
     *  target 用 QPointer 兜着:卡片可能已经被 clearResults() 删掉了。
     *  side = 目标方块的边长（列表 48 / 详情页 96）—— 同一份缓存，按格子大小缩。 */
    void startIcon(const QString &url, const QString &id, QLabel *target, int side = kIconSide) {
        if (url.isEmpty() || id.isEmpty() || target == nullptr) {
            return;
        }
        const QString dir = uiLauncherDataRoot() + QStringLiteral("/icons");
        (void)QDir().mkpath(dir);
        /* 文件名只用 id 的"安全字符":CF 的 id 是数字、Modrinth 是短 id,
         * 但真出格字符时不能让它跑到路径里去。 */
        QString safe = id;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
        const QString path = dir + QLatin1Char('/') + safe + QStringLiteral(".png");
        if (QFile::exists(path)) {
            setIconFromFile(path, target, side);
            return;
        }
        ModsWorker::Request req;
        req.op = ModsWorker::DownloadFile;
        req.url = url;
        req.dest = path;
        req.settingsFile = uiSettingsFilePath();
        auto *worker = new ModsWorker(req, this);
        const QPointer<QLabel> guard(target);
        QObject::connect(worker, &ModsWorker::finished, this,
                         [guard, path, side](bool ok, const QString &, const QString &, qint64) {
                             if (ok && !guard.isNull()) {
                                 setIconFromFile(path, guard.data(), side);
                             }
                         });
        worker->start();
    }

    static void setIconFromFile(const QString &path, QLabel *target, int side = kIconSide) {
        QPixmap pm(path);
        if (pm.isNull() || target == nullptr) {
            return; // 解不开的图 = 什么都不画（不拿占位图冒充）
        }
        /* **按比例缩进固定方块**：不拉伸、不裁切，也绝不让图片决定行高。 */
        target->setPixmap(pm.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    bool m_shaders = false;
    Phase m_phase = Idle;
    /** 一次搜索要跑的几个请求(勾了几个源就有几个),**串行**发:worker 槽只有一个。 */
    QStringList m_sources;            // 勾上的源(固定顺序,与勾选先后无关)
    QStringList m_requested;          // 这一轮真发过请求的源(计数与取证都用它)
    QVector<PendingFetch> m_pending;  // 还没发的请求
    QHash<QString, QVector<PendingFetch>> m_fallback; // 源 -> 还没试的候选(失败才接着试)
    QVector<ModsHitRow> m_rows;       // 合并后的结果(界面只认这一份)
    bool m_busy = false;              // 有没有活正在跑(搜索/取版本/下载)
    QString m_fetchSource;
    QString m_fetchVia;               // 当前这一次走的是哪条路(失败取证用)
    QString m_installSource;          // 正在装的那条结果属于哪个源(决定下载候选)
    QString m_gameDir;
    QString m_instance;
    QString m_mc;      // 拿去筛版本的**原版版本号**:只认版本文件里的,认不出就是空(=不筛)
    QString m_loader;
    BgTask *m_versionTask = nullptr; // 实例扫描(磁盘活,**只许工作线程**)
    VersionRowInfo m_versionRow;     // 工作线程写、界面线程读(队列投递保证先后)
    bool m_versionScanned = false;   // 这一趟扫完了吗(取证行里区分 pending 与"扫过但认不出")
    bool m_versionFound = false;     // 目录里真有这个实例吗
    QString m_versionError;          // 扫不动时的人话(也进取证行)
    QString m_currentText;
    QString m_projectTitle;
    QString m_deps;
    SearchLineEdit *m_search = nullptr;
    PrimaryPushButton *m_go = nullptr;
    BodyLabel *m_context = nullptr;
    FlowLayout *m_filterFlow = nullptr;        // 筛选区**第 2 行**（流式：放不下换行，不压控件）
    LineEdit *m_versionEdit = nullptr;         // 版本筛选：**可输入**（留空 = 全部）
    bool m_versionAuto = false;                // 程序回填默认值时不要当成"用户改了"
    bool m_versionFilterTouched = false;       // 用户动过输入框就不再自动改它
    QTimer *m_autoTimer = nullptr;             // 默认榜的"静默期"计时器
    bool m_shown = false;                      // 这一页显示过没有（默认榜只在第一次显示时排）
    bool m_autoDone = false;                   // 默认榜排过了（或条件不满足已经放弃）
    bool m_userTouched = false;                // 用户动过任何一件筛选 -> 默认榜不再自动拉
    QString m_autoInstance;                    // 上一次默认榜用的实例（换实例要重拉）
    int m_run = 0;                             // 这一趟请求链的编号（旧的回来就丢掉）
    QByteArray m_lastSort;                     // 这一趟的排序（"downloads" / "relevance"），取证行用
    ModDetailPage *m_detail = nullptr;         // 点一行进去的详情页（这一栏创建并持有，外壳只负责摆）
    ModsHitRow m_detailRow;                    // 详情页眼前是哪一条（「安装」按它装）
    QHash<QString, QCheckBox *> m_sourceBoxes; // "modrinth" / "curseforge" -> 那个勾选框
    QVBoxLayout *m_list = nullptr;
    ModsWorker *m_worker = nullptr;
};

} // namespace

QWidget *createModsPage(QWidget *parent, bool shaders) { return new ModsPane(parent, shaders); }

} // namespace sxcl::ui
