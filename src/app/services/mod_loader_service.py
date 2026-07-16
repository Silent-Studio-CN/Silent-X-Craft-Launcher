# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher
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
"""Re-export from src.services.mod_loader.api — kept for backward compatibility."""

from src.services.mod_loader.api import (  # noqa: F401
    ForgeAPI,
    FabricAPI,
    NeoForgeAPI,
    LoaderSource,
    check_optifine,
    fetch_forge_versions,
    fetch_fabric_versions,
    fetch_neoforge_versions,
    filter_neoforge_by_mc_version,
)
