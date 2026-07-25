"""Battery status for laptop and Xbox controller via UPower / sysfs."""

from __future__ import annotations

import glob
import os
from dataclasses import dataclass
from typing import Optional

try:
    import gi

    gi.require_version("UPowerGlib", "1.0")
    from gi.repository import UPowerGlib

    _HAS_UPOWER = True
except (ImportError, ValueError):
    UPowerGlib = None  # type: ignore
    _HAS_UPOWER = False


@dataclass
class BatteryInfo:
    name: str
    percent: Optional[int] = None
    level: Optional[str] = None  # Low / Normal / High / Full / Unknown
    charging: bool = False
    present: bool = False


_LEVEL_MAP = {
    0: "Unknown",
    1: "Critical",
    2: "Low",
    3: "Normal",
    4: "High",
    5: "Full",
}

_SYSFS_LEVEL = {
    "Unknown": "Unknown",
    "Critical": "Critical",
    "Low": "Low",
    "Normal": "Normal",
    "High": "High",
    "Full": "Full",
}


def _read_sysfs(path: str) -> Optional[str]:
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return None


def _controller_from_sysfs() -> Optional[BatteryInfo]:
    for supply in glob.glob("/sys/class/power_supply/*/"):
        model = _read_sysfs(os.path.join(supply, "model_name")) or ""
        name = os.path.basename(os.path.normpath(supply))
        scope = _read_sysfs(os.path.join(supply, "scope")) or ""
        look = f"{model} {name}".lower()
        if not any(k in look for k in ("xbox", "microsoft", "gip")):
            # Device-scoped batteries without a clear name still count if
            # model mentions controller-ish strings; skip system batteries.
            if scope != "Device" or "BAT" in name.upper():
                continue
            if not model:
                continue

        level_raw = _read_sysfs(os.path.join(supply, "capacity_level"))
        capacity = _read_sysfs(os.path.join(supply, "capacity"))
        status = (_read_sysfs(os.path.join(supply, "status")) or "").lower()

        percent = None
        if capacity is not None and capacity.isdigit():
            percent = int(capacity)

        level = None
        if level_raw:
            level = _SYSFS_LEVEL.get(level_raw, level_raw)

        return BatteryInfo(
            name=model or name,
            percent=percent,
            level=level,
            charging=status in ("charging", "full"),
            present=True,
        )
    return None


def _laptop_from_sysfs() -> Optional[BatteryInfo]:
    for supply in sorted(glob.glob("/sys/class/power_supply/BAT*/")):
        capacity = _read_sysfs(os.path.join(supply, "capacity"))
        status = (_read_sysfs(os.path.join(supply, "status")) or "").lower()
        model = _read_sysfs(os.path.join(supply, "model_name")) or "Laptop"
        percent = int(capacity) if capacity and capacity.isdigit() else None
        return BatteryInfo(
            name=model,
            percent=percent,
            level=None,
            charging=status in ("charging", "full"),
            present=percent is not None,
        )
    return None


def _is_controller_device(dev) -> bool:
    model = (dev.props.model or "").lower()
    native = (dev.props.native_path or "").lower()
    return any(k in model or k in native for k in ("xbox", "microsoft", "gip", "controller"))


def _battery_from_upower_device(dev) -> BatteryInfo:
    percent = None
    try:
        p = dev.props.percentage
        if p is not None and p >= 0:
            percent = int(round(p))
    except Exception:
        pass

    level = None
    try:
        # UPower capacity level enum when available
        if hasattr(dev.props, "battery_level"):
            level = _LEVEL_MAP.get(int(dev.props.battery_level))
    except Exception:
        pass

    state = None
    try:
        state = int(dev.props.state)
    except Exception:
        pass
    # 1=charging, 2=discharging, 4=fully charged
    charging = state in (1, 4) if state is not None else False

    return BatteryInfo(
        name=dev.props.model or dev.props.native_path or "Battery",
        percent=percent,
        level=level,
        charging=charging,
        present=True,
    )


def get_laptop_battery() -> BatteryInfo:
    if _HAS_UPOWER:
        try:
            client = UPowerGlib.Client.new()
            for dev in client.get_devices():
                if int(dev.props.kind) == int(UPowerGlib.DeviceKind.BATTERY):
                    if _is_controller_device(dev):
                        continue
                    info = _battery_from_upower_device(dev)
                    if info.percent is not None or info.present:
                        return info
        except Exception:
            pass

    sysfs = _laptop_from_sysfs()
    if sysfs:
        return sysfs
    return BatteryInfo(name="Laptop", present=False)


def get_controller_battery() -> BatteryInfo:
    if _HAS_UPOWER:
        try:
            client = UPowerGlib.Client.new()
            for dev in client.get_devices():
                if not _is_controller_device(dev):
                    continue
                return _battery_from_upower_device(dev)
        except Exception:
            pass

    sysfs = _controller_from_sysfs()
    if sysfs:
        return sysfs
    return BatteryInfo(name="Controller", present=False)


def format_battery(info: BatteryInfo) -> str:
    if not info.present:
        return "—"
    parts = []
    if info.percent is not None:
        parts.append(f"{info.percent}%")
    if info.level and info.level != "Unknown":
        parts.append(info.level)
    if not parts:
        return "—"
    text = " · ".join(parts)
    if info.charging:
        text += " (charging)"
    return text
