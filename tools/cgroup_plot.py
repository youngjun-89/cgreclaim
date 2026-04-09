#!/usr/bin/env python3
"""
cgroup_plot.py  — with/without cgrd cgroup comparison with run averaging.

Usage:
    python cgroup_plot.py \
        --with-cgrd    tools/data/SESSION/with_cgrd \
        --without-cgrd tools/data/SESSION/without_cgrd \
        --out          report/SESSION/cgroup_comparison.png

Averaging: n>=3 drop min+max by mean(mem_mb), average rest ±1σ; n<3 overlay.
Shows top-N cgroups by |avg mem_mb delta with - without|.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.gridspec import GridSpec

BG_FIG   = "#1C1C2E"
BG_AX    = "#13131F"
SPINE    = "#444"
COLOR_W  = "#4C9BE8"
COLOR_WO = "#E8644C"
TOP_N    = 8
N_GRID   = 400


def style_ax(ax, ylabel=""):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray", labelsize=7)
    if ylabel:
        ax.set_ylabel(ylabel, color="lightgray", fontsize=8)
    ax.grid(True, alpha=0.15, linestyle="--")
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)


# ── CSV loader ────────────────────────────────────────────────────────────────

def load_cgroup_csv(path: Path):
    from datetime import datetime
    t_rel, mem_mb, swap_mb, pgmaj_rate, pgswapin_rate = [], [], [], [], []
    ws_anon_rate, ws_file_rate, psi_some, psi_full = [], [], [], []
    t0 = None
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            if len(p) < 11:
                continue
            try:
                ts = datetime.strptime(p[0], "%H:%M:%S")
                if t0 is None:
                    t0 = ts
                t_rel.append((ts - t0).total_seconds())
                mem_mb.append(float(p[1]))
                swap_mb.append(float(p[2]))
                pgmaj_rate.append(float(p[4]))
                if len(p) >= 13:
                    pgswapin_rate.append(float(p[6]))
                    ws_anon_rate.append(float(p[8]))
                    ws_file_rate.append(float(p[10]))
                    psi_some.append(float(p[11]))
                    psi_full.append(float(p[12]))
                else:
                    pgswapin_rate.append(0.0)
                    ws_anon_rate.append(float(p[6]))
                    ws_file_rate.append(float(p[8]))
                    psi_some.append(float(p[9]))
                    psi_full.append(float(p[10]))
            except (ValueError, IndexError):
                continue
    if not t_rel:
        return None
    return dict(t=np.array(t_rel), mem_mb=np.array(mem_mb), swap_mb=np.array(swap_mb),
                pgmaj_rate=np.array(pgmaj_rate), psi_some=np.array(psi_some),
                psi_full=np.array(psi_full))


def load_group_cgroups(group_dir: Path) -> dict:
    """Returns {cgroup_name: [run_data, ...]}"""
    runs = sorted(group_dir.glob("run_*"))
    run_maps = []
    for run_dir in runs:
        csv_dir = run_dir / "cgroup_sampler"
        if not csv_dir.is_dir():
            continue
        cg_map = {}
        for csv in sorted(csv_dir.glob("*.csv")):
            d = load_cgroup_csv(csv)
            if d is not None:
                cg_map[csv.stem] = d
        if cg_map:
            run_maps.append(cg_map)
    if not run_maps:
        return {}
    common = set(run_maps[0])
    for rm in run_maps[1:]:
        common &= set(rm)
    return {cg: [rm[cg] for rm in run_maps] for cg in common}


def compute_avg(runs: list) -> dict:
    n = len(runs)
    if n == 0:
        return None
    valid = runs
    excluded = []
    if n >= 3:
        sums = np.array([r["mem_mb"].mean() for r in runs])
        mi, ma = int(np.argmin(sums)), int(np.argmax(sums))
        excluded = [f"run_{mi+1}", f"run_{ma+1}"]
        valid = [r for i, r in enumerate(runs) if i not in (mi, ma)] or runs
    t_max = max(r["t"][-1] for r in valid if len(r["t"]) > 0)
    t_grid = np.linspace(0, t_max, N_GRID)
    result = {"mode": "avg" if n >= 3 else "individual",
              "t_grid": t_grid, "n_total": n, "n_valid": len(valid),
              "excluded": excluded, "runs": valid}
    for m in ["mem_mb", "swap_mb", "pgmaj_rate", "psi_some", "psi_full"]:
        arrs = [np.interp(t_grid, r["t"], r[m], left=r[m][0], right=r[m][-1])
                for r in valid if len(r["t"]) >= 2]
        if arrs:
            a = np.vstack(arrs)
            result[f"{m}_mean"] = a.mean(axis=0)
            result[f"{m}_std"]  = a.std(axis=0)
        else:
            result[f"{m}_mean"] = np.zeros(N_GRID)
            result[f"{m}_std"]  = np.zeros(N_GRID)
    return result


def plot_line(ax, agg, color, label, metric):
    if agg is None:
        return
    t, mean, std = agg["t_grid"], agg[f"{metric}_mean"], agg[f"{metric}_std"]
    if agg["mode"] == "avg":
        ax.plot(t, mean, lw=1.8, color=color, label=label)
        ax.fill_between(t, mean - std, mean + std, alpha=0.18, color=color)
    else:
        pal = [color, "#7BB8F0" if color == COLOR_W else "#F0917B"]
        for i, r in enumerate(agg["runs"]):
            ax.plot(r["t"], r[metric], lw=1.4, color=pal[i % len(pal)],
                    label=f"{label} r{i+1}", alpha=0.85)


def draw(cg_w: dict, cg_wo: dict, out_path: str,
         label_w="with cgrd", label_wo="without cgrd"):
    all_cgs = sorted(set(cg_w) | set(cg_wo))
    avgs_w  = {cg: compute_avg(cg_w.get(cg, []))  for cg in all_cgs}
    avgs_wo = {cg: compute_avg(cg_wo.get(cg, [])) for cg in all_cgs}

    # Rank by abs delta of mean mem_mb
    def mean_scalar(agg, m="mem_mb"):
        if agg and f"{m}_mean" in agg:
            return float(agg[f"{m}_mean"].mean())
        return 0.0

    scored = sorted(all_cgs,
                    key=lambda cg: abs(mean_scalar(avgs_w[cg]) - mean_scalar(avgs_wo[cg])),
                    reverse=True)
    top_cgs = scored[:TOP_N]

    rows = [("mem_mb",    "Mem (MB)"),
            ("swap_mb",   "Swap (MB)"),
            ("pgmaj_rate","MajFault/s"),
            ("psi_some",  "PSI some avg10 (%)")]
    nrows = len(rows)

    fig = plt.figure(figsize=(14, 4 * nrows), facecolor=BG_FIG)
    fig.suptitle(f"cgroup Memory — {label_w} vs {label_wo}  (top {TOP_N} by |Δmem|)",
                 fontsize=13, fontweight="bold", color="white", y=0.995)
    gs = GridSpec(nrows, 2, figure=fig, hspace=0.10, wspace=0.20,
                  left=0.08, right=0.97)

    col_axes = [[fig.add_subplot(gs[r, c]) for r in range(nrows)] for c in range(2)]

    for col, (col_ax, color, label, cg_avgs) in enumerate([
        (col_axes[0], COLOR_W,  label_w,  avgs_w),
        (col_axes[1], COLOR_WO, label_wo, avgs_wo),
    ]):
        col_ax[0].set_title(label, color="white", fontsize=11)
        for row, (ax, (metric, ylabel)) in enumerate(zip(col_ax, rows)):
            style_ax(ax, ylabel if col == 0 else "")
            for i, cg in enumerate(top_cgs):
                short = cg.replace("system.slice_", "").replace(".service", "").replace(".scope", "")
                c = plt.cm.tab10(i / max(1, len(top_cgs) - 1))
                agg = cg_avgs.get(cg)
                if agg is None:
                    continue
                t, mean, std = agg["t_grid"], agg[f"{metric}_mean"], agg[f"{metric}_std"]
                if agg["mode"] == "avg":
                    ax.plot(t, mean, lw=1.5, color=c, label=short)
                    ax.fill_between(t, mean - std, mean + std, alpha=0.12, color=c)
                else:
                    for j, r in enumerate(agg["runs"]):
                        ax.plot(r["t"], r[metric], lw=1.2, color=c,
                                label=short if j == 0 else None, alpha=0.8)
            ax.legend(facecolor="#2A2A3E", labelcolor="white", framealpha=0.8,
                      fontsize=6.5, loc="upper right", ncol=1)
            if row < nrows - 1:
                plt.setp(ax.get_xticklabels(), visible=False)
            else:
                ax.set_xlabel("Elapsed (s)", color="lightgray", fontsize=8)

    # Footer
    parts = []
    for label, cg_avgs in [(label_w, avgs_w), (label_wo, avgs_wo)]:
        if cg_avgs:
            first = next(iter(cg_avgs.values()))
            if first:
                parts.append(f"{label}: {first['n_valid']}/{first['n_total']} runs")
    if parts:
        fig.text(0.01, 0.002, "  |  ".join(parts), fontsize=7.5, color="#888", va="bottom")

    import os
    os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".", exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches="tight", facecolor=fig.get_facecolor())
    print(f"Saved: {out_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--with-cgrd",    required=True, type=Path)
    parser.add_argument("--without-cgrd", required=True, type=Path)
    parser.add_argument("--out", required=True)
    # Legacy passthrough for old --merge csvfiles interface (ignored)
    parser.add_argument("csvfiles", nargs="*")
    parser.add_argument("--merge", action="store_true")
    args = parser.parse_args()

    cg_w  = load_group_cgroups(args.with_cgrd)
    cg_wo = load_group_cgroups(args.without_cgrd)

    if not cg_w and not cg_wo:
        print("ERROR: no cgroup CSV files found.", file=sys.stderr)
        sys.exit(1)

    draw(cg_w, cg_wo, args.out)


if __name__ == "__main__":
    main()
