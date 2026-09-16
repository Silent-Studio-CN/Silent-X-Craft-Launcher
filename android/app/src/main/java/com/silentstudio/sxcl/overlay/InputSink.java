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

import android.util.Log;

/**
 * 按键注入接口 —— 按键覆盖层和游戏本体之间的唯一接口。
 *
 * 手机上跑 Java 版 Minecraft 需要把触摸事件送进被打过补丁的 GLFW（FCL 是 JNI 直连
 * fcl_bridge.c）。这里不绑死实现：接上哪种注入器都行，覆盖层只管把
 * "KEY_SPACE 按下 / MOUSE_LEFT 抬起" 这种逻辑事件发出去。
 */
public interface InputSink {

    /** 游戏进程是否已就绪。没就绪时按键直接丢弃，不会排队卡住界面。 */
    boolean isConnected();

    void keyDown(String key);

    void keyUp(String key);

    default void keyTap(String key) {
        keyDown(key);
        keyUp(key);
    }

    default void mouseDown(String button) {
        keyDown(button);
    }

    default void mouseUp(String button) {
        keyUp(button);
    }

    /** 默认实现：只打日志，方便没接注入器时先把界面/教学跑通。 */
    final class LogSink implements InputSink {
        private static final String TAG = "SXCL-Input";

        @Override
        public boolean isConnected() {
            return true;
        }

        @Override
        public void keyDown(String key) {
            Log.d(TAG, "down " + key);
        }

        @Override
        public void keyUp(String key) {
            Log.d(TAG, "up " + key);
        }
    }
}
