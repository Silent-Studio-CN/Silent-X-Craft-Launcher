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
from qfluentwidgets import setTheme, Theme, qconfig

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

    load_config()

    from src.app.common.config_manager import config

    qconfig.set(cfg.downloadSource, DownloadSource(config.get("download_source", "bmclapi")))
    qconfig.set(cfg.javaPath, config.get("java_path", ""))
    qconfig.set(cfg.maxMemoryMb, config.get("max_memory", 4096))
    qconfig.set(cfg.gameDirectory, config.get("game_directory", str(Path.home() / ".minecraft")))

    theme_str = config.get("theme", "auto")
    if theme_str == "light":
        qconfig.set(cfg.themeMode, Theme.LIGHT)
    elif theme_str == "dark":
        qconfig.set(cfg.themeMode, Theme.DARK)
    else:
        qconfig.set(cfg.themeMode, Theme.AUTO)

    lang_str = config.get("language", "zh-CN")
    qconfig.set(
        cfg.language,
        LauncherLanguage.ZH_CN if lang_str == "zh-CN" else LauncherLanguage.EN_US,
    )

    # ── 初始化语言系统（自动下载缺失的语言文件） ──
    init_language("zh-cn" if lang_str == "zh-CN" else "en-us")

    qconfig.set(cfg.autoCheckUpdate, config.get("auto_check_update", True))
    qconfig.set(cfg.debugMode, config.get("debug_mode", False))
    qconfig.set(cfg.versionIsolation, config.get("version_isolation", False))

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

    theme = cfg.themeMode.value
    if theme == Theme.LIGHT:
        setTheme(Theme.LIGHT)
    elif theme == Theme.DARK:
        setTheme(Theme.DARK)
    else:
        setTheme(Theme.AUTO)

    window = MainWindow()
    window.show()

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
