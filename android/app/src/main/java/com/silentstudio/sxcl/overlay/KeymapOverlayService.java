/*
 * 版权所有 © Silent X Craft Launcher Dev 开发团队
 *
 * Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
 * 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
 *
 * Copyright © Silent X Craft Launcher Development Team
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version, WITH the Additional Terms described
 * in the LICENSE file accompanying this program.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

package com.silentstudio.sxcl.overlay;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Toast;

import com.silentstudio.sxcl.data.AssetKeymaps;
import com.silentstudio.sxcl.data.KeymapPrefs;
import com.silentstudio.sxcl.keymap.KeymapLayout;

/**
 * 把虚拟按键铺在游戏画面上的前台服务（需要"显示在其他应用上层"权限）。
 *
 * 和 FCL 的做法一致（都是悬浮窗 + 注入按键），但这里：
 *   * 布局来自共享的 sxcl.keymap.v1 JSON，电脑上编好直接推过来
 *   * 抬起手/切后台一定会 releaseAll()，不会出现"一直往前走"的经典 bug
 *   * 通知栏能直接停掉，不用回启动器
 */
public class KeymapOverlayService extends Service {

    public static final String ACTION_START = "com.silentstudio.sxcl.overlay.START";
    public static final String ACTION_STOP = "com.silentstudio.sxcl.overlay.STOP";
    public static final String EXTRA_FILE = "layout_file";
    public static final String EXTRA_HINTS = "show_hints";
    public static final String EXTRA_EDIT = "edit_mode";

    private static final String CHANNEL_ID = "sxcl_overlay";
    private static final int NOTIFICATION_ID = 4101;

    private WindowManager windowManager;
    private ControlOverlayView overlayView;
    private InputSink sink = new InputSink.LogSink();

    public static void start(Context context, String file, boolean hints, boolean edit) {
        Intent intent = new Intent(context, KeymapOverlayService.class);
        intent.setAction(ACTION_START);
        intent.putExtra(EXTRA_FILE, file);
        intent.putExtra(EXTRA_HINTS, hints);
        intent.putExtra(EXTRA_EDIT, edit);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent);
        } else {
            context.startService(intent);
        }
    }

    public static void stop(Context context) {
        Intent intent = new Intent(context, KeymapOverlayService.class);
        intent.setAction(ACTION_STOP);
        context.startService(intent);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            detachOverlay();
            stopSelf();
            return START_NOT_STICKY;
        }
        startForeground(NOTIFICATION_ID, buildNotification());
        String file = intent == null ? null : intent.getStringExtra(EXTRA_FILE);
        if (file == null) {
            file = KeymapPrefs.activeFile(this);
        }
        boolean hints = intent == null || intent.getBooleanExtra(EXTRA_HINTS, true);
        boolean edit = intent != null && intent.getBooleanExtra(EXTRA_EDIT, false);
        attachOverlay(file, hints, edit);
        return START_STICKY;
    }

    private void attachOverlay(String file, boolean hints, boolean edit) {
        KeymapLayout layout = AssetKeymaps.load(this, file);
        if (layout == null) {
            Toast.makeText(this, "读不到按键布局：" + file, Toast.LENGTH_LONG).show();
            stopSelf();
            return;
        }
        windowManager = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
        detachOverlay();

        overlayView = new ControlOverlayView(this);
        overlayView.setLayout(layout);
        overlayView.setInputSink(sink);
        overlayView.setInteractive(true);
        overlayView.setShowHints(hints);
        overlayView.setEditMode(edit);

        int type = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                type,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                        | WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.TOP | Gravity.START;
        try {
            windowManager.addView(overlayView, params);
        } catch (RuntimeException error) {
            Toast.makeText(this, "悬浮按键没法显示：" + error.getMessage(),
                    Toast.LENGTH_LONG).show();
            stopSelf();
        }
    }

    /** 一定要把按住的键松开再移除视图。 */
    private void detachOverlay() {
        if (overlayView != null) {
            overlayView.releaseAll();
            if (windowManager != null) {
                try {
                    windowManager.removeView(overlayView);
                } catch (RuntimeException ignored) {
                    // 已经移除过了
                }
            }
            overlayView = null;
        }
    }

    /** 外部（将来的 JNI 注入器）可以把自己的实现塞进来。 */
    public void setInputSink(InputSink sink) {
        this.sink = sink == null ? new InputSink.LogSink() : sink;
        if (overlayView != null) {
            overlayView.setInputSink(this.sink);
        }
    }

    @Override
    public void onDestroy() {
        detachOverlay();
        running = false;
        super.onDestroy();
    }

    private Notification buildNotification() {
        NotificationManager manager =
                (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && manager != null) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "虚拟按键",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("游戏中的按键覆盖层");
            manager.createNotificationChannel(channel);
        }
        Intent stopIntent = new Intent(this, KeymapOverlayService.class);
        stopIntent.setAction(ACTION_STOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        PendingIntent pending = PendingIntent.getService(this, 1, stopIntent, flags);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder
                .setContentTitle("虚拟按键已开启")
                .setContentText("在游戏里点通知的「停止」可以关掉")
                .setSmallIcon(android.R.drawable.ic_menu_edit)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(
                        android.R.drawable.ic_media_pause, "停止", pending).build())
                .build();
    }

    /** 供界面查询当前是否在跑（启动器的"停止悬浮按键"按钮要用）。 */
    private static volatile boolean running;

    public static boolean isRunning() {
        return running;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        running = true;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        detachOverlay();
        running = false;
        super.onTaskRemoved(rootIntent);
    }
}
