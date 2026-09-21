/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

package com.silentstudio.sxcl;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

/* GameActivity - 游戏进程(:game)自己的窗口(Android only)。
 *
 * 用户 2026-09-21 拍板 B 方案,这个类就是那个"游戏自己一个窗口":
 *   * manifest 里 android:process=":game" -> 与启动器**不同进程**;
 *   * launchMode=singleTask + 独立 taskAffinity -> 游戏自己一个 task,
 *     绝不动启动器那个 task(否则 singleTask 会把启动器从栈上清掉,画中画也一起没);
 *   * 自己建 SurfaceView(游戏画面将来画在这块 surface 上);
 *   * JVM 在**本进程内**起:走 ndk 侧 libsxclgame.so -> sxcl_game.c ->
 *     sxcl_jre_bootstrap.c(与诊断探针同一份自举实现),不 exec、不借别的进程。
 *
 * 本轮范围:只打通架构(两个进程 + 通道 + 收尾)。**不接游戏主类** ——
 * Intent 里没给 jre 时就如实报 java=absent 并待命;给了 jre 才真的起 JVM。
 * JRE 包等用户上传,代码里没有任何写死的 JRE 路径、也不下载任何东西。
 *
 * 结束语义:主进程发 END -> 原生层有序收尾(flush 日志 + 写退出记录)-> 这里 finish()
 * 并结束**本进程**。结束游戏绝不等于"杀掉整个 App":这里 Process.killProcess() 杀的是
 * :game 这一个进程,启动器主进程一动不动。
 */
public class GameActivity extends Activity {

    private static final String TAG = "sxcl";
    private static final String LIB = "sxclgame";

    private static GameActivity instance = null;
    private static boolean libReady = false;

    private SurfaceView surface = null;
    /** 边界输入(与主进程 GameHost 同一份 GameLaunchSpec) */
    private GameHost.GameLaunchSpec spec = new GameHost.GameLaunchSpec();
    /** 只请一次画中画,避免每次 onResume 都把启动器拉回来 */
    private boolean pipRequested = false;

    /* ---- 原生层(JNI;实现见 android/app/sxcl_game.c) ----
     * 参数就是"游戏运行器"边界的输入(GameLaunchSpec)逐字段展开:
     * 会话目录/通道名/JRE/classpath/natives/游戏目录/主类/JVM 参数/游戏参数/渲染器 + 崩溃注入。 */
    private static native int nativeStart(String dir, String socket, String jre, String nativelib,
                                          String classpath, String gamedir, String mainclass,
                                          String jvmargs, String gameargs, String renderer,
                                          int crashMode);

    private static native int nativeRequestStop();

    private static native int nativeHasJvm();

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;

        /* 游戏窗口:常亮 + 真全屏(自己的窗口,与启动器的窗口互不影响) */
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        try {
            final View decor = getWindow().getDecorView();
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        } catch (Throwable t) {
            Log.w(TAG, "game-process: 全屏旗标没设上:" + t);
        }

        surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Log.i(TAG, "game-process: 游戏自己的 SurfaceView 已创建(本进程独立窗口)");
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                Log.i(TAG, "game-process: surface 尺寸 " + w + "x" + h);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "game-process: surface 已销毁");
            }
        });
        setContentView(surface);

        /* 边界输入:与主进程 GameHost 用的是同一份 GameLaunchSpec(同一套键) */
        final Intent it = getIntent();
        spec = GameHost.GameLaunchSpec.fromExtras(it != null ? it.getExtras() : null);
        Log.i(TAG, "game-process: onCreate pid=" + Process.myPid() + " spec{" + spec.toLine() + "}");
        if (spec.jreHome.isEmpty())
            Log.i(TAG, "game-process: 没有配 JRE(本轮只验架构:java=absent,通道与收尾照常工作)");

        if (!ensureLib()) {
            Log.e(TAG, "game-process: 原生库 " + LIB + " 没装上,进程无法工作");
            onGameStopped(3, 0, "native-lib-missing");
            return;
        }

        /* nativeStart 会一直阻塞到游戏结束(JVM 的生命周期就是它的生命周期),
         * 所以必须离开 UI 线程 —— 界面线程要留着跑窗口与 surface。 */
        Thread starter = new Thread(new Runnable() {
            @Override
            public void run() {
                final int rc = nativeStart(spec.sessionDir, spec.socketName, spec.jreHome,
                        spec.nativesDir, spec.classpath, spec.gameDir, spec.mainClass, spec.jvmArgs,
                        spec.gameArgs, spec.renderer, spec.crashMode);
                Log.i(TAG, "game-process: nativeStart 返回 rc=" + rc + " (hasJvm=" + nativeHasJvm()
                        + ")");
            }
        }, "sxcl-game-native");
        starter.start();
    }

    private static boolean ensureLib() {
        if (libReady)
            return true;
        try {
            System.loadLibrary(LIB);
            libReady = true;
            Log.i(TAG, "game-process: System.loadLibrary(" + LIB + ") 成功");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "game-process: System.loadLibrary(" + LIB + ") 失败:" + t);
            return false;
        }
    }

    /* ---- 原生层的收尾回调(任意线程调用;这里切回 UI 线程) ----
     * 走到这里说明:日志已 flush、退出记录已落盘、通道上已经发了 EXIT。
     * 剩下的事只有两件:结束这个 Activity,然后结束 :game 这一个进程。 */
    private static void onGameStopped(final int code, final int signal, final String reason) {
        Log.i(TAG, "game-process: onGameStopped code=" + code + " signal=" + signal + " reason="
                + reason);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                final GameActivity a = instance;
                if (a != null) {
                    try {
                        a.finishAndRemoveTask();
                    } catch (Throwable t) {
                        try {
                            a.finish();
                        } catch (Throwable t2) {
                            Log.w(TAG, "game-process: finish 失败:" + t2);
                        }
                    }
                }
                new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        /* Activity 结束后进程默认会被系统**缓存**;游戏进程必须真的消失,
                         * 否则"结束游戏后 :game 进程消失"这条验收过不了。杀的是本进程,
                         * 启动器主进程不受影响。 */
                        Log.i(TAG, "game-process: 结束 :game 进程(pid=" + Process.myPid()
                                + ");启动器主进程照常活着");
                        Process.killProcess(Process.myPid());
                    }
                }, 300);
            }
        });
    }

    @Override
    public void onResume() {
        super.onResume();
        Log.i(TAG, "game-process: onResume(游戏窗口在前台,launcherPip=" + spec.launcherPip + ")");
    }

    /** 原生层转达的"请启动器回前台进画中画"(主进程对 PIP? 的回答 need=1 才会走到)。
     *  为什么由**游戏**(前台的一侧)发起、而不是启动器自己:见 requestLauncherPip 的说明。 */
    private static void onLauncherPipNeeded() {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                final GameActivity a = instance;
                if (a == null || a.pipRequested) {
                    Log.i(TAG, "game-process: 画中画请求:跳过(instance=" + (a != null) + ")");
                    return;
                }
                a.pipRequested = true;
                a.requestLauncherPip();
            }
        });
    }

    /* 请启动器回前台并进画中画。
     *
     * 为什么由**游戏**(前台的一侧)发起:Android 14+ 把"处于画中画(pinned)的活动发起新活动"
     * 直接判成后台活动启动,系统原文是
     *     Background activity launch blocked! ... callingUidHasVisibleNotPinnedActivity: false
     *     Abort background activity starts from <uid>   (result code=102, BAL_BLOCK)
     * 所以"启动器先进画中画、再去起游戏"这条路在新系统上是不通的;反过来,前台可见的
     * 游戏活动去拉起启动器是允许的。启动器回到前台后在 onResume 里自己 enterFloating()
     * (复用已有的画中画实现),于是变成:游戏全屏在下、启动器画中画在上。
     * 真机原文与取舍见 docs/21 §5.1。 */
    private void requestLauncherPip() {
        try {
            Intent it = new Intent(this, SxclActivity.class);
            it.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP
                    | Intent.FLAG_ACTIVITY_NEW_TASK);
            it.putExtra("sxclRequestPip", true);
            /* 直接请系统"把启动器以画中画形态拉到前台"(API 26+ 的 makeLaunchIntoPip)。
             * 系统不吃这个旗标也没关系:启动器回到前台后会在 onResume 里自己
             * enterFloating()(见 SxclActivity.requestFloatingAfterResume)—— 两条路都保留。 */
            android.app.PictureInPictureParams.Builder pb =
                    new android.app.PictureInPictureParams.Builder();
            pb.setAspectRatio(new android.util.Rational(16, 9));
            final android.app.ActivityOptions opts =
                    android.app.ActivityOptions.makeLaunchIntoPip(pb.build());
            startActivity(it, opts.toBundle());
            Log.i(TAG, "game-process: 已请启动器回前台并进画中画(游戏进程继续跑)");
        } catch (Throwable t) {
            Log.e(TAG, "game-process: 请求启动器进画中画失败:" + t);
        }
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Log.i(TAG, "game-process: onNewIntent(singleTask 复用同一个游戏进程)");
    }

    @Override
    public void onBackPressed() {
        /* 返回键 = 有序结束游戏:先让原生层 flush + 写退出记录,再由 onGameStopped 收尾 */
        Log.i(TAG, "game-process: 返回键 -> 请求有序结束");
        try {
            nativeRequestStop();
        } catch (Throwable t) {
            Log.w(TAG, "game-process: nativeRequestStop 失败:" + t);
            super.onBackPressed();
        }
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "game-process: onDestroy pid=" + Process.myPid());
        if (libReady) {
            try {
                nativeRequestStop();
            } catch (Throwable ignored) {
                /* 进程即将结束,这里失败不影响收尾 */
            }
        }
        if (instance == this)
            instance = null;
        super.onDestroy();
    }
}
