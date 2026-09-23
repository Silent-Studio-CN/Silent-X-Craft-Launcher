/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 下载页的「拼图（MOD）」与「太阳（光影）」两栏（docs/22 的 A1，走 PCL 线路）。
//
// 顶部三行：当前实例上下文 / 搜索框 / **来源滑块**（Modrinth 免 key，CurseForge 要官方 key）；
// 下面    ：结果卡片（图标 / 标题 / 作者 / 下载量 / 简介 + 一键「装」）。
//
// 口径（与 sxcl/mods.h 完全一致）：
//   * **两个源**：Modrinth（免 key）+ CurseForge（官方 API 必须带 x-api-key）。
//     **没配 key 就不发请求**，如实告诉用户缺什么 —— 绝不改用另一个源假装是 CF 的结果；
//   * 筛选交给**服务端**（Modrinth 的 facets / CF 的 classId + modLoaderType），本地不过滤；
//   * **不自动换加载器**：实例是 Fabric 就只列/只挑 Fabric 的文件，挑不出来就如实说；
//   * 依赖**只展示不装**（装完把 required 的 id 打在提示里）；
//   * 装进**版本隔离**后的目录（<游戏>/versions/<实例>/mods），没开隔离就还是根 mods/；
//   * 网络全在工作线程（取 JSON / 下文件 / 下图标），界面线程一个字节都不等。

#include "page_factory.h"
#include "game_folders.h"
#include "page_shell.h"

#include "../workers/mods_worker.h"
#include "../workers/ui_paths.h"

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
#include "fluent/fluent_segmented.h"   // Pivot:来源滑块(与主页离线/正版同一个形态)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QDir>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <vector>

#include "sxcl/fs.h"
#include "sxcl/mods.h"
#include "sxcl/settings.h"

namespace sxcl::ui {
namespace {

// 设置键（模组页自己也写这两个：key 就在这一页上填，没必要逼用户翻到设置页）。
const char *const kKeyModsSource = "mods.source";             // "modrinth" / "curseforge"
const char *const kKeyCfApiKey = "mods.curseforge_api_key";   // CurseForge 官方 API key

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
        reloadContext();
    }

private:
    enum Phase { Idle, Searching, LoadingVersions, Downloading };

    // ─────────────────────────── 搭界面 ───────────────────────────
    void build() {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);

        m_context = new BodyLabel(QString(), this);
        m_context->setWordWrap(true);
        const QColor secondary = pageTokenColor("textSecondary");
        m_context->setTextColor(secondary, secondary);
        lay->addWidget(m_context);

        auto *row = new QHBoxLayout();
        row->setSpacing(8);
        m_search = new SearchLineEdit(this);
        m_search->setPlaceholderText(m_shaders ? QStringLiteral("搜光影包")
                                               : QStringLiteral("搜模组"));
        m_search->setFixedHeight(34);
        /* 稳定的 objectName:验收钩子(SXCL_UI_MODS_QUERY)靠它**往真控件里打字再点真的搜索按钮**,
         * 而不是在 main.cpp 里另起一条"自己调核心库"的旁路(那样验不到接线本身)。 */
        m_search->setObjectName(QStringLiteral("modsSearchBox"));
        row->addWidget(m_search, 1);
        m_go = new PrimaryPushButton(QStringLiteral("搜索"), this);
        applyButtonFont(m_go);
        m_go->setObjectName(QStringLiteral("modsSearchButton"));
        m_go->setFixedHeight(34);
        row->addWidget(m_go, 0);
        lay->addLayout(row);

        // ── 来源滑块（PCL 那样的滑动选项；与主页离线/正版同一个控件） ──
        auto *sourceRow = new QWidget(this);
        auto *sourceLay = new QHBoxLayout(sourceRow);
        sourceLay->setContentsMargins(0, 0, 0, 0);
        sourceLay->setSpacing(8);
        m_sourcePivot = new Pivot(sourceRow);
        m_sourcePivot->addItem(QStringLiteral("modrinth"), QStringLiteral("Modrinth"));
        m_sourcePivot->addItem(QStringLiteral("curseforge"), QStringLiteral("CurseForge"));
        m_sourcePivot->setIndicatorColor(FluentTheme::instance().tokens().accent,
                                        FluentTheme::instance().tokens().accent);
        for (const QString &key : {QStringLiteral("modrinth"), QStringLiteral("curseforge")}) {
            if (PivotItem *it = m_sourcePivot->item(key)) {
                QFont f = it->font();
                f.setPixelSize(14);
                f.setWeight(QFont::DemiBold);
                it->setFont(f);
                it->setProperty("hasIcon", false);
                it->setFixedHeight(34);
                it->setCursor(Qt::PointingHandCursor);
            }
        }
        m_sourcePivot->setFixedHeight(38);
        m_sourcePivot->setStyleSheet(
            QStringLiteral("Pivot { background: transparent; border: none; }"
                           "PivotItem { background: transparent; border: none; padding: 4px 10px; }"
                           "PivotItem[isSelected='true'] { color: %1; }"
                           "PivotItem[isSelected='false'] { color: %2; }")
                .arg(pageTokenText("accent"), pageTokenText("textSecondary")));
        sourceLay->addWidget(m_sourcePivot, 0, Qt::AlignLeft);

        m_sourceNote = new BodyLabel(QString(), sourceRow);
        m_sourceNote->setTextColor(secondary, secondary);
        m_sourceNote->setWordWrap(true);
        sourceLay->addWidget(m_sourceNote, 1);
        lay->addWidget(sourceRow);

        // ── CurseForge 的 key 那一栏（只在这一源被选中时才出现） ──
        m_keyRow = new QWidget(this);
        auto *keyLay = new QHBoxLayout(m_keyRow);
        keyLay->setContentsMargins(0, 0, 0, 0);
        keyLay->setSpacing(8);
        m_keyEdit = new QLineEdit(m_keyRow);
        m_keyEdit->setFixedHeight(34);
        m_keyEdit->setPlaceholderText(QStringLiteral(
            "CurseForge API Key（在 curseforge.com 申请；只存在本地设置里）"));
        m_keyEdit->setObjectName(QStringLiteral("modsCfKeyEdit"));
        keyLay->addWidget(m_keyEdit, 1);
        auto *keySave = new PushButton(QStringLiteral("保存"), m_keyRow);
        applyButtonFont(keySave);
        keySave->setFixedHeight(34);
        keyLay->addWidget(keySave, 0);
        lay->addWidget(m_keyRow);

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

        QObject::connect(m_go, &QAbstractButton::clicked, this, [this] { startSearch(); });
        QObject::connect(m_search, &QLineEdit::returnPressed, this, [this] { startSearch(); });
        QObject::connect(m_sourcePivot, &Pivot::currentItemChanged, this,
                         [this](const QString &key) { setSource(key); });
        QObject::connect(keySave, &QAbstractButton::clicked, this, [this] { saveKeyFromEdit(); });
        QObject::connect(m_keyEdit, &QLineEdit::returnPressed, this, [this] { saveKeyFromEdit(); });
    }

    // ─────────────────────────── 设置 ───────────────────────────
    void reloadSettings() {
        const QByteArray path = uiSettingsFilePath().toUtf8();
        sxcl_settings *st = sxcl_settings_open(path.constData());
        if (st == nullptr) {
            m_source = QStringLiteral("modrinth");
            m_cfKey.clear();
            return;
        }
        m_source = QString::fromUtf8(sxcl_settings_get(st, kKeyModsSource, "modrinth")).toLower();
        if (m_source != QLatin1String("curseforge")) {
            m_source = QStringLiteral("modrinth");
        }
        m_cfKey = QString::fromUtf8(sxcl_settings_get(st, kKeyCfApiKey, "")).trimmed();
        sxcl_settings_free(st);
        if (m_sourcePivot != nullptr) {
            m_sourcePivot->setCurrentItem(m_source);   // 触发 setSource -> 刷新说明与 key 那一栏
            refreshSourceUi();
        }
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

    bool curseforge() const { return m_source == QLatin1String("curseforge"); }

    void setSource(const QString &key) {
        const QString next = (key == QLatin1String("curseforge")) ? QStringLiteral("curseforge")
                                                                  : QStringLiteral("modrinth");
        if (next == m_source) {
            refreshSourceUi();
            return;
        }
        m_source = next;
        saveSetting(kKeyModsSource, m_source);
        clearResults();   // 上一个源的结果不能留在屏幕上冒充这一个源的
        reloadContext();
        refreshSourceUi();
    }

    /** 来源说明 + key 那一栏的显隐（"没配 key" 这件事必须写在脸上）。 */
    void refreshSourceUi() {
        if (m_sourceNote == nullptr) {
            return;
        }
        if (!curseforge()) {
            m_sourceNote->setText(QStringLiteral("免 key，直接就能搜。"));
            if (m_keyRow != nullptr) {
                m_keyRow->setVisible(false);
            }
            return;
        }
        if (m_cfKey.isEmpty()) {
            m_sourceNote->setText(QStringLiteral(
                "官方 API 要求带 x-api-key。还没配 key —— 这一源**不会发请求**，请把上面那栏填上。"));
            if (m_keyRow != nullptr) {
                m_keyRow->setVisible(true);
            }
        } else {
            m_sourceNote->setText(QStringLiteral("已配置 key（%1…）—— 请求时放在 x-api-key 头里，不进 URL。")
                                      .arg(m_cfKey.left(4)));
            if (m_keyRow != nullptr) {
                m_keyRow->setVisible(false);
            }
        }
    }

    void saveKeyFromEdit() {
        if (m_keyEdit == nullptr) {
            return;
        }
        const QString typed = m_keyEdit->text().trimmed();
        m_cfKey = typed;
        saveSetting(kKeyCfApiKey, typed);
        m_keyEdit->clear();
        refreshSourceUi();
        if (typed.isEmpty()) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("key 清空了"),
                          QStringLiteral("CurseForge 这一源现在不会发请求（不会拿别的源的结果冒充它）"),
                          this, 5000);
        } else {
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("key 已保存"),
                          QStringLiteral("存在本地设置文件里；请求时放在 x-api-key 头里，绝不写进 URL。"),
                          this, 5000);
        }
    }

    // ─────────────────────────── 上下文 ───────────────────────────
    void reloadContext() {
        m_gameDir = uiGameDirectory();
        m_instance = selectedVersionName();
        const QString tag = versionLoaderTag(m_gameDir, m_instance);
        m_loader = tag.isEmpty() ? QString() : tag.toLower();
        /* 实例名常是 <mc>-<loader>-<版本>(如 1.20.1-fabric-0.15.11)——取头一段当游戏版本。
         * 版本隔离之后"实例名 = 版本目录名",PCL 口径下它本来就可以随便叫,
         * 所以这里**只做一次很保守的猜测**,猜不出来就不筛版本(交给 facets 里的加载器兜着)。 */
        m_mc = m_instance;
        if (m_mc.contains(QLatin1Char('-'))) {
            const QString head = m_mc.section(QLatin1Char('-'), 0, 0);
            if (head.startsWith(QLatin1String("1.")) || head.startsWith(QLatin1String("2."))) {
                m_mc = head;
            }
        }
        m_currentText = QStringLiteral("当前实例：%1 · 游戏版本 %2 · 加载器 %3 · %4 · 源 %5")
                            .arg(m_instance.isEmpty() ? QStringLiteral("（还没选版本）") : m_instance,
                                 m_mc.isEmpty() ? QStringLiteral("不筛") : m_mc,
                                 m_loader.isEmpty() ? QStringLiteral("原版（不筛加载器）") : m_loader,
                                 versionIsolationOn()
                                     ? QStringLiteral("版本隔离已开（装进实例自己的目录）")
                                     : QStringLiteral("版本隔离没开，会装进根目录"),
                                 curseforge() ? QStringLiteral("CurseForge") : QStringLiteral("Modrinth"));
        m_context->setText(m_currentText);
    }

    /** 这一栏实际拿去做筛选、挑文件的加载器：光影那一栏换加载器自己的 slug（见上）。 */
    QByteArray effectiveLoader() const {
        const QByteArray loader = m_loader.toUtf8();
        if (!m_shaders) {
            return loader;
        }
        return shaderLoaderSlug(loader, curseforge());
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
        m_go->setEnabled(!busy);
        m_go->setText(busy ? QStringLiteral("查询中…") : QStringLiteral("搜索"));
        m_context->setText(text.isEmpty() ? m_currentText : text);
    }

    // ─────────────────────────── 搜索 ───────────────────────────
    void startSearch() {
        if (m_worker != nullptr) {
            return;
        }
        reloadContext();
        const bool cf = curseforge();
        if (cf && m_cfKey.isEmpty()) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("CurseForge 还没配 API Key"),
                          QStringLiteral("官方 API 必须带 x-api-key 才回数据。key 请填在上面那一栏"
                                         "（设置页里也有）。没填我们就不发这个请求 —— "
                                         "也不会改用 Modrinth 假装是 CurseForge 的结果。"),
                          this, 10000);
            if (m_keyRow != nullptr) {
                m_keyRow->setVisible(true);
            }
            if (m_keyEdit != nullptr) {
                m_keyEdit->setFocus();
            }
            return;
        }
        sxcl_mods_query q;
        std::memset(&q, 0, sizeof(q));
        const QByteArray text = m_search->text().trimmed().toUtf8();
        const QByteArray mc = m_mc.toUtf8();
        const QByteArray loader = effectiveLoader();
        /* 资源类型:模组那一栏必须钉死 "mod"（Modrinth 默认不筛类型，不钉的话光影/资源包会混进来）；
         * 光影那一栏钉 "shader"。CF 那边走 classId（6 / 6552），同一个 project_type 进去。 */
        const QByteArray type = QByteArrayLiteral("mod");
        const QByteArray shaderType = QByteArrayLiteral("shader");
        q.text = text.constData();
        q.game_version = mc.constData();
        q.loader = loader.constData();
        q.project_type = m_shaders ? shaderType.constData() : type.constData();
        q.limit = 20;
        char url[1200];
        const int rc = cf ? sxcl_mods_curseforge_search_url(&q, url, sizeof(url))
                          : sxcl_mods_modrinth_search_url(&q, url, sizeof(url));
        if (rc != 0) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("条件太长"),
                          QStringLiteral("搜索条件拼不成 URL，换个短点的关键词"), this, 4000);
            return;
        }
        clearResults();
        m_phase = Searching;
        setBusy(true, cf ? QStringLiteral("正在搜 CurseForge…（classId %1 + 加载器 %2；key 走 x-api-key 头）")
                               .arg(m_shaders ? 6552 : 6)
                               .arg(loader.isEmpty() ? QStringLiteral("不筛") : QString::fromUtf8(loader))
                       : QStringLiteral("正在搜 Modrinth…（筛选走服务端 facets：版本 %1 / 加载器 %2）")
                             .arg(m_mc.isEmpty() ? QStringLiteral("全部") : m_mc,
                                  loader.isEmpty() ? QStringLiteral("全部") : QString::fromUtf8(loader)));
        startFetch(QString::fromUtf8(url), cf ? QStringLiteral("curseforge") : QStringLiteral("modrinth"));
    }

    void startFetch(const QString &url, const QString &source) {
        m_fetchSource = source;
        ModsWorker::Request req;
        req.op = ModsWorker::FetchText;
        req.url = url;
        req.settingsFile = uiSettingsFilePath();
        if (source == QLatin1String("curseforge")) {
            /* key 放在**请求头**里:x-api-key。绝不进 URL(URL 会进日志/错误消息)。 */
            req.headers << QStringLiteral("Accept: application/json");
            req.headers << QStringLiteral("x-api-key: %1").arg(m_cfKey);
        }
        auto *worker = new ModsWorker(req, this);
        m_worker = worker;
        QObject::connect(worker, &ModsWorker::finished, this,
                         [this](bool ok, const QString &error, const QString &text, qint64) {
                             m_worker = nullptr;
                             onFetchDone(ok, error, text);
                         });
        worker->start();
    }

    void onFetchDone(bool ok, const QString &error, const QString &text) {
        const Phase phase = m_phase;
        const bool cf = (m_fetchSource == QLatin1String("curseforge"));
        m_phase = Idle;
        if (!ok) {
            setBusy(false, QStringLiteral("查不到：%1").arg(error));
            return;
        }
        const QByteArray body = text.toUtf8();
        char err[192];
        err[0] = '\0';
        if (phase == Searching) {
            sxcl_mod_page page;
            const int prc = cf ? sxcl_mods_curseforge_search_parse(body.constData(), (size_t)body.size(),
                                                                  &page, err, sizeof(err))
                               : sxcl_mods_modrinth_search_parse(body.constData(), (size_t)body.size(),
                                                                 &page, err, sizeof(err));
            if (prc != 0) {
                setBusy(false, QStringLiteral("解析失败：%1").arg(QString::fromUtf8(err)));
                return;
            }
            const size_t total = page.total;
            const size_t count = page.count;
            showResults(page);
            sxcl_mods_page_free(&page);
            setBusy(false, QStringLiteral("共命中 %1 条（这一页 %2 条）· 点「装」直接进 %3")
                              .arg(total)
                              .arg(count)
                              .arg(versionIsolationOn() ? QStringLiteral("实例自己的目录")
                                                        : QStringLiteral("根目录")));
            return;
        }
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
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("取版本失败"),
                              QString::fromUtf8(err), this, 6000);
                return;
            }
            sxcl_mod_file picked;
            std::memset(&picked, 0, sizeof(picked));
            const QByteArray pickLoader = effectiveLoader();
            if (count == 0 ||
                sxcl_mods_pick_file(files.data(), count, m_mc.toUtf8().constData(),
                                    pickLoader.constData(), &picked) != 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没有能用的文件"),
                              QStringLiteral("「%1」里没有匹配这个实例（版本 %2 / 加载器 %3）的文件 —— "
                                             "PCL 口径**不自动换加载器**，要么换个包，要么装对应加载器的版本。")
                                  .arg(m_projectTitle, m_mc.isEmpty() ? QStringLiteral("任何") : m_mc,
                                       pickLoader.isEmpty() ? QStringLiteral("任何")
                                                            : QString::fromUtf8(pickLoader)),
                              this, 9000);
                return;
            }
            startDownload(picked);
        }
    }

    void startDownload(const sxcl_mod_file &picked) {
        const char *kind = m_shaders ? "shaderpacks" : "mods";
        char dir[1200];
        if (sxcl_mods_dir(m_gameDir.toUtf8().constData(), m_instance.toUtf8().constData(), kind,
                          versionIsolationOn() ? 1 : 0, dir, sizeof(dir)) != 0) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("路径太长"),
                          QStringLiteral("模组目录拼不出来"), this, 5000);
            return;
        }
        if (sxcl_fs_mkdirs(dir) != 0) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("目录建不了"),
                          QString::fromUtf8(dir), this, 6000);
            return;
        }
        const QString dest = QString::fromUtf8(dir) + QLatin1Char('/') +
                             QString::fromUtf8(picked.filename);
        m_deps = QString::fromUtf8(picked.required_deps);
        ModsWorker::Request req;
        req.op = ModsWorker::DownloadFile;
        req.url = QString::fromUtf8(picked.url);
        req.dest = dest;
        req.sha1 = QString::fromUtf8(picked.sha1);
        req.size = picked.size;
        req.settingsFile = uiSettingsFilePath();
        m_phase = Downloading;
        setBusy(true, QStringLiteral("正在下 %1 …（官方 sha1 有就强校验）")
                          .arg(QString::fromUtf8(picked.filename)));
        auto *worker = new ModsWorker(req, this);
        m_worker = worker;
        QObject::connect(worker, &ModsWorker::finished, this,
                         [this, dest](bool ok, const QString &error, const QString &, qint64 bytes) {
                             m_worker = nullptr;
                             m_phase = Idle;
                             if (!ok) {
                                 setBusy(false, QString());
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
                             setBusy(false, QStringLiteral("已装好：%1")
                                                .arg(QDir::toNativeSeparators(dest)));
                             InfoBar::push(InfoBar::Type::Success, QStringLiteral("已就位"),
                                           QStringLiteral("%1（%2 字节）%3")
                                               .arg(QDir::toNativeSeparators(dest))
                                               .arg(bytes)
                                               .arg(extra),
                                           this, 9000);
                         });
        worker->start();
    }

    void install(const sxcl_mod_hit &hit) {
        m_projectTitle = hit.title[0] != '\0' ? QString::fromUtf8(hit.title)
                                              : QString::fromUtf8(hit.slug);
        /* 用**这一条自己的** source:结果可能还是上一个源留下的,不能按当前滑块去解析。 */
        const QString src = QString::fromUtf8(hit.source) == QLatin1String("curseforge")
                                ? QStringLiteral("curseforge")
                                : QStringLiteral("modrinth");
        char url[1200];
        if (src == QLatin1String("curseforge")) {
            bool numeric = false;
            const qlonglong id = QString::fromUtf8(hit.id).toLongLong(&numeric);
            if (!numeric || id <= 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("工程 id 拿不到"),
                              QStringLiteral("这一条没有可用的数字 id（%1）")
                                  .arg(QString::fromUtf8(hit.id)),
                              this, 5000);
                return;
            }
            if (sxcl_mods_curseforge_versions_url(id, m_mc.toUtf8().constData(),
                                                  effectiveLoader().constData(), url,
                                                  sizeof(url)) != 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("条件太长"),
                              QStringLiteral("文件列表 URL 拼不出来"), this, 5000);
                return;
            }
        } else if (sxcl_mods_modrinth_versions_url(hit.id, m_mc.toUtf8().constData(),
                                                  effectiveLoader().constData(), url,
                                                  sizeof(url)) != 1) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("工程 id 拿不到"),
                          QStringLiteral("这一条没有可用的 id"), this, 5000);
            return;
        }
        m_phase = LoadingVersions;
        setBusy(true, QStringLiteral("正在取「%1」的版本列表…").arg(m_projectTitle));
        startFetch(QString::fromUtf8(url), src);
    }

    void showResults(const sxcl_mod_page &page) {
        if (page.count == 0) {
            auto *empty = new BodyLabel(
                QStringLiteral("没找到。换个关键词或关掉筛选条件（版本 %1 / 加载器 %2）再试。")
                    .arg(m_mc.isEmpty() ? QStringLiteral("全部") : m_mc,
                         effectiveLoader().isEmpty() ? QStringLiteral("全部")
                                                     : QString::fromUtf8(effectiveLoader())),
                m_list->parentWidget());
            empty->setWordWrap(true);
            m_list->addWidget(empty);
            return;
        }
        for (size_t i = 0; i < page.count; ++i) {
            const sxcl_mod_hit &hit = page.items[i];
            auto *card = new CardWidget(m_list->parentWidget());
            card->setMinimumHeight(66);
            auto *rowLay = new QHBoxLayout(card);
            rowLay->setContentsMargins(16, 10, 16, 10);
            rowLay->setSpacing(12);

            /* 图标位：先摆一个空框，图标在工作线程下回来再填 ——
             * 界面绝不为了一个图标卡住（用户点名过"未响应"）。取不到就留空框，不造假图。 */
            auto *icon = new QLabel(card);
            icon->setFixedSize(40, 40);
            icon->setStyleSheet(QStringLiteral("QLabel { background: rgba(255,255,255,0.06);"
                                               " border-radius: 8px; }"));
            rowLay->addWidget(icon, 0, Qt::AlignTop);
            if (hit.icon_url[0] != '\0') {
                startIcon(QString::fromUtf8(hit.icon_url), QString::fromUtf8(hit.id), icon);
            }

            auto *textCol = new QVBoxLayout();
            textCol->setSpacing(2);
            auto *title = new BodyLabel(QString::fromUtf8(hit.title), card);
            {
                QFont font = title->font();
                font.setPixelSize(15);
                font.setWeight(QFont::DemiBold);
                title->setFont(font);
            }
            textCol->addWidget(title);
            /* CF 的搜索响应里没有作者时如实少一段,不编一个名字出来。 */
            const QString author = QString::fromUtf8(hit.author);
            auto *sub = new BodyLabel(QStringLiteral("%1%2 · 下载 %3%4")
                                          .arg(author.isEmpty() ? QString() : author + QStringLiteral(" · "))
                                          .arg(QString::fromUtf8(hit.source))
                                          .arg(hit.downloads)
                                          .arg(hit.versions[0] != '\0'
                                                   ? QStringLiteral(" · 支持 %1")
                                                         .arg(QString::fromUtf8(hit.versions))
                                                   : QString()),
                                      card);
            const QColor secondary = pageTokenColor("textSecondary");
            sub->setTextColor(secondary, secondary);
            sub->setWordWrap(true);
            textCol->addWidget(sub);
            auto *desc = new BodyLabel(QString::fromUtf8(hit.description), card);
            desc->setTextColor(secondary, secondary);
            desc->setWordWrap(true);
            textCol->addWidget(desc);
            rowLay->addLayout(textCol, 1);

            auto *btn = new PushButton(QStringLiteral("装"), card);
            applyButtonFont(btn);
            /* 稳定 objectName:验收钩子(SXCL_UI_MODS_INSTALL)点**真的那一个「装」** ——
             * 走产品路径(取版本 -> 挑文件 -> 工作线程下载 + sha1 强校验 -> 进隔离目录)。 */
            btn->setObjectName(QStringLiteral("modsInstallButton"));
            const sxcl_mod_hit copy = hit; /* 值拷贝:page 释放后按钮回调还要用 */
            QObject::connect(btn, &QAbstractButton::clicked, this, [this, copy] { install(copy); });
            rowLay->addWidget(btn, 0, Qt::AlignVCenter);
            m_list->addWidget(card);
        }
    }

    /** 取一张图标（工作线程下到 <数据根>/icons/<id>.png，命中缓存就不再下）。
     *  target 用 QPointer 兜着:卡片可能已经被 clearResults() 删掉了。 */
    void startIcon(const QString &url, const QString &id, QLabel *target) {
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
            setIconFromFile(path, target);
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
                         [guard, path](bool ok, const QString &, const QString &, qint64) {
                             if (ok && !guard.isNull()) {
                                 setIconFromFile(path, guard.data());
                             }
                         });
        worker->start();
    }

    static void setIconFromFile(const QString &path, QLabel *target) {
        QPixmap pm(path);
        if (pm.isNull() || target == nullptr) {
            return;
        }
        target->setPixmap(pm.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        target->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));
    }

    bool m_shaders = false;
    Phase m_phase = Idle;
    QString m_source = QStringLiteral("modrinth");
    QString m_fetchSource;
    QString m_cfKey;
    QString m_gameDir;
    QString m_instance;
    QString m_mc;
    QString m_loader;
    QString m_currentText;
    QString m_projectTitle;
    QString m_deps;
    SearchLineEdit *m_search = nullptr;
    PrimaryPushButton *m_go = nullptr;
    BodyLabel *m_context = nullptr;
    BodyLabel *m_sourceNote = nullptr;
    Pivot *m_sourcePivot = nullptr;
    QWidget *m_keyRow = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QVBoxLayout *m_list = nullptr;
    ModsWorker *m_worker = nullptr;
};

} // namespace

QWidget *createModsPage(QWidget *parent, bool shaders) { return new ModsPane(parent, shaders); }

} // namespace sxcl::ui
