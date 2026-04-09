#!/usr/bin/env python3
"""
log_timeline.py  — YouTube→Netflix transition syslog visualizer

Usage:
    # Compare two groups from a benchmark session (most common)
    python3 tools/syslog/log_timeline.py --session 20260408_213044

    # Multi-run statistics across all runs in a session
    python3 tools/syslog/log_timeline.py --session 20260408_213044 --stats

    # Single syslog file
    python3 tools/syslog/log_timeline.py --file path/to/syslog.log

    # Two explicit files side-by-side
    python3 tools/syslog/log_timeline.py \
        --with-cgrd  path/to/with_cgrd/run_1/syslog/syslog_*.log \
        --without-cgrd path/to/without_cgrd/run_1/syslog/syslog_*.log

Output:
    tools/report/<session>/timeline_comparison.png   (compare mode)
    tools/report/<session>/stats_comparison.png      (stats mode)
    tools/report/<session>/stats_report.md           (stats mode)
    tools/report/timeline_<basename>.png             (single mode)
"""

import argparse
import glob
import os
import re
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# ── event rules ───────────────────────────────────────────────────────────────
# (regex, label, color, tier)
#   tier 0: SAM / netflix.bin
#   tier 1: YouTube / system events
#   tier 2: Surface Manager
RULES = [
    (r'NL_APP_LAUNCH_BEGIN.*netflix',
        'Launch\nrequested',        '#e74c3c', 0),
    (r'APP_LAUNCHED.*netflix',
        'PID\nforked',              '#f39c12', 0),
    (r'APP_LAUNCH.*netflix.*connected',
        'IPC\nready',               '#1abc9c', 0),
    (r'FOREGROUND_INFO.*current.*netflix',
        'SAM foreground\nconfirmed','#2ecc71', 0),
    (r'SplashState: IN',
        'SplashState\nIN',          '#f1c40f', 0),
    (r'SplashState: HOLD',
        'SplashState\nHOLD',        '#e67e22', 0),
    (r'SplashState: OUT',
        'SplashState\nOUT ✓',       '#27ae60', 0),
    # tier 1 — YouTube
    (r'SUBSCRIPTION_REPLY.*background.*youtube',
        'YouTube\nbackground',      '#3498db', 1),
    # tier 2 — surface splash on/off
    (r'surface-manager.*NL_VSC.*app_id.*com.webos.app.splash.*visible.*true',
        'Sys-splash\nON',           '#9b59b6', 2),
    (r'surface-manager.*NL_VSC.*app_id.*com.webos.app.splash.*visible.*false',
        'Sys-splash\nOFF',          '#7f8c8d', 2),
]

TS_RE  = re.compile(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z')
UPT_RE = re.compile(r'\[(\d+\.\d+)\]')

# Key metric definitions reused in stats and delta table
# (start_label, end_label, display_name)
METRIC_PAIRS = [
    ('Launch\nrequested',        'PID\nforked',              'req → PID fork'),
    ('PID\nforked',              'IPC\nready',               'PID → IPC ready'),
    ('Launch\nrequested',        'SAM foreground\nconfirmed','req → SAM foreground'),
    ('Launch\nrequested',        'SplashState\nIN',          'req → Splash IN'),
    ('Launch\nrequested',        'SplashState\nOUT ✓',       'req → content ready (Splash OUT)'),
]


def parse_syslog(path: str) -> list:
    """Return list of (relative_time_sec, label, color, tier)."""
    events = []
    with open(path) as f:
        for line in f:
            if not TS_RE.match(line):
                continue
            m = UPT_RE.search(line)
            if not m:
                continue
            upt = float(m.group(1))
            for pattern, label, color, tier in RULES:
                if re.search(pattern, line, re.IGNORECASE):
                    events.append((upt, label, color, tier))
                    break

    if not events:
        return []
    events.sort(key=lambda x: x[0])
    t0 = events[0][0]
    relative = [(t - t0, lbl, col, tier) for t, lbl, col, tier in events]

    # Deduplicate: keep only first occurrence of (label, tier)
    seen: set = set()
    deduped = []
    for item in relative:
        key = (item[1], item[3])
        if key not in seen:
            seen.add(key)
            deduped.append(item)

    # Nudge simultaneous events slightly apart
    NUDGE = 0.07
    t_list = [e[0] for e in deduped]
    nudged = []
    for i, (t, lbl, col, tier) in enumerate(deduped):
        offset = sum(1 for j, tj in enumerate(t_list) if j < i and abs(tj - t) < 0.02)
        nudged.append((t + offset * NUDGE, lbl, col, tier))

    return nudged


def find_syslog(directory: str):
    """Find first syslog_*.log under directory."""
    matches = sorted(glob.glob(os.path.join(directory, "**", "syslog_*.log"), recursive=True))
    return matches[0] if matches else None


def key_times(events: list) -> dict:
    """Map label → relative time (first occurrence)."""
    lookup = {}
    for t, lbl, _, _ in events:
        key = lbl.replace('\n', ' ').strip()
        if key not in lookup:
            lookup[key] = t
    return lookup


def metric_delta(kt: dict, start_lbl: str, end_lbl: str):
    s = kt.get(start_lbl.replace('\n', ' '))
    e = kt.get(end_lbl.replace('\n', ' '))
    return (e - s) if s is not None and e is not None else None


TIER_Y     = {0: 0.6, 1: 2.2, 2: 3.8}
TIER_LABEL = {0: 'SAM / netflix.bin', 1: 'YouTube lifecycle', 2: 'Surface Manager'}


def draw_single(ax, events, title, t_end, show_xlabel=True):
    """Draw one timeline into ax."""
    ax.set_facecolor('#16213e')
    ax.set_xlim(-0.5, t_end + 0.5)
    ax.set_ylim(-1.0, 5.4)
    if show_xlabel:
        ax.set_xlabel("Time from launch request (s)", color='#cccccc', fontsize=9)
    ax.set_title(title, color='white', fontsize=11, pad=6)
    ax.tick_params(axis='x', colors='#cccccc')
    ax.tick_params(axis='y', left=False, labelleft=False)
    for spine in ax.spines.values():
        spine.set_edgecolor('#2c3e50')

    for tier, y in TIER_Y.items():
        ax.axhline(y, color='#2c3e50', linewidth=1.5, zorder=1)
        ax.text(-0.45, y, TIER_LABEL[tier], va='center', ha='right',
                fontsize=7, color='#95a5a6', style='italic')

    LEVELS = [0.55, 0.85, 1.15]
    above_idx = {tier: 0 for tier in TIER_Y}
    below_idx = {tier: 0 for tier in TIER_Y}
    flip_state = {tier: True for tier in TIER_Y}

    for t, lbl, color, tier in events:
        y = TIER_Y[tier]
        ax.plot([t, t], [y - 0.22, y + 0.22], color=color, lw=2, zorder=3, alpha=0.9)
        ax.scatter([t], [y], color=color, s=35, zorder=4)

        above = flip_state[tier]
        flip_state[tier] = not above
        if above:
            dy = LEVELS[above_idx[tier] % len(LEVELS)]
            above_idx[tier] += 1
            va = 'bottom'
        else:
            dy = -LEVELS[below_idx[tier] % len(LEVELS)]
            below_idx[tier] += 1
            va = 'top'

        ax.annotate(
            f"+{t:.2f}s\n{lbl}",
            xy=(t, y), xytext=(t, y + dy),
            ha='center', va=va, fontsize=6.8, color=color, zorder=5,
            arrowprops=dict(arrowstyle='-', color=color, lw=0.7, alpha=0.45),
        )


def _make_delta_table(ax_tbl, rows, label_a, label_b):
    ax_tbl.set_facecolor('#0d0d1a')
    ax_tbl.axis('off')
    tbl = ax_tbl.table(
        cellText=[[r[0], r[1], r[2], r[3]] for r in rows],
        colLabels=['Metric', label_a, label_b, f'{label_b} − {label_a}'],
        loc='center', cellLoc='center',
    )
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(8.5)
    for (row, col), cell in tbl.get_celld().items():
        cell.set_facecolor('#1a1a2e')
        cell.set_edgecolor('#2c3e50')
        if row == 0:
            cell.set_text_props(color='white', fontweight='bold')
        elif col == 3:
            cell.set_text_props(color=rows[row - 1][4])
        else:
            cell.set_text_props(color='#cccccc')


def draw_comparison(events_a, label_a, events_b, label_b, out_path: str):
    """Two-panel timeline comparison + delta table."""
    t_end = max(
        (events_a[-1][0] if events_a else 0),
        (events_b[-1][0] if events_b else 0),
    ) + 1.0

    fig = plt.figure(figsize=(22, 13))
    fig.patch.set_facecolor('#1a1a2e')

    # Top panel (no xlabel to avoid overlap with bottom panel title)
    ax1 = fig.add_axes([0.07, 0.57, 0.87, 0.35])
    # Bottom panel
    ax2 = fig.add_axes([0.07, 0.16, 0.87, 0.35])

    draw_single(ax1, events_a, label_a, t_end, show_xlabel=False)
    draw_single(ax2, events_b, label_b, t_end, show_xlabel=True)

    # Build delta rows
    kt_a, kt_b = key_times(events_a), key_times(events_b)
    rows = []
    for start_lbl, end_lbl, name in METRIC_PAIRS:
        ta = metric_delta(kt_a, start_lbl, end_lbl)
        tb = metric_delta(kt_b, start_lbl, end_lbl)
        if ta is None and tb is None:
            continue
        ta_s = f"{ta:.3f}s" if ta is not None else "n/a"
        tb_s = f"{tb:.3f}s" if tb is not None else "n/a"
        if ta is not None and tb is not None:
            d = tb - ta
            sign = '+' if d >= 0 else ''
            diff_s = f"{sign}{d:.3f}s"
            dc = '#e74c3c' if d > 0.1 else ('#2ecc71' if d < -0.1 else '#f1c40f')
        else:
            diff_s, dc = "n/a", '#95a5a6'
        rows.append((name, ta_s, tb_s, diff_s, dc))

    if rows:
        ax_tbl = fig.add_axes([0.10, 0.01, 0.80, 0.11])
        _make_delta_table(ax_tbl, rows, label_a, label_b)

    fig.suptitle("Netflix Launch Timeline Comparison", color='white', fontsize=14, y=0.97)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"Saved: {out_path}")


def collect_run_timings(group_dir: str) -> list:
    """Scan all run_N dirs, return list of key_times dicts."""
    results = []
    for run_dir in sorted(glob.glob(os.path.join(group_dir, 'run_*'))):
        syslog = find_syslog(run_dir)
        if syslog:
            ev = parse_syslog(syslog)
            kt = key_times(ev)
            kt['_run'] = os.path.basename(run_dir)
            results.append(kt)
    return results


# ── Outlier detection ─────────────────────────────────────────────────────────
# Required anchor events for a run to be considered complete.
# SplashState OUT = Netflix content fully visible (splash dismissed) — this is
# the definitive scenario-end marker, ~10s after Netflix Fullscreen surface switch.
# Netflix Fullscreen fires earlier (surface switches to netflix) but content is
# not yet visible; SplashState OUT is the user-visible completion point.

REQUIRED_EVENTS = [
    'Launch requested',
    'SplashState\nOUT ✓',
]

# IQR-based numeric outlier: flag a value if it deviates from Q1/Q3 by more
# than IQR_FACTOR * IQR (only applied when n >= 4).
IQR_FACTOR = 2.0


def filter_outliers(timings: list) -> tuple:
    """
    Return (valid, outliers) where each item is a key_times dict.

    A run is an outlier ONLY if:
      - n >= 4 AND the req→Netflix Fullscreen value is a numeric IQR outlier.

    Runs with missing events are kept in valid (events that are missing will
    produce n/a naturally in per-metric extract()), but annotated for logging.
    """
    import numpy as np

    # Annotate runs with missing events but keep them in valid
    valid, outliers = [], []
    for kt in timings:
        missing = [e for e in REQUIRED_EVENTS if kt.get(e) is None]
        if missing:
            kt['_missing_events'] = missing
        valid.append(kt)

    # IQR numeric filter on the primary metric (only when n >= 4)
    # Only consider runs that have both endpoints for the primary metric
    metric_start, metric_end = REQUIRED_EVENTS[0], REQUIRED_EVENTS[-1]
    complete = [kt for kt in valid
                if kt.get(metric_start) is not None and kt.get(metric_end) is not None]
    if len(complete) >= 4:
        vals = np.array([metric_delta(kt, metric_start, metric_end)
                         for kt in complete], dtype=float)
        q1, q3 = np.percentile(vals, 25), np.percentile(vals, 75)
        iqr = q3 - q1
        lo, hi = q1 - IQR_FACTOR * iqr, q3 + IQR_FACTOR * iqr
        iqr_outlier_ids = set()
        for kt, v in zip(complete, vals):
            if v < lo or v > hi:
                kt['_outlier_reason'] = f"IQR outlier: {v:.3f}s outside [{lo:.3f}, {hi:.3f}]"
                iqr_outlier_ids.add(id(kt))
        valid = [kt for kt in valid if id(kt) not in iqr_outlier_ids]
        outliers = [kt for kt in timings if id(kt) in iqr_outlier_ids]

    return valid, outliers


def draw_stats(timings_a: list, label_a: str, timings_b: list, label_b: str, out_path: str):
    """Bar+scatter stats comparison: min/max/avg/individual per metric."""
    import numpy as np

    # Apply outlier filter before plotting
    valid_a, bad_a = filter_outliers(timings_a)
    valid_b, bad_b = filter_outliers(timings_b)

    for kt in valid_a:
        if kt.get('_missing_events'):
            print(f"  [warn] {label_a} {kt.get('_run','?')}: missing events {kt['_missing_events']} (partial data)")
    for kt in valid_b:
        if kt.get('_missing_events'):
            print(f"  [warn] {label_b} {kt.get('_run','?')}: missing events {kt['_missing_events']} (partial data)")
    for kt in bad_a:
        print(f"  [outlier excluded] {label_a} {kt.get('_run','?')}: {kt.get('_outlier_reason','')}")
    for kt in bad_b:
        print(f"  [outlier excluded] {label_b} {kt.get('_run','?')}: {kt.get('_outlier_reason','')}")

    metrics = [(s, e, n) for s, e, n in METRIC_PAIRS]
    metric_names = [n for _, _, n in metrics]

    def extract(timings, start_lbl, end_lbl):
        vals = []
        for kt in timings:
            v = metric_delta(kt, start_lbl, end_lbl)
            if v is not None:
                vals.append(v)
        return vals

    data_a = [extract(valid_a, s, e) for s, e, _ in metrics]
    data_b = [extract(valid_b, s, e) for s, e, _ in metrics]

    # Only keep metrics that have data in at least one group
    keep = [i for i in range(len(metrics)) if data_a[i] or data_b[i]]
    metric_names = [metric_names[i] for i in keep]
    data_a = [data_a[i] for i in keep]
    data_b = [data_b[i] for i in keep]

    n = len(metric_names)
    x = np.arange(n)
    bar_w = 0.30

    fig, ax = plt.subplots(figsize=(max(10, n * 2.5), 7))
    fig.patch.set_facecolor('#1a1a2e')
    ax.set_facecolor('#16213e')

    COLOR_A = '#e74c3c'
    COLOR_B = '#3498db'

    def plot_group(vals_list, offset, color, label):
        avgs, mins, maxs = [], [], []
        for vals in vals_list:
            if vals:
                avgs.append(np.mean(vals))
                mins.append(np.min(vals))
                maxs.append(np.max(vals))
            else:
                avgs.append(np.nan)
                mins.append(np.nan)
                maxs.append(np.nan)
        avgs = np.array(avgs, dtype=float)
        mins = np.array(mins, dtype=float)
        maxs = np.array(maxs, dtype=float)

        bars = ax.bar(x + offset, avgs, bar_w, color=color, alpha=0.7,
                      label=f'{label} (avg)', zorder=3)

        # min/max error bars
        lo = avgs - mins
        hi = maxs - avgs
        valid = ~np.isnan(avgs)
        ax.errorbar(x[valid] + offset, avgs[valid],
                    yerr=[lo[valid], hi[valid]],
                    fmt='none', color='white', capsize=4, lw=1.5, zorder=4)

        # individual scatter
        for i, vals in enumerate(vals_list):
            for v in vals:
                ax.scatter(x[i] + offset, v, color=color, s=40, zorder=5,
                           edgecolors='white', linewidths=0.5)
            # avg label on bar
            if not np.isnan(avgs[i]):
                ax.text(x[i] + offset, avgs[i] + 0.1, f"{avgs[i]:.2f}s",
                        ha='center', va='bottom', fontsize=7, color='white', zorder=6)

    plot_group(data_a, -bar_w / 2, COLOR_A, label_a)
    plot_group(data_b,  bar_w / 2, COLOR_B, label_b)

    ax.set_xticks(x)
    ax.set_xticklabels(metric_names, color='#cccccc', fontsize=8.5, rotation=15, ha='right')
    ax.set_ylabel("Time from launch request (s)", color='#cccccc', fontsize=9)
    ax.tick_params(axis='y', colors='#cccccc')
    ax.tick_params(axis='x', colors='#cccccc')
    for spine in ax.spines.values():
        spine.set_edgecolor('#2c3e50')
    ax.yaxis.grid(True, color='#2c3e50', linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)

    legend = ax.legend(fontsize=9, facecolor='#1a1a2e', edgecolor='#2c3e50',
                       labelcolor='white')

    fig.suptitle(
        f"Netflix Launch Timing Statistics: with vs without cgrd"
        f"\n{label_a}: {len(valid_a)} valid"
        + (f", {len(bad_a)} outlier(s) excluded" if bad_a else "")
        + f"   |   {label_b}: {len(valid_b)} valid"
        + (f", {len(bad_b)} outlier(s) excluded" if bad_b else ""),
        color='white', fontsize=11)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"Saved: {out_path}")


def write_stats_report(timings_a, label_a, timings_b, label_b, out_path: str):
    """Write markdown stats report."""
    import numpy as np

    # Apply outlier filter
    valid_a, bad_a = filter_outliers(timings_a)
    valid_b, bad_b = filter_outliers(timings_b)

    def stats(timings, start_lbl, end_lbl):
        vals = []
        for kt in timings:
            v = metric_delta(kt, start_lbl, end_lbl)
            if v is not None:
                vals.append(v)
        if not vals:
            return None
        return {
            'n': len(vals),
            'min': np.min(vals),
            'max': np.max(vals),
            'avg': np.mean(vals),
            'vals': vals,
        }

    outlier_lines = []
    for kt in bad_a:
        outlier_lines.append(f"- `{label_a}` **{kt.get('_run','?')}**: {kt.get('_outlier_reason','')}")
    for kt in bad_b:
        outlier_lines.append(f"- `{label_b}` **{kt.get('_run','?')}**: {kt.get('_outlier_reason','')}")

    warn_lines = []
    for kt in valid_a:
        if kt.get('_missing_events'):
            warn_lines.append(f"- `{label_a}` **{kt.get('_run','?')}**: missing events {kt['_missing_events']} (partial data included)")
    for kt in valid_b:
        if kt.get('_missing_events'):
            warn_lines.append(f"- `{label_b}` **{kt.get('_run','?')}**: missing events {kt['_missing_events']} (partial data included)")

    lines = [
        "# Netflix Launch Timing — Statistical Report",
        "",
        f"**Groups:** `{label_a}` vs `{label_b}`",
        f"**Runs (total / valid):** {len(timings_a)} → {len(valid_a)} / {len(timings_b)} → {len(valid_b)}",
        "",
    ]

    if warn_lines:
        lines += ["### ⚠️ Partial data (events missing, metrics using available events only)", ""] + warn_lines + [""]

    if outlier_lines:
        lines += [
            "### ❌ Outliers excluded from statistics",
            "",
        ] + outlier_lines + [""]

    lines += ["---", ""]

    for start_lbl, end_lbl, name in METRIC_PAIRS:
        sa = stats(valid_a, start_lbl, end_lbl)
        sb = stats(valid_b, start_lbl, end_lbl)
        if sa is None and sb is None:
            continue

        lines.append(f"## {name}")
        lines.append("")
        lines.append(f"| | {label_a} | {label_b} | diff (b−a) |")
        lines.append("|---|---|---|---|")

        def fmt(s, key):
            return f"{s[key]:.3f}s" if s else "n/a"

        for key, label in [('avg', 'avg'), ('min', 'min'), ('max', 'max')]:
            va = fmt(sa, key)
            vb = fmt(sb, key)
            if sa and sb:
                d = sb[key] - sa[key]
                sign = '+' if d >= 0 else ''
                diff = f"{sign}{d:.3f}s"
                flag = " ⬆ slower" if d > 0.1 else (" ⬇ faster" if d < -0.1 else "")
            else:
                diff, flag = "n/a", ""
            lines.append(f"| **{label}** | {va} | {vb} | {diff}{flag} |")

        if sa:
            runs_a = ', '.join(f"`{v:.3f}s`" for v in sa['vals'])
            lines.append(f"| individual | {runs_a} | | |")
        if sb:
            runs_b = ', '.join(f"`{v:.3f}s`" for v in sb['vals'])
            lines.append(f"| individual | | {runs_b} | |")
        lines.append("")

    lines += [
        "---",
        "",
        "> Generated by `tools/syslog/log_timeline.py --stats`",
    ]

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Saved: {out_path}")


def draw_single_file(events, title, out_path):
    """Single timeline PNG."""
    t_end = (events[-1][0] if events else 10) + 1.0
    fig, ax = plt.subplots(figsize=(18, 5))
    fig.patch.set_facecolor('#1a1a2e')
    draw_single(ax, events, title, t_end, show_xlabel=True)
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    fig.savefig(out_path, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"Saved: {out_path}")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Netflix launch timeline visualizer")
    parser.add_argument('--session',      help='Benchmark session timestamp (e.g. 20260408_213044)')
    parser.add_argument('--stats',        action='store_true',
                        help='Generate multi-run statistics chart + report instead of single timeline')
    parser.add_argument('--data-root',    default='tools/data',  help='Root of benchmark data')
    parser.add_argument('--report-root',  default='tools/report', help='Root of report output')
    parser.add_argument('--file',         help='Single syslog file path')
    parser.add_argument('--with-cgrd',    help='Syslog file for with_cgrd group')
    parser.add_argument('--without-cgrd', help='Syslog file for without_cgrd group')
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    # ── mode 1: --session ─────────────────────────────────────────────────────
    if args.session:
        session_dir = os.path.join(repo_root, args.data_root, args.session)
        out_dir     = os.path.join(repo_root, args.report_root, args.session)
        os.makedirs(out_dir, exist_ok=True)

        if args.stats:
            # Multi-run stats mode
            t_with    = collect_run_timings(os.path.join(session_dir, 'with_cgrd'))
            t_without = collect_run_timings(os.path.join(session_dir, 'without_cgrd'))
            if not t_with and not t_without:
                sys.exit(f"No syslog files found under {session_dir}")
            draw_stats(t_with, 'with cgrd', t_without, 'without cgrd',
                       os.path.join(out_dir, 'stats_comparison.png'))
            write_stats_report(t_with, 'with cgrd', t_without, 'without cgrd',
                               os.path.join(out_dir, 'stats_report.md'))
        else:
            # Single-run comparison (first run of each group)
            f_with    = find_syslog(os.path.join(session_dir, 'with_cgrd'))
            f_without = find_syslog(os.path.join(session_dir, 'without_cgrd'))
            if not f_with and not f_without:
                sys.exit(f"No syslog files found under {session_dir}")
            if f_with and f_without:
                draw_comparison(parse_syslog(f_with), 'with cgrd',
                                parse_syslog(f_without), 'without cgrd',
                                os.path.join(out_dir, 'timeline_comparison.png'))
            elif f_with:
                draw_single_file(parse_syslog(f_with), 'with cgrd',
                                 os.path.join(out_dir, 'timeline_with_cgrd.png'))
            else:
                draw_single_file(parse_syslog(f_without), 'without cgrd',
                                 os.path.join(out_dir, 'timeline_without_cgrd.png'))
        return

    # ── mode 2: explicit --with-cgrd / --without-cgrd ────────────────────────
    if args.with_cgrd or args.without_cgrd:
        out_dir = os.path.join(repo_root, args.report_root)
        if args.with_cgrd and args.without_cgrd:
            draw_comparison(parse_syslog(args.with_cgrd), 'with cgrd',
                            parse_syslog(args.without_cgrd), 'without cgrd',
                            os.path.join(out_dir, 'timeline_comparison.png'))
        elif args.with_cgrd:
            draw_single_file(parse_syslog(args.with_cgrd), 'with cgrd',
                             os.path.join(out_dir, 'timeline_with_cgrd.png'))
        else:
            draw_single_file(parse_syslog(args.without_cgrd), 'without cgrd',
                             os.path.join(out_dir, 'timeline_without_cgrd.png'))
        return

    # ── mode 3: single --file ─────────────────────────────────────────────────
    if args.file:
        ev   = parse_syslog(args.file)
        base = os.path.splitext(os.path.basename(args.file))[0]
        out  = os.path.join(repo_root, args.report_root, f'timeline_{base}.png')
        draw_single_file(ev, base, out)
        return

    parser.print_help()
    sys.exit(1)


if __name__ == '__main__':
    main()
