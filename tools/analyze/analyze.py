#!/usr/bin/env python3
# TOOLS_VERSION=2
"""
analyze.py — cgreclaim benchmark analyzer
------------------------------------------
Compares with_cgrd vs without_cgrd memory profiles collected by run_bench.sh.

Outputs:
  report/<session>/report.md         — Markdown summary (AI-ready)
  report/<session>/meminfo_phase1.png
  report/<session>/meminfo_phase2.png
  report/<session>/cgroup_top10.png
  report/<session>/cgroup_psi.png

Usage:
  python3 analyze.py                        # latest session
  python3 analyze.py --session 20260408_194000
  python3 analyze.py --data-dir /path/to/data --report-dir /path/to/report
"""

import argparse
import os
import sys
import glob
import warnings
from pathlib import Path
from datetime import datetime

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

warnings.filterwarnings("ignore")

HERE = Path(__file__).parent.resolve()
TOOLS_DIR = HERE.parent
DEFAULT_DATA_DIR = TOOLS_DIR / "data"
DEFAULT_REPORT_DIR = TOOLS_DIR / "report"

GROUPS = ["with_cgrd", "without_cgrd"]
COLORS = {"with_cgrd": "#2196F3", "without_cgrd": "#FF5722"}
PHASE_LABELS = {1: "Phase 1 (YouTube)", 2: "Phase 2 (Netflix)"}


# ── data loading ──────────────────────────────────────────────────────────────

def load_meminfo(session_dir: Path, group: str) -> pd.DataFrame:
    """Load and concatenate all meminfo CSVs for a group, adding run number."""
    frames = []
    run_dirs = sorted((session_dir / group).glob("run_*/"))
    for run_dir in run_dirs:
        run_num = int(run_dir.name.split("_")[1])
        csvs = list((run_dir / "meminfo_sampler").glob("*.csv"))
        if not csvs:
            continue
        df = pd.read_csv(csvs[0])
        df["run"] = run_num
        frames.append(df)
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)


def load_cgroup(session_dir: Path, group: str, cgroup_name: str) -> pd.DataFrame:
    """Load cgroup CSVs for a specific cgroup across all runs."""
    frames = []
    run_dirs = sorted((session_dir / group).glob("run_*/"))
    for run_dir in run_dirs:
        run_num = int(run_dir.name.split("_")[1])
        csv = run_dir / "cgroup_sampler" / cgroup_name
        if not csv.exists():
            continue
        df = pd.read_csv(csv)
        df["run"] = run_num
        frames.append(df)
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)


def get_common_cgroups(session_dir: Path) -> list[str]:
    """Find cgroup CSV files present in both groups (any run)."""
    sets = []
    for group in GROUPS:
        names = set()
        for csv in (session_dir / group).glob("run_*/cgroup_sampler/*.csv"):
            names.add(csv.name)
        if names:
            sets.append(names)
    if len(sets) < 2:
        return []
    return sorted(sets[0] & sets[1])


# ── statistics ────────────────────────────────────────────────────────────────

def meminfo_stats(df: pd.DataFrame, phase: int) -> dict:
    """Compute per-run mean, then overall mean/std/min for a phase."""
    if df.empty:
        return {}
    sub = df[df["phase"] == phase]
    if sub.empty:
        return {}
    cols = ["mem_free_kb", "mem_available_kb", "swap_free_kb",
            "swap_total_kb", "active_anon_kb", "inactive_anon_kb"]
    per_run = sub.groupby("run")[cols].mean()
    result = {}
    for col in cols:
        result[col] = {
            "mean": per_run[col].mean() / 1024,
            "std":  per_run[col].std()  / 1024,
            "min":  per_run[col].min()  / 1024,
        }
    # derived
    swap_used = (sub["swap_total_kb"] - sub["swap_free_kb"])
    per_run_swap = sub.groupby("run").apply(
        lambda x: (x["swap_total_kb"] - x["swap_free_kb"]).mean())
    result["swap_used_kb"] = {
        "mean": per_run_swap.mean() / 1024,
        "std":  per_run_swap.std()  / 1024,
        "min":  per_run_swap.min()  / 1024,
    }
    return result


def cgroup_stats(session_dir: Path, group: str, cgroup_name: str) -> dict:
    df = load_cgroup(session_dir, group, cgroup_name)
    if df.empty:
        return {}
    per_run = df.groupby("run")[
        ["mem_mb", "swap_mb", "pgmajfault_rate",
         "ws_refault_anon_rate", "ws_refault_file_rate",
         "psi_some_avg10", "psi_full_avg10"]
    ].mean()
    return {col: {"mean": per_run[col].mean(), "std": per_run[col].std()}
            for col in per_run.columns}


# ── plots ─────────────────────────────────────────────────────────────────────

def plot_meminfo_phase(session_dir: Path, phase: int, report_dir: Path) -> Path:
    """Time-series overlay of MemFree and MemAvail for both groups, phase N."""
    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=False)
    fig.suptitle(f"Memory — {PHASE_LABELS[phase]}\nwith_cgrd vs without_cgrd",
                 fontsize=13, fontweight="bold")

    metrics = [
        ("mem_free_kb",      "MemFree (MB)"),
        ("mem_available_kb", "MemAvailable (MB)"),
    ]
    swap_ax = axes[2]

    for group in GROUPS:
        df = load_meminfo(session_dir, group)
        if df.empty:
            continue
        sub = df[df["phase"] == phase].copy()
        # average across runs per sample index
        sub["idx"] = sub.groupby("run").cumcount()
        agg = sub.groupby("idx")[
            ["mem_free_kb", "mem_available_kb", "swap_total_kb", "swap_free_kb",
             "active_anon_kb", "inactive_anon_kb"]
        ].mean()

        c = COLORS[group]
        axes[0].plot(agg["mem_free_kb"] / 1024,
                     label=group, color=c, linewidth=1.5)
        axes[1].plot(agg["mem_available_kb"] / 1024,
                     label=group, color=c, linewidth=1.5)
        swap_used = (agg["swap_total_kb"] - agg["swap_free_kb"]) / 1024
        swap_ax.plot(swap_used, label=group, color=c, linewidth=1.5)

    for ax, ylabel in zip(axes[:2], ["MemFree (MB)", "MemAvailable (MB)"]):
        ax.set_ylabel(ylabel)
        ax.legend()
        ax.grid(True, alpha=0.3)

    swap_ax.set_ylabel("SwapUsed (MB)")
    swap_ax.set_xlabel("Sample index")
    swap_ax.legend()
    swap_ax.grid(True, alpha=0.3)

    plt.tight_layout()
    out = report_dir / f"meminfo_phase{phase}.png"
    fig.savefig(out, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return out


def plot_cgroup_top10(session_dir: Path, common_cgroups: list[str],
                      report_dir: Path) -> Path:
    """Bar chart: mean mem_mb for top 10 cgroups, both groups side by side."""
    scores = {}
    for cg in common_cgroups:
        total = 0
        for group in GROUPS:
            s = cgroup_stats(session_dir, group, cg)
            total += s.get("mem_mb", {}).get("mean", 0)
        scores[cg] = total

    top = sorted(scores, key=scores.get, reverse=True)[:10]
    labels = [c.replace("system.slice_", "").replace(".service.csv", "")
              .replace(".csv", "") for c in top]

    x = np.arange(len(top))
    width = 0.35
    fig, ax = plt.subplots(figsize=(14, 6))

    for i, group in enumerate(GROUPS):
        vals = []
        for cg in top:
            s = cgroup_stats(session_dir, group, cg)
            vals.append(s.get("mem_mb", {}).get("mean", 0))
        ax.bar(x + i * width, vals, width, label=group,
               color=COLORS[group], alpha=0.85)

    ax.set_xlabel("cgroup")
    ax.set_ylabel("Mean mem_mb")
    ax.set_title("Top 10 cgroups — Memory Usage (with_cgrd vs without_cgrd)")
    ax.set_xticks(x + width / 2)
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)
    plt.tight_layout()
    out = report_dir / "cgroup_top10.png"
    fig.savefig(out, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return out


def plot_cgroup_psi(session_dir: Path, common_cgroups: list[str],
                    report_dir: Path) -> Path:
    """Bar chart: PSI some_avg10 for top 10 cgroups."""
    scores = {}
    for cg in common_cgroups:
        total = 0
        for group in GROUPS:
            s = cgroup_stats(session_dir, group, cg)
            total += s.get("psi_some_avg10", {}).get("mean", 0)
        scores[cg] = total

    top = sorted(scores, key=scores.get, reverse=True)[:10]
    labels = [c.replace("system.slice_", "").replace(".service.csv", "")
              .replace(".csv", "") for c in top]

    x = np.arange(len(top))
    width = 0.35
    fig, ax = plt.subplots(figsize=(14, 6))

    for i, group in enumerate(GROUPS):
        vals = []
        for cg in top:
            s = cgroup_stats(session_dir, group, cg)
            vals.append(s.get("psi_some_avg10", {}).get("mean", 0))
        ax.bar(x + i * width, vals, width, label=group,
               color=COLORS[group], alpha=0.85)

    ax.set_xlabel("cgroup")
    ax.set_ylabel("PSI some_avg10")
    ax.set_title("Top 10 cgroups — Memory Pressure PSI (with_cgrd vs without_cgrd)")
    ax.set_xticks(x + width / 2)
    ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=8)
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)
    plt.tight_layout()
    out = report_dir / "cgroup_psi.png"
    fig.savefig(out, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return out


# ── report generation ─────────────────────────────────────────────────────────

def fmt(val, unit="MB", decimals=1):
    if val is None or (isinstance(val, float) and np.isnan(val)):
        return "N/A"
    return f"{val:.{decimals}f} {unit}"


def delta_str(a, b):
    """Return delta string with sign, e.g. '+12.3 MB'"""
    if a is None or b is None:
        return "N/A"
    d = a - b
    sign = "+" if d >= 0 else ""
    return f"{sign}{d:.1f} MB"


def generate_report(session_dir: Path, report_dir: Path,
                    png_paths: dict) -> Path:
    lines = []
    session = session_dir.name
    now = datetime.now().strftime("%Y-%m-%d %H:%M")

    # count runs
    run_counts = {}
    for group in GROUPS:
        run_counts[group] = len(list((session_dir / group).glob("run_*/")))

    lines += [
        f"# cgreclaim Benchmark Report",
        f"",
        f"**Session:** `{session}`  ",
        f"**Generated:** {now}  ",
        f"**Runs:** with_cgrd={run_counts.get('with_cgrd',0)}, "
        f"without_cgrd={run_counts.get('without_cgrd',0)}",
        f"",
        f"---",
        f"",
    ]

    # ── Executive Summary ──
    lines += ["## Executive Summary", ""]

    summary_rows = []
    for phase in [1, 2]:
        s_with    = meminfo_stats(load_meminfo(session_dir, "with_cgrd"),    phase)
        s_without = meminfo_stats(load_meminfo(session_dir, "without_cgrd"), phase)
        if not s_with or not s_without:
            continue
        mf_w  = s_with.get("mem_free_kb",      {}).get("mean")
        mf_wo = s_without.get("mem_free_kb",   {}).get("mean")
        ma_w  = s_with.get("mem_available_kb", {}).get("mean")
        ma_wo = s_without.get("mem_available_kb",{}).get("mean")
        sw_w  = s_with.get("swap_used_kb",     {}).get("mean")
        sw_wo = s_without.get("swap_used_kb",  {}).get("mean")
        label = PHASE_LABELS[phase]
        summary_rows.append(
            f"| {label} | MemFree | {fmt(mf_w)} | {fmt(mf_wo)} "
            f"| **{delta_str(mf_w, mf_wo)}** |"
        )
        summary_rows.append(
            f"| {label} | MemAvail | {fmt(ma_w)} | {fmt(ma_wo)} "
            f"| **{delta_str(ma_w, ma_wo)}** |"
        )
        summary_rows.append(
            f"| {label} | SwapUsed | {fmt(sw_w)} | {fmt(sw_wo)} "
            f"| **{delta_str(sw_w, sw_wo)}** |"
        )

    lines += [
        "| Phase | Metric | with_cgrd | without_cgrd | Δ (with−without) |",
        "|---|---|---|---|---|",
    ] + summary_rows + [""]

    lines += [
        "> **Positive Δ** means with_cgrd has *more* free memory / less swap — better.",
        "> **Negative Δ** means with_cgrd used more memory — worse.",
        "",
        "---",
        "",
    ]

    # ── Per-phase detail ──
    for phase in [1, 2]:
        label = PHASE_LABELS[phase]
        lines += [f"## {label}", ""]

        s_with    = meminfo_stats(load_meminfo(session_dir, "with_cgrd"),    phase)
        s_without = meminfo_stats(load_meminfo(session_dir, "without_cgrd"), phase)

        if s_with and s_without:
            lines += [
                "| Metric | with_cgrd mean | without_cgrd mean | Δ | with std | without std |",
                "|---|---|---|---|---|---|",
            ]
            metric_map = [
                ("mem_free_kb",      "MemFree"),
                ("mem_available_kb", "MemAvailable"),
                ("swap_used_kb",     "SwapUsed"),
                ("active_anon_kb",   "Active Anon"),
                ("inactive_anon_kb", "Inactive Anon"),
            ]
            for key, name in metric_map:
                mw  = s_with.get(key,    {}).get("mean")
                mwo = s_without.get(key, {}).get("mean")
                sw  = s_with.get(key,    {}).get("std")
                swo = s_without.get(key, {}).get("std")
                lines.append(
                    f"| {name} | {fmt(mw)} | {fmt(mwo)} | "
                    f"{delta_str(mw, mwo)} | "
                    f"±{fmt(sw, '')} | ±{fmt(swo, '')} |"
                )
            lines.append("")

        png_key = f"meminfo_phase{phase}"
        if png_key in png_paths:
            lines.append(f"![{label}]({png_paths[png_key].name})")
            lines.append("")

        lines += ["---", ""]

    # ── Cgroup comparison ──
    common = get_common_cgroups(session_dir)
    lines += ["## Cgroup Memory Comparison (Top 10 by mem_mb)", ""]

    if common:
        # rank by avg mem across groups
        scores = {}
        for cg in common:
            total = 0
            for group in GROUPS:
                s = cgroup_stats(session_dir, group, cg)
                total += s.get("mem_mb", {}).get("mean", 0)
            scores[cg] = total
        top10 = sorted(scores, key=scores.get, reverse=True)[:10]

        lines += [
            "| cgroup | with_cgrd mem_mb | without_cgrd mem_mb | Δ mem | "
            "with pgmajfault_rate | without pgmajfault_rate | "
            "with PSI some | without PSI some |",
            "|---|---|---|---|---|---|---|---|",
        ]
        for cg in top10:
            sw  = cgroup_stats(session_dir, "with_cgrd",    cg)
            swo = cgroup_stats(session_dir, "without_cgrd", cg)
            name = cg.replace("system.slice_","").replace(".service.csv","").replace(".csv","")
            m_w  = sw.get("mem_mb",           {}).get("mean")
            m_wo = swo.get("mem_mb",          {}).get("mean")
            p_w  = sw.get("pgmajfault_rate",  {}).get("mean", 0)
            p_wo = swo.get("pgmajfault_rate", {}).get("mean", 0)
            psi_w  = sw.get("psi_some_avg10",  {}).get("mean", 0)
            psi_wo = swo.get("psi_some_avg10", {}).get("mean", 0)
            d = (m_w - m_wo) if m_w and m_wo else None
            d_str = f"{d:+.1f}" if d is not None else "N/A"
            lines.append(
                f"| {name} | {fmt(m_w)} | {fmt(m_wo)} | {d_str} MB | "
                f"{p_w:.3f} | {p_wo:.3f} | "
                f"{psi_w:.2f} | {psi_wo:.2f} |"
            )
        lines.append("")

        if "cgroup_top10" in png_paths:
            lines.append(f"![cgroup top10]({png_paths['cgroup_top10'].name})")
            lines.append("")

        lines += ["### PSI (Memory Pressure)", ""]
        if "cgroup_psi" in png_paths:
            lines.append(f"![cgroup PSI]({png_paths['cgroup_psi'].name})")
            lines.append("")
    else:
        lines += ["_No common cgroups found between groups._", ""]

    lines += ["---", ""]

    # ── Raw data paths ──
    lines += [
        "## Data Paths (for further analysis)",
        "",
        f"```",
        f"Session dir : {session_dir}",
        f"Report dir  : {report_dir}",
        f"```",
        "",
        "### Files",
        "",
    ]
    for group in GROUPS:
        lines.append(f"**{group}:**")
        for run_dir in sorted((session_dir / group).glob("run_*/")):
            lines.append(f"- `{run_dir}`")
    lines += [
        "",
        "---",
        "",
        "_Report generated by `tools/analyze/analyze.py`._",
        "_Next step: open Copilot CLI and ask: "
        "\"report.md 읽고 cgrd 메모리 효과 분석해줘\"_",
    ]

    out = report_dir / "report.md"
    out.write_text("\n".join(lines), encoding="utf-8")
    return out


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session",    metavar="YYYYMMDD_HHMMSS",
                        help="Session ID (default: latest)")
    parser.add_argument("--data-dir",  default=str(DEFAULT_DATA_DIR),
                        metavar="DIR", help=f"Data root (default: {DEFAULT_DATA_DIR})")
    parser.add_argument("--report-dir", default=str(DEFAULT_REPORT_DIR),
                        metavar="DIR", help=f"Report root (default: {DEFAULT_REPORT_DIR})")
    args = parser.parse_args()

    data_root   = Path(args.data_dir)
    report_root = Path(args.report_dir)

    # find session
    if args.session:
        session_dir = data_root / args.session
    else:
        sessions = sorted(data_root.glob("*/"))
        if not sessions:
            print(f"No sessions found in {data_root}", file=sys.stderr)
            sys.exit(1)
        session_dir = sessions[-1]

    if not session_dir.exists():
        print(f"Session dir not found: {session_dir}", file=sys.stderr)
        sys.exit(1)

    report_dir = report_root / session_dir.name
    report_dir.mkdir(parents=True, exist_ok=True)

    print(f"Analyzing session: {session_dir.name}")
    print(f"Output: {report_dir}")

    png_paths = {}

    # check which groups have data
    available = [g for g in GROUPS if (session_dir / g).exists()]
    print(f"Groups found: {available}")

    if len(available) == 2:
        print("Generating meminfo phase plots...")
        png_paths["meminfo_phase1"] = plot_meminfo_phase(session_dir, 1, report_dir)
        png_paths["meminfo_phase2"] = plot_meminfo_phase(session_dir, 2, report_dir)

        common = get_common_cgroups(session_dir)
        if common:
            print(f"Generating cgroup plots ({len(common)} common cgroups)...")
            png_paths["cgroup_top10"] = plot_cgroup_top10(session_dir, common, report_dir)
            png_paths["cgroup_psi"]   = plot_cgroup_psi(session_dir, common, report_dir)
    else:
        print(f"Only {available} available — skipping comparison plots")

    print("Generating report.md...")
    report_path = generate_report(session_dir, report_dir, png_paths)
    print(f"Done: {report_path}")


if __name__ == "__main__":
    main()
