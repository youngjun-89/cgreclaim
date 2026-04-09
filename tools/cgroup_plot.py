#!/usr/bin/env python3
"""
cgroup_plot.py
--------------
Plot per-cgroup memory CSVs produced by cgroup_sampler.py.

Modes:
  --merge     Overlay all cgroups on the same axes (comparison)
  (default)   One subplot column per cgroup (individual view)

Panels (5 rows):
  1. Memory usage (mem_mb)
  2. Swap usage (swap_mb)
  3. Major fault rate (/s) split by type:
       anon majfault/s = pgswapin_rate         (swap-in, anon pages from disk)
       file majfault/s = pgmajfault_rate - pgswapin_rate  (file-backed pages from disk)
  4. Workingset refault rate (/s)  — anon + file thrashing
  5. PSI avg10 (some / full)       — memory pressure stall %

Usage:
    # Individual view (one column per cgroup):
    python cgroup_plot.py log/foo.csv log/bar.csv

    # Merged view (all cgroups overlaid):
    python cgroup_plot.py --merge log/*.csv
"""

import argparse
import sys
from pathlib import Path
from datetime import datetime

import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import matplotlib.ticker as mticker
from matplotlib.gridspec import GridSpec

# ── palette ───────────────────────────────────────────────────────────────────

BG_FIG = "#1C1C2E"
BG_AX  = "#13131F"
SPINE  = "#444"

COLORS = [
    "#4C9BE8", "#E8644C", "#3DBE8B", "#C97BDB",
    "#E8C44C", "#E87A4C", "#4CE8C4", "#E84C9B",
]


# ── CSV loader ────────────────────────────────────────────────────────────────

# CSV columns:
# timestamp, mem_mb, swap_mb,
# pgmajfault, pgmajfault_rate,
# pgswapin, pgswapin_rate,
# ws_refault_anon, ws_refault_anon_rate,
# ws_refault_file, ws_refault_file_rate,
# psi_some_avg10, psi_full_avg10

def load_csv(path: Path) -> dict:
    ts = []
    mem_mb = []; swap_mb = []
    pgmaj_rate = []
    pgswapin_rate = []
    ws_anon_rate = []; ws_file_rate = []
    psi_some = []; psi_full = []

    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            # Support both old (11-col) and new (13-col) format
            if len(p) < 11:
                continue
            try:
                ts.append(datetime.strptime(p[0], "%H:%M:%S"))
                mem_mb.append(float(p[1]))
                swap_mb.append(float(p[2]))
                # p[3] = pgmajfault cumulative, p[4] = rate
                pgmaj_rate.append(float(p[4]))
                if len(p) >= 13:
                    # new format: p[5]=pgswapin, p[6]=pgswapin_rate
                    pgswapin_rate.append(float(p[6]))
                    ws_anon_rate.append(float(p[8]))
                    ws_file_rate.append(float(p[10]))
                    psi_some.append(float(p[11]))
                    psi_full.append(float(p[12]))
                else:
                    # old format without pgswapin
                    pgswapin_rate.append(0.0)
                    ws_anon_rate.append(float(p[6]))
                    ws_file_rate.append(float(p[8]))
                    psi_some.append(float(p[9]))
                    psi_full.append(float(p[10]))
            except (ValueError, IndexError):
                continue

    # file majfault/s = pgmajfault_rate - pgswapin_rate (clamped ≥ 0)
    majfault_anon_rate = pgswapin_rate
    majfault_file_rate = [max(0.0, m - s) for m, s in zip(pgmaj_rate, pgswapin_rate)]

    n = len(ts)
    dur = (ts[-1] - ts[0]).total_seconds() if n > 1 else 0
    return dict(
        ts=ts, n=n, dur=dur,
        mem_mb=mem_mb, swap_mb=swap_mb,
        pgmaj_rate=pgmaj_rate,
        majfault_anon_rate=majfault_anon_rate,
        majfault_file_rate=majfault_file_rate,
        ws_anon_rate=ws_anon_rate, ws_file_rate=ws_file_rate,
        psi_some=psi_some, psi_full=psi_full,
    )


# ── axis styling ──────────────────────────────────────────────────────────────

def style_ax(ax, ylabel: str, hide_xlabel: bool = True):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray", labelsize=7)
    ax.set_ylabel(ylabel, color="lightgray", fontsize=8)
    ax.grid(True, alpha=0.15, linestyle="--")
    ax.grid(True, which="minor", alpha=0.07, linestyle=":")
    ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=8, integer=False))
    ax.yaxis.set_minor_locator(mticker.AutoMinorLocator(4))
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:.1f}"))
    ax.tick_params(axis="y", which="minor", length=2, color="#555")
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)
    if hide_xlabel:
        plt.setp(ax.get_xticklabels(), visible=False)


def finalize_xaxis(ax):
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    ax.tick_params(axis="x", colors="lightgray", labelsize=7)


def add_legend(ax, max_items: int = 20):
    handles, labels = ax.get_legend_handles_labels()
    if len(handles) > max_items:
        handles = handles[:max_items]
        labels = labels[:max_items]
        labels[-1] = f"… ({len(ax.get_lines())} total)"
    ax.legend(handles, labels, facecolor="#2A2A3E", labelcolor="white", framealpha=0.8,
              fontsize=7, loc="upper right")


# ── merged view ───────────────────────────────────────────────────────────────

def plot_merged(datasets: list[tuple[str, dict]]):
    nrows = 5
    fig = plt.figure(figsize=(14, 12), facecolor=BG_FIG)
    fig.suptitle("cgroup Memory Stats — Merged View",
                 fontsize=14, fontweight="bold", color="white", y=0.99)

    gs = GridSpec(nrows, 1, figure=fig, hspace=0.08)
    axes = [fig.add_subplot(gs[i]) for i in range(nrows)]
    labels = ["Mem (MB)", "Swap (MB)", "MajFault/s (anon=swap / file=disk)", "WS Refault/s", "PSI avg10 (%)"]
    for i, (ax, lbl) in enumerate(zip(axes, labels)):
        style_ax(ax, lbl, hide_xlabel=(i < nrows - 1))
    finalize_xaxis(axes[-1])

    for idx, (name, d) in enumerate(datasets):
        c = COLORS[idx % len(COLORS)]
        ts = d["ts"]
        axes[0].plot(ts, d["mem_mb"],              lw=1.5, color=c, label=name)
        axes[1].plot(ts, d["swap_mb"],             lw=1.5, color=c, label=name)
        axes[2].plot(ts, d["majfault_anon_rate"],  lw=1.5, color=c, label=f"{name} anon(swap-in)")
        axes[2].plot(ts, d["majfault_file_rate"],  lw=1.0, color=c, linestyle="--", label=f"{name} file(disk)")
        ws_total = [a + b for a, b in zip(d["ws_anon_rate"], d["ws_file_rate"])]
        axes[3].plot(ts, ws_total,                 lw=1.5, color=c, label=f"{name} (anon+file)")
        axes[4].plot(ts, d["psi_some"],            lw=1.5, color=c, label=f"{name} some")
        axes[4].plot(ts, d["psi_full"],            lw=1.0, color=c, linestyle="--", label=f"{name} full")

    for ax in axes:
        add_legend(ax)

    stats = "  |  ".join(f"{n}: {d['n']} samples, {d['dur']:.0f}s" for n, d in datasets)
    fig.text(0.01, 0.002, stats, fontsize=7, color="#888", va="bottom")
    fig.autofmt_xdate(rotation=30, ha="right")
    plt.tight_layout(rect=[0, 0.015, 1, 0.985])

# ── individual view ───────────────────────────────────────────────────────────

def plot_individual(datasets: list[tuple[str, dict]]):
    nrows = 5
    ncols = len(datasets)
    fig = plt.figure(figsize=(7 * ncols, 12), facecolor=BG_FIG)
    fig.suptitle("cgroup Memory Stats — Individual View",
                 fontsize=14, fontweight="bold", color="white", y=0.99)

    gs = GridSpec(nrows, ncols, figure=fig, hspace=0.08, wspace=0.25)

    row_labels = ["Mem (MB)", "Swap (MB)", "MajFault/s (anon=swap / file=disk)", "WS Refault/s", "PSI avg10 (%)"]

    for col, (name, d) in enumerate(datasets):
        c = COLORS[col % len(COLORS)]
        c2 = COLORS[(col + 2) % len(COLORS)]
        ts = d["ts"]

        axes_col = [fig.add_subplot(gs[row, col]) for row in range(nrows)]

        for row, ax in enumerate(axes_col):
            style_ax(ax, row_labels[row] if col == 0 else "",
                     hide_xlabel=(row < nrows - 1))
            if col > 0:
                ax.yaxis.set_tick_params(labelleft=True)

        if col == 0:
            for row, ax in enumerate(axes_col):
                ax.set_ylabel(row_labels[row], color="lightgray", fontsize=8)

        axes_col[0].set_title(name, color="white", fontsize=9, pad=4)

        axes_col[0].plot(ts, d["mem_mb"],     lw=1.5, color=c, label="mem")
        axes_col[0].fill_between(ts, d["mem_mb"], alpha=0.10, color=c)

        axes_col[1].plot(ts, d["swap_mb"],    lw=1.5, color=c, label="swap")
        axes_col[1].fill_between(ts, d["swap_mb"], alpha=0.10, color=c)

        axes_col[2].plot(ts, d["majfault_anon_rate"], lw=1.5, color=c,  label="anon majfault/s (swap-in)")
        axes_col[2].plot(ts, d["majfault_file_rate"], lw=1.5, color=c2, label="file majfault/s (disk)", linestyle="--")
        axes_col[2].fill_between(ts, d["majfault_anon_rate"], alpha=0.10, color=c)
        axes_col[2].fill_between(ts, d["majfault_file_rate"], alpha=0.10, color=c2)

        axes_col[3].plot(ts, d["ws_anon_rate"], lw=1.5, color=c,  label="anon refault/s")
        axes_col[3].plot(ts, d["ws_file_rate"], lw=1.5, color=c2, label="file refault/s", linestyle="--")

        axes_col[4].plot(ts, d["psi_some"], lw=1.5, color=c,  label="PSI some")
        axes_col[4].plot(ts, d["psi_full"], lw=1.0, color=c2, label="PSI full", linestyle="--")

        finalize_xaxis(axes_col[-1])
        for ax in axes_col:
            add_legend(ax)

    fig.autofmt_xdate(rotation=30, ha="right")
    plt.tight_layout(rect=[0, 0.01, 1, 0.985])


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("csvfiles", nargs="+", metavar="CSV",
                        help="CSV file(s) produced by cgroup_sampler.py")
    parser.add_argument("--merge", action="store_true",
                        help="Overlay all cgroups on the same axes")
    parser.add_argument("--out", metavar="FILE",
                        help="Save figure to FILE instead of displaying")
    args = parser.parse_args()

    files = [Path(f) for f in args.csvfiles]
    missing = [f for f in files if not f.exists()]
    if missing:
        for f in missing:
            print(f"File not found: {f}", file=sys.stderr)
        sys.exit(1)

    datasets = []
    for f in files:
        data = load_csv(f)
        if data["n"] == 0:
            print(f"No data in {f}, skipping.", file=sys.stderr)
            continue
        datasets.append((f.stem, data))

    if not datasets:
        print("No valid data to plot.", file=sys.stderr)
        sys.exit(1)

    if args.merge or len(datasets) == 1:
        plot_merged(datasets)
    else:
        plot_individual(datasets)

    if args.out:
        import os
        os.makedirs(os.path.dirname(args.out) if os.path.dirname(args.out) else ".", exist_ok=True)
        plt.savefig(args.out, dpi=150, bbox_inches=None)
        print(f"Saved: {args.out}")
        plt.close()
    else:
        plt.show()


if __name__ == "__main__":
    main()
