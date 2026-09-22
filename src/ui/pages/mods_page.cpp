/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 下载页的「拼图（MOD）」那一栏（docs/22 的 A1，走 PCL 线路）。
//
// 顶部一行：搜索框 + 「当前实例」上下文（版本 / 加载器 / 版本隔离开没开）+ 搜索按钮；
// 下面    ：结果卡片（标题 / 作者 / 下载量 / 简介 / 支持版本 + 一键「装」）。
//
// 口径（与 sxcl/mods.h 完全一致）：
//   * 筛选交给**服务端 facets**（游戏版本 + 加载器），本地不过滤；
//   * **不自动换加载器**：实例是 Fabric 就只列/只挑 Fabric 的文件，挑不出来就如实说；
//   * 依赖**只展示不装**（装完把 required 的工程 id 打在提示里）；
//   * 装进**版本隔离**后的目录（<游戏>/versions/<实例>/mods），没开隔离就还是根 mods/；
//   * 网络全在工作线程（取 JSON / 下文件），界面线程一个字节都不等。

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
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>

#include "sxcl/fs.h"
#include "sxcl/mods.h"
#include "sxcl/settings.h"

namespace sxcl::ui {
namespace {

/** 版本隔离开着吗（与启动层同一个键）。 */
bool versionIsolationOn() {
    const QByteArray path = uiSettingsFilePath().toUtf8();
    sxcl_settings *st = sxcl_settings_open(path.constData());
    if (st == nullptr) {
        return false;
    }
    const int on = (int)sxcl_settings_get_int(st, "general.version_isolation", 0);
    sxcl_settings_free(st);
    return on != 0;
}

class ModsPane : public QWidget {
public:
    ModsPane(QWidget *parent, bool shaders) : QWidget(parent), m_shaders(shaders) {
        build();
        reloadContext();
    }

private:
    enum Phase { Idle, Searching, LoadingVersions, Downloading };

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
        m_search->setPlaceholderText(m_shaders ? QStringLiteral("搜光影包（Modrinth）")
                                               : QStringLiteral("搜模组（Modrinth）"));
        m_search->setFixedHeight(34);
        row->addWidget(m_search, 1);
        m_go = new PrimaryPushButton(QStringLiteral("搜索"), this);
        applyButtonFont(m_go);
        m_go->setFixedHeight(34);
        row->addWidget(m_go, 0);
        lay->addLayout(row);

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
    }

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
        m_currentText = QStringLiteral("当前实例：%1 · 游戏版本 %2 · 加载器 %3 · %4")
                            .arg(m_instance.isEmpty() ? QStringLiteral("（还没选版本）") : m_instance,
                                 m_mc.isEmpty() ? QStringLiteral("不筛") : m_mc,
                                 m_loader.isEmpty() ? QStringLiteral("原版（不筛加载器）") : m_loader,
                                 versionIsolationOn()
                                     ? QStringLiteral("版本隔离已开（装进实例自己的 mods/）")
                                     : QStringLiteral("版本隔离没开，会装进根目录的 mods/"));
        m_context->setText(m_currentText);
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

    void startSearch() {
        if (m_worker != nullptr) {
            return;
        }
        reloadContext();
        sxcl_mods_query q;
        std::memset(&q, 0, sizeof(q));
        const QByteArray text = m_search->text().trimmed().toUtf8();
        const QByteArray mc = m_mc.toUtf8();
        const QByteArray loader = m_loader.toUtf8();
        q.text = text.constData();
        q.game_version = mc.constData();
        q.loader = loader.constData();
        q.limit = 20;
        char url[1200];
        if (sxcl_mods_modrinth_search_url(&q, url, sizeof(url)) != 0) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("条件太长"),
                          QStringLiteral("搜索条件拼不成 URL，换个短点的关键词"), this, 4000);
            return;
        }
        clearResults();
        m_phase = Searching;
        setBusy(true, QStringLiteral("正在搜 Modrinth…（筛选走服务端 facets：版本 %1 / 加载器 %2）")
                          .arg(m_mc.isEmpty() ? QStringLiteral("全部") : m_mc,
                               m_loader.isEmpty() ? QStringLiteral("全部") : m_loader));
        startFetch(QString::fromUtf8(url));
    }

    void startFetch(const QString &url) {
        ModsWorker::Request req;
        req.op = ModsWorker::FetchText;
        req.url = url;
        req.settingsFile = uiSettingsFilePath();
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
            if (sxcl_mods_modrinth_search_parse(body.constData(), (size_t)body.size(), &page, err,
                                                sizeof(err)) != 0) {
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
                              .arg(versionIsolationOn() ? QStringLiteral("实例的 mods/")
                                                        : QStringLiteral("根目录的 mods/")));
            return;
        }
        if (phase == LoadingVersions) {
            sxcl_mod_file files[32];
            size_t count = 0;
            if (sxcl_mods_modrinth_versions_parse(body.constData(), (size_t)body.size(), files, 32,
                                                  &count, err, sizeof(err)) != 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("取版本失败"),
                              QString::fromUtf8(err), this, 6000);
                return;
            }
            sxcl_mod_file picked;
            std::memset(&picked, 0, sizeof(picked));
            if (count == 0 || sxcl_mods_pick_file(files, count, m_mc.toUtf8().constData(),
                                                  m_loader.toUtf8().constData(), &picked) != 0) {
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没有能用的文件"),
                              QStringLiteral("「%1」里没有匹配这个实例（版本 %2 / 加载器 %3）的文件 —— "
                                             "PCL 口径**不自动换加载器**，要么换个包，要么装对应加载器的版本。")
                                  .arg(m_projectTitle, m_mc.isEmpty() ? QStringLiteral("任何") : m_mc,
                                       m_loader.isEmpty() ? QStringLiteral("任何") : m_loader),
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
                                 extra = QStringLiteral("；依赖（只展示不装）：%1").arg(m_deps);
                             }
                             setBusy(false, QStringLiteral("已装好：%1")
                                                .arg(QDir::toNativeSeparators(dest)));
                             InfoBar::push(InfoBar::Type::Success, QStringLiteral("模组已就位"),
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
        char url[1200];
        if (sxcl_mods_modrinth_versions_url(hit.id, m_mc.toUtf8().constData(),
                                           m_loader.toUtf8().constData(), url, sizeof(url)) != 1) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("工程 id 拿不到"),
                          QStringLiteral("这一条没有可用的 id"), this, 5000);
            return;
        }
        m_phase = LoadingVersions;
        setBusy(true, QStringLiteral("正在取「%1」的版本列表…").arg(m_projectTitle));
        startFetch(QString::fromUtf8(url));
    }

    void showResults(const sxcl_mod_page &page) {
        if (page.count == 0) {
            auto *empty = new BodyLabel(
                QStringLiteral("没找到。换个关键词或关掉筛选条件（版本 %1 / 加载器 %2）再试。")
                    .arg(m_mc.isEmpty() ? QStringLiteral("全部") : m_mc,
                         m_loader.isEmpty() ? QStringLiteral("全部") : m_loader),
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
            auto *sub = new BodyLabel(QStringLiteral("%1 · 下载 %2 · 支持 %3")
                                          .arg(QString::fromUtf8(hit.author))
                                          .arg(hit.downloads)
                                          .arg(QString::fromUtf8(hit.versions)),
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
            const sxcl_mod_hit copy = hit; /* 值拷贝:page 释放后按钮回调还要用 */
            QObject::connect(btn, &QAbstractButton::clicked, this, [this, copy] { install(copy); });
            rowLay->addWidget(btn, 0, Qt::AlignVCenter);
            m_list->addWidget(card);
        }
    }

    bool m_shaders = false;
    Phase m_phase = Idle;
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
    QVBoxLayout *m_list = nullptr;
    ModsWorker *m_worker = nullptr;
};

} // namespace

QWidget *createModsPage(QWidget *parent, bool shaders) { return new ModsPane(parent, shaders); }

} // namespace sxcl::ui
