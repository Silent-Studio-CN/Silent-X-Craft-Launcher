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

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

import com.silentstudio.sxcl.keymap.Binding;
import com.silentstudio.sxcl.keymap.ControlButton;
import com.silentstudio.sxcl.keymap.DirectionControl;
import com.silentstudio.sxcl.keymap.GuideStep;
import com.silentstudio.sxcl.keymap.KeymapLayout;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * 屏幕上的虚拟按键层（也当预览用）。
 *
 * 比 FCL 多出来的东西都在这儿：
 *   * 教学提示：每个键上方直接写"这是什么、怎么用"，长按/双击的差别也写清楚
 *   * 高亮定位：搜索到某个动作时，对应按键会闪，不用满屏找
 *   * 归一化坐标：换手机、换分辨率、横竖屏切换都不会错位
 *   * 手势可调：长按时长、双击间隔都在一处定义，FCL 是写死的
 */
public class ControlOverlayView extends View {

    /** 长按判定 400ms，和 FCL 保持一致，用户从 FCL 过来不会有割裂感。 */
    public static final long LONG_PRESS_MS = 400L;
    /** 双击间隔同样 400ms。 */
    public static final long DOUBLE_CLICK_MS = 400L;

    public interface Listener {
        /** 教学模式下点到某个控件：界面拿它来显示提示。 */
        void onControlTouched(String controlId, String label, String hint, String gesture);

        /** 拖动了控件（编辑模式）：返回 true 表示已处理。 */
        boolean onControlMoved(String controlId);
    }

    private KeymapLayout layout;
    private InputSink sink = new InputSink.LogSink();
    private Listener listener;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();

    private boolean interactive = true;       // 预览时关掉，避免误发按键
    private boolean showHints = true;         // 教学提示常显
    private boolean editMode = false;         // 拖动改位置
    private int accent = Color.parseColor("#0067C0");
    private int panelColor = Color.parseColor("#33202020");
    private int textColor = Color.WHITE;

    private String highlightId = "";
    private long highlightUntil;
    private final Handler handler = new Handler(Looper.getMainLooper());

    /** 手指 -> 控件 id（多点触控：左手摇杆右手按键同时按不会互相顶掉）。 */
    private final Map<Integer, String> pointerControl = new HashMap<Integer, String>();
    /** 手指 -> 按下的时刻（判断长按）。 */
    private final Map<Integer, Long> pointerDownAt = new HashMap<Integer, Long>();
    /** 手指 -> 已触发的事件类型，避免重复发。 */
    private final Map<Integer, String> pointerFired = new HashMap<Integer, String>();
    /** 控件 -> 该控件上当前按住的逻辑键，抬手时要发 up。 */
    private final Map<String, List<String>> heldKeys = new HashMap<String, List<String>>();
    private final Map<String, Boolean> toggled = new HashMap<String, Boolean>();
    private final Set<String> pendingSingle = new HashSet<String>();
    private String lastClickedId = "";
    private long lastClickAt;
    private int dragPointer = -1;
    private float dragOffsetX;
    private float dragOffsetY;

    public ControlOverlayView(Context context) {
        this(context, null);
    }

    public ControlOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setFakeBoldText(false);
        setFocusable(false);
    }

    // ── 配置 ───────────────────────────────────────────────────

    public void setLayout(KeymapLayout layout) {
        releaseAll();
        this.layout = layout;
        invalidate();
    }

    public KeymapLayout getLayout() {
        return layout;
    }

    public void setInputSink(InputSink sink) {
        this.sink = sink == null ? new InputSink.LogSink() : sink;
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public void setInteractive(boolean interactive) {
        this.interactive = interactive;
    }

    public void setShowHints(boolean showHints) {
        this.showHints = showHints;
        invalidate();
    }

    public void setEditMode(boolean editMode) {
        this.editMode = editMode;
        invalidate();
    }

    public boolean isEditMode() {
        return editMode;
    }

    public void setAccentColor(int color) {
        this.accent = color;
        invalidate();
    }

    /** 让某个按键闪一下（"这个键在哪"）。 */
    public void highlight(String controlId) {
        this.highlightId = controlId == null ? "" : controlId;
        this.highlightUntil = System.currentTimeMillis() + 1500L;
        invalidate();
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (System.currentTimeMillis() >= highlightUntil) {
                    highlightId = "";
                    invalidate();
                }
            }
        }, 1600L);
    }

    // ── 绘制 ───────────────────────────────────────────────────

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (layout == null) {
            return;
        }
        int width = getWidth();
        int height = getHeight();
        for (Object control : layout.controls()) {
            double[] box = KeymapLayout.rectOf(control);
            rect.set((float) (box[0] * width), (float) (box[1] * height),
                    (float) ((box[0] + box[2]) * width), (float) ((box[1] + box[3]) * height));
            boolean hot = KeymapLayout.idOf(control).equals(highlightId);
            boolean pressed = heldKeys.containsKey(KeymapLayout.idOf(control));

            int alpha = (int) (255 * (control instanceof ControlButton
                    ? ((ControlButton) control).opacity : ((DirectionControl) control).opacity));
            fillPaint.setStyle(Paint.Style.FILL);
            fillPaint.setColor(panelColor);
            fillPaint.setAlpha(Math.max(60, alpha));
            strokePaint.setStyle(Paint.Style.STROKE);
            strokePaint.setStrokeWidth(hot || pressed ? 5f : 2f);
            strokePaint.setColor(hot ? accent : (pressed ? accent : Color.argb(150, 255, 255, 255)));

            if (control instanceof DirectionControl) {
                drawDirection(canvas, (DirectionControl) control);
            } else {
                ControlButton button = (ControlButton) control;
                float radius = "round".equals(button.shape)
                        ? Math.min(rect.width(), rect.height()) / 2f : 14f;
                if ("round".equals(button.shape)) {
                    canvas.drawCircle(rect.centerX(), rect.centerY(), radius, fillPaint);
                    canvas.drawCircle(rect.centerX(), rect.centerY(), radius, strokePaint);
                } else {
                    canvas.drawRoundRect(rect, radius, radius, fillPaint);
                    canvas.drawRoundRect(rect, radius, radius, strokePaint);
                }
                textPaint.setColor(textColor);
                textPaint.setTextSize(Math.max(22f, Math.min(rect.height() / 2.6f, 46f)));
                textPaint.setFakeBoldText(hot || pressed);
                String label = button.shortLabel();
                if (button.events.containsKey(ControlButton.LONG_PRESS)
                        && button.events.size() > 1) {
                    canvas.drawText(label, rect.centerX(), rect.centerY() - 4f, textPaint);
                    textPaint.setTextSize(Math.max(16f, textPaint.getTextSize() * 0.55f));
                    textPaint.setFakeBoldText(false);
                    canvas.drawText("长按另有功能", rect.centerX(), rect.centerY() + 26f, textPaint);
                } else {
                    canvas.drawText(label, rect.centerX(), rect.centerY() + textPaint.getTextSize() / 3f,
                            textPaint);
                }
            }

            if (showHints) {
                drawHint(canvas, control, box);
            }
        }
    }

    private void drawDirection(Canvas canvas, DirectionControl control) {
        float radius = Math.min(rect.width(), rect.height()) / 2f;
        canvas.drawCircle(rect.centerX(), rect.centerY(), radius, fillPaint);
        canvas.drawCircle(rect.centerX(), rect.centerY(), radius, strokePaint);
        // 死区画出来：用户一眼知道推多少才会走（FCL 不画，新手常常以为失灵）
        float dead = (float) (control.deadZone * radius);
        strokePaint.setStrokeWidth(1.5f);
        strokePaint.setColor(Color.argb(120, 255, 255, 255));
        canvas.drawCircle(rect.centerX(), rect.centerY(), dead, strokePaint);
        fillPaint.setColor(Color.argb(200, 255, 255, 255));
        canvas.drawCircle(rect.centerX(), rect.centerY(), Math.max(12f, radius * 0.16f), fillPaint);
        textPaint.setColor(textColor);
        textPaint.setTextSize(Math.max(18f, radius * 0.28f));
        textPaint.setFakeBoldText(false);
        canvas.drawText(control.shortLabel(), rect.centerX(), rect.top + radius * 0.42f, textPaint);
    }

    private void drawHint(Canvas canvas, Object control, double[] box) {
        String hint = KeymapLayout.hintOf(control);
        String keys = String.join(" ", KeymapLayout.keysOf(control));
        if ((hint == null || hint.isEmpty()) && keys.isEmpty()) {
            return;
        }
        textPaint.setTextSize(20f);
        textPaint.setFakeBoldText(false);
        textPaint.setColor(Color.argb(230, 255, 255, 255));
        float y = rect.top - 8f;
        if (y < 24f) {
            y = rect.bottom + 26f;
        }
        String line = hint == null || hint.isEmpty() ? keys : hint;
        if (line.length() > 22) {
            line = line.substring(0, 22) + "…";
        }
        canvas.drawText(line, rect.centerX(), y, textPaint);
    }

    // ── 触摸 ───────────────────────────────────────────────────

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!interactive || layout == null) {
            return false;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int index = event.getActionIndex();
                handleDown(event.getPointerId(index), event.getX(index), event.getY(index));
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    handleMove(event.getPointerId(i), event.getX(i), event.getY(i));
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int index = event.getActionIndex();
                handleUp(event.getPointerId(index));
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                releaseAll();
                return true;
            }
            default:
                return super.onTouchEvent(event);
        }
    }

    private void handleDown(int pointerId, float x, float y) {
        if (editMode) {
            Object control = controlAt(x, y);
            if (control != null) {
                double[] box = KeymapLayout.rectOf(control);
                dragPointer = pointerId;
                dragOffsetX = (float) (x / getWidth() - box[0]);
                dragOffsetY = (float) (y / getHeight() - box[1]);
            }
            return;
        }
        Object control = controlAt(x, y);
        if (control == null) {
            return;
        }
        String id = KeymapLayout.idOf(control);
        pointerControl.put(pointerId, id);
        pointerDownAt.put(pointerId, System.currentTimeMillis());
        pointerFired.put(pointerId, "");
        if (control instanceof DirectionControl) {
            heldKeys.put(id, new ArrayList<String>());
            handleMove(pointerId, x, y);
        }
        if (listener != null) {
            listener.onControlTouched(id, KeymapLayout.labelOf(control), KeymapLayout.hintOf(control),
                    "按住");
        }
        invalidate();
    }

    private void handleMove(int pointerId, float x, float y) {
        if (editMode && pointerId == dragPointer) {
            Object dragging = layout.control(pointerControl.containsKey(pointerId)
                    ? pointerControl.get(pointerId) : "");
            if (dragging != null && getWidth() > 0 && getHeight() > 0) {
                double[] box = KeymapLayout.rectOf(dragging);
                double nx = x / getWidth() - dragOffsetX;
                double ny = y / getHeight() - dragOffsetY;
                nx = Math.max(0, Math.min(1 - box[2], nx));
                ny = Math.max(0, Math.min(1 - box[3], ny));
                setRect(dragging, nx, ny);
                invalidate();
            }
            return;
        }
        String id = pointerControl.get(pointerId);
        if (id == null) {
            return;
        }
        Object control = layout.control(id);
        if (!(control instanceof DirectionControl)) {
            maybeFireLongPress(pointerId, id);
            return;
        }
        DirectionControl direction = (DirectionControl) control;
        double[] box = KeymapLayout.rectOf(direction);
        float centerX = (float) ((box[0] + box[2] / 2) * getWidth());
        float centerY = (float) ((box[1] + box[3] / 2) * getHeight());
        float dx = (x - centerX) / (float) (box[2] * getWidth() / 2);
        float dy = (y - centerY) / (float) (box[3] * getHeight() / 2);
        String want = direction.directionOf(dx, dy);
        List<String> held = heldKeys.get(id);
        if (held == null) {
            held = new ArrayList<String>();
            heldKeys.put(id, held);
        }
        List<String> next = new ArrayList<String>();
        if (!want.isEmpty()) {
            next.add(direction.keys.get(want));
            if (direction.isSprint(dx, dy)) {
                next.add(direction.sprintKey);
            }
        }
        for (String key : next) {
            if (!held.contains(key)) {
                sink.keyDown(key);
            }
        }
        for (String key : held) {
            if (!next.contains(key)) {
                sink.keyUp(key);
            }
        }
        held.clear();
        held.addAll(next);
        invalidate();
    }

    private void handleUp(int pointerId) {
        if (editMode && pointerId == dragPointer) {
            dragPointer = -1;
            if (listener != null) {
                listener.onControlMoved("");
            }
            return;
        }
        String id = pointerControl.remove(pointerId);
        Long downAt = pointerDownAt.remove(pointerId);
        String fired = pointerFired.remove(pointerId);
        if (id == null) {
            return;
        }
        Object control = layout.control(id);
        long held = downAt == null ? 0 : System.currentTimeMillis() - downAt;

        if (control instanceof ControlButton) {
            ControlButton button = (ControlButton) control;
            String gesture = "press";
            if (held >= LONG_PRESS_MS && button.events.containsKey(ControlButton.LONG_PRESS)) {
                gesture = ControlButton.LONG_PRESS;
            } else if (System.currentTimeMillis() - lastClickAt < DOUBLE_CLICK_MS
                    && id.equals(lastClickedId) && button.events.containsKey(ControlButton.DOUBLE_CLICK)) {
                gesture = ControlButton.DOUBLE_CLICK;
            } else if (button.events.containsKey(ControlButton.CLICK)) {
                gesture = ControlButton.CLICK;
            }
            if ("press".equals(gesture) && !button.events.containsKey(ControlButton.PRESS)
                    && button.events.containsKey(ControlButton.CLICK)) {
                gesture = ControlButton.CLICK;
            }
            Binding binding = button.events.get(gesture);
            if (binding == null) {
                binding = button.primary();
            }
            if (binding != null) {
                if (Binding.TOGGLE.equals(binding.behavior)) {
                    Boolean on = toggled.get(id);
                    boolean next = on == null || !on;
                    toggled.put(id, next);
                    for (String key : binding.keys) {
                        if (next) {
                            sink.keyDown(key);
                        } else {
                            sink.keyUp(key);
                        }
                    }
                } else if (Binding.TAP.equals(binding.behavior)) {
                    for (String key : binding.keys) {
                        sink.keyTap(key);
                    }
                } else if ("press".equals(gesture) || ControlButton.LONG_PRESS.equals(gesture)) {
                    for (String key : binding.keys) {
                        sink.keyUp(key);
                    }
                } else {
                    for (String key : binding.keys) {
                        sink.keyTap(key);
                    }
                }
            }
            lastClickedId = id;
            lastClickAt = System.currentTimeMillis();
            if (listener != null) {
                listener.onControlTouched(id, button.shortLabel(), button.hint, gestureLabel(gesture));
            }
        } else if (control instanceof DirectionControl) {
            List<String> keys = heldKeys.remove(id);
            if (keys != null) {
                for (String key : keys) {
                    sink.keyUp(key);
                }
            }
        }
        invalidate();
    }

    private void maybeFireLongPress(int pointerId, String id) {
        Long downAt = pointerDownAt.get(pointerId);
        if (downAt == null) {
            return;
        }
        String fired = pointerFired.get(pointerId);
        if (fired != null && !fired.isEmpty()) {
            return;
        }
        if (System.currentTimeMillis() - downAt < LONG_PRESS_MS) {
            return;
        }
        Object control = layout.control(id);
        if (!(control instanceof ControlButton)) {
            return;
        }
        ControlButton button = (ControlButton) control;
        Binding binding = button.events.get(ControlButton.LONG_PRESS);
        if (binding == null) {
            return;
        }
        pointerFired.put(pointerId, ControlButton.LONG_PRESS);
        if (Binding.TOGGLE.equals(binding.behavior)) {
            Boolean on = toggled.get(id);
            boolean next = on == null || !on;
            toggled.put(id, next);
            for (String key : binding.keys) {
                if (next) {
                    sink.keyDown(key);
                } else {
                    sink.keyUp(key);
                }
            }
        } else {
            for (String key : binding.keys) {
                sink.keyDown(key);
            }
            heldKeys.put(id + ":" + ControlButton.LONG_PRESS, new ArrayList<String>(binding.keys));
        }
        if (listener != null) {
            listener.onControlTouched(id, button.shortLabel(), button.hint, "长按");
        }
        invalidate();
    }

    /** 松开所有键（切前台/退出时一定要调，不然游戏里会一直往前走）。 */
    public void releaseAll() {
        for (List<String> keys : heldKeys.values()) {
            for (String key : keys) {
                sink.keyUp(key);
            }
        }
        heldKeys.clear();
        for (Map.Entry<String, Boolean> entry : toggled.entrySet()) {
            if (Boolean.TRUE.equals(entry.getValue())) {
                Object control = layout == null ? null : layout.control(entry.getKey());
                if (control instanceof ControlButton) {
                    Binding binding = ((ControlButton) control).events.get(ControlButton.PRESS);
                    if (binding != null) {
                        for (String key : binding.keys) {
                            sink.keyUp(key);
                        }
                    }
                }
            }
        }
        toggled.clear();
        pointerControl.clear();
        pointerDownAt.clear();
        pointerFired.clear();
        invalidate();
    }

    public List<String> activeToggleIds() {
        List<String> ids = new ArrayList<String>();
        for (Map.Entry<String, Boolean> entry : toggled.entrySet()) {
            if (Boolean.TRUE.equals(entry.getValue())) {
                ids.add(entry.getKey());
            }
        }
        return ids;
    }

    // ── 工具 ───────────────────────────────────────────────────

    private Object controlAt(float x, float y) {
        if (layout == null || getWidth() == 0 || getHeight() == 0) {
            return null;
        }
        double nx = x / getWidth();
        double ny = y / getHeight();
        List<Object> controls = layout.controls();
        for (int i = controls.size() - 1; i >= 0; i--) {
            Object control = controls.get(i);
            double[] box = KeymapLayout.rectOf(control);
            if (nx >= box[0] && nx <= box[0] + box[2] && ny >= box[1] && ny <= box[1] + box[3]) {
                return control;
            }
        }
        return null;
    }

    private static void setRect(Object control, double x, double y) {
        if (control instanceof ControlButton) {
            ((ControlButton) control).x = x;
            ((ControlButton) control).y = y;
        } else if (control instanceof DirectionControl) {
            ((DirectionControl) control).x = x;
            ((DirectionControl) control).y = y;
        }
    }

    private static String gestureLabel(String gesture) {
        if (ControlButton.LONG_PRESS.equals(gesture)) {
            return "长按";
        }
        if (ControlButton.DOUBLE_CLICK.equals(gesture)) {
            return "双击";
        }
        if (ControlButton.CLICK.equals(gesture)) {
            return "单击";
        }
        return "按住";
    }

    /** 教学面板要用：控件上所有手势的说明。 */
    public static List<GuideStep> guideSteps(KeymapLayout layout) {
        return GuideStep.fromLayout(layout);
    }
}
