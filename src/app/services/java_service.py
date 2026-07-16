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
