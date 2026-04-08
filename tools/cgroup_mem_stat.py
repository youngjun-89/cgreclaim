#!/usr/bin/env python3
"""
cgroup_mem_stat.py
------------------
Recursively walks /sys/fs/cgroup and prints memory.current,
memory.high, and swap (memory.swap.current) for every cgroup
that exposes those knobs.

All values are displayed in MB.

Usage:
    python cgroup_mem_stat.py [--root PATH] [--sort {name,mem,swap}]
                              [--min-mb FLOAT] [--no-header] [--tsv]
"""

import argparse
import os
import sys
from pathlib import Path

CGROUP_ROOT = Path("/sys/fs/cgroup")
MB = 1024 * 1024


# ── helpers ────────────────────────────────────────────────────────────────

def read_knob(path: Path) -> str | None:
    """Return stripped text of a cgroup knob file, or None on error."""
    try:
        return path.read_text().strip()
    except OSError:
        return None


def knob_mb(path: Path) -> float | None:
    """
    Return knob value in MB.
    Returns None if the file doesn't exist, or float('inf') if value is 'max'.
    """
    raw = read_knob(path)
    if raw is None:
        return None
    if raw == "max":
        return float("inf")
    try:
        return int(raw) / MB
    except ValueError:
        return None


def fmt_mb(value: float | None, inf_str: str = "max") -> str:
    if value is None:
        return "-"
    if value == float("inf"):
        return inf_str
    return f"{value:>10.2f}"


# ── walk ───────────────────────────────────────────────────────────────────

def collect(root: Path, min_mb: float) -> list[dict]:
    rows: list[dict] = []

    for dirpath, dirnames, _ in os.walk(root):
        dirnames.sort()                  # deterministic order
        d = Path(dirpath)

        mem_cur = knob_mb(d / "memory.current")
        if mem_cur is None:
            continue                     # not a leaf / unified cgroup

        mem_high = knob_mb(d / "memory.high")
        swap_cur = knob_mb(d / "memory.swap.current")

        if mem_cur < min_mb:
            continue

        # cgroup path relative to root, show "/" for root itself
        rel = str(d.relative_to(root))
        cg_path = "/" if rel == "." else f"/{rel}"

        rows.append(
            dict(
                cg_path=cg_path,
                mem_cur=mem_cur,
                mem_high=mem_high,
                swap_cur=swap_cur,
            )
        )

    return rows


# ── formatting ─────────────────────────────────────────────────────────────

COL_WIDTHS = {
    "cgroup":   55,
    "mem_cur":  13,
    "mem_high": 13,
    "swap_cur": 13,
}

HEADER = (
    f"{'CGROUP':<{COL_WIDTHS['cgroup']}} "
    f"{'MEM_CUR(MB)':>{COL_WIDTHS['mem_cur']}} "
    f"{'MEM_HIGH(MB)':>{COL_WIDTHS['mem_high']}} "
    f"{'SWAP_CUR(MB)':>{COL_WIDTHS['swap_cur']}}"
)

SEPARATOR = "-" * len(HEADER)


def print_table(rows: list[dict], no_header: bool, tsv: bool) -> None:
    if tsv:
        if not no_header:
            print("cgroup\tmem_cur_mb\tmem_high_mb\tswap_cur_mb")
        for r in rows:
            print(
                f"{r['cg_path']}\t"
                f"{fmt_mb(r['mem_cur'], 'max')}\t"
                f"{fmt_mb(r['mem_high'], 'max')}\t"
                f"{fmt_mb(r['swap_cur'], '-')}"
            )
        return

    if not no_header:
        print(SEPARATOR)
        print(HEADER)
        print(SEPARATOR)

    for r in rows:
        path = r["cg_path"]
        # truncate long paths with leading ellipsis
        if len(path) > COL_WIDTHS["cgroup"]:
            path = "…" + path[-(COL_WIDTHS["cgroup"] - 1):]
        print(
            f"{path:<{COL_WIDTHS['cgroup']}} "
            f"{fmt_mb(r['mem_cur']):>{COL_WIDTHS['mem_cur']}} "
            f"{fmt_mb(r['mem_high'], 'max'):>{COL_WIDTHS['mem_high']}} "
            f"{fmt_mb(r['swap_cur'], '-'):>{COL_WIDTHS['swap_cur']}}"
        )

    if not no_header:
        print(SEPARATOR)
        total_mem  = sum(r["mem_cur"] for r in rows if r["mem_cur"] is not None)
        total_swap = sum(r["swap_cur"] for r in rows if r["swap_cur"] is not None)
        print(
            f"{'TOTAL':<{COL_WIDTHS['cgroup']}} "
            f"{total_mem:>{COL_WIDTHS['mem_cur']}.2f} "
            f"{'':>{COL_WIDTHS['mem_high']}} "
            f"{total_swap:>{COL_WIDTHS['swap_cur']}.2f}"
        )
        print(SEPARATOR)
        print(f"  {len(rows)} cgroup(s) shown")


# ── main ───────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=str(CGROUP_ROOT), metavar="PATH",
                        help=f"cgroup v2 mount point (default: {CGROUP_ROOT})")
    parser.add_argument("--sort", choices=["name", "mem", "swap"], default="name",
                        help="Sort order: name | mem (memory.current desc) | swap (swap desc)")
    parser.add_argument("--min-mb", type=float, default=0.0, metavar="FLOAT",
                        help="Hide cgroups whose memory.current is below this threshold (MB)")
    parser.add_argument("--no-header", action="store_true",
                        help="Suppress header and footer lines")
    parser.add_argument("--tsv", action="store_true",
                        help="Tab-separated output (pipe-friendly)")
    args = parser.parse_args()

    root = Path(args.root)
    if not root.is_dir():
        print(f"error: cgroup root not found: {root}", file=sys.stderr)
        sys.exit(1)

    rows = collect(root, args.min_mb)

    if args.sort == "mem":
        rows.sort(key=lambda r: r["mem_cur"] or 0, reverse=True)
    elif args.sort == "swap":
        rows.sort(key=lambda r: r["swap_cur"] or 0, reverse=True)
    else:
        rows.sort(key=lambda r: r["cg_path"])

    print_table(rows, args.no_header, args.tsv)


if __name__ == "__main__":
    main()
