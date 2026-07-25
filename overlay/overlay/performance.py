"""CPU / RAM / optional GPU utilization."""

from __future__ import annotations

import glob
import subprocess
import time
from dataclasses import dataclass
from typing import Optional


@dataclass
class PerformanceInfo:
    cpu_percent: Optional[float] = None
    ram_percent: Optional[float] = None
    gpu_percent: Optional[float] = None
    gpu_label: Optional[str] = None


_prev_cpu: Optional[tuple[int, int]] = None


def _read_cpu_times() -> Optional[tuple[int, int]]:
    try:
        with open("/proc/stat", encoding="utf-8") as fh:
            parts = fh.readline().split()
        # user nice system idle iowait irq softirq steal
        values = [int(x) for x in parts[1:8]]
        idle = values[3] + values[4]
        total = sum(values)
        return total, idle
    except (OSError, ValueError, IndexError):
        return None


def get_cpu_percent() -> Optional[float]:
    global _prev_cpu
    cur = _read_cpu_times()
    if cur is None:
        return None
    if _prev_cpu is None:
        _prev_cpu = cur
        time.sleep(0.05)
        cur2 = _read_cpu_times()
        if cur2 is None:
            return None
        prev = _prev_cpu
        _prev_cpu = cur2
        cur = cur2
    else:
        prev = _prev_cpu
        _prev_cpu = cur

    dt = cur[0] - prev[0]
    di = cur[1] - prev[1]
    if dt <= 0:
        return 0.0
    return max(0.0, min(100.0, 100.0 * (1.0 - di / dt)))


def get_ram_percent() -> Optional[float]:
    mem_total = None
    mem_available = None
    try:
        with open("/proc/meminfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("MemTotal:"):
                    mem_total = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    mem_available = int(line.split()[1])
        if mem_total and mem_available is not None:
            used = mem_total - mem_available
            return max(0.0, min(100.0, 100.0 * used / mem_total))
    except (OSError, ValueError, IndexError):
        pass
    return None


def _nvidia_gpu() -> Optional[tuple[float, str]]:
    try:
        out = subprocess.check_output(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu,name",
                "--format=csv,noheader,nounits",
            ],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.5,
        ).strip()
        if not out:
            return None
        line = out.splitlines()[0]
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 1:
            return None
        util = float(parts[0])
        name = parts[1] if len(parts) > 1 else "NVIDIA"
        return util, name
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def _amd_gpu() -> Optional[tuple[float, str]]:
    for path in glob.glob("/sys/class/drm/card*/device/gpu_busy_percent"):
        try:
            with open(path, encoding="utf-8") as fh:
                util = float(fh.read().strip())
            return util, "AMD GPU"
        except (OSError, ValueError):
            continue
    return None


def get_gpu() -> tuple[Optional[float], Optional[str]]:
    nvidia = _nvidia_gpu()
    if nvidia:
        return nvidia[0], nvidia[1]
    amd = _amd_gpu()
    if amd:
        return amd[0], amd[1]
    return None, None


def get_performance() -> PerformanceInfo:
    gpu, label = get_gpu()
    return PerformanceInfo(
        cpu_percent=get_cpu_percent(),
        ram_percent=get_ram_percent(),
        gpu_percent=gpu,
        gpu_label=label,
    )


def format_percent(value: Optional[float]) -> str:
    if value is None:
        return "—"
    return f"{value:.0f}%"
