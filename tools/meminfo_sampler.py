#!/usr/bin/env python3
"""
/proc/meminfo sampler
---------------------
Continuously samples MemFree and Swap from /proc/meminfo.
Press Ctrl+C (or send SIGTERM) to stop — the collected data is
then plotted as a time-series graph automatically.

Usage:
    python meminfo_sampler.py [--interval SECONDS]
"""

import argparse
import signal
import sys
import time
from datetime import datetime
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt

MEMINFO_PATH = Path("/proc/meminfo")


def parse_meminfo() -> dict[str, int]:
    """Return key → value (kB) from /proc/meminfo."""
    data: dict[str, int] = {}
    for line in MEMINFO_PATH.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2:
            data[parts[0].rstrip(":")] = int(parts[1])
    return data


def plot(
    timestamps: list[datetime],
    free_mem_kb: list[int],
    swap_free_kb: list[int],
    swap_total_kb: list[int],
    interval: float,
) -> None:
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 8), sharex=True)
    fig.suptitle("/proc/meminfo — Memory Sampling", fontsize=14, fontweight="bold")

    to_mb = lambda seq: [v / 1024 for v in seq]

    free_mb      = to_mb(free_mem_kb)
    swap_free_mb = to_mb(swap_free_kb)
    swap_used_mb = [(t - f) / 1024 for t, f in zip(swap_total_kb, swap_free_kb)]

    # --- Free memory ---
    ax1.plot(timestamps, free_mb, color="steelblue", linewidth=1.5, label="MemFree")
    ax1.fill_between(timestamps, free_mb, alpha=0.15, color="steelblue")
    ax1.set_ylabel("Free Memory (MB)")
    ax1.legend(loc="upper right")
    ax1.grid(True, alpha=0.3)

    # --- Swap ---
    ax2.plot(timestamps, swap_free_mb, color="mediumseagreen", linewidth=1.5, label="SwapFree")
    ax2.plot(timestamps, swap_used_mb, color="tomato",         linewidth=1.5, label="SwapUsed")
    ax2.fill_between(timestamps, swap_used_mb, alpha=0.15, color="tomato")
    ax2.set_ylabel("Swap (MB)")
    ax2.legend(loc="upper right")
    ax2.grid(True, alpha=0.3)

    time_fmt = mdates.DateFormatter("%H:%M:%S")
    ax2.xaxis.set_major_formatter(time_fmt)
    fig.autofmt_xdate()

    n = len(timestamps)
    duration = (timestamps[-1] - timestamps[0]).total_seconds() if n > 1 else 0
    fig.text(
        0.02, 0.005,
        f"Samples: {n}  |  Duration: {duration:.0f}s  |  Interval: {interval}s",
        fontsize=9, color="gray",
    )

    plt.tight_layout()
    plt.show()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interval", "-i", type=float, default=1.0, metavar="SECONDS",
                        help="Sampling interval in seconds (default: 1.0)")
    args = parser.parse_args()
    interval: float = args.interval

    timestamps:    list[datetime] = []
    free_mem_kb:   list[int]      = []
    swap_free_kb:  list[int]      = []
    swap_total_kb: list[int]      = []

    stop = False

    def _stop(sig, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT,  _stop)
    signal.signal(signal.SIGTERM, _stop)

    print(f"Sampling /proc/meminfo every {interval}s — press Ctrl+C to stop and plot.")

    while not stop:
        info = parse_meminfo()
        now = datetime.now()

        timestamps.append(now)
        free_mem_kb.append(info.get("MemFree", 0))
        swap_free_kb.append(info.get("SwapFree", 0))
        swap_total_kb.append(info.get("SwapTotal", 0))

        print(
            f"\r[{now.strftime('%H:%M:%S')}]  "
            f"MemFree: {free_mem_kb[-1] / 1024:8.1f} MB  "
            f"SwapFree: {swap_free_kb[-1] / 1024:8.1f} MB  "
            f"SwapUsed: {(swap_total_kb[-1] - swap_free_kb[-1]) / 1024:8.1f} MB",
            end="", flush=True,
        )

        time.sleep(interval)

    print("\nSampling stopped — plotting…")

    if len(timestamps) < 2:
        print("Need at least 2 samples to plot. Exiting.")
        sys.exit(0)

    plot(timestamps, free_mem_kb, swap_free_kb, swap_total_kb, interval)


if __name__ == "__main__":
    main()
