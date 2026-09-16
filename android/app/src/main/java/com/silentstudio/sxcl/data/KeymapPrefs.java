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

package com.silentstudio.sxcl.data;

import android.content.Context;
import android.content.SharedPreferences;

/** 记住用户选了哪套按键、悬浮层开没开。 */
public final class KeymapPrefs {

    private static final String FILE = "sxcl_keymap";
    private static final String KEY_ACTIVE = "active_file";
    private static final String KEY_HINTS = "show_hints";
    private static final String KEY_EDIT = "edit_mode";
    public static final String DEFAULT_ACTIVE = "minimal-landscape.json";

    private KeymapPrefs() {
    }

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    public static String activeFile(Context context) {
        return prefs(context).getString(KEY_ACTIVE, DEFAULT_ACTIVE);
    }

    public static void setActiveFile(Context context, String file) {
        prefs(context).edit().putString(KEY_ACTIVE, file).apply();
    }

    public static boolean showHints(Context context) {
        return prefs(context).getBoolean(KEY_HINTS, true);
    }

    public static void setShowHints(Context context, boolean show) {
        prefs(context).edit().putBoolean(KEY_HINTS, show).apply();
    }

    public static boolean editMode(Context context) {
        return prefs(context).getBoolean(KEY_EDIT, false);
    }

    public static void setEditMode(Context context, boolean edit) {
        prefs(context).edit().putBoolean(KEY_EDIT, edit).apply();
    }
}
