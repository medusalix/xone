"""Xbox / GIP controller helpers (power off via xone sysfs)."""

from __future__ import annotations

import glob
import logging
import os
from typing import List, Tuple

log = logging.getLogger("xone-overlay.controller")

_POWER_OFF_GLOB = "/sys/bus/xone-gip/devices/gip*.*/power_off"


def _is_gamepad_client(device_dir: str) -> bool:
    driver = os.path.join(device_dir, "driver")
    if not os.path.exists(driver):
        return False
    try:
        target = os.path.realpath(driver)
    except OSError:
        return False
    return "gamepad" in os.path.basename(target).lower()


def list_power_off_paths() -> List[str]:
    """Return writable power_off sysfs paths for connected gamepads."""
    paths: List[str] = []
    for path in sorted(glob.glob(_POWER_OFF_GLOB)):
        device_dir = os.path.dirname(path)
        if not _is_gamepad_client(device_dir):
            continue
        paths.append(path)
    return paths


def power_off_controllers() -> Tuple[bool, str]:
    """Send GIP power-off to all connected Xbox gamepads.

    Returns (ok, message). Controllers wake again with the Xbox/Guide button.
    """
    paths = list_power_off_paths()
    if not paths:
        # Fallback: any GIP client with power_off (older layouts / odd names)
        paths = sorted(glob.glob(_POWER_OFF_GLOB))
    if not paths:
        return False, "Nenhum controle GIP encontrado (driver xone atualizado?)"

    ok_count = 0
    last_err = ""
    for path in paths:
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("1\n")
            ok_count += 1
            log.info("powered off via %s", path)
        except PermissionError:
            last_err = "Sem permissão (grupo input + udev; relogue após install)"
            log.warning("power_off denied: %s", path)
        except OSError as exc:
            last_err = str(exc)
            log.warning("power_off failed %s: %s", path, exc)

    if ok_count:
        if ok_count == 1:
            return True, "Controle desligado"
        return True, f"{ok_count} controles desligados"
    return False, last_err or "Falha ao desligar o controle"
