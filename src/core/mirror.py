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
"""Download-source URL mirroring — centralises every official ↔ BMCLAPI mapping.

**Rule**: every place in the code that builds a download URL **must** pass it through
``maybe_mirror_url()`` so the configured download source is always respected.
"""

from __future__ import annotations

from src.core.constants import DownloadSource


# ── Official → BMCLAPI mirror map ─────────────────────────────────
# (order matters: more-specific prefixes first, generic fallbacks last)

_MIRROR_RULES: list[tuple[str, str]] = [
    # Minecraft version manifest
    ("piston-meta.mojang.com", "bmclapi2.bangbang93.com"),
    # Minecraft downloads (JAR, JSON, etc.)
    ("launcher.mojang.com", "bmclapi2.bangbang93.com"),
    ("launchermeta.mojang.com", "bmclapi2.bangbang93.com"),
    # Minecraft data (skins, etc.)
    ("piston-data.mojang.com", "bmclapi2.bangbang93.com"),
    # Minecraft assets (hashed object files)
    #   official: https://resources.download.minecraft.net/{hash[:2]}/{hash}
    #   mirror:   https://bmclapi2.bangbang93.com/assets/{hash[:2]}/{hash}
    ("resources.download.minecraft.net", "bmclapi2.bangbang93.com/assets"),
    # Minecraft libraries
    #   official: https://libraries.minecraft.net/{path}
    #   mirror:   https://bmclapi2.bangbang93.com/maven/{path}
    ("libraries.minecraft.net", "bmclapi2.bangbang93.com/maven"),
    # Forge
    ("maven.minecraftforge.net", "bmclapi2.bangbang93.com/maven"),
    ("files.minecraftforge.net", "bmclapi2.bangbang93.com/maven"),
    # Fabric
    ("meta.fabricmc.net", "bmclapi2.bangbang93.com/fabric-meta"),
    ("maven.fabricmc.net", "bmclapi2.bangbang93.com/maven"),
    # NeoForge
    #   official: https://maven.neoforged.net/releases/net/neoforged/forge
    #   mirror:   https://bmclapi2.bangbang93.com/maven/net/neoforged/forge
    ("maven.neoforged.net", "bmclapi2.bangbang93.com/maven"),
    # Quilt
    ("maven.quiltmc.org", "bmclapi2.bangbang93.com/maven"),
    ("meta.quiltmc.org", "bmclapi2.bangbang93.com/quilt-meta"),
]

_MIRROR_CACHE: dict[str, str] | None = None


def _build_mirror_map() -> dict[str, str]:
    """Build a *full-host* → *mirror-host* lookup from the rules above."""
    result: dict[str, str] = {}
    for official, mirror in _MIRROR_RULES:
        result[official] = mirror
    return result


def maybe_mirror_url(url: str, source: DownloadSource | str | None = None) -> str:
    """If *source* (or the global config) is BMCLAPI, replace official hosts.

    Args:
        url: The original download URL (official Mojang/Forge/etc.)
        source: Explicit source; if ``None``, reads from ``launcher_config.cfg``.

    Returns:
        The (possibly mirrored) URL.
    """
    # Resolve source
    if source is None or source == DownloadSource.BMCLAPI or source == "bmclapi":
        # Only mirror when source is BMCLAPI
        if source is None:
            # Lazy import to avoid circular dependency
            try:
                from src.app.common.launcher_config import cfg  # noqa: PLC0415
                configured = cfg.downloadSource.value
                if configured != DownloadSource.BMCLAPI:
                    return url
            except Exception:
                return url  # fall through — keep original on error
        # Apply mirror rules
        global _MIRROR_CACHE  # noqa: PLW0603
        if _MIRROR_CACHE is None:
            _MIRROR_CACHE = _build_mirror_map()
        for official, mirror in _MIRROR_RULES:
            if official in url:
                return url.replace(official, mirror, 1)
    return url


def is_bmclapi(source: DownloadSource | str | None = None) -> bool:
    """Check whether the current (or given) source is BMCLAPI."""
    if source is not None:
        return source == DownloadSource.BMCLAPI or source == "bmclapi"
    try:
        from src.app.common.launcher_config import cfg  # noqa: PLC0415
        return cfg.downloadSource.value == DownloadSource.BMCLAPI
    except Exception:
        return False
