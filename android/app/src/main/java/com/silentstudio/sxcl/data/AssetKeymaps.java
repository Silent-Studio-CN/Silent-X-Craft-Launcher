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

import com.silentstudio.sxcl.keymap.KeymapLayout;
import com.silentstudio.sxcl.keymap.KeymapLibrary;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * 从 APK 的 assets/keymaps 读布局。
 *
 * 这些 JSON 由桌面端 `scripts/export_keymap_assets.py` 从同一份预设生成，
 * 所以手机上看到的按键和教学文案，跟电脑上编辑器里看到的完全一致。
 */
public final class AssetKeymaps {

    public static final String DIR = "keymaps";

    private AssetKeymaps() {
    }

    /** 用户自己改过的布局放这儿（手机上拖完位置保存，服务会优先读它）。 */
    public static File userDir(Context context) {
        File dir = new File(context.getFilesDir(), DIR);
        if (!dir.exists()) {
            dir.mkdirs();
        }
        return dir;
    }

    public static final class Loader implements KeymapLibrary.Loader {
        private final Context context;

        public Loader(Context context) {
            this.context = context.getApplicationContext();
        }

        @Override
        public String read(String name) throws Exception {
            File user = new File(userDir(context), name);
            if (user.isFile()) {
                return new String(java.nio.file.Files.readAllBytes(user.toPath()),
                        StandardCharsets.UTF_8);
            }
            InputStream stream = context.getAssets().open(DIR + "/" + name);
            try {
                ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                byte[] chunk = new byte[8192];
                int read;
                while ((read = stream.read(chunk)) > 0) {
                    buffer.write(chunk, 0, read);
                }
                return new String(buffer.toByteArray(), StandardCharsets.UTF_8);
            } finally {
                stream.close();
            }
        }
    }

    public static KeymapLibrary.Loader loader(Context context) {
        return new Loader(context);
    }

    /** 保存用户拖动后的布局；返回是否成功。 */
    public static boolean saveUserLayout(Context context, KeymapLayout layout, String fileName) {
        try {
            File target = new File(userDir(context), fileName);
            FileOutputStream out = new FileOutputStream(target);
            try {
                out.write(layout.toJsonString().getBytes(StandardCharsets.UTF_8));
            } finally {
                out.close();
            }
            return true;
        } catch (Exception error) {
            return false;
        }
    }

    /** 用户在手机上改过的布局文件名（导出回电脑用）。 */
    public static List<String> userFiles(Context context) {
        List<String> names = new ArrayList<String>();
        File[] files = userDir(context).listFiles();
        if (files == null) {
            return names;
        }
        for (File file : files) {
            if (file.getName().endsWith(".json")) {
                names.add(file.getName());
            }
        }
        return names;
    }

    public static KeymapLayout load(Context context, String fileName) {
        try {
            return KeymapLibrary.load(loader(context), fileName);
        } catch (Exception error) {
            return null;
        }
    }
}
