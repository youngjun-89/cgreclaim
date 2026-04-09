#!/usr/bin/env python3
"""
meminfo_zoom_plot.py  — meminfo view zoomed to the syslog timing window.

Shows /proc/meminfo data only during the Netflix launch window
(Launch requested → SplashState OUT + 5 s buffer), with syslog event
markers overlaid as vertical dashed lines.

Usage:
    python3 tools/meminfo_zoom_plot.py \
        --with-cgrd    tools/data/SESSION/with_cgrd \
        --without-cgrd tools/data/SESSION/without_cgrd \
        --out          tools/report/SESSION/meminfo_zoom.png

Averaging (same as meminfo_plot.py):
    n >= 3 : remove run with min mean(MemFree) and run with max mean(MemFree),
             average the rest → mean line + ±1σ shaded band
    n == 2 : overlay both runs individually
    n == 1 : single line
"""

import argparse
import glob
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

# ── styling ───────────────────────────────────────────────────────────────────
BG_FIG  = "#1C1C2E"
BG_AX   = "#13131F"
SPINE   = "#444"
COLOR_W  = "#4C9BE8"
COLOR_WO = "#E8644C"

KST = timezone(timedelta(hours=9))
PRE_BUF  = 5   # seconds before launch to include
POST_BUF = 5   # seconds after SplashOut to include

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


def style_ax(ax):
    ax.set_facecolor(BG_AX)
    ax.tick_params(colors="lightgray")
    ax.yaxis.label.set_color("lightgray")
    ax.xaxis.label.set_color("lightgray")
    ax.grid(True, alpha=0.15, linestyle="--")
    for spine in ax.spines.values():
        spine.set_edgecolor(SPINE)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:.0f}"))


# ── syslog helpers ────────────────────────────────────────────────────────────

TS_ISO = re.compile(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)Z')
LAUNCH_PAT = re.compile(r'NL_APP_LAUNCH_BEGIN.*netflix', re.IGNORECASE)


def find_syslog(run_dir: Path):
    matches = sorted(run_dir.glob("syslog/syslog_*.log"))
    return matches[0] if matches else None


def get_launch_kst(syslog_path: Path):
    """Return datetime (KST, aware) of the Launch requested event."""
    with syslog_path.open() as f:
        for line in f:
            if LAUNCH_PAT.search(line):
                m = TS_ISO.match(line)
                if m:
                    utc = datetime.fromisoformat(m.group(1)).replace(tzinfo=timezone.utc)
                    return utc.astimezone(KST)
    return None


def get_events_relative(syslog_path: Path):
    """Return (key_times_dict, total_window_sec) relative to Launch."""
    events = lt.parse_syslog(str(syslog_path))
    kt = lt.key_times(events)
    end = kt.get("SplashState OUT ✓") or kt.get("SplashState\nOUT ✓") or 20.0
    return kt, end


# ── CSV loader (windowed) ─────────────────────────────────────────────────────

def load_csv_window(csv_path: Path, launch_kst: datetime, window_end_sec: float):
    """
    Load meminfo CSV rows that fall within [launch-PRE_BUF, launch+window_end+POST_BUF].
    Returns dict of arrays with t relative to launch, or None.
    """
    t_rel, mem_free, swap_used = [], [], []

    t_start = launch_kst - timedelta(seconds=PRE_BUF)
    t_end   = launch_kst + timedelta(seconds=window_end_sec + POST_BUF)

    with csv_path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("timestamp"):
                continue
            p = line.split(",")
            if len(p) < 6:
                continue
            try:
                ts = datetime.strptime(p[0], "%H:%M:%S").replace(
                    year=launch_kst.year,
                    month=launch_kst.month,
                    day=launch_kst.day,
                    tzinfo=KST,
                )
                if ts < t_start or ts > t_end:
                    continue
                rel = (ts - launch_kst).total_seconds()
                t_rel.append(rel)
                mem_free.append(float(p[2]) / 1024)
                swap_total = float(p[5]) / 1024
                swap_free  = float(p[4]) / 1024
                swap_used.append(swap_total - swap_free)
            except (ValueError, IndexError):
                continue

    if not t_rel:
        return None
    return dict(
        t=np.array(t_rel),
        mem_free=np.array(mem_free),
        swap_used=np.array(swap_used),
    )


# ── group loading ─────────────────────────────────────────────────────────────

def load_group(group_dir: Path):
    """
    Returns list of per-run dicts:
      { name, t, mem_free, swap_used, kt (event times), window_end }
    """
    runs = []
    for run_dir in sorted(group_dir.glob("run_*")):
        syslog_path = find_syslog(run_dir)
        if syslog_path is None:
            continue
        launch_kst = get_launch_kst(syslog_path)
        if launch_kst is None:
            continue
        kt, window_end = get_events_relative(syslog_path)

        csvs = sorted(run_dir.glob("meminfo_sampler/*.csv"))
        if not csvs:
            continue
        data = load_csv_window(csvs[0], launch_kst, window_end)
        if data is None:
            continue
        runs.append({"name": run_dir.name, "kt": kt,
                     "window_end": window_end, **data})
    return runs


# ── averaging ─────────────────────────────────────────────────────────────────

N_GRID = 300


def compute_avg(runs: list) -> dict:
    """Same min/max drop logic as meminfo_plot.py."""
    n = len(runs)
    if n == 0:
        return None

    valid = runs
    excluded = []
    if n >= 3:
        summaries = np.array([r["mem_free"].mean() for r in runs])
        mi, ma = int(np.argmin(summaries)), int(np.argmax(summaries))
        excluded = [runs[mi]["name"], runs[ma]["name"]]
        valid = [r for i, r in enumerate(runs) if i not in (mi, ma)] or runs

    t_lo = min(r["t"][0] for r in valid)
    t_hi = max(r["t"][-1] for r in valid)
    t_grid = np.linspace(t_lo, t_hi, N_GRID)

    result = {
        "mode": "avg" if n >= 3 else "individual",
        "t_grid": t_grid,
        "n_total": n, "n_valid": len(valid),
        "excluded": excluded, "runs": valid,
    }

    for metric in ("mem_free", "swap_used"):
        arr = []
        for r in valid:
            if len(r["t"]) >= 2:
                arr.append(np.interp(t_grid, r["t"], r[metric],
                                     left=r[metric][0], right=r[metric][-1]))
        if arr:
            stack = np.vstack(arr)
            result[f"{metric}_mean"] = stack.mean(axis=0)
            result[f"{metric}_std"]  = stack.std(axis=0)
        else:
            result[f"{metric}_mean"] = np.zeros(N_GRID)
            result[f"{metric}_std"]  = np.zeros(N_GRID)

    # Average event times across valid runs
    avg_kt = {}
    for key in (runs[0]["kt"] if runs else {}).keys():
        vals = [r["kt"][key] for r in valid if key in r["kt"]]
        if vals:
            avg_kt[key] = float(np.mean(vals))
    result["avg_kt"] = avg_kt

    return result


# ── plotting ──────────────────────────────────────────────────────────────────

def plot_metric(ax, agg: dict, color: str, label: str, metric: str):
    if agg is None:
        return
    if agg["mode"] == "avg":
        mean = agg[f"{metric}_mean"]
        std  = agg[f"{metric}_std"]
        t    = agg["t_grid"]
        ax.plot(t, mean, lw=2, color=color,
                label=f"{label} (avg n={agg['n_valid']})")
        ax.fill_between(t, mean - std, mean + std, alpha=0.2, color=color)
    else:
        palette = [color, "#7BB8F0" if color == COLOR_W else "#F0917B"]
        for i, r in enumerate(agg["runs"]):
            ax.plot(r["t"], r[metric], lw=1.5,
                    color=palette[i % len(palette)],
                    label=f"{label} {r['name']}", alpha=0.85)


def add_event_markers(ax, avg_kt: dict, ymin, ymax):
    """Draw vertical dashed lines at average event times."""
    label_y = ymax - (ymax - ymin) * 0.04
    for lbl, t in sorted(avg_kt.items(), key=lambda x: x[1]):
        col = EVENT_COLORS.get(lbl, "#aaaaaa")
        ax.axvline(t, color=col, lw=0.9, linestyle="--", alpha=0.75)
        short = lbl.replace("\n", " ").split(" ")[0]
        ax.text(t, label_y, short, color=col, fontsize=6.5,
                rotation=90, ha="center", va="top")
        label_y -= (ymax - ymin) * 0.07
        if label_y < ymin + (ymax - ymin) * 0.3:
            label_y = ymax - (ymax - ymin) * 0.04


def draw(agg_w, agg_wo, out_path: str,
         label_w="with cgrd", label_wo="without cgrd"):

    metrics = [
        ("mem_free",  "MemFree (MB)"),
        ("swap_used", "SwapUsed (MB)"),
    ]

    fig = plt.figure(figsize=(14, 8), facecolor=BG_FIG)
    fig.suptitle("/proc/meminfo — Netflix launch window (with vs without cgrd)",
                 fontsize=13, fontweight="bold", color="white", y=0.995)

    gs = GridSpec(len(metrics), 1, figure=fig, hspace=0.10)
    axes = [fig.add_subplot(gs[i]) for i in range(len(metrics))]

    avg_kt_combined = {}
    for agg in (agg_w, agg_wo):
        if agg and "avg_kt" in agg:
            for k, v in agg["avg_kt"].items():
                if k not in avg_kt_combined:
                    avg_kt_combined[k] = v
                else:
                    avg_kt_combined[k] = (avg_kt_combined[k] + v) / 2

    for i, (ax, (metric, ylabel)) in enumerate(zip(axes, metrics)):
        style_ax(ax)
        ax.set_ylabel(ylabel, color="lightgray", fontsize=9)
        plot_metric(ax, agg_w,  COLOR_W,  label_w,  metric)
        plot_metric(ax, agg_wo, COLOR_WO, label_wo, metric)

        ax.axvline(0, color="#e74c3c", lw=1.5, linestyle="-", alpha=0.6)

        ymin, ymax = ax.get_ylim()
        if ymin == ymax:
            ymin -= 1; ymax += 1
        add_event_markers(ax, avg_kt_combined, ymin, ymax)
        ax.set_xlim(left=-PRE_BUF)

        ax.legend(facecolor="#2A2A3E", labelcolor="white",
                  framealpha=0.8, fontsize=8, loc="upper right")
        if i < len(metrics) - 1:
            plt.setp(ax.get_xticklabels(), visible=False)

    axes[-1].set_xlabel("Seconds from Launch requested", color="lightgray")

    footer_parts = []
    for lbl, agg in [(label_w, agg_w), (label_wo, agg_wo)]:
        if agg:
            s = f"{lbl}: {agg['n_valid']}/{agg['n_total']} runs"
            if agg["excluded"]:
                s += f" (excl: {', '.join(agg['excluded'])})"
            footer_parts.append(s)
    if footer_parts:
        fig.text(0.01, 0.002, "  |  ".join(footer_parts),
                 fontsize=8, color="#888", va="bottom")

    os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".",
                exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    print(f"Saved: {out_path}")
    plt.close(fig)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-cgrd",    required=True, metavar="DIR")
    ap.add_argument("--without-cgrd", required=True, metavar="DIR")
    ap.add_argument("--out",          required=True, metavar="FILE")
    args = ap.parse_args()

    runs_w  = load_group(Path(args.with_cgrd))
    runs_wo = load_group(Path(args.without_cgrd))

    if not runs_w and not runs_wo:
        print("ERROR: no usable run data found", file=sys.stderr)
        sys.exit(1)

    agg_w  = compute_avg(runs_w)
    agg_wo = compute_avg(runs_wo)

    draw(agg_w, agg_wo, args.out)


if __name__ == "__main__":
    main()
