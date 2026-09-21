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
    }

    @Override
    public void onPictureInPictureModeChanged(boolean inPip, android.content.res.Configuration cfg) {
        super.onPictureInPictureModeChanged(inPip, cfg);
        Log.i(TAG, "pip mode changed: in-pip=" + inPip);
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
