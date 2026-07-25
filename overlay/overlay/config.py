"""Persistent settings for the xone overlay."""

from __future__ import annotations

import json
import logging
import os
from pathlib import Path
from typing import Any, Dict

log = logging.getLogger("xone-overlay.config")

POSITIONS = (
    "top",
    "right",
    "bottom",
    "left",
)

POSITION_LABELS = {
    "top": "Topo",
    "right": "Direita",
    "bottom": "Baixo",
    "left": "Esquerda",
}

# Migrate legacy corner/center keys from earlier builds
_LEGACY_POSITIONS = {
    "top-left": "top",
    "top-center": "top",
    "top-right": "top",
    "center": "top",
    "bottom-left": "bottom",
    "bottom-center": "bottom",
    "bottom-right": "bottom",
}

OPACITY_MIN = 0.40
OPACITY_MAX = 1.00
MARGIN_MIN = 16
MARGIN_MAX = 96

DEFAULTS: Dict[str, Any] = {
    "position": "top",
    "margin": 48,
    "opacity": 0.90,
    "screenshot_dir": "",
    "obs_host": "127.0.0.1",
    "obs_port": 4455,
    "obs_password": "",
}


def _config_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME")
    if base:
        root = Path(base)
    else:
        root = Path.home() / ".config"
    path = root / "xone-overlay"
    path.mkdir(parents=True, exist_ok=True)
    return path / "config.json"


def default_screenshot_dir() -> Path:
    return Path.home() / "Pictures" / "Screenshots"


def _clamp_opacity(value: Any) -> float:
    try:
        opacity = float(value)
    except (TypeError, ValueError):
        opacity = float(DEFAULTS["opacity"])
    return max(OPACITY_MIN, min(OPACITY_MAX, opacity))


def normalize_position(value: Any) -> str:
    if value in POSITIONS:
        return value  # type: ignore[return-value]
    if isinstance(value, str) and value in _LEGACY_POSITIONS:
        return _LEGACY_POSITIONS[value]
    return str(DEFAULTS["position"])


def normalize_screenshot_dir(value: Any) -> str:
    """Return an absolute directory path, or '' to mean the default."""
    if value is None:
        return ""
    text = str(value).strip()
    if not text:
        return ""
    try:
        path = Path(text).expanduser()
        if not path.is_absolute():
            path = Path.home() / path
        return str(path.resolve())
    except (OSError, RuntimeError, ValueError):
        return ""


def resolve_screenshot_dir(value: Any = None) -> Path:
    """Configured directory, falling back to ~/Pictures/Screenshots."""
    if value is None:
        value = load().get("screenshot_dir", "")
    normalized = normalize_screenshot_dir(value)
    if normalized:
        return Path(normalized)
    return default_screenshot_dir()


def is_vertical(position: str) -> bool:
    return normalize_position(position) in ("left", "right")


def load() -> Dict[str, Any]:
    path = _config_path()
    data = dict(DEFAULTS)
    if path.is_file():
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                data.update(raw)
        except (OSError, json.JSONDecodeError) as exc:
            log.warning("could not read config %s: %s", path, exc)

    data["position"] = normalize_position(data.get("position"))
    try:
        data["margin"] = max(
            MARGIN_MIN, min(MARGIN_MAX, int(data.get("margin", DEFAULTS["margin"])))
        )
    except (TypeError, ValueError):
        data["margin"] = DEFAULTS["margin"]
    data["opacity"] = _clamp_opacity(data.get("opacity", DEFAULTS["opacity"]))
    data["screenshot_dir"] = normalize_screenshot_dir(data.get("screenshot_dir", ""))
    data["obs_host"] = str(data.get("obs_host") or DEFAULTS["obs_host"]).strip() or "127.0.0.1"
    try:
        data["obs_port"] = max(1, min(65535, int(data.get("obs_port", DEFAULTS["obs_port"]))))
    except (TypeError, ValueError):
        data["obs_port"] = int(DEFAULTS["obs_port"])
    data["obs_password"] = str(data.get("obs_password") or "")
    return data


def save(data: Dict[str, Any]) -> None:
    path = _config_path()
    merged = dict(DEFAULTS)
    merged.update(data)
    merged["position"] = normalize_position(merged.get("position"))
    merged["opacity"] = _clamp_opacity(merged.get("opacity"))
    merged["screenshot_dir"] = normalize_screenshot_dir(
        merged.get("screenshot_dir", "")
    )
    merged["obs_host"] = str(merged.get("obs_host") or DEFAULTS["obs_host"]).strip() or "127.0.0.1"
    try:
        merged["obs_port"] = max(1, min(65535, int(merged.get("obs_port", DEFAULTS["obs_port"]))))
    except (TypeError, ValueError):
        merged["obs_port"] = int(DEFAULTS["obs_port"])
    merged["obs_password"] = str(merged.get("obs_password") or "")
    try:
        path.write_text(json.dumps(merged, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as exc:
        log.warning("could not write config %s: %s", path, exc)


def set_position(position: str) -> Dict[str, Any]:
    data = load()
    data["position"] = normalize_position(position)
    save(data)
    return data


def set_opacity(opacity: float) -> Dict[str, Any]:
    data = load()
    data["opacity"] = _clamp_opacity(opacity)
    save(data)
    return data


def set_screenshot_dir(directory: str) -> Dict[str, Any]:
    data = load()
    data["screenshot_dir"] = normalize_screenshot_dir(directory)
    save(data)
    return data


def set_obs_connection(host: str, port: int, password: str) -> Dict[str, Any]:
    data = load()
    data["obs_host"] = host
    data["obs_port"] = port
    data["obs_password"] = password
    save(data)
    return data
