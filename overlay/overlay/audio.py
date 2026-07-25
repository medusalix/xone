"""System sink volume / mute via pactl (PulseAudio or PipeWire)."""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from typing import Optional


@dataclass
class AudioState:
    sink: Optional[str]
    volume_percent: int
    muted: bool
    available: bool


def _run(args: list[str]) -> Optional[str]:
    try:
        return subprocess.check_output(
            args, stderr=subprocess.DEVNULL, text=True, timeout=2
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def get_default_sink() -> Optional[str]:
    out = _run(["pactl", "get-default-sink"])
    return out or None


def get_audio_state() -> AudioState:
    sink = get_default_sink()
    if not sink:
        return AudioState(sink=None, volume_percent=0, muted=False, available=False)

    vol_out = _run(["pactl", "get-sink-volume", sink]) or ""
    mute_out = _run(["pactl", "get-sink-mute", sink]) or ""

    # Example: Volume: front-left: 30000 /  46% / -20.00 dB, ...
    percents = [int(m) for m in re.findall(r"(\d+)%", vol_out)]
    volume = max(percents) if percents else 0
    muted = "yes" in mute_out.lower()

    return AudioState(
        sink=sink,
        volume_percent=max(0, min(150, volume)),
        muted=muted,
        available=True,
    )


def set_volume_percent(percent: int) -> bool:
    sink = get_default_sink()
    if not sink:
        return False
    percent = max(0, min(150, int(percent)))
    try:
        subprocess.check_call(
            ["pactl", "set-sink-volume", sink, f"{percent}%"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=2,
        )
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def set_mute(muted: bool) -> bool:
    sink = get_default_sink()
    if not sink:
        return False
    try:
        subprocess.check_call(
            ["pactl", "set-sink-mute", sink, "1" if muted else "0"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=2,
        )
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def toggle_mute() -> Optional[bool]:
    state = get_audio_state()
    if not state.available:
        return None
    new = not state.muted
    if set_mute(new):
        return new
    return None
