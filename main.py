# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# Silent X Craft Launcher (SXCL) is a third-party Minecraft launcher developed
# by the Silent X Craft Launcher Dev team, operating under the management of
# SilentCodeTeams, and overseen by SilentStudio.
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
"""Application entry point."""

import sys
import warnings
from pathlib import Path

# ── Initialise logging as early as possible ──
from src.core.logger import log
log.info("=" * 50)
log.info("Silent X Craft Launcher 启动")
log.info("=" * 50)

# ── Workaround: suppress noisy QFluentWidgets event-filter errors on ARM64 ──
# QFluentWidgets 1.11.2 + PySide6 6.11.1 on Python 3.14 ARM64 can produce
# spurious AttributeError inside style_sheet event filters when accessing
# ``e.type()``.  These are harmless (caught internally by Qt) but flood stderr.
# We patch the methods to swallow the exception gracefully.
try:
    from qfluentwidgets.common.style_sheet import (
        CustomStyleSheetWatcher,
        DirtyStyleSheetWatcher,
    )

    for _cls in (CustomStyleSheetWatcher, DirtyStyleSheetWatcher):
        _orig = _cls.eventFilter

        def _patched(self, obj, e, _orig=_orig):
            try:
                return _orig(self, obj, e)
            except AttributeError:
                return False

        _cls.eventFilter = _patched
except Exception:
    pass

# ── Workaround: suppress QEvent.Type deprecation warnings ──
warnings.filterwarnings("ignore", category=DeprecationWarning, module="qfluentwidgets")

# ===== 强制清除 pages 模块缓存 =====
print("=" * 60)
print("MAIN.PY - 启动")
print("=" * 60)

for name in list(sys.modules.keys()):
    if "pages" in name or "versions_page" in name:
        del sys.modules[name]
        print(f"✅ 清除缓存: {name}")

from PySide6.QtCore import Qt, QLoggingCategory
from PySide6.QtWidgets import QApplication
from qfluentwidgets import isDarkTheme, qconfig

# ── Suppress Qt warning noise from QFluentWidgets internals on ARM64 ──
QLoggingCategory.setFilterRules("*.warning=false\n*.critical=false")

from src.core.constants import APP_NAME, ORGANIZATION, DownloadSource
from src.core.lang import init_language
from src.app.common.launcher_config import load_config, cfg, LauncherLanguage
from src.app.main_window import MainWindow


def main() -> int:
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )

    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setOrganizationName(ORGANIZATION)
    # Fusion 样式会老老实实跟随调色板（Windows 默认样式在深色下会忽略部分调色板项）
    app.setStyle("Fusion")

    # ── 配置：只认 config.json（QFluentWidgets QConfig） ──
    # 历史遗留的 C:\ProgramData\sxcl\config.ini 曾在这里反向覆盖 8 项设置，
    # 而 UI 从不写那个文件，导致"设置重启后失效"。现在彻底不再读取它。
    load_config()

    # ── 从 QConfig 读取语言设置 ──
    lang_code = cfg.language.value.value.lower()  # LauncherLanguage.ZH_CN → "zh-cn"
    init_language(lang_code)

    # ── 游戏目录：没配 / 配的目录不存在 -> 自动检测一个 ──
    try:
        from pathlib import Path as _Path
        from src.services.minecraft.folders import best_game_folder, describe
        _current = cfg.gameDirectory.value
        if not _current or not _Path(_current).is_dir():
            _best = best_game_folder()
            if _best is not None:
                qconfig.set(cfg.gameDirectory, str(_best.path))
                save_config()
                log.info("自动检测到游戏目录: %s", describe(_best))
    except Exception as e:
        log.warning("游戏目录检测失败: %s", e)

    # ── 下载引擎：把限速/并发同步给全局引擎 ──
    try:
        from src.core.download import limiter
        limiter().set_rate(float(cfg.speedLimitKbps.value or 0) * 1024.0)
        from src.core.source_stats import stats as _source_stats
        log.info("下载引擎: 并发=%s, 限速=%sKB/s, 校验SHA1=%s, 下载源=%s",
                 cfg.maxConnections.value, cfg.speedLimitKbps.value, cfg.verifySha1.value,
                 cfg.downloadSource.value.label)
        log.info("下载源实测偏好: %s", _source_stats().snapshot() or "（暂无数据，默认镜像优先）")
    except Exception as e:
        log.warning("下载引擎初始化参数失败: %s", e)

    # ── Auto-detect Java if none configured ──
    if not cfg.javaPath.value:
        try:
            from src.services.java.finder import recommend_java  # noqa: PLC0415
            best = recommend_java()
            if best:
                qconfig.set(cfg.javaPath, str(best.path))
                log.info("自动检测到 Java: %s", best.display_name)
            else:
                log.warning("未检测到 Java 运行时，请前往设置页面手动选择或下载")
        except Exception as e:
            log.warning("Java 自动检测异常: %s", e)

    # ── 主题：只认库内置项 qconfig.themeMode ──
    # 之前自定义了一个 cfg.themeMode，和库的主题状态是两套东西，切换时各改各的，
    # 结果就是"导航栏变深色、内容区还是浅色"。现在统一走 apply_theme()。
    from src.app.theme import apply_theme
    apply_theme(qconfig.themeMode.value, app)
    log.info("主题: %s (当前深色=%s)", qconfig.themeMode.value.name, isDarkTheme())

    window = MainWindow()
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
