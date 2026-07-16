# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher Dev.
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
"""Re-export from src.services.java.finder — kept for backward compatibility."""

from src.services.java.finder import (  # noqa: F401
    JavaInstallation,
    parse_java_major,
    inspect_java,
    discover_java_installations,
    best_java_installation,
)


class JavaService:
    """Legacy Java service interface wrapping the new module-level functions."""
    
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    @staticmethod
    def detect_all():
        return discover_java_installations()

    @staticmethod
    def find_best(mc_version, installations=None):
        if installations is None:
            installations = discover_java_installations()
        return best_java_installation(installations)
