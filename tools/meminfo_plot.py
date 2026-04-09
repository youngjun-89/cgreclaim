#!/usr/bin/env python3
"""
meminfo_plot.py  — with/without cgrd multi-run comparison with averaging.

Usage:
    python meminfo_plot.py \
        --with-cgrd    tools/data/SESSION/with_cgrd \
        --without-cgrd tools/data/SESSION/without_cgrd \
        --out          report/SESSION/meminfo_comparison.png

Averaging logic:
    n >= 3 : remove run with min mean(MemFree) and run with max mean(MemFree),
             average the rest → mean line + ±1σ shaded band
    n == 2 : overlay both runs individually
    n == 1 : single line

CSV format (meminfo_sampler.sh output):
    timestamp, phase, mem_free_kb, mem_available_kb, swap_free_kb,
    swap_total_kb, active_anon_kb, inactive_anon_kb, active_file_kb,
    inactive_file_kb
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.gridspec import GridSpec

# ── styling ───────────────────────────────────────────────────────────────────

BG_FIG = "#1C1C2E"
BG_AX  = "#13131F"
SPINE  = "#444"
COLOR_W  = "#4C9BE8"   # with_cgrd  — blue
COLOR_WO = "#E8644C"   # without_cgrd — red


def style_ax(ax):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray")
    ax.yaxis.label.set_color("lightgray")
    ax.xaxis.label.set_color("lightgray")
    ax.grid(True, alpha=0.15, linestyle="--")
    ax.grid(True, which="minor", alpha=0.07, linestyle=":")
    ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=10, integer=True))
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:.0f}"))
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)


# ── CSV loader ────────────────────────────────────────────────────────────────

def load_csv(path: Path) -> dict:
    """Load one meminfo CSV. Returns dict of numpy arrays + phase list."""
    t_rel, phase = [], []
    mem_free, mem_avail = [], []
    swap_free, swap_total = [], []
    act_anon, inact_anon = [], []
    act_file, inact_file = [], []

    t0 = None
    from datetime import datetime

    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            if len(p) < 9:
                continue
            try:
                ts = datetime.strptime(p[0], "%H:%M:%S")
                if t0 is None:
                    t0 = ts
                t_rel.append((ts - t0).total_seconds())
                phase.append(int(p[1]))
                mem_free.append(float(p[2]) / 1024)
                mem_avail.append(float(p[3]) / 1024)
                swap_free.append(float(p[4]) / 1024)
                swap_total.append(float(p[5]) / 1024)
                act_anon.append(float(p[6]) / 1024)
                inact_anon.append(float(p[7]) / 1024)
                act_file.append(float(p[8]) / 1024)
                inact_file.append(float(p[9]) / 1024 if len(p) > 9 else 0)
            except (ValueError, IndexError):
                continue

    if not t_rel:
        return None

    swap_used = [t - f for t, f in zip(swap_total, swap_free)]
    return dict(
        t=np.array(t_rel),
        phase=np.array(phase),
        mem_free=np.array(mem_free),
        mem_avail=np.array(mem_avail),
        swap_used=np.array(swap_used),
        act_anon=np.array(act_anon),
        inact_anon=np.array(inact_anon),
        act_file=np.array(act_file),
        inact_file=np.array(inact_file),
    )


# ── run loading ───────────────────────────────────────────────────────────────

def load_group_runs(group_dir: Path) -> list:
    """Return list of run dicts loaded from group_dir/run_*/meminfo_sampler/*.csv."""
    runs = []
    for run_dir in sorted(group_dir.glob("run_*")):
        csvs = sorted(run_dir.glob("meminfo_sampler/*.csv"))
        if not csvs:
            continue
        data = load_csv(csvs[0])
        if data is not None:
            runs.append({"name": run_dir.name, **data})
    return runs


# ── averaging ─────────────────────────────────────────────────────────────────

METRICS = ["mem_free", "mem_avail", "swap_used", "act_anon", "inact_anon"]
N_GRID  = 400


def compute_group_avg(runs: list) -> dict:
    """
    n >= 3: drop min + max run by mean(MemFree), interpolate + average rest.
    n == 1 or 2: return individual runs (no averaging).
    Returns dict with mode, t_grid, per-metric mean/std (or raw runs).
    """
    n = len(runs)
    if n == 0:
        return None

    excluded = []
    valid = runs

    if n >= 3:
        summaries = np.array([r["mem_free"].mean() for r in runs])
        min_i = int(np.argmin(summaries))
        max_i = int(np.argmax(summaries))
        excluded = [runs[min_i]["name"], runs[max_i]["name"]]
        valid = [r for i, r in enumerate(runs) if i not in (min_i, max_i)]
        if not valid:
            valid = runs
            excluded = []

    t_max = max(r["t"][-1] for r in valid if len(r["t"]) > 0)
    t_grid = np.linspace(0, t_max, N_GRID)

    result = {
        "mode": "avg" if n >= 3 else "individual",
        "t_grid": t_grid,
        "n_total": n,
        "n_valid": len(valid),
        "excluded": excluded,
        "runs": valid,
    }

    for m in METRICS:
        interp_arr = []
        for r in valid:
            if len(r["t"]) < 2:
                continue
            interp_arr.append(np.interp(t_grid, r["t"], r[m],
                                        left=r[m][0], right=r[m][-1]))
        if not interp_arr:
            result[f"{m}_mean"] = np.zeros(N_GRID)
            result[f"{m}_std"]  = np.zeros(N_GRID)
        else:
            arr = np.vstack(interp_arr)
            result[f"{m}_mean"] = arr.mean(axis=0)
            result[f"{m}_std"]  = arr.std(axis=0)

    # Phase boundary: find where phase changes to 2 (use first valid run)
    phase_t = None
    for r in valid:
        idx = np.where(r["phase"] == 2)[0]
        if len(idx) > 0:
            phase_t = r["t"][idx[0]]
            break
    result["phase_t"] = phase_t

    return result


# ── plotting ──────────────────────────────────────────────────────────────────

def plot_group(ax, agg: dict, color: str, label: str, metric: str):
    """Plot mean line + std shade, or individual lines."""
    if agg is None:
        return

    if agg["mode"] == "avg":
        mean = agg[f"{metric}_mean"]
        std  = agg[f"{metric}_std"]
        t    = agg["t_grid"]
        ax.plot(t, mean, lw=2.0, color=color, label=f"{label} (avg n={agg['n_valid']})")
        ax.fill_between(t, mean - std, mean + std, alpha=0.20, color=color)
    else:
        # individual runs
        palette = [color, "#7BB8F0" if color == COLOR_W else "#F0917B"]
        for i, r in enumerate(agg["runs"]):
            c = palette[i % len(palette)]
            ax.plot(r["t"], r[metric], lw=1.5, color=c,
                    label=f"{label} {r['name']}", alpha=0.85)


def add_phase_line(ax, phase_t):
    if phase_t is not None:
        ax.axvline(phase_t, color="#f1c40f", lw=1.2, linestyle=":", alpha=0.7)
        ax.text(phase_t + 0.5, ax.get_ylim()[1] * 0.95, "▶ Netflix",
                color="#f1c40f", fontsize=7, va="top")


def draw(agg_w: dict, agg_wo: dict, out_path: str,
         label_w: str = "with cgrd", label_wo: str = "without cgrd"):

    rows = [
        ("mem_free",  "MemFree (MB)"),
        ("mem_avail", "MemAvail (MB)"),
        ("swap_used", "SwapUsed (MB)"),
        ("act_anon",  "Active Anon (MB)"),
        ("inact_anon","Inactive Anon (MB)"),
    ]

    fig = plt.figure(figsize=(15, 14), facecolor=BG_FIG)
    fig.suptitle("/proc/meminfo — with vs without cgrd",
                 fontsize=14, fontweight="bold", color="white", y=0.995)

    gs = GridSpec(len(rows), 1, figure=fig, hspace=0.08)
    axes = [fig.add_subplot(gs[i]) for i in range(len(rows))]

    for i, (ax, (metric, ylabel)) in enumerate(zip(axes, rows)):
        style_ax(ax)
        ax.set_ylabel(ylabel, color="lightgray", fontsize=9)
        plot_group(ax, agg_w,  COLOR_W,  label_w,  metric)
        plot_group(ax, agg_wo, COLOR_WO, label_wo, metric)

        # Phase boundary line (use whichever group has it)
        pt = (agg_w or {}).get("phase_t") or (agg_wo or {}).get("phase_t")
        if pt:
            ax.axvline(pt, color="#f1c40f", lw=1.2, linestyle=":", alpha=0.7)
            if i == 0:
                ymax = ax.get_ylim()[1]
                ax.text(pt + 0.3, ymax * 0.95, "▶ Netflix",
                        color="#f1c40f", fontsize=7.5, va="top")

        ax.legend(facecolor="#2A2A3E", labelcolor="white",
                  framealpha=0.8, fontsize=8, loc="upper right")
        if i < len(rows) - 1:
            plt.setp(ax.get_xticklabels(), visible=False)

    axes[-1].set_xlabel("Elapsed (s)", color="lightgray")

    # Footer: run counts + excluded
    footer_parts = []
    for label, agg in [(label_w, agg_w), (label_wo, agg_wo)]:
        if agg:
            s = f"{label}: {agg['n_valid']}/{agg['n_total']} runs"
            if agg["excluded"]:
                s += f" (excluded: {', '.join(agg['excluded'])})"
            footer_parts.append(s)
    if footer_parts:
        fig.text(0.01, 0.002, "  |  ".join(footer_parts),
                 fontsize=8, color="#888", va="bottom")

    import os
    os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".", exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches="tight", facecolor=fig.get_facecolor())
    print(f"Saved: {out_path}")
    plt.close(fig)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--with-cgrd",    required=True, type=Path,
                        help="Group dir: .../with_cgrd  (contains run_* subdirs)")
    parser.add_argument("--without-cgrd", required=True, type=Path,
                        help="Group dir: .../without_cgrd")
    parser.add_argument("--out", required=True, help="Output PNG path")
    args = parser.parse_args()

    runs_w  = load_group_runs(args.with_cgrd)
    runs_wo = load_group_runs(args.without_cgrd)

    if not runs_w and not runs_wo:
        print("ERROR: no meminfo CSV files found in either group dir.", file=sys.stderr)
        sys.exit(1)

    agg_w  = compute_group_avg(runs_w)  if runs_w  else None
    agg_wo = compute_group_avg(runs_wo) if runs_wo else None

    draw(agg_w, agg_wo, args.out)


if __name__ == "__main__":
    main()
