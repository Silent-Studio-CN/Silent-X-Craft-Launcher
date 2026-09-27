/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// 玩家皮肤(正版账户的 Java 版皮肤贴图)的**取图**那一层:网络 + 缓存 + 线程。
//
// 取法与 FCL / 官方启动器同一套(不是我们编的):
//   ① 会话服务按 UUID 给出档案,里面 properties 里有一项 name=textures 的属性,
//      它的 value 是一段 base64 的 JSON;
//   ② 解开之后 textures.SKIN.url 就是皮肤 PNG 的地址;
//   ③ 那张 PNG 才是要画的东西(64x64 或旧格式 64x32,拼图规则见 skin_image.h)。
//
// 线程:整条链在工作线程里跑,界面线程只收 changed() —— 与账户那套(account.cpp)同一个口径,
// 主线程绝不为了头像去等网络。拿不到就**什么都不发**(界面自己画占位,不写解释文字)。
//
// 缓存:拿到的那张 PNG 存一份到设置文件旁边的 skin-cache/<uuid>.png ——
// 下次启动(含断网)直接用它,不必再走一遍网络。

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

class QThread;

namespace sxcl::ui {

/** 取消位 + **在飞的那条传输**(定义在 .cpp):换账户 / 退出时先撤请求,不让界面等网络。
 *  前置声明就够 —— shared_ptr 的析构在 .cpp 里实例化(类自己声明了析构函数)。 */
struct SkinFetchCancel;

class SkinStore : public QObject {
    Q_OBJECT
public:
    static SkinStore &instance();

    /** 换账户(空串 = 没登录):同一个 uuid 且已经有图/正在取,就什么都不做。
     *  取证通路:SXCL_UI_SKIN_FILE 指向本地一张贴图时,空 uuid 也照样把它画出来(见 .cpp)。 */
    void setAccount(const QString &uuid);
    QString account() const { return m_uuid; }
    /** 已经拿到的皮肤贴图;空 = 还没有(界面用占位)。 */
    QImage skin() const { return m_skin; }

signals:
    void changed();

private:
    SkinStore();
    ~SkinStore() override;
    SkinStore(const SkinStore &) = delete;
    SkinStore &operator=(const SkinStore &) = delete;

    void start(const QString &uuid);
    void deliver(const QString &uuid, const QImage &skin);
    /** 置取消位 + 撤在飞的 HTTP(线程安全)。 */
    void cancelFetch();

    QString m_uuid;
    QImage m_skin;
    QThread *m_thread = nullptr; // 在跑的那条;结束自删(finished 信号里置空)
    /* 取消位**每条线程一份**(shared_ptr):换账户时旧那条可能还在飞,它读的必须是自己的那一份 ——
     * 不能让它在单例析构之后还去碰成员。 */
    std::shared_ptr<SkinFetchCancel> m_cancel;
};

/** 皮肤 PNG 的缓存文件路径(设置文件旁边那一份)。 */
QString skinCachePath(const QString &uuid);

} // namespace sxcl::ui
