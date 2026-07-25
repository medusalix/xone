"""Screenshot capture via gnome-screenshot (+ crop for a single monitor)."""

from __future__ import annotations

import logging
import subprocess
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

import gi

gi.require_version("Gdk", "3.0")
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import Gdk, GdkPixbuf

from . import config

log = logging.getLogger("xone-overlay.screenshot")

Mode = str  # fullscreen | area | window | monitor


def default_screenshot_dir() -> Path:
    return config.default_screenshot_dir()


def list_monitors() -> List[Dict[str, Any]]:
    """Return connected monitors with geometry suitable for cropping."""
    display = Gdk.Display.get_default()
    if display is None:
        return []

    monitors: List[Dict[str, Any]] = []
    n = display.get_n_monitors()
    for i in range(n):
        mon = display.get_monitor(i)
        if mon is None:
            continue
        geo = mon.get_geometry()
        model = ""
        try:
            model = (mon.get_model() or "").strip()
        except Exception:
            model = ""
        manufacturer = ""
        try:
            manufacturer = (mon.get_manufacturer() or "").strip()
        except Exception:
            manufacturer = ""
        name_bits = [p for p in (manufacturer, model) if p]
        name = " ".join(name_bits) if name_bits else f"Monitor {i + 1}"
        primary = False
        try:
            primary = bool(mon.is_primary())
        except Exception:
            primary = i == 0
        label = f"{name} ({geo.width}×{geo.height})"
        if primary:
            label += " · principal"
        monitors.append(
            {
                "index": i,
                "label": label,
                "x": int(geo.x),
                "y": int(geo.y),
                "width": int(geo.width),
                "height": int(geo.height),
                "primary": primary,
            }
        )
    return monitors


def _prepare_path(directory: Optional[Union[str, Path]]) -> Tuple[Optional[Path], str]:
    if directory is None:
        target = config.resolve_screenshot_dir()
    else:
        target = config.resolve_screenshot_dir(str(directory))
    try:
        target.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        return None, f"pasta inválida: {exc}"
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    return target / f"xone-overlay_{stamp}.png", ""


def _run_gnome_screenshot(args: List[str], timeout: int) -> Tuple[bool, str]:
    try:
        subprocess.check_call(
            args,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=timeout,
        )
        return True, ""
    except FileNotFoundError:
        return False, "gnome-screenshot não instalado"
    except subprocess.TimeoutExpired:
        return False, "tempo esgotado na captura"
    except subprocess.CalledProcessError:
        return False, "Captura cancelada"
    except subprocess.SubprocessError as exc:
        return False, f"falha na captura: {exc}"


def _crop_pixbuf_to_monitor(src: Path, dest: Path, mon: Dict[str, Any]) -> Tuple[bool, str]:
    try:
        full = GdkPixbuf.Pixbuf.new_from_file(str(src))
    except Exception as exc:
        return False, f"não foi possível ler a captura: {exc}"

    img_w = full.get_width()
    img_h = full.get_height()
    x = max(0, min(mon["x"], img_w - 1))
    y = max(0, min(mon["y"], img_h - 1))
    w = max(1, min(mon["width"], img_w - x))
    h = max(1, min(mon["height"], img_h - y))

    try:
        cropped = full.new_subpixbuf(x, y, w, h)
        if cropped is None:
            return False, "recorte do monitor falhou"
        cropped.savev(str(dest), "png", [], [])
    except Exception as exc:
        return False, f"recorte falhou: {exc}"
    return True, ""


def take_screenshot(
    mode: Mode = "fullscreen",
    monitor_index: Optional[int] = None,
    directory: Optional[Union[str, Path]] = None,
) -> Tuple[bool, str]:
    """
    Capture the screen according to mode.

    Modes:
      - fullscreen: entire virtual desktop (all monitors)
      - area: interactive region selection
      - window: interactive window / application selection
      - monitor: single monitor by index (crop from full capture)

    Returns (ok, message) where message is the saved path or an error string.
    """
    path, err = _prepare_path(directory)
    if path is None:
        return False, err

    mode = (mode or "fullscreen").lower()
    log.info("screenshot mode=%s monitor=%s → %s", mode, monitor_index, path)

    if mode == "area":
        ok, msg = _run_gnome_screenshot(
            ["gnome-screenshot", "-a", "-f", str(path)], timeout=120
        )
        if not ok:
            return False, msg
        return (True, str(path)) if path.is_file() else (False, "arquivo não criado")

    if mode == "window":
        ok, msg = _run_gnome_screenshot(
            ["gnome-screenshot", "-w", "-f", str(path)], timeout=120
        )
        if not ok:
            return False, msg
        return (True, str(path)) if path.is_file() else (False, "arquivo não criado")

    if mode == "monitor":
        monitors = list_monitors()
        if not monitors:
            return False, "nenhum monitor detectado"
        if monitor_index is None:
            monitor_index = next(
                (m["index"] for m in monitors if m.get("primary")), monitors[0]["index"]
            )
        mon = next((m for m in monitors if m["index"] == monitor_index), None)
        if mon is None:
            return False, f"monitor {monitor_index} não encontrado"

        with tempfile.TemporaryDirectory(prefix="xone-shot-") as tmp:
            full_path = Path(tmp) / "full.png"
            ok, msg = _run_gnome_screenshot(
                ["gnome-screenshot", "-f", str(full_path)], timeout=20
            )
            if not ok:
                return False, msg
            if not full_path.is_file():
                return False, "arquivo temporário não criado"
            ok, msg = _crop_pixbuf_to_monitor(full_path, path, mon)
            if not ok:
                return False, msg
        return (True, str(path)) if path.is_file() else (False, "arquivo não criado")

    # fullscreen (default)
    ok, msg = _run_gnome_screenshot(
        ["gnome-screenshot", "-f", str(path)], timeout=20
    )
    if not ok:
        return False, msg
    return (True, str(path)) if path.is_file() else (False, "arquivo não criado")
