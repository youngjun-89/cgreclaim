#!/usr/bin/env python3
"""
cgroup_zoom_plot.py  — cgroup view zoomed to the syslog timing window.

Shows per-cgroup memory data only during the Netflix launch window
(Launch requested → SplashState OUT + 5 s buffer), with syslog event
markers overlaid as vertical dashed lines.

Usage:
    python3 tools/cgroup_zoom_plot.py \
        --with-cgrd    tools/data/SESSION/with_cgrd \
        --without-cgrd tools/data/SESSION/without_cgrd \
        --out          tools/report/SESSION/cgroup_zoom.png
"""

import argparse
import os
import re
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.gridspec import GridSpec

sys.path.insert(0, str(Path(__file__).parent / "syslog"))
import log_timeline as lt

BG_FIG   = "#1C1C2E"
BG_AX    = "#13131F"
SPINE    = "#444"
COLOR_W  = "#4C9BE8"
COLOR_WO = "#E8644C"
TOP_N    = 6
N_GRID   = 300
KST      = timezone(timedelta(hours=9))
PRE_BUF  = 5
POST_BUF = 5

EVENT_COLORS = {
    "Launch\nrequested":        "#e74c3c",
    "PID\nforked":              "#f39c12",
    "IPC\nready":               "#1abc9c",
    "Sys-splash\nON":           "#9b59b6",
    "Sys-splash\nOFF":          "#7f8c8d",
    "YouTube\nbackground":      "#3498db",
    "SplashState\nIN":          "#f1c40f",
    "SplashState\nHOLD":        "#e67e22",
    "SplashState\nOUT ✓":       "#27ae60",
    "SAM foreground\nconfirmed":"#2ecc71",
}

TS_ISO     = re.compile(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)Z')
LAUNCH_PAT = re.compile(r'NL_APP_LAUNCH_BEGIN.*netflix', re.IGNORECASE)


def style_ax(ax, ylabel=""):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray", labelsize=7)
    if ylabel:
        ax.set_ylabel(ylabel, color="lightgray", fontsize=8)
    ax.grid(True, alpha=0.15, linestyle="--")
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)


# ── syslog helpers ────────────────────────────────────────────────────────────

def find_syslog(run_dir: Path):
    matches = sorted(run_dir.glob("syslog/syslog_*.log"))
    return matches[0] if matches else None


def get_launch_kst(syslog_path: Path):
    with syslog_path.open() as f:
        for line in f:
            if LAUNCH_PAT.search(line):
                m = TS_ISO.match(line)
                if m:
                    utc = datetime.fromisoformat(m.group(1)).replace(tzinfo=timezone.utc)
                    return utc.astimezone(KST)
    return None


def get_events_relative(syslog_path: Path):
    events = lt.parse_syslog(str(syslog_path))
    kt = lt.key_times(events)
    end = kt.get("SplashState OUT ✓") or kt.get("SplashState\nOUT ✓") or 20.0
    return kt, end


# ── CSV loader (windowed) ─────────────────────────────────────────────────────

def load_cgroup_csv_window(csv_path: Path, launch_kst: datetime, window_end: float):
    t_rel, mem_mb, swap_mb = [], [], []
    ws_anon_rate, ws_file_rate = [], []

    t_start = launch_kst - timedelta(seconds=PRE_BUF)
    t_end   = launch_kst + timedelta(seconds=window_end + POST_BUF)

    with csv_path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            if len(p) < 11:
                continue
            try:
                ts = datetime.strptime(p[0], "%H:%M:%S").replace(
                    year=launch_kst.year, month=launch_kst.month,
                    day=launch_kst.day, tzinfo=KST)
                if ts < t_start or ts > t_end:
                    continue
                rel = (ts - launch_kst).total_seconds()
                t_rel.append(rel)
                mem_mb.append(float(p[1]))
                swap_mb.append(float(p[2]))
                # columns: pgmaj,pgmaj_rate,pgswapin,pgswapin_rate,ws_anon,ws_anon_rate,ws_file,ws_file_rate,...
                if len(p) >= 13:
                    ws_anon_rate.append(float(p[8]))
                    ws_file_rate.append(float(p[10]))
                else:
                    ws_anon_rate.append(float(p[6]))
                    ws_file_rate.append(float(p[8]))
            except (ValueError, IndexError):
                continue

    if not t_rel:
        return None
    return dict(
        t=np.array(t_rel),
        mem_mb=np.array(mem_mb),
        swap_mb=np.array(swap_mb),
        ws_anon_rate=np.array(ws_anon_rate),
        ws_file_rate=np.array(ws_file_rate),
    )


# ── group loading ─────────────────────────────────────────────────────────────

def load_group(group_dir: Path):
    """Returns { cgroup_name: [ {t, mem_mb, ...}, ... ] }, avg_kt"""
    run_dirs = sorted(group_dir.glob("run_*"))
    all_run_maps = []
    all_kt = []

    for run_dir in run_dirs:
        syslog_path = find_syslog(run_dir)
        if syslog_path is None:
            continue
        launch_kst = get_launch_kst(syslog_path)
        if launch_kst is None:
            continue
        kt, window_end = get_events_relative(syslog_path)
        all_kt.append(kt)

        csv_dir = run_dir / "cgroup_sampler"
        if not csv_dir.is_dir():
            continue

        cg_map = {}
        for csv in sorted(csv_dir.glob("*.csv")):
            d = load_cgroup_csv_window(csv, launch_kst, window_end)
            if d is not None:
                cg_map[csv.stem] = d
        if cg_map:
            all_run_maps.append(cg_map)

    if not all_run_maps:
        return {}, {}

    # Average event times
    avg_kt = {}
    for kt in all_kt:
        for k, v in kt.items():
            avg_kt.setdefault(k, []).append(v)
    avg_kt = {k: float(np.mean(v)) for k, v in avg_kt.items()}

    common = set(all_run_maps[0])
    for rm in all_run_maps[1:]:
        common &= set(rm)
    cg_runs = {cg: [rm[cg] for rm in all_run_maps] for cg in common}
    return cg_runs, avg_kt


# ── averaging ─────────────────────────────────────────────────────────────────

def compute_avg(runs: list) -> dict:
    n = len(runs)
    if n == 0:
        return None
    valid = runs
    excluded = []
    if n >= 3:
        summaries = np.array([r["mem_mb"].mean() for r in runs])
        mi, ma = int(np.argmin(summaries)), int(np.argmax(summaries))
        excluded = [f"run{mi+1}", f"run{ma+1}"]
        valid = [r for i, r in enumerate(runs) if i not in (mi, ma)] or runs

    t_lo = min(r["t"][0] for r in valid if len(r["t"]) > 0)
    t_hi = max(r["t"][-1] for r in valid if len(r["t"]) > 0)
    t_grid = np.linspace(t_lo, t_hi, N_GRID)

    result = {
        "mode": "avg" if n >= 3 else "individual",
        "t_grid": t_grid,
        "n_total": n, "n_valid": len(valid),
        "excluded": excluded, "runs": valid,
    }
    for metric in ("mem_mb", "swap_mb", "ws_anon_rate", "ws_file_rate"):
        arr = []
        for r in valid:
            if len(r["t"]) >= 2 and metric in r:
                arr.append(np.interp(t_grid, r["t"], r[metric],
                                     left=r[metric][0], right=r[metric][-1]))
        if arr:
            stack = np.vstack(arr)
            result[f"{metric}_mean"] = stack.mean(axis=0)
            result[f"{metric}_std"]  = stack.std(axis=0)
        else:
            result[f"{metric}_mean"] = np.zeros(N_GRID)
            result[f"{metric}_std"]  = np.zeros(N_GRID)
    return result


# ── select top-N cgroups ──────────────────────────────────────────────────────

def select_top_n(cg_w: dict, cg_wo: dict, n: int) -> list:
    """Select top-N cgroups by peak mem_mb during window across both groups."""
    scores = {}
    for cg, runs in cg_w.items():
        peak = max((r["mem_mb"].max() for r in runs if len(r["mem_mb"]) > 0), default=0)
        scores[cg] = scores.get(cg, 0) + peak
    for cg, runs in cg_wo.items():
        peak = max((r["mem_mb"].max() for r in runs if len(r["mem_mb"]) > 0), default=0)
        scores[cg] = scores.get(cg, 0) + peak
    return sorted(scores, key=lambda c: -scores[c])[:n]


# ── event markers ─────────────────────────────────────────────────────────────

def add_event_markers(ax, avg_kt: dict):
    ymin, ymax = ax.get_ylim()
    label_y = ymax - (ymax - ymin) * 0.03
    for lbl, t in sorted(avg_kt.items(), key=lambda x: x[1]):
        col = EVENT_COLORS.get(lbl, "#aaaaaa")
        ax.axvline(t, color=col, lw=0.9, linestyle="--", alpha=0.75)
        short = lbl.replace("\n", " ").split(" ")[0]
        ax.text(t, label_y, short, color=col, fontsize=6,
                rotation=90, ha="center", va="top")
        label_y -= (ymax - ymin) * 0.09
        if label_y < ymin + (ymax - ymin) * 0.3:
            label_y = ymax - (ymax - ymin) * 0.03


# ── plot one cgroup subplot ───────────────────────────────────────────────────

def plot_cg(ax, agg_w, agg_wo, cg_name, metric, ylabel, avg_kt):
    style_ax(ax, ylabel)
    ax.set_title(f"  {cg_name}", color="lightgray", fontsize=8, loc="left", pad=2)

    for agg, color, label in [(agg_w, COLOR_W, "with cgrd"),
                               (agg_wo, COLOR_WO, "without cgrd")]:
        if agg is None:
            continue
        if agg["mode"] == "avg":
            t = agg["t_grid"]
            mean = agg[f"{metric}_mean"]
            std  = agg[f"{metric}_std"]
            ax.plot(t, mean, lw=1.8, color=color,
                    label=f"{label} (n={agg['n_valid']})")
            ax.fill_between(t, mean - std, mean + std, alpha=0.2, color=color)
        else:
            pal = [color, "#7BB8F0" if color == COLOR_W else "#F0917B"]
            for i, r in enumerate(agg["runs"]):
                ax.plot(r["t"], r[metric], lw=1.5,
                        color=pal[i % len(pal)],
                        label=f"{label} r{i+1}", alpha=0.85)

    ax.axvline(0, color="#e74c3c", lw=1.2, linestyle="-", alpha=0.5)
    add_event_markers(ax, avg_kt)
    ax.set_xlim(left=-PRE_BUF)
    ax.legend(facecolor="#2A2A3E", labelcolor="white",
              framealpha=0.8, fontsize=7, loc="upper right")


# ── main draw ─────────────────────────────────────────────────────────────────

def draw(cg_w: dict, avg_kt_w: dict, cg_wo: dict, avg_kt_wo: dict,
         out_path: str):

    top = select_top_n(cg_w, cg_wo, TOP_N)
    if not top:
        print("WARNING: no common cgroups found", file=sys.stderr)
        return

    # Combined avg_kt
    avg_kt = {}
    for k in set(avg_kt_w) | set(avg_kt_wo):
        vals = [v for d in (avg_kt_w, avg_kt_wo) for v in ([d[k]] if k in d else [])]
        avg_kt[k] = float(np.mean(vals))

    # 2 subplots per cgroup: mem_mb + ws_refault_rate
    rows_per_cg = 2
    n_rows = len(top) * rows_per_cg

    fig = plt.figure(figsize=(14, n_rows * 2.2 + 1), facecolor=BG_FIG)
    fig.suptitle("Cgroup memory — Netflix launch window (with vs without cgrd)",
                 fontsize=13, fontweight="bold", color="white", y=0.998)

    gs = GridSpec(n_rows, 1, figure=fig, hspace=0.08)
    row = 0

    for cg in top:
        agg_w  = compute_avg(cg_w.get(cg, []))
        agg_wo = compute_avg(cg_wo.get(cg, []))

        ax_mem = fig.add_subplot(gs[row]);   row += 1
        ax_ref = fig.add_subplot(gs[row]);   row += 1

        plot_cg(ax_mem, agg_w, agg_wo, cg, "mem_mb",
                f"{cg[:28]}\nMem (MB)", avg_kt)
        plot_cg(ax_ref, agg_w, agg_wo, cg, "ws_anon_rate",
                "Refault anon/s", avg_kt)

        # share x-axis label suppression
        plt.setp(ax_mem.get_xticklabels(), visible=False)
        if row < n_rows:
            plt.setp(ax_ref.get_xticklabels(), visible=False)

    axes = fig.get_axes()
    if axes:
        axes[-1].set_xlabel("Seconds from Launch requested",
                            color="lightgray", fontsize=9)

    os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".",
                exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    print(f"Saved: {out_path}")
    plt.close(fig)


# ── entry point ───────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-cgrd",    required=True, metavar="DIR")
    ap.add_argument("--without-cgrd", required=True, metavar="DIR")
    ap.add_argument("--out",          required=True, metavar="FILE")
    args = ap.parse_args()

    cg_w,  avg_kt_w  = load_group(Path(args.with_cgrd))
    cg_wo, avg_kt_wo = load_group(Path(args.without_cgrd))

    if not cg_w and not cg_wo:
        print("ERROR: no usable cgroup data found", file=sys.stderr)
        sys.exit(1)

    draw(cg_w, avg_kt_w, cg_wo, avg_kt_wo, args.out)


if __name__ == "__main__":
    main()
