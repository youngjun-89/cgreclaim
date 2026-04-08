# Benchmark Flow — cgrd vs. no-cgrd Memory Profiling

## Purpose

Compare per-run `/proc/meminfo` and cgroup v2 memory stats between two conditions:

| Group | Description |
|---|---|
| `with_cgrd` | `/home/root/cgrd` runs 20 s before the test, adapting `memory.high` throughout |
| `without_cgrd` | Test runs without cgrd — kernel defaults, no soft-limit adjustment |

Each group collects **N runs** (default 5). Every run is preceded by a clean reboot so board state is consistent.

---

## Repository Layout (tools/)

```
tools/
├── meminfo_sampler.sh       # /proc/meminfo sampler (runs on board)
├── cgroup_sampler.py        # cgroup v2 stat sampler (runs on board)
├── log/                     # created at runtime on board
│   ├── meminfo_sampler/     # meminfo_samples_<PID>.csv
│   └── cgroup_sampler/      # <cgroup_name>.csv
├── data/                    # collected on HOST by run_bench.sh
│   ├── with_cgrd/
│   │   ├── run_1/
│   │   │   ├── meminfo/     # meminfo CSVs from board
│   │   │   ├── cgroup/      # per-cgroup CSVs from board
│   │   │   ├── test_run.log # stdout/stderr of test.sh
│   │   │   └── cgrd.log     # cgrd stdout/stderr
│   │   └── run_2/ ...
│   └── without_cgrd/
│       └── run_1/ ...
└── script/
    ├── test.sh              # board-side test sequence
    └── run_bench.sh         # HOST-side orchestrator (not run on board)
```

---

## Board Layout

Deploy the `tools/` directory to `/home/root/profile/` on the board:

```sh
scp -r tools/ root@<BOARD_IP>:/home/root/profile/
```

Expected layout on board:
```
/home/root/
├── cgrd                         # cgreclaim daemon binary
└── profile/
    ├── meminfo_sampler.sh
    ├── cgroup_sampler.py
    ├── requirements.txt
    └── script/
        └── test.sh
```

---

## Running the Benchmark (from host)

```sh
cd tools/script

# Both groups, 5 runs each (default)
./run_bench.sh

# Custom board IP, 3 runs each
./run_bench.sh -b 192.168.0.200 -n 3

# Only collect without_cgrd group
./run_bench.sh -m without_cgrd

# Full help
./run_bench.sh --help
```

The script:
1. Verifies SSH access and board prerequisites before each group
2. For each run:
   a. (with_cgrd only) Starts cgrd, waits 20 s for it to stabilize
   b. SSHes into board and runs `test.sh -c` (blocking, ~100 s)
   c. SCPs `log/meminfo_sampler/`, `log/cgroup_sampler/`, and `test_run.log` back to host
   d. Issues `reboot` over SSH; waits for board to return before next run

---

## test.sh Phase Markers

`test.sh` signals the meminfo sampler at phase boundaries via `SIGUSR1`.

| Phase | `phase` column value | Period |
|---|---|---|
| 1 | `1` | Start → YouTube launched + 70 s |
| 2 | `2` | Netflix launched → end |

The `phase` column appears as the second column in every `meminfo_samples_*.csv`.

---

## Comparing Results

After all runs are collected in `tools/data/`, use the Python plotters:

```sh
# Plot a single meminfo CSV
python tools/meminfo_plot.py tools/data/with_cgrd/run_1/meminfo/meminfo_samples_*.csv

# Plot cgroup stats for a specific service
python tools/cgroup_plot.py tools/data/with_cgrd/run_1/cgroup/youtube.leanback.v4.csv
```

To compare groups, load both groups' CSV files in the same plot. The `phase` column can be used to align runs at the YouTube→Netflix boundary regardless of wall-clock time.

---

## Sequence Diagram

```
HOST                          BOARD
 │                              │
 │  ── SSH: check prereqs ─────>│
 │  ←─ OK ──────────────────────│
 │                              │
 │  [with_cgrd group, run i]    │
 │  ── SSH: rm log/ ───────────>│
 │  ── SSH: start cgrd ────────>│  cgrd running
 │  (sleep 20s on host)         │
 │  ── SSH: test.sh -c ────────>│  meminfo_sampler (bg)
 │                              │  cgroup_sampler  (bg)
 │                              │  setPreloadPolicy
 │                              │  closeAllApps
 │                              │  launch youtube  ─ phase 1
 │                              │  sleep 70
 │                              │  [SIGUSR1 → phase 2]
 │                              │  launch netflix
 │                              │  sleep 15
 │                              │  kill samplers → CSV saved
 │  ←─ SSH returns ─────────────│
 │  ── SCP log/ ───────────────>│
 │  ←─ data/with_cgrd/run_i/ ───│
 │  ── SSH: reboot ────────────>│  (board reboots)
 │  (sleep 60s + poll SSH)      │
 │                              │  (boot complete)
 │  [next run or group]         │
```
