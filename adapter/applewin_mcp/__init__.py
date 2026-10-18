# SPDX-License-Identifier: MIT
"""MCP agent adapter for AppleWinHarness (spec: docs/specs/m5.md)."""
import os


def _read_version():
    """VERSION is the repo root file (adapter/applewin_mcp/../../VERSION) or, in the release bundle, the file the
    packaging script stamps at adapter/VERSION (../VERSION)."""
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (os.path.join(here, "..", "..", "VERSION"), os.path.join(here, "..", "VERSION")):
        try:
            with open(path, encoding="ascii") as f:
                version = f.read().strip()
        except (OSError, UnicodeDecodeError):
            continue
        if version:
            return version
    return "0.0.0+unknown"


__version__ = _read_version()
