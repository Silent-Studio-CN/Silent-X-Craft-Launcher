"""Re-export from src.core.platform — kept for backward compatibility."""

from src.core.platform import (           # noqa: F401
    PlatformType,
    ArchType,
    current_platform,
    current_arch,
    is_windows,
    is_macos,
    is_linux,
    is_arm64,
    java_executable_name,
    classpath_separator,
    create_no_window_flag,
    normalize_path,
    find_on_path,
    default_config_directory,
    default_game_directory,
    default_jvm_directory,
)

# Legacy aliases
system_name = current_platform
