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
"""Cross-platform language loader.

Language files are stored in ``{config_dir}/lang/`` and auto-downloaded
from GitHub if missing at startup.

File format
-----------
Simple ``key = value`` lines.  ``#`` for comments.  Empty lines ignored.

Usage
-----
::

    from src.core.lang import lang
    title = lang.get("page.versions.title", "Game Versions")
"""

from __future__ import annotations

from pathlib import Path

import requests

from src.core.logger import log
from src.core.platform import default_config_directory

# ── Remote sources ────────────────────────────────────────────────

_BASE_URL = "https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/lang"
_MIRROR_URL = "https://gh-proxy.com/https://raw.githubusercontent.com/Silent-Studio-CN/index/main/SXCL/lang"

_SUPPORTED = {"zh-cn", "en-us"}


# ── Language dictionary ───────────────────────────────────────────


class Language:
    """Thread-safe, lazy-loading language dictionary."""

    def __init__(self, lang_code: str = "zh-cn"):
        self._code = lang_code
        self._data: dict[str, str] = {}
        self._loaded = False

    @property
    def code(self) -> str:
        return self._code

    def get(self, key: str, default: str = "") -> str:
        """Return the translated string for *key*, or *default* if missing."""
        if not self._loaded:
            self._load()
        return self._data.get(key, default)

    def _load(self):
        """Load the .lang file from disk (downloading first if needed)."""
        self._loaded = True
        lang_dir = default_config_directory() / "lang"
        lang_dir.mkdir(parents=True, exist_ok=True)
        lang_file = lang_dir / f"{self._code}.lang"

        # Helper: try built-in fallback
        def _use_builtin():
            builtin = Path(__file__).resolve().parents[2] / "config" / "lang" / f"{self._code}.lang"
            if builtin.exists():
                import shutil
                shutil.copy2(builtin, lang_file)
                log.info("语言文件已从内置目录复制: %s", builtin)
                return True
            return False

        # Download if missing
        if not lang_file.exists():
            self._download(lang_file)

        # If still missing, try built-in
        if not lang_file.exists():
            _use_builtin()

        # Parse
        if lang_file.exists():
            self._parse(lang_file)
            # 如果解析到 0 条，说明文件内容无效，强制回退内置
            if not self._data:
                log.warning("语言文件内容无效（0 条），强制回退内置文件")
                _use_builtin()
                self._data = {}
                self._parse(lang_file)
        else:
            log.warning("语言文件不存在（远程和本地均无）: %s", lang_file)

    def _download(self, dest: Path):
        """Try to download the language file from remote mirrors."""
        name = f"{self._code}.lang"
        for base in [_MIRROR_URL, _BASE_URL]:
            url = f"{base}/{name}"
            try:
                resp = requests.get(url, timeout=10)
                resp.raise_for_status()
                text = resp.text
                # 验证内容有效（至少包含一个 key=value）
                if "=" not in text:
                    log.warning("语言文件内容无效（空/被拦截）: %s", url[:60])
                    continue
                dest.write_text(text, encoding="utf-8")
                log.info("语言文件已下载: %s", url)
                return
            except Exception as e:
                log.warning("语言文件下载失败: %s | %s", url[:60], e)
        log.warning("语言文件 %s 所有下载源均失败，使用内置后备", name)

    def _parse(self, path: Path):
        """Parse the ``key = value`` format."""
        try:
            text = path.read_text(encoding="utf-8")
            for line in text.splitlines():
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                if "=" in stripped:
                    key, value = stripped.split("=", 1)
                    self._data[key.strip()] = value.strip()
            log.info("语言文件已加载: %s (%d 条)", path, len(self._data))
        except Exception as e:
            log.error("语言文件解析失败: %s | %s", path, e)


# ── Global singleton ──────────────────────────────────────────────

lang = Language()


def init_language(lang_code: str = "zh-cn"):
    """Initialise the global language singleton (called once at startup)."""
    if lang_code not in _SUPPORTED:
        lang_code = "zh-cn"
    global lang
    lang = Language(lang_code)
    _ = lang.get("app.name")  # trigger load
