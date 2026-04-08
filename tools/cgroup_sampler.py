#!/usr/bin/env python3
# TOOLS_VERSION=2
"""
cgroup_sampler.py
-----------------
Continuously samples cgroup v2 memory stats every N seconds and writes
per-cgroup CSV files into a ./log/ directory.

Collected metrics per cgroup:
  memory.current         → mem_mb
  memory.swap.current    → swap_mb
  memory.stat pgmajfault → pgmajfault (cumulative) + pgmajfault_rate (/s)
  memory.stat workingset_refault_anon → ws_refault_anon + rate
  memory.stat workingset_refault_file → ws_refault_file + rate
  memory.pressure        → psi_some_avg10, psi_full_avg10

CSV per cgroup:
  timestamp, mem_mb, swap_mb,
  pgmajfault, pgmajfault_rate,
  ws_refault_anon, ws_refault_anon_rate,
  ws_refault_file, ws_refault_file_rate,
  psi_some_avg10, psi_full_avg10

Usage:
    python cgroup_sampler.py [--root PATH] [--interval SECS] [--min-mb FLOAT]
                             [--outdir DIR]
"""

import argparse
import os
import re
import signal
import sys
import time
from datetime import datetime
from pathlib import Path

CGROUP_ROOT = Path("/sys/fs/cgroup")
MB = 1024 * 1024

CSV_HEADER = (
    "timestamp,mem_mb,swap_mb,"
    "pgmajfault,pgmajfault_rate,"
    "ws_refault_anon,ws_refault_anon_rate,"
    "ws_refault_file,ws_refault_file_rate,"
    "psi_some_avg10,psi_full_avg10\n"
)


# ── cgroup readers ────────────────────────────────────────────────────────────

def read_knob(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def read_int(path: Path) -> int | None:
    raw = read_knob(path)
    if raw is None or raw == "max":
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def read_stat(path: Path) -> dict[str, int]:
    """Parse memory.stat key-value pairs into a dict."""
    result: dict[str, int] = {}
    raw = read_knob(path)
    if not raw:
        return result
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) == 2:
            try:
                result[parts[0]] = int(parts[1])
            except ValueError:
                pass
    return result


def read_psi(path: Path) -> tuple[float, float]:
    """Return (some_avg10, full_avg10) from memory.pressure, or (0, 0)."""
    raw = read_knob(path)
    if not raw:
        return 0.0, 0.0
    some_avg10 = full_avg10 = 0.0
    for line in raw.splitlines():
        m = re.match(r"(some|full)\s+avg10=([0-9.]+)", line)
        if m:
            val = float(m.group(2))
            if m.group(1) == "some":
                some_avg10 = val
            else:
                full_avg10 = val
    return some_avg10, full_avg10


# ── cgroup discovery ──────────────────────────────────────────────────────────

def discover_cgroups(root: Path, min_mb: float) -> list[Path]:
    found = []
    for dirpath, dirnames, _ in os.walk(root):
        dirnames.sort()
        d = Path(dirpath)
        cur = read_int(d / "memory.current")
        if cur is None:
            continue
        if cur / MB < min_mb:
            continue
        found.append(d)
    return found


def cgroup_csv_name(cg_path: Path, root: Path) -> str:
    """Convert cgroup path to a safe filename: /foo/bar → foo_bar.csv"""
    rel = str(cg_path.relative_to(root))
    if rel == ".":
        return "root.csv"
    safe = rel.replace("/", "_").replace(" ", "_").strip("_")
    return safe + ".csv"


# ── sampling ──────────────────────────────────────────────────────────────────

class CgroupTracker:
    """Tracks a single cgroup and writes its CSV."""

    def __init__(self, path: Path, root: Path, outdir: Path):
        self.path = path
        self.csv = outdir / cgroup_csv_name(path, root)
        self._prev: dict[str, int] = {}
        self._prev_time: float = 0.0

        if not self.csv.exists():
            self.csv.write_text(CSV_HEADER)

    def sample(self, now: str, now_ts: float) -> str | None:
        cur = read_int(self.path / "memory.current")
        if cur is None:
            return None

        swap = read_int(self.path / "memory.swap.current") or 0
        stat = read_stat(self.path / "memory.stat")
        psi_some, psi_full = read_psi(self.path / "memory.pressure")

        pgmaj      = stat.get("pgmajfault", 0)
        ws_anon    = stat.get("workingset_refault_anon", 0)
        ws_file    = stat.get("workingset_refault_file", 0)

        dt = now_ts - self._prev_time if self._prev_time else 1.0
        dt = max(dt, 0.001)

        def rate(key: int, prev_key: str) -> float:
            prev = self._prev.get(prev_key, key)
            return max(0.0, (key - prev) / dt)

        pgmaj_rate   = rate(pgmaj,   "pgmajfault")
        wsanon_rate  = rate(ws_anon, "ws_refault_anon")
        wsfile_rate  = rate(ws_file, "ws_refault_file")

        self._prev = {"pgmajfault": pgmaj, "ws_refault_anon": ws_anon, "ws_refault_file": ws_file}
        self._prev_time = now_ts

        row = (
            f"{now},"
            f"{cur/MB:.3f},{swap/MB:.3f},"
            f"{pgmaj},{pgmaj_rate:.3f},"
            f"{ws_anon},{wsanon_rate:.3f},"
            f"{ws_file},{wsfile_rate:.3f},"
            f"{psi_some:.2f},{psi_full:.2f}\n"
        )
        with self.csv.open("a") as f:
            f.write(row)
        return row


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=str(CGROUP_ROOT), metavar="PATH",
                        help=f"cgroup v2 mount point (default: {CGROUP_ROOT})")
    parser.add_argument("--interval", "-i", type=float, default=1.0, metavar="SECS",
                        help="Sampling interval in seconds (default: 1.0)")
    parser.add_argument("--min-mb", type=float, default=1.0, metavar="FLOAT",
                        help="Skip cgroups whose memory.current is below this MB (default: 1.0)")
    parser.add_argument("--outdir", default="log/cgroup_sampler", metavar="DIR",
                        help="Output directory for CSV files (default: ./log/cgroup_sampler)")
    args = parser.parse_args()

    root   = Path(args.root)
    outdir = Path(args.outdir)

    if not root.is_dir():
        print(f"error: cgroup root not found: {root}", file=sys.stderr)
        sys.exit(1)

    outdir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {outdir.resolve()}")

    stop = False

    def _stop(sig, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT,  _stop)
    signal.signal(signal.SIGTERM, _stop)

    trackers: dict[Path, CgroupTracker] = {}
    print(f"Sampling cgroup v2 stats every {args.interval}s — Ctrl+C to stop.")

    while not stop:
        now_ts = time.monotonic()
        now    = datetime.now().strftime("%H:%M:%S")

        # Refresh cgroup list each cycle (handles new/removed cgroups)
        active = discover_cgroups(root, args.min_mb)
        active_set = set(active)

        for cg in active:
            if cg not in trackers:
                trackers[cg] = CgroupTracker(cg, root, outdir)

        # Remove stale trackers
        for gone in set(trackers) - active_set:
            del trackers[gone]

        for tracker in trackers.values():
            tracker.sample(now, now_ts)

        n = len(trackers)
        print(f"\r[{now}]  Tracking {n} cgroup(s) → {outdir}/", end="", flush=True)

        elapsed = time.monotonic() - now_ts
        sleep_time = args.interval - elapsed
        if sleep_time > 0:
            time.sleep(sleep_time)

    print(f"\nSampling stopped. CSVs saved in {outdir.resolve()}/")
    for t in sorted(trackers):
        print(f"  {trackers[t].csv}")


if __name__ == "__main__":
    main()
