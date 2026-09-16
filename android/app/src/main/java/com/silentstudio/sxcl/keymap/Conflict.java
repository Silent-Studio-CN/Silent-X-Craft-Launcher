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

package com.silentstudio.sxcl.keymap;

import java.util.ArrayList;
import java.util.List;

/** 一条校验/冲突结果（error 必须修，warning 建议修）。 */
public final class Conflict {

    public static final String ERROR = "error";
    public static final String WARNING = "warning";

    public final String level;
    public final String message;
    public final List<String> controls = new ArrayList<String>();

    public Conflict(String level, String message) {
        this.level = level;
        this.message = message;
    }

    public Conflict(String level, String message, List<String> controls) {
        this(level, message);
        if (controls != null) {
            this.controls.addAll(controls);
        }
    }

    public boolean isError() {
        return ERROR.equals(level);
    }

    /** 教学/提示界面里显示的那一行。 */
    public String display() {
        return (isError() ? "✗ " : "⚠ ") + message;
    }

    @Override
    public String toString() {
        return display();
    }
}
