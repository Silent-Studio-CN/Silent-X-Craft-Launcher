"""Custom exception hierarchy for Silent X Craft Launcher."""

from __future__ import annotations


class SXCLError(Exception):
    """Base exception for all launcher errors."""


# ── Configuration ─────────────────────────────────────────────────


class ConfigError(SXCLError):
    """Raised when configuration loading or validation fails."""


class ConfigNotFoundError(ConfigError):
    """Raised when a required configuration file is missing."""


# ── Network / Download ────────────────────────────────────────────


class NetworkError(SXCLError):
    """Raised when a network operation fails."""


class DownloadError(NetworkError):
    """Raised when a file download fails."""


class DownloadCancelledError(DownloadError):
    """Raised when the user cancels a download."""


class ChecksumMismatchError(DownloadError):
    """Raised when a downloaded file's hash does not match."""


# ── Java ──────────────────────────────────────────────────────────


class JavaError(SXCLError):
    """Raised when a Java-related operation fails."""


class JavaNotFoundError(JavaError):
    """Raised when no suitable Java runtime is found."""


class JavaVersionError(JavaError):
    """Raised when the Java version is incompatible."""


# ── Installation ──────────────────────────────────────────────────


class InstallationError(SXCLError):
    """Raised when game installation fails."""


class LoaderInstallationError(InstallationError):
    """Raised when mod loader installation fails."""


class VersionNotFoundError(InstallationError):
    """Raised when a requested Minecraft version is not found."""


# ── Launch ────────────────────────────────────────────────────────


class LaunchError(SXCLError):
    """Raised when game launch fails."""
