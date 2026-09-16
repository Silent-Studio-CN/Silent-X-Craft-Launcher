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
"""自研异步下载引擎（asyncio + 标准库）。

对外只需要三个东西：
    from src.core.download import download_specs, limiter, ProgressInfo
"""

from src.core.download.engine import (
    AsyncDownloadEngine,
    Cancelled,
    DownloadResult,
    HttpError,
    ProgressInfo,
    default_engine,
    download_specs,
    limiter,
)
from src.core.download.limiter import RateLimiter, parse_rate
from src.core.download.spec import (
    FileSpec,
    spec_for,
    specs_for_asset_objects,
    specs_for_java_runtime,
    specs_for_libraries,
    specs_for_version_json,
)
from src.core.download.verify import HashResult, sha1_file, verify_file

__all__ = [
    "AsyncDownloadEngine", "DownloadResult", "ProgressInfo", "HttpError", "Cancelled",
    "download_specs", "default_engine", "limiter", "RateLimiter", "parse_rate",
    "FileSpec", "spec_for", "specs_for_version_json", "specs_for_libraries",
    "specs_for_asset_objects", "specs_for_java_runtime",
    "verify_file", "sha1_file", "HashResult",
]
