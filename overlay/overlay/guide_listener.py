"""Listen for Xbox Guide (BTN_MODE) without grabbing the device."""

from __future__ import annotations

import logging
import select
import threading
import time
from typing import Callable, List, Optional, Set

log = logging.getLogger("xone-overlay.guide")

try:
    import evdev
    from evdev import ecodes

    _HAS_EVDEV = True
except ImportError:
    evdev = None  # type: ignore
    ecodes = None  # type: ignore
    _HAS_EVDEV = False

NAME_HINTS = ("xbox", "microsoft", "gip", "gamepad", "controller")


def _device_matches(dev) -> bool:
    try:
        caps = dev.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.BTN_MODE not in caps:
            return False
    except (OSError, AttributeError):
        return False

    name = (dev.name or "").lower()
    # Prefer Xbox-like names; still accept any BTN_MODE joystick if named oddly
    if any(h in name for h in NAME_HINTS):
        return True
    # Fallback: joystick/gamepad with BTN_MODE
    try:
        if ecodes.BTN_GAMEPAD in caps or ecodes.BTN_SOUTH in caps:
            return True
    except Exception:
        pass
    return False


def list_guide_devices() -> List[str]:
    if not _HAS_EVDEV:
        return []
    paths: List[str] = []
    for path in evdev.list_devices():
        try:
            dev = evdev.InputDevice(path)
        except (OSError, PermissionError):
            continue
        try:
            if _device_matches(dev):
                paths.append(path)
                log.info("guide device: %s (%s)", path, dev.name)
        finally:
            try:
                dev.close()
            except Exception:
                pass
    return paths


class GuideListener:
    """
    Background watcher: on BTN_MODE key-down, invoke callback (from listener thread).
    Re-scans /dev/input periodically when devices appear/disappear.
    """

    def __init__(self, on_guide: Callable[[], None], rescan_interval: float = 2.0):
        self._on_guide = on_guide
        self._rescan_interval = rescan_interval
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._devices: dict = {}
        self._last_fire = 0.0
        self._debounce_s = 0.35

    @property
    def available(self) -> bool:
        return _HAS_EVDEV

    def start(self) -> None:
        if not _HAS_EVDEV:
            log.error("python3-evdev is not installed")
            return
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="guide-listener", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        self._close_all()

    def _close_all(self) -> None:
        for path, dev in list(self._devices.items()):
            try:
                dev.close()
            except Exception:
                pass
        self._devices.clear()

    def _open_devices(self, paths: Set[str]) -> None:
        # Close removed
        for path in list(self._devices):
            if path not in paths:
                try:
                    self._devices[path].close()
                except Exception:
                    pass
                del self._devices[path]
                log.info("removed guide device %s", path)

        for path in paths:
            if path in self._devices:
                continue
            try:
                dev = evdev.InputDevice(path)
                self._devices[path] = dev
                log.info("watching %s (%s)", path, dev.name)
            except (OSError, PermissionError) as exc:
                log.warning("cannot open %s: %s", path, exc)

    def _fire(self) -> None:
        now = time.monotonic()
        if now - self._last_fire < self._debounce_s:
            return
        self._last_fire = now
        try:
            self._on_guide()
        except Exception:
            log.exception("guide callback failed")

    def _run(self) -> None:
        next_scan = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_scan:
                try:
                    paths = set(list_guide_devices())
                    self._open_devices(paths)
                except Exception:
                    log.exception("rescan failed")
                next_scan = now + self._rescan_interval

            if not self._devices:
                self._stop.wait(0.5)
                continue

            fds = {dev.fd: dev for dev in self._devices.values()}
            try:
                readable, _, _ = select.select(list(fds.keys()), [], [], 0.5)
            except (OSError, ValueError):
                # Device disappeared mid-select
                self._open_devices(set(list_guide_devices()))
                continue

            for fd in readable:
                dev = fds.get(fd)
                if dev is None:
                    continue
                try:
                    for event in dev.read():
                        if event.type != ecodes.EV_KEY:
                            continue
                        if event.code != ecodes.BTN_MODE:
                            continue
                        # value 1 = press (edge)
                        if event.value == 1:
                            self._fire()
                except (OSError, BlockingIOError):
                    # Device unplugged
                    path = getattr(dev, "path", None)
                    if path and path in self._devices:
                        try:
                            self._devices[path].close()
                        except Exception:
                            pass
                        del self._devices[path]
                    break
