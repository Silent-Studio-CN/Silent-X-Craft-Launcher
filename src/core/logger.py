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
