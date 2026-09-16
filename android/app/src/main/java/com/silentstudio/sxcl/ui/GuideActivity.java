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
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import com.silentstudio.sxcl.data.AssetKeymaps;
import com.silentstudio.sxcl.keymap.GuideStep;
import com.silentstudio.sxcl.keymap.KeymapLayout;
import com.silentstudio.sxcl.overlay.ControlOverlayView;

import java.util.ArrayList;
import java.util.List;

/**
 * 教学界面 —— "比 FCL 更好的按键帮助"落地的地方。
 *
 * 三步走：① 上面列一步步怎么做 ② 点某一步，下面预览里对应按键闪一下
 *        ③ 有个搜索框，忘了某个键在哪直接搜（中文/英文/按键名/动作名都行）
 * 这是 FCL 完全没有的：它只有一堆能拖的按钮，没人告诉你该按哪个。
 */
public class GuideActivity extends Activity {

    public static final String EXTRA_FILE = "layout_file";

    private KeymapLayout layout;
    private List<GuideStep> steps = new ArrayList<GuideStep>();
    private ListView listView;
    private ArrayAdapter<String> adapter;
    private ControlOverlayView preview;
    private TextView detail;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String file = getIntent() == null ? null : getIntent().getStringExtra(EXTRA_FILE);
        layout = AssetKeymaps.load(this, file == null ? "minimal-landscape.json" : file);
        setContentView(buildUi());
        if (layout == null) {
            detail.setText("读不到布局文件");
            return;
        }
        steps = GuideStep.fromLayout(layout);
        preview.setLayout(layout);
        refresh("");
    }

    private ViewGroup buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#101418"));
        int pad = Math.round(getResources().getDisplayMetrics().density * 12);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("按键教学");
        title.setTextColor(Color.WHITE);
        title.setTextSize(20f);
        root.addView(title);

        EditText search = new EditText(this);
        search.setHint("忘了哪个键在哪？输入 潜行 / jump / 背包");
        search.setHintTextColor(Color.parseColor("#7A848E"));
        search.setTextColor(Color.WHITE);
        search.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable editable) {
                String query = editable.toString();
                refresh(query);
                List<String> hits = layout == null ? new ArrayList<String>() : layout.find(query);
                if (!hits.isEmpty()) {
                    preview.highlight(hits.get(0));
                }
            }
        });
        root.addView(search);

        detail = new TextView(this);
        detail.setTextColor(Color.parseColor("#9AA4AE"));
        detail.setTextSize(13f);
        root.addView(detail);

        listView = new ListView(this);
        listView.setBackgroundColor(Color.parseColor("#181C22"));
        root.addView(listView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        preview = new ControlOverlayView(this);
        preview.setInteractive(false);
        preview.setShowHints(true);
        root.addView(preview, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        listView.setOnItemClickListener((parent, view, position, id) -> {
            GuideStep step = steps.get(position);
            detail.setText(step.instruction + (step.why == null || step.why.isEmpty()
                    ? "" : "\n为什么：" + step.why));
            if (!step.controlId.isEmpty()) {
                preview.highlight(step.controlId);
            }
        });
        return root;
    }

    private void refresh(String query) {
        List<String> lines = new ArrayList<String>();
        String needle = query == null ? "" : query.trim().toLowerCase();
        for (GuideStep step : steps) {
            if (!needle.isEmpty()) {
                boolean hit = step.title.toLowerCase().contains(needle)
                        || step.instruction.toLowerCase().contains(needle)
                        || step.why.toLowerCase().contains(needle)
                        || step.controlId.toLowerCase().contains(needle)
                        || step.keys.toString().toLowerCase().contains(needle);
                if (!hit) {
                    continue;
                }
            }
            String flag = step.optional ? "（可选）" : "";
            lines.add(step.order + ". " + step.title + flag + "  [" + step.tapLabel() + "]");
        }
        if (lines.isEmpty()) {
            lines.add("没有匹配的步骤，换个词试试（比如 跳 / 背包 / 潜行）");
        }
        adapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, lines);
        listView.setAdapter(adapter);
        if (layout != null) {
            detail.setText(layout.name + " · " + layout.screenLabel() + "：" + lines.size()
                    + " 条教学。点一条看详细说明，对应按键会闪一下。");
        }
    }
}
