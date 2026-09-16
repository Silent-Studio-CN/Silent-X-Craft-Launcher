# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""按键映射页"看得见"的检查：不截图给人看，直接数像素。

    python scripts/check_keymap_ui.py

检查项：
  1. 每个控件在画布上都真的画出来了（中心像素 != 卡片底色）
  2. 搜索高亮会让强调色像素显著变多（"这个键在哪"真的能指出来）
  3. 横竖屏切换后手机区域长宽比符合预期
  4. 深色/浅色两套主题都能渲染出内容，且底色不同
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication            # noqa: E402
from qfluentwidgets import Theme                      # noqa: E402

from src.app.theme import apply_theme, qcolor         # noqa: E402
from src.core.keymap import build_preset              # noqa: E402


def accent_pixels(image, accent: str) -> int:
    """统计接近强调色的像素数（高亮效果）。"""
    from PySide6.QtGui import QColor

    target = QColor(accent)
    count = 0
    for y in range(0, image.height(), 2):
        for x in range(0, image.width(), 2):
            c = image.pixelColor(x, y)
            if (abs(c.red() - target.red()) + abs(c.green() - target.green())
                    + abs(c.blue() - target.blue())) < 60:
                count += 1
    return count


def main() -> int:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    from src.app.pages.keymap_page import KeymapPage

    page = KeymapPage()
    page.resize(1280, 820)
    page.show()
    app.processEvents()

    failures: list[str] = []
    report: list[str] = []

    for name, theme in (("深色", Theme.DARK), ("浅色", Theme.LIGHT)):
        apply_theme(theme, app)
        app.processEvents()
        canvas = page._canvas
        image = canvas.grab().toImage()
        bg = qcolor("card").name()
        card = image.pixelColor(int(canvas._phone_rect().center().x()),
                                int(canvas._phone_rect().center().y())).name()

        drawn, missing = 0, []
        for control in page.layout_data.controls:
            rect = canvas._to_screen(control.x, control.y, control.w, control.h)
            center = rect.center()
            pixel = image.pixelColor(int(center.x()), int(center.y())).name()
            if pixel != card:
                drawn += 1
            else:
                missing.append(control.id)

        surround = image.pixelColor(4, 4).name()
        report.append(f"{name}: 画布 {image.width()}x{image.height()} 手机内底色 {card}（card={bg}）"
                      f" 外底色 {surround}（bg={qcolor('bg').name()}）"
                      f" 控件 {drawn}/{len(page.layout_data.controls)} 可见")
        if surround != qcolor("bg").name():
            failures.append(f"{name}: 画布外底色 {surround} 不是页面背景色 {qcolor('bg').name()}")
        if missing:
            failures.append(f"{name}: 这些控件没画出来 -> {missing}")

        page._canvas.highlight("")
        app.processEvents()
        plain = accent_pixels(page._canvas.grab().toImage(), qcolor("accent").name())
        page._search.setText("潜行")
        app.processEvents()
        hot = accent_pixels(page._canvas.grab().toImage(), qcolor("accent").name())
        report.append(f"{name}: 强调色像素 无高亮={plain} 搜索高亮={hot}")
        if hot <= plain:
            failures.append(f"{name}: 搜索高亮没有让强调色像素增加（{plain} -> {hot}）")
        page._search.setText("")

        for screen, expect in (("portrait", 9 / 16), ("landscape", 16 / 9)):
            page._screen_combo.setCurrentIndex(1 if screen == "portrait" else 0)
            app.processEvents()
            phone = page._canvas._phone_rect()
            ratio = phone.width() / max(1.0, phone.height())
            report.append(f"{name}: {screen} 手机区域 {phone.width():.0f}x{phone.height():.0f} "
                          f"比例 {ratio:.3f}（期望 {expect:.3f}）")
            if abs(ratio - expect) > 0.02:
                failures.append(f"{name}: {screen} 比例不对 {ratio:.3f} != {expect:.3f}")
            if page.layout_data.screen != screen:
                failures.append(f"{name}: 切换 {screen} 后布局 screen={page.layout_data.screen}")

    # 冲突面板 & 教学面板在预设下都应有内容
    page._screen_combo.setCurrentIndex(0)
    app.processEvents()
    if page._guide_list.count() == 0:
        failures.append("教学面板是空的")
    if page._conflict_list.count() == 0:
        failures.append("冲突面板是空的（至少应有「没有发现问题」提示）")
    report.append(f"教学步骤 {page._guide_list.count()} 条，冲突面板 {page._conflict_list.count()} 行")

    print("\n".join(report))
    if failures:
        print("\n[FAIL]")
        for item in failures:
            print("  -", item)
        return 1
    print("\n[PASS] 按键映射页渲染检查通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
