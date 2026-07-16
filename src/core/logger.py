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
"""Application-wide logging configuration.

Logs are written to ``{config_dir}/logs/log.log`` with daily rotation.
"""

from __future__ import annotations

import logging
import sys
import traceback
from pathlib import Path

from src.core.platform import default_config_directory


def _setup_logger() -> logging.Logger:
    """Configure and return the root application logger."""
    log_dir = default_config_directory("SilentXCraftLauncher") / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("sxcl")
    logger.setLevel(logging.DEBUG)

    # File handler — everything
    fh = logging.FileHandler(
        log_dir / "log.log",
        encoding="utf-8",
    )
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(
        logging.Formatter(
            "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        )
    )

    # Console handler — INFO and above, with HH:MM:SS
    ch = logging.StreamHandler(sys.stdout)
    ch.setLevel(logging.INFO)
    ch.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S"))

    logger.addHandler(fh)
    logger.addHandler(ch)

    return logger


log = _setup_logger()


def log_exception(
    logger: logging.Logger | None = None,
    message: str = "",
) -> None:
    """Log an exception with full traceback at ERROR level."""
    (logger or log).error(
        "%s\n%s",
        message,
        traceback.format_exc(),
    )
