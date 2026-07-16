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
