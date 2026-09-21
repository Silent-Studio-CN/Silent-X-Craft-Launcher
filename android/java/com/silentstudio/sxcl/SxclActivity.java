/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

package com.silentstudio.sxcl;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import org.qtproject.qt.android.bindings.QtActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class SxclActivity extends QtActivity {

    private static final String TAG = "sxcl";
    /* bump when the packaged assets change so stale copies are refreshed */
    private static final String ASSET_STAMP = "sxcl-c-0.1.0-android-1";

    /* latest instance, so the static helpers below can reach a Context */
    private static SxclActivity instance = null;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        instance = this;
        try {
            prepare();
        } catch (Throwable t) {
            Log.e(TAG, "android prepare failed", t);
        }
        super.onCreate(savedInstanceState);
        /* 游戏会话的主进程一侧(本地 socket 监听 + 退出码/信号回收)。
         * 只在启动器进程里装;:game 进程走 GameActivity,不碰这里。 */
        try {
            GameHost.attach(getApplicationContext());
        } catch (Throwable t) {
            Log.e(TAG, "GameHost.attach failed", t);
        }
        /* evidence line: is shared storage readable? (see the manifest comment) */
        Log.i(TAG, "all-files-access=" + hasAllFilesAccess());
    }

    @Override
    public void onResume() {
        super.onResume();
        /* the user may have just come back from the "All files access" settings page */
        Log.i(TAG, "resume: all-files-access=" + hasAllFilesAccess());
        Log.i(TAG, "resume: pip-supported=" + isPipSupported() + " in-floating=" + isInFloating()
                + " overlay-granted=" + hasOverlayPermission());
        /* 游戏运行器把我们拉回前台了(它在前台才有权发起,见 GameActivity.requestLauncherPip):
         * 现在启动器是前台/可见的,进画中画这一步在这里做才是有效的。 */
        if (pendingPipRequest) {
            pendingPipRequest = false;
            Log.i(TAG, "resume: 收到运行器的画中画请求 -> enterFloating()");
            requestFloatingAfterResume(0);
        }
    }

    /* 收到运行器请求后进画中画。**复用已有的 enterFloating()**(与最大化键同一条路),
     * 只是发起时机从"用户点最大化键"变成"游戏窗口就绪后把我们拉回前台"。
     * 失败时重试两次并如实记日志(绝不静默)。 */
    private static boolean pendingPipRequest = false;

    private void requestFloatingAfterResume(final int attempt) {
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                if (isInFloating()) {
                    Log.i(TAG, "pip request: 已经在画中画里(launchIntoPip 生效)attempt=" + attempt);
                    return;
                }
                final boolean ok = enterFloating();
                Log.i(TAG, "pip request: enterFloating=" + ok + " attempt=" + attempt);
                if (!ok && attempt < 2)
                    requestFloatingAfterResume(attempt + 1);
                else if (!ok)
                    Log.w(TAG, "pip request: 三次都没进画中画(如实记录):启动器停在全屏,"
                            + "游戏进程仍在跑");
            }
        }, attempt == 0 ? 200 : 700);
    }

    @Override
    public void onPictureInPictureModeChanged(boolean inPip, android.content.res.Configuration cfg) {
        super.onPictureInPictureModeChanged(inPip, cfg);
        Log.i(TAG, "pip mode changed: in-pip=" + inPip);
        /* 起游戏的**顺序**:先让启动器进画中画(此时它还是前台,enterPictureInPictureMode
         * 才生效),画中画一进去 -> 这里回调 -> 才真的拉 GameActivity(独立 task,全屏)。
         * 反过来的话活动已经不是前台了,画中画根本进不去。 */
        try {
            GameHost.onPipChanged(inPip);
        } catch (Throwable t) {
            Log.e(TAG, "GameHost.onPipChanged failed", t);
        }
        /* 画中画是一个很小的窗口:给它一个原生按钮"回到启动器"(点它 = exitFloating),
         * 免得用户进了画中画之后没有任何出口。 */
        showFloatingBackButton(inPip);
    }

    /* ---- 运行中接收命令(am start 一个已存在的实例会走这里) -------------------
     * 两个动作,与将来界面按钮要走的是**同一条路**:
     *   --es action gamestart [--es gamejre <jre>] [--es gamemain <class>]
     *                        [--es gameargs "<args>"] [--es gamecrash abort|term|kill]
     *        -> GameHost.requestStart(...) + enterFloating()(复用已有的画中画实现)
     *   --es action endgame   -> GameHost.endGame()(主进程发 END,游戏进程有序收尾)
     * 验收脚本用它们做"反复起/停游戏",不需要重启 App。 */
    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        final String action = intent != null ? intent.getStringExtra("action") : null;
        Log.i(TAG, "onNewIntent: action=" + action);
        /* 游戏运行器(前台的一侧)请求:启动器回前台 + 进画中画。见 GameActivity.requestLauncherPip */
        if (intent != null && intent.getBooleanExtra("sxclRequestPip", false)) {
            /* 两种情况都要覆盖:
             *   A) 启动器已经在前台(最常见:它只是被游戏请求"进画中画")-> 不会有 onResume,
             *      所以这里就发起;
             *   B) 启动器在后台被拉回前台 -> 这里发起的那次会失败,onResume 里那次(见下)兜住。 */
            pendingPipRequest = false;
            GameHost.noteLauncherPipRequest();
            Log.i(TAG, "onNewIntent: 运行器请求启动器进画中画 -> 立即发起(失败时 onResume 再兜一次)");
            requestFloatingAfterResume(0);
            return;
        }
        if (action == null)
            return;
        if ("gamestart".equals(action)) {
            final String st = GameHost.requestStart(intent.getStringExtra("gamejre"),
                    intent.getStringExtra("gamemain"), intent.getStringExtra("gameargs"),
                    intent.getStringExtra("gamecrash"), "onNewIntent");
            Log.i(TAG, "onNewIntent: 起游戏会话 -> " + st);
            /* 画中画不在这里进:见 GameHost 类头注释(docs/19 §2.2)。游戏窗口就绪后会经通道
             * 请求 PIP?,那时启动器回前台、在 onResume 里 enterFloating()(复用已有实现)。 */
            Log.i(TAG, "onNewIntent: 画中画等运行器的 PIP? 请求(启动器此刻仍在前台,游戏已开始拉起)");
        } else if ("endgame".equals(action)) {
            final boolean ok = GameHost.endGame();
            Log.i(TAG, "onNewIntent: 结束游戏命令已发 ok=" + ok + " " + GameHost.stateLine());
        }
    }

    /* ---- 画中画里的"回到启动器"按钮 ---------------------------------------
     * 挂在本 activity 窗口的 decor 上(FrameLayout 的最后一个子 view = 最上层),
     * 因此压不住 Qt 的 SurfaceView 布局,也不需要任何权限。离开画中画即移除。
     */
    private static android.widget.Button floatingBackButton = null;

    private static void showFloatingBackButton(boolean on) {
        if (instance == null)
            return;
        try {
            final android.view.ViewGroup decor =
                    (android.view.ViewGroup) instance.getWindow().getDecorView();
            if (!on) {
                if (floatingBackButton != null) {
                    decor.removeView(floatingBackButton);
                    floatingBackButton = null;
                    Log.i(TAG, "pip back-button removed");
                }
                return;
            }
            if (floatingBackButton != null)
                return;
            final float d = instance.getResources().getDisplayMetrics().density;
            android.widget.Button b = new android.widget.Button(instance);
            b.setText("回到启动器");
            b.setAllCaps(false);
            b.setTextSize(14);
            b.setTextColor(0xFFFFFFFF);
            android.graphics.drawable.GradientDrawable bg =
                    new android.graphics.drawable.GradientDrawable();
            bg.setColor(0xF0202020);
            bg.setCornerRadius(10 * d);
            bg.setStroke((int) Math.max(2, 2 * d), 0xFFC044A3);
            b.setBackground(bg);
            b.setPadding((int) (14 * d), (int) (4 * d), (int) (14 * d), (int) (4 * d));
            android.widget.FrameLayout.LayoutParams lp =
                    new android.widget.FrameLayout.LayoutParams(
                            android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
                            android.widget.FrameLayout.LayoutParams.WRAP_CONTENT);
            lp.gravity = android.view.Gravity.TOP | android.view.Gravity.CENTER_HORIZONTAL;
            lp.topMargin = (int) (6 * d);
            b.setOnClickListener(new android.view.View.OnClickListener() {
                @Override
                public void onClick(android.view.View v) {
                    Log.i(TAG, "pip back-button tapped -> 回到启动器(游戏进程继续跑)");
                    exitFloating();
                }
            });
            decor.addView(b, lp);
            floatingBackButton = b;
            Log.i(TAG, "pip back-button shown(画中画里的出口;decor children="
                    + decor.getChildCount() + ")");
        } catch (Throwable t) {
            Log.e(TAG, "pip back-button failed", t);
        }
    }

    /* ---- 游戏会话的主进程出口(Qt 侧用 QJniObject 调这三个) ---------------- */
    public static String startGameSession(String jreHome, String mainClass, String gameArgs,
                                          String crashMode) {
        final String pip = isInFloating() ? "already-in-pip" : "will-enter-pip";
        return GameHost.requestStart(jreHome, mainClass, gameArgs, crashMode, pip);
    }

    public static boolean endGameSession() {
        return GameHost.endGame();
    }

    public static String gameSessionState() {
        return GameHost.stateLine();
    }

    public static boolean isGameSessionLive() {
        return GameHost.isSessionLive();
    }

    /* ---- shared-storage access -------------------------------------------
     * Called from the Qt side via QJniObject:
     *   hasAllFilesAccess()      -> "Z"
     *   requestAllFilesAccess()  -> "V"
     * Android 11+ (API 30) uses MANAGE_EXTERNAL_STORAGE / Environment.
     * isExternalStorageManager(); older releases use the legacy runtime grant.
     */
    public static boolean hasAllFilesAccess() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            try {
                return android.os.Environment.isExternalStorageManager();
            } catch (Throwable t) {
                return false;
            }
        }
        if (instance == null)
            return false;
        return instance.checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    public static void requestAllFilesAccess() {
        if (instance == null)
            return;
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            android.content.Intent i = new android.content.Intent(
                    android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    android.net.Uri.parse("package:" + instance.getPackageName()));
            try {
                instance.startActivity(i);
                Log.i(TAG, "opened All-files-access settings page");
                return;
            } catch (Throwable t) {
                Log.w(TAG, "per-app all-files page unavailable, falling back to the list", t);
                try {
                    instance.startActivity(new android.content.Intent(
                            android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Throwable t2) {
                    Log.e(TAG, "cannot open all-files settings", t2);
                }
                return;
            }
        }
        instance.requestPermissions(
                new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE}, 1001);
    }

    /* ---- 1. minimise = move the whole task to the back --------------------
     * Android has no system tray, so "minimise" cannot mean hide-to-tray. It
     * means "leave the screen but keep running": the task goes behind whatever
     * the user was doing (the same thing the HOME key does), the process, its
     * Qt event loop and every download/install worker thread keep running, and
     * the launcher is reachable again from the bottom-swipe gesture / Recents.
     * The Qt window is deliberately NOT hidden: hide() would leave the Qt
     * window invisible when the activity is resumed again.
     */
    public static boolean moveTaskToBack() {
        if (instance == null)
            return false;
        try {
            /* nonRoot=true: works whether or not this activity is the root of
             * its task. Some builds answer false for the root case, hence the
             * second attempt before reporting failure. */
            boolean ok = instance.moveTaskToBack(true);
            if (!ok)
                ok = instance.moveTaskToBack(false);
            Log.i(TAG, "moveTaskToBack -> " + ok);
            return ok;
        } catch (Throwable t) {
            Log.e(TAG, "moveTaskToBack failed", t);
            return false;
        }
    }

    /* ---- 2. maximise = full screen <-> floating window --------------------
     * First choice is picture-in-picture: an Android-native floating window
     * that shows THIS activity's own UI, needs no permission, and floats above
     * other apps. Falls back to a SYSTEM_ALERT_WINDOW overlay panel (see below)
     * when the device/ROM will not do PiP.
     */
    public static boolean isPipSupported() {
        if (instance == null)
            return false;
        if (android.os.Build.VERSION.SDK_INT < 26)
            return false;
        try {
            return instance.getPackageManager()
                    .hasSystemFeature(android.content.pm.PackageManager.FEATURE_PICTURE_IN_PICTURE);
        } catch (Throwable t) {
            return false;
        }
    }

    public static boolean isInFloating() {
        if (instance == null)
            return false;
        try {
            return android.os.Build.VERSION.SDK_INT >= 26 && instance.isInPictureInPictureMode();
        } catch (Throwable t) {
            return false;
        }
    }

    /* Enter PiP. Returns false when the system refuses, so the Qt side can
     * show a plain-language hint instead of failing silently. */
    public static boolean enterFloating() {
        if (instance == null)
            return false;
        if (android.os.Build.VERSION.SDK_INT < 26) {
            Log.w(TAG, "enterFloating: API " + android.os.Build.VERSION.SDK_INT + " < 26, no PiP");
            return false;
        }
        try {
            if (instance.isInPictureInPictureMode())
                return true;
            android.app.PictureInPictureParams.Builder b =
                    new android.app.PictureInPictureParams.Builder();
            /* 16:9 keeps the launcher's landscape layout legible in the corner window */
            b.setAspectRatio(new android.util.Rational(16, 9));
            boolean ok = instance.enterPictureInPictureMode(b.build());
            Log.i(TAG, "enterPictureInPictureMode -> " + ok);
            return ok;
        } catch (Throwable t) {
            Log.e(TAG, "enterPictureInPictureMode failed", t);
            return false;
        }
    }

    /* Leave PiP. There is no "exitPictureInPictureMode()" API: bringing our own
     * activity back to the front is what makes the system expand it again. */
    public static boolean exitFloating() {
        if (instance == null)
            return false;
        try {
            Intent it = new Intent(instance, SxclActivity.class);
            it.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP
                    | Intent.FLAG_ACTIVITY_NEW_TASK);
            instance.startActivity(it);
            Log.i(TAG, "exitFloating: brought activity to front");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "exitFloating failed", t);
            return false;
        }
    }

    /* One entry point for the maximise button: full screen -> floating, and
     * floating -> full screen. */
    public static boolean toggleFloating() {
        if (instance == null)
            return false;
        if (isInFloating())
            return exitFloating();
        return enterFloating();
    }

    /* ---- 2b. fallback: "display over other apps" overlay panel ------------
     * HARD FACT, not a shortcut: the launcher's own UI is one SurfaceView
     * owned by the activity. A WindowManager overlay window cannot host it, so
     * this route can only carry a NATIVE panel (task + progress + a button that
     * brings the launcher back) - never the launcher interface itself.
     */
    public static boolean hasOverlayPermission() {
        if (instance == null)
            return false;
        if (android.os.Build.VERSION.SDK_INT < 23)
            return true;
        try {
            return android.provider.Settings.canDrawOverlays(instance);
        } catch (Throwable t) {
            return false;
        }
    }

    public static void requestOverlayPermission() {
        if (instance == null)
            return;
        try {
            instance.startActivity(new Intent(
                    android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    android.net.Uri.parse("package:" + instance.getPackageName())));
            Log.i(TAG, "opened the per-app overlay settings page");
        } catch (Throwable t) {
            Log.w(TAG, "per-app overlay page unavailable, falling back to the list", t);
            try {
                instance.startActivity(
                        new Intent(android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION));
            } catch (Throwable t2) {
                Log.e(TAG, "cannot open overlay settings", t2);
            }
        }
    }

    private static android.view.WindowManager overlayWindow = null;
    private static android.widget.TextView overlayTask = null;
    private static android.widget.ProgressBar overlayBar = null;

    public static boolean isOverlayPanelShown() {
        return overlayWindow != null;
    }

    private static void bringLauncherToFront() {
        if (instance == null)
            return;
        try {
            Intent it = new Intent(instance, SxclActivity.class);
            it.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP
                    | Intent.FLAG_ACTIVITY_NEW_TASK);
            instance.startActivity(it);
        } catch (Throwable t) {
            Log.e(TAG, "cannot bring the launcher to front", t);
        }
    }

    public static void hideOverlayPanel() {
        if (instance == null || overlayWindow == null)
            return;
        try {
            if (overlayTask != null)
                overlayWindow.removeView(overlayTask.getRootView());
        } catch (Throwable t) {
            Log.w(TAG, "removeView failed", t);
        }
        overlayWindow = null;
        overlayTask = null;
        overlayBar = null;
        Log.i(TAG, "overlay panel hidden");
    }

    /* Show or update the native floating panel. Called from Qt on the Android
     * main thread (Qt's event loop runs there), so touching views is safe. */
    public static boolean showOverlayPanel(String task, int percent) {
        if (instance == null)
            return false;
        if (!hasOverlayPermission()) {
            Log.w(TAG, "showOverlayPanel refused: no overlay permission");
            return false;
        }
        if (overlayWindow != null) {
            if (overlayTask != null)
                overlayTask.setText(task);
            if (overlayBar != null)
                overlayBar.setProgress(Math.max(0, Math.min(100, percent)));
            Log.i(TAG, "overlay panel updated: " + task + " " + percent + "%");
            return true;
        }
        try {
            final float d = instance.getResources().getDisplayMetrics().density;
            int pad = (int) (14 * d);

            android.widget.LinearLayout panel = new android.widget.LinearLayout(instance);
            panel.setOrientation(android.widget.LinearLayout.VERTICAL);
            android.graphics.drawable.GradientDrawable bg =
                    new android.graphics.drawable.GradientDrawable();
            bg.setColor(0xF0202020);          /* SXCL dark token */
            bg.setCornerRadius(14 * d);
            bg.setStroke((int) Math.max(2, 2 * d), 0xFFC044A3); /* SXCL accent */
            panel.setBackground(bg);
            panel.setPadding(pad, pad, pad, pad);

            android.widget.TextView title = new android.widget.TextView(instance);
            title.setText("Silent X Craft Launcher");
            title.setTextColor(0xFFC044A3);
            title.setTextSize(12);

            overlayTask = new android.widget.TextView(instance);
            overlayTask.setText(task == null || task.isEmpty() ? "当前任务:无" : task);
            overlayTask.setTextColor(0xFFE4E4E4);
            overlayTask.setTextSize(13);

            overlayBar = new android.widget.ProgressBar(instance, null,
                    android.R.attr.progressBarStyleHorizontal);
            overlayBar.setMax(100);
            overlayBar.setProgress(Math.max(0, Math.min(100, percent)));

            android.widget.Button back = new android.widget.Button(instance);
            back.setText("回到启动器");
            back.setOnClickListener(new android.view.View.OnClickListener() {
                @Override
                public void onClick(android.view.View v) {
                    hideOverlayPanel();
                    bringLauncherToFront();
                }
            });

            panel.addView(title);
            panel.addView(overlayTask);
            panel.addView(overlayBar);
            panel.addView(back);

            /* minSdk is 28, so TYPE_APPLICATION_OVERLAY (API 26+) is always available */
            final int type = android.view.WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
            final android.view.WindowManager.LayoutParams lp =
                    new android.view.WindowManager.LayoutParams(
                            android.view.WindowManager.LayoutParams.WRAP_CONTENT,
                            android.view.WindowManager.LayoutParams.WRAP_CONTENT,
                            type,
                            android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                            android.graphics.PixelFormat.TRANSLUCENT);
            lp.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
            lp.x = (int) (36 * d);
            lp.y = (int) (140 * d);

            /* draggable: the whole panel moves with the finger */
            panel.setOnTouchListener(new android.view.View.OnTouchListener() {
                private float downX, downY;
                private int startX, startY;

                @Override
                public boolean onTouch(android.view.View v, android.view.MotionEvent e) {
                    switch (e.getActionMasked()) {
                    case android.view.MotionEvent.ACTION_DOWN:
                        downX = e.getRawX();
                        downY = e.getRawY();
                        startX = lp.x;
                        startY = lp.y;
                        return true;
                    case android.view.MotionEvent.ACTION_MOVE:
                        lp.x = startX + (int) (e.getRawX() - downX);
                        lp.y = startY + (int) (e.getRawY() - downY);
                        try {
                            overlayWindow.updateViewLayout(v, lp);
                        } catch (Throwable t) {
                            Log.w(TAG, "overlay drag failed", t);
                        }
                        return true;
                    default:
                        return false;
                    }
                }
            });

            overlayWindow = (android.view.WindowManager)
                    instance.getSystemService(android.content.Context.WINDOW_SERVICE);
            overlayWindow.addView(panel, lp);
            Log.i(TAG, "overlay panel shown: " + task + " " + percent + "%");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "showOverlayPanel failed", t);
            overlayWindow = null;
            overlayTask = null;
            overlayBar = null;
            return false;
        }
    }

    private void prepare() throws IOException {
        final File files = getFilesDir();
        final File assetRoot = new File(files, "assets");
        final File stamp = new File(assetRoot, ".stamp");
        if (!stamp.isFile() || !ASSET_STAMP.equals(readText(stamp))) {
            copyAssetTree("", assetRoot);
            writeText(stamp, ASSET_STAMP);
        }

        File ext = getExternalFilesDir(null);
        File shotDir = new File(ext != null ? ext : files, "shots");
        if (!shotDir.isDirectory() && !shotDir.mkdirs())
            Log.w(TAG, "cannot create " + shotDir);

        Intent it = getIntent();
        String route = it != null ? it.getStringExtra("route") : null;
        String scale = it != null ? it.getStringExtra("scale") : null;
        String accent = it != null ? it.getStringExtra("accent") : null;
        String theme = it != null ? it.getStringExtra("theme") : null;
        String popup = it != null ? it.getStringExtra("popup") : null;
        String tallmenu = it != null ? it.getStringExtra("tallmenu") : null;
        /* acceptance switches, off unless the launch asks for them:
         *   passthrough  -> SXCL_TOUCH_PASSTHROUGH (child widgets click-through; A/B only)
         *   animtrace    -> SXCL_ANIM_TRACE       (per-frame animation sampling to logcat)
         * The environment is not settable for an Android app, so they ride the boot file. */
        String passthrough = it != null ? it.getStringExtra("passthrough") : null;
        String animtrace = it != null ? it.getStringExtra("animgtrace") : null;
        /* 游戏独立进程(本轮架构)的验收开关:native 入口按 boot 文件驱动时序 ——
         * 建会话(本地 socket 先监听)-> 启动器进画中画 -> 画中画回调里才拉 GameActivity
         * (独立 task、android:process=":game")。
         *   am start ... --es gamestart 1 [--es gamejre <jre home>] [--es gamemain <class>]
         *               [--es gameargs "<args>"] [--es gamecrash abort|term|kill]
         *               [--es gamedelay <ms>]
         * 不写 gamestart 时一行都不会跑(与 jreprobe 同一纪律)。 */
        String gamestart = it != null ? it.getStringExtra("gamestart") : null;
        String gamejre = it != null ? it.getStringExtra("gamejre") : null;
        String gamemain = it != null ? it.getStringExtra("gamemain") : null;
        String gameargs = it != null ? it.getStringExtra("gameargs") : null;
        String gamecrash = it != null ? it.getStringExtra("gamecrash") : null;
        String gamedelay = it != null ? it.getStringExtra("gamedelay") : null;
        /* 验收开关:起游戏后 N 毫秒由主进程发"结束游戏"命令(走 GameHost.endGame(),
         * 与将来界面上的"结束游戏"按钮是同一条路)。不写 = 不发。 */
        String gamestop = it != null ? it.getStringExtra("gamestop") : null;
        /* 进程内 JVM 自举探针(诊断用,默认关):
         *   am start ... --es jreprobe /data/data/<pkg>/files/runtime/jre25
         * nativeLibraryDir 由这里补上(应用自己的 ApplicationInfo,Java 侧拿最省事)。
         * 探针本身在 android/app/sxcl_jre_probe.c:dlopen(libjli.so) + dlsym(JLI_Launch)。 */
        String jreprobe = it != null ? it.getStringExtra("jreprobe") : null;
        /* acceptance hooks for the widget-tree dump (SXCL_UI_DUMP) and the grab delay
         * (SXCL_UI_SHOT_DELAY): the shared desktop entry reads both from the
         * environment, which an Android app cannot be given. They ride the boot file
         * exactly like passthrough/animgtrace above; unset = off = product behaviour. */
        String dump = it != null ? it.getStringExtra("dump") : null;
        String shotdelay = it != null ? it.getStringExtra("shotdelay") : null;
        boolean shot = it != null && it.getBooleanExtra("shot", false);
        boolean offscreen = it != null && it.getBooleanExtra("offscreen", false);

        StringBuilder sb = new StringBuilder();
        sb.append("assets=").append(assetRoot.getAbsolutePath()).append('\n');
        sb.append("shots=").append(shotDir.getAbsolutePath()).append('\n');
        sb.append("shot=").append(shot ? "1" : "0").append('\n');
        sb.append("offscreen=").append(offscreen ? "1" : "0").append('\n');
        if (route != null) sb.append("route=").append(route).append('\n');
        if (scale != null) sb.append("scale=").append(scale).append('\n');
        if (accent != null) sb.append("accent=").append(accent).append('\n');
        if (theme != null) sb.append("theme=").append(theme).append('\n');
        if (popup != null) sb.append("popup=").append(popup).append('\n');
        if (tallmenu != null) sb.append("tallmenu=").append(tallmenu).append('\n');
        if (passthrough != null) sb.append("passthrough=").append(passthrough).append('\n');
        if (animtrace != null) sb.append("animgtrace=").append(animtrace).append('\n');
        if (dump != null) sb.append("dump=").append(dump).append('\n');
        if (shotdelay != null) sb.append("shotdelay=").append(shotdelay).append('\n');
        if (jreprobe != null) {
            sb.append("jreprobe=").append(jreprobe).append('\n');
            sb.append("nativelib=").append(getApplicationInfo().nativeLibraryDir).append('\n');
        }
        if (gamestart != null) {
            sb.append("gamestart=").append(gamestart).append('\n');
            sb.append("nativelib=").append(getApplicationInfo().nativeLibraryDir).append('\n');
        }
        if (gamejre != null) sb.append("gamejre=").append(gamejre).append('\n');
        if (gamemain != null) sb.append("gamemain=").append(gamemain).append('\n');
        if (gameargs != null) sb.append("gameargs=").append(gameargs).append('\n');
        if (gamecrash != null) sb.append("gamecrash=").append(gamecrash).append('\n');
        if (gamedelay != null) sb.append("gamedelay=").append(gamedelay).append('\n');
        if (gamestop != null) sb.append("gamestop=").append(gamestop).append('\n');
        writeText(new File(files, "sxcl_boot.txt"), sb.toString());
        Log.i(TAG, "boot: " + sb.toString().replace('\n', ' '));
    }

    private void copyAssetTree(String path, File destDir) throws IOException {
        String[] children = getAssets().list(path);
        if (children == null || children.length == 0)
            return;
        if (!destDir.isDirectory() && !destDir.mkdirs())
            throw new IOException("mkdirs failed: " + destDir);
        for (String child : children) {
            final String childPath = path.isEmpty() ? child : path + "/" + child;
            String[] grand = getAssets().list(childPath);
            if (grand != null && grand.length > 0)
                copyAssetTree(childPath, new File(destDir, child));
            else
                copyAssetFile(childPath, new File(destDir, child));
        }
    }

    private void copyAssetFile(String assetPath, File dest) throws IOException {
        InputStream in = getAssets().open(assetPath);
        try {
            OutputStream out = new FileOutputStream(dest);
            try {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0)
                    out.write(buf, 0, n);
                out.flush();
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
    }

    private static String readText(File f) {
        try {
            FileInputStream in = new FileInputStream(f);
            try {
                byte[] all = new byte[(int) f.length()];
                int off = 0;
                while (off < all.length) {
                    int n = in.read(all, off, all.length - off);
                    if (n <= 0) break;
                    off += n;
                }
                return new String(all, 0, off, "UTF-8").trim();
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String text) throws IOException {
        File parent = f.getParentFile();
        if (parent != null && !parent.isDirectory())
            parent.mkdirs();
        FileWriter w = new FileWriter(f);
        try {
            w.write(text);
        } finally {
            w.close();
        }
    }
}
