"""Re-export from src.services.java.finder — kept for backward compatibility."""

from src.services.java.finder import (    # noqa: F401
    JavaInstallation,
    parse_java_major,
    compatibility_for_major,
    inspect_java,
    discover_java_installations,
    best_java_installation,
)
