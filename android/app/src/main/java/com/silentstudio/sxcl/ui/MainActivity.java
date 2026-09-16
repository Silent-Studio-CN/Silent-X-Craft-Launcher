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

package com.silentstudio.sxcl.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.silentstudio.sxcl.data.AssetKeymaps;
import com.silentstudio.sxcl.data.KeymapPrefs;
import com.silentstudio.sxcl.keymap.Conflict;
import com.silentstudio.sxcl.keymap.KeymapLayout;
import com.silentstudio.sxcl.keymap.KeymapLibrary;
import com.silentstudio.sxcl.overlay.ControlOverlayView;
import com.silentstudio.sxcl.overlay.KeymapOverlayService;

import java.util.ArrayList;
import java.util.List;

/**
 * 安卓端主界面：选布局 → 预览（带教学提示）→ 开悬浮按键。
 *
 * 与 FCL 的差别（也是这一版的重点）：
 *   1. 每个键上方直接写明"是什么、怎么用"，长按/双击的区别也标出来
 *   2. 冲突检查：抢键、重叠、缺关键动作都会提示，FCL 得自己踩坑
 *   3. 编辑模式可以直接拖按键，改完存在手机上（悬浮服务优先读用户版本）
 *   4. 可选"单/双手"预设与横竖屏，坐标归一化，换机型不用重排
 */
public class MainActivity extends Activity implements ControlOverlayView.Listener {

    private final List<KeymapLibrary.Entry> entries = new ArrayList<KeymapLibrary.Entry>();
    private KeymapLayout current;
    private String currentFile = "";
    private ListView listView;
    private ControlOverlayView preview;
    private TextView titleView;
    private TextView subtitleView;
    private CheckBox hintBox;
    private CheckBox editBox;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        loadEntries();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (current != null && preview != null) {
            preview.setLayout(current);
        }
    }

    // ── 界面 ───────────────────────────────────────────────────

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#101418"));
        int pad = dp(12);
        root.setPadding(pad, pad, pad, pad);

        titleView = new TextView(this);
        titleView.setText("按键映射");
        titleView.setTextColor(Color.WHITE);
        titleView.setTextSize(20f);
        root.addView(titleView);

        subtitleView = new TextView(this);
        subtitleView.setTextColor(Color.parseColor("#9AA4AE"));
        subtitleView.setTextSize(12f);
        root.addView(subtitleView);

        listView = new ListView(this);
        listView.setBackgroundColor(Color.parseColor("#181C22"));
        root.addView(listView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(140)));

        hintBox = new CheckBox(this);
        hintBox.setText("显示教学提示");
        hintBox.setTextColor(Color.WHITE);
        hintBox.setChecked(KeymapPrefs.showHints(this));
        hintBox.setOnCheckedChangeListener((view, checked) -> {
            KeymapPrefs.setShowHints(this, checked);
            preview.setShowHints(checked);
        });

        editBox = new CheckBox(this);
        editBox.setText("编辑位置（拖动按键）");
        editBox.setTextColor(Color.WHITE);
        editBox.setChecked(KeymapPrefs.editMode(this));
        editBox.setOnCheckedChangeListener((view, checked) -> {
            KeymapPrefs.setEditMode(this, checked);
            preview.setEditMode(checked);
        });

        LinearLayout toggles = new LinearLayout(this);
        toggles.setOrientation(LinearLayout.HORIZONTAL);
        toggles.addView(hintBox);
        toggles.addView(editBox);
        root.addView(toggles);

        preview = new ControlOverlayView(this);
        preview.setInteractive(false);
        preview.setShowHints(hintBox.isChecked());
        preview.setListener(this);
        root.addView(preview, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(action("开始悬浮按键", view -> startOverlay()));
        buttons.addView(action("停止", view -> KeymapOverlayService.stop(this)));
        buttons.addView(action("教学", view -> openGuide()));
        buttons.addView(action("冲突检查", view -> showConflicts()));
        buttons.addView(action("保存", view -> saveUserLayout()));
        root.addView(buttons);
        return root;
    }

    private Button action(String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(12f);
        button.setAllCaps(false);
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        button.setLayoutParams(params);
        return button;
    }

    // ── 数据 ───────────────────────────────────────────────────

    private void loadEntries() {
        entries.clear();
        try {
            entries.addAll(KeymapLibrary.loadIndex(AssetKeymaps.loader(this)));
        } catch (Exception error) {
            Toast.makeText(this, "读取按键清单失败：" + error.getMessage(),
                    Toast.LENGTH_LONG).show();
        }
        List<String> labels = new ArrayList<String>();
        for (KeymapLibrary.Entry entry : entries) {
            String badge = entry.conflicts > 0 ? "  ⚠ " + entry.conflicts : "";
            labels.add(entry.label + "   " + entry.controlCount() + " 键 / 教学 "
                    + entry.guideSteps + " 条" + badge);
        }
        listView.setAdapter(new ArrayAdapter<String>(this,
                android.R.layout.simple_list_item_1, labels));
        listView.setOnItemClickListener((parent, view, position, id) -> select(entries.get(position)));

        String activeFile = KeymapPrefs.activeFile(this);
        int index = 0;
        for (int i = 0; i < entries.size(); i++) {
            if (entries.get(i).file.equals(activeFile)) {
                index = i;
            }
        }
        if (!entries.isEmpty()) {
            listView.setItemChecked(index, true);
            select(entries.get(index));
        }
    }

    private void select(KeymapLibrary.Entry entry) {
        currentFile = entry.file;
        current = AssetKeymaps.load(this, entry.file);
        KeymapPrefs.setActiveFile(this, entry.file);
        if (current != null) {
            titleView.setText(current.name + " · " + current.screenLabel());
            subtitleView.setText(current.description + "（" + entry.guideSteps
                    + " 条教学，长按/双击都有说明）");
            preview.setLayout(current);
            preview.setEditMode(editBox.isChecked());
        }
    }

    // ── 动作 ───────────────────────────────────────────────────

    private void startOverlay() {
        if (current == null) {
            Toast.makeText(this, "先选一套按键", Toast.LENGTH_SHORT).show();
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
            Toast.makeText(this, "请先允许「显示在其他应用上层」，回来再点一次",
                    Toast.LENGTH_LONG).show();
            return;
        }
        KeymapOverlayService.start(this, currentFile, hintBox.isChecked(), editBox.isChecked());
        Toast.makeText(this, "悬浮按键已开启，进游戏试试", Toast.LENGTH_SHORT).show();
    }

    private void openGuide() {
        Intent intent = new Intent(this, GuideActivity.class);
        intent.putExtra(GuideActivity.EXTRA_FILE, currentFile);
        startActivity(intent);
    }

    private void saveUserLayout() {
        if (current == null) {
            return;
        }
        boolean ok = AssetKeymaps.saveUserLayout(this, current, currentFile);
        Toast.makeText(this, ok ? "已保存在手机上（服务会优先用这份）" : "保存失败",
                Toast.LENGTH_SHORT).show();
    }

    private void showConflicts() {
        if (current == null) {
            return;
        }
        StringBuilder text = new StringBuilder();
        for (String error : current.validate()) {
            text.append("✗ ").append(error).append('\n');
        }
        for (Conflict conflict : current.conflicts()) {
            text.append(conflict.display()).append('\n');
        }
        if (text.length() == 0) {
            text.append("✓ 这套按键没有问题，直接用").append('\n');
        }
        text.append('\n').append("提示：抢键 = 同一个键被不同动作用；重叠 = 两个键压在一起，容易误触");

        ScrollView scroll = new ScrollView(this);
        TextView view = new TextView(this);
        view.setText(text.toString());
        view.setTextSize(14f);
        view.setPadding(dp(16), dp(16), dp(16), dp(16));
        scroll.addView(view);
        new AlertDialog.Builder(this)
                .setTitle("冲突检查")
                .setView(scroll)
                .setPositiveButton("知道了", null)
                .show();
    }

    // ── ControlOverlayView.Listener ────────────────────────────

    @Override
    public void onControlTouched(String controlId, String label, String hint, String gesture) {
        subtitleView.setText("「" + label + "」" + gesture + "：" + (hint == null ? "" : hint));
    }

    @Override
    public boolean onControlMoved(String controlId) {
        if (current != null) {
            subtitleView.setText("位置改好了，点「保存」存到手机上");
        }
        return true;
    }

    private int dp(int value) {
        return Math.round(getResources().getDisplayMetrics().density * value);
    }
}
