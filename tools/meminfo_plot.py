#!/usr/bin/env python3
"""
meminfo CSV plotter
-------------------
Plot MemFree, MemAvailable, Swap, and Anon/File page stats
from a CSV produced by meminfo_sampler.sh.

Usage:
    python meminfo_plot.py <csvfile> [<csvfile2> ...]
    python meminfo_plot.py meminfo_samples_*.csv
"""

import sys
from pathlib import Path
from datetime import datetime

import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import matplotlib.ticker as mticker
from matplotlib.gridspec import GridSpec


# ── data loading ──────────────────────────────────────────────────────────────

def load_csv(path: Path) -> dict:
    ts = []
    mem_free = []; mem_avail = []
    swap_free = []; swap_total = []
    act_anon = []; inact_anon = []
    act_file = []; inact_file = []

    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            if len(p) < 4:
                continue
            try:
                ts.append(datetime.strptime(p[0], "%H:%M:%S"))
                mem_free.append(int(p[1]) / 1024)
                mem_avail.append(int(p[2]) / 1024  if len(p) > 2 else 0)
                swap_free.append(int(p[3]) / 1024  if len(p) > 3 else 0)
                swap_total.append(int(p[4]) / 1024 if len(p) > 4 else 0)
                act_anon.append(int(p[5]) / 1024   if len(p) > 5 else 0)
                inact_anon.append(int(p[6]) / 1024 if len(p) > 6 else 0)
                act_file.append(int(p[7]) / 1024   if len(p) > 7 else 0)
                inact_file.append(int(p[8]) / 1024 if len(p) > 8 else 0)
            except (ValueError, IndexError):
                continue

    swap_used = [t - f for t, f in zip(swap_total, swap_free)]
    return dict(
        ts=ts,
        mem_free=mem_free, mem_avail=mem_avail,
        swap_free=swap_free, swap_used=swap_used,
        act_anon=act_anon, inact_anon=inact_anon,
        act_file=act_file, inact_file=inact_file,
    )


# ── styling helpers ───────────────────────────────────────────────────────────

BG_FIG  = "#1C1C2E"
BG_AX   = "#13131F"
SPINE   = "#444"

PALETTE = [
    ("#4C9BE8", "#7BB8F0"),  # blue pair
    ("#E8644C", "#F0917B"),  # red pair
    ("#3DBE8B", "#70D4A8"),  # green pair
    ("#C97BDB", "#DBA0E8"),  # purple pair
    ("#E8C44C", "#F0D57B"),  # yellow pair
]

def style_ax(ax):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray")
    ax.yaxis.label.set_color("lightgray")
    ax.xaxis.label.set_color("lightgray")
    ax.grid(True, alpha=0.15, linestyle="--")
    ax.grid(True, which="minor", alpha=0.07, linestyle=":")
    ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=15, integer=True))
    ax.yaxis.set_minor_locator(mticker.AutoMinorLocator(4))
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:.0f}"))
    ax.tick_params(axis="y", which="minor", length=3, color="#555")
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)

def legend(ax):
    ax.legend(facecolor="#2A2A3E", labelcolor="white", framealpha=0.8, fontsize=8)


# ── per-file plot ─────────────────────────────────────────────────────────────

def plot_file(data: dict, axes: list, sfx: str, ci: int):
    ts = data["ts"]
    if not ts:
        return 0, 0
    n = len(ts)
    dur = (ts[-1] - ts[0]).total_seconds() if n > 1 else 0
    c0, c1 = PALETTE[ci % len(PALETTE)]

    ax_mem, ax_swap, ax_anon, ax_file = axes

    ax_mem.plot(ts, data["mem_free"],  lw=1.6, color=c0, label=f"MemFree{sfx}")
    ax_mem.plot(ts, data["mem_avail"], lw=1.6, color=c1, label=f"MemAvail{sfx}", linestyle="--")
    ax_mem.fill_between(ts, data["mem_free"], alpha=0.10, color=c0)

    ax_swap.plot(ts, data["swap_used"], lw=1.6, color=c0, label=f"SwapUsed{sfx}")
    ax_swap.plot(ts, data["swap_free"], lw=1.6, color=c1, label=f"SwapFree{sfx}", linestyle="--")
    ax_swap.fill_between(ts, data["swap_used"], alpha=0.10, color=c0)

    ax_anon.plot(ts, data["act_anon"],   lw=1.6, color=c0, label=f"Active(anon){sfx}")
    ax_anon.plot(ts, data["inact_anon"], lw=1.6, color=c1, label=f"Inactive(anon){sfx}", linestyle="--")
    ax_anon.fill_between(ts, data["inact_anon"], alpha=0.10, color=c1)

    ax_file.plot(ts, data["act_file"],   lw=1.6, color=c0, label=f"Active(file){sfx}")
    ax_file.plot(ts, data["inact_file"], lw=1.6, color=c1, label=f"Inactive(file){sfx}", linestyle="--")
    ax_file.fill_between(ts, data["inact_file"], alpha=0.10, color=c1)

    return n, dur


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    files = [Path(a) for a in sys.argv[1:]]
    if not files:
        print(__doc__)
        sys.exit(1)

    missing = [f for f in files if not f.exists()]
    if missing:
        for f in missing:
            print(f"File not found: {f}", file=sys.stderr)
        sys.exit(1)

    fig = plt.figure(figsize=(15, 12), facecolor=BG_FIG)
    fig.suptitle("/proc/meminfo — Memory Sampling",
                 fontsize=15, fontweight="bold", color="white", y=0.99)

    gs = GridSpec(4, 1, figure=fig, hspace=0.10)
    ax_mem  = fig.add_subplot(gs[0])
    ax_swap = fig.add_subplot(gs[1], sharex=ax_mem)
    ax_anon = fig.add_subplot(gs[2], sharex=ax_mem)
    ax_file = fig.add_subplot(gs[3], sharex=ax_mem)
    axes = [ax_mem, ax_swap, ax_anon, ax_file]

    for ax in axes:
        style_ax(ax)
    for ax in (ax_mem, ax_swap, ax_anon):
        plt.setp(ax.get_xticklabels(), visible=False)

    multi = len(files) > 1
    stats = []
    for i, f in enumerate(files):
        data = load_csv(f)
        sfx = f" ({f.stem})" if multi else ""
        n, dur = plot_file(data, axes, sfx, ci=i)
        if n:
            stats.append(f"{f.name}: {n} samples, {dur:.0f}s")

    ax_mem.set_ylabel("Free Mem (MB)", color="lightgray")
    ax_swap.set_ylabel("Swap (MB)",    color="lightgray")
    ax_anon.set_ylabel("Anon (MB)",    color="lightgray")
    ax_file.set_ylabel("File Cache (MB)", color="lightgray")
    ax_file.set_xlabel("Time",         color="lightgray")

    for ax in axes:
        legend(ax)

    fmt = mdates.DateFormatter("%H:%M:%S")
    ax_file.xaxis.set_major_formatter(fmt)
    fig.autofmt_xdate(rotation=30, ha="right")

    if stats:
        fig.text(0.01, 0.002, "  |  ".join(stats), fontsize=8, color="#888", va="bottom")

    plt.tight_layout(rect=[0, 0.015, 1, 0.985])
    plt.show()


if __name__ == "__main__":
    main()

