#!/usr/bin/env python3
"""Entrypoint for the xone Linux Mint overlay companion."""

from __future__ import annotations

import os
import sys

# Allow running from the overlay/ directory without installing the package.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from overlay.app import run


if __name__ == "__main__":
    sys.exit(run() or 0)
