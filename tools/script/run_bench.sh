#!/bin/sh
# run_bench.sh — host-side benchmark orchestrator
# ------------------------------------------------
# SSH into the target board, collect memory profiles with and without cgrd,
# reboot between every run, and save labeled CSVs locally for comparison.
#
# Groups:
#   with_cgrd    — start /home/root/cgrd 20 s before test.sh -c
#   without_cgrd — run test.sh -c alone (no cgrd)
#
# Local output layout:
#   tools/data/
#     with_cgrd/
#       run_1/  meminfo/  cgroup/  test_run.log
#       run_2/  ...
#     without_cgrd/
#       run_1/  ...
#
# Usage: run_bench.sh [OPTIONS]
#   -b, --board IP        Board IP (default: 192.168.0.157)
#   -u, --user  USER      SSH user   (default: root)
#   -n, --runs  N         Runs per group (default: 5)
#   -m, --mode  MODE      with_cgrd | without_cgrd | both (default: both)
#   -h, --help            Show this help

BOARD_IP="192.168.0.157"
BOARD_USER="root"
BOARD_PROFILE_DIR="/home/root/profile"
BOARD_CGRD="/home/root/cgrd"
CGRD_HEADSTART=20   # seconds to let cgrd warm up before test starts
RUNS=5
MODE="both"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$SCRIPT_DIR/../data"

REBOOT_INITIAL_WAIT=30   # seconds to wait before starting SSH polling after reboot
SSH_POLL_INTERVAL=10     # seconds between SSH attempts
SSH_POLL_TIMEOUT=300     # max seconds to poll SSH after initial wait

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=15 -o BatchMode=yes -o ServerAliveInterval=10 -o ServerAliveCountMax=3"

# ── helpers ───────────────────────────────────────────────────────────────────

log() { printf "[%s] %s\n" "$(date '+%H:%M:%S')" "$*"; }

die() { printf "ERROR: %s\n" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

  -b, --board IP        Board IP address (default: $BOARD_IP)
  -u, --user  USER      SSH login user   (default: $BOARD_USER)
  -n, --runs  N         Runs per group   (default: $RUNS)
  -m, --mode  MODE      with_cgrd | without_cgrd | both (default: $MODE)
  -h, --help            Show this help

Examples:
  $(basename "$0")                        # both groups, 5 runs each
  $(basename "$0") -n 3 -m with_cgrd      # 3 runs, cgrd group only
  $(basename "$0") -b 192.168.0.200 -n 1  # custom board IP, 1 run each
EOF
    exit 0
}

# ── argument parsing ──────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--board) BOARD_IP="$2";   shift 2 ;;
        -u|--user)  BOARD_USER="$2"; shift 2 ;;
        -n|--runs)  RUNS="$2";       shift 2 ;;
        -m|--mode)  MODE="$2";       shift 2 ;;
        -h|--help)  usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

BOARD="$BOARD_USER@$BOARD_IP"

case "$MODE" in
    with_cgrd|without_cgrd|both) ;;
    *) die "Invalid mode '$MODE'. Use: with_cgrd | without_cgrd | both" ;;
esac

# ── SSH helpers ───────────────────────────────────────────────────────────────

ssh_cmd() {
    # shellcheck disable=SC2086
    ssh $SSH_OPTS "$BOARD" "$@"
}

scp_from_board() {
    # shellcheck disable=SC2086
    scp -r $SSH_OPTS "$BOARD:$1" "$2" 2>/dev/null || true
}

wait_for_ssh() {
    log "Polling SSH on $BOARD_IP (interval=${SSH_POLL_INTERVAL}s, timeout=${SSH_POLL_TIMEOUT}s)..."
    ELAPSED=0
    while ! ssh_cmd "true" >/dev/null 2>&1; do
        if [ "$ELAPSED" -ge "$SSH_POLL_TIMEOUT" ]; then
            die "Board $BOARD_IP did not respond to SSH after $((REBOOT_INITIAL_WAIT+SSH_POLL_TIMEOUT))s total"
        fi
        printf "  [%s] still waiting... (%ds elapsed)\n" "$(date '+%H:%M:%S')" "$ELAPSED"
        sleep "$SSH_POLL_INTERVAL"
        ELAPSED=$((ELAPSED + SSH_POLL_INTERVAL))
    done
    log "Board is up (SSH ready)."
}

# Wait after a reboot: short initial delay (board won't be up yet), then poll
wait_after_reboot() {
    log "Waiting ${REBOOT_INITIAL_WAIT}s before polling (board is rebooting)..."
    sleep "$REBOOT_INITIAL_WAIT"
    wait_for_ssh
}

# ── prereq check ──────────────────────────────────────────────────────────────

check_local_prereqs() {
    command -v ssh  >/dev/null 2>&1 || die "ssh not found"
    command -v scp  >/dev/null 2>&1 || die "scp not found"
}

check_board_prereqs() {
    GROUP="$1"
    log "Checking board prerequisites..."
    ssh_cmd "test -d '$BOARD_PROFILE_DIR'" \
        || die "Board: $BOARD_PROFILE_DIR not found — deploy tools first"
    ssh_cmd "test -f '$BOARD_PROFILE_DIR/script/test.sh'" \
        || die "Board: $BOARD_PROFILE_DIR/script/test.sh not found"
    ssh_cmd "test -f '$BOARD_PROFILE_DIR/meminfo_sampler.sh'" \
        || die "Board: $BOARD_PROFILE_DIR/meminfo_sampler.sh not found"
    ssh_cmd "test -f '$BOARD_PROFILE_DIR/cgroup_sampler.py'" \
        || die "Board: $BOARD_PROFILE_DIR/cgroup_sampler.py not found"
    ssh_cmd "command -v python3 >/dev/null 2>&1" \
        || die "Board: python3 not found"
    if [ "$GROUP" = "with_cgrd" ]; then
        ssh_cmd "test -x '$BOARD_CGRD'" \
            || die "Board: $BOARD_CGRD not found or not executable"
    fi
    log "Board prerequisites OK."
}

# ── log collection ────────────────────────────────────────────────────────────

collect_logs() {
    GROUP="$1"
    RUN="$2"
    DEST="$DATA_DIR/$GROUP/run_$RUN"
    mkdir -p "$DEST/meminfo" "$DEST/cgroup"

    log "Collecting logs → $DEST"
    scp_from_board "$BOARD_PROFILE_DIR/log/meminfo_sampler/." "$DEST/meminfo/"
    scp_from_board "$BOARD_PROFILE_DIR/log/cgroup_sampler/."  "$DEST/cgroup/"
    scp_from_board "/tmp/test_run.log"                         "$DEST/test_run.log"

    if [ "$GROUP" = "with_cgrd" ]; then
        scp_from_board "/tmp/cgrd.log" "$DEST/cgrd.log"
    fi

    log "Logs saved to $DEST/"
}

# ── single run ────────────────────────────────────────────────────────────────

do_run() {
    GROUP="$1"
    RUN="$2"
    log "--- $GROUP run $RUN/$RUNS ---"

    # Clean up previous run's logs on board
    ssh_cmd "rm -rf '$BOARD_PROFILE_DIR/log/' /tmp/test_run.log /tmp/cgrd.log"

    if [ "$GROUP" = "with_cgrd" ]; then
        log "Starting cgrd ($CGRD_HEADSTART s head start)..."
        ssh_cmd "nohup $BOARD_CGRD >'/tmp/cgrd.log' 2>&1 &"
        sleep "$CGRD_HEADSTART"
    fi

    log "Running test.sh -c on board (this takes ~100 s)..."
    ssh_cmd "cd '$BOARD_PROFILE_DIR' && sh script/test.sh -c >/tmp/test_run.log 2>&1"
    log "test.sh completed."

    collect_logs "$GROUP" "$RUN"

    log "Rebooting board after run $RUN..."
    ssh_cmd "reboot" 2>/dev/null || true
}

# ── group runner ──────────────────────────────────────────────────────────────

run_group() {
    GROUP="$1"
    log "=== Group: $GROUP — $RUNS run(s) ==="

    # Board is already up (initial reboot or previous group's wait_after_reboot)
    check_board_prereqs "$GROUP"

    i=1
    while [ "$i" -le "$RUNS" ]; do
        do_run "$GROUP" "$i"
        # After reboot, wait before next run (skip after last run of last group)
        if [ "$i" -lt "$RUNS" ] || [ "$GROUP" = "with_cgrd" -a "$MODE" = "both" ]; then
            wait_after_reboot
        fi
        i=$((i + 1))
    done

    log "=== Group $GROUP complete. ==="
}

# ── main ──────────────────────────────────────────────────────────────────────

check_local_prereqs
mkdir -p "$DATA_DIR"

# Initial clean reboot before any measurement
log "Initial reboot to ensure clean board state..."
wait_for_ssh   # verify board is reachable first
ssh_cmd "reboot" 2>/dev/null || true
wait_after_reboot

case "$MODE" in
    with_cgrd)    run_group with_cgrd ;;
    without_cgrd) run_group without_cgrd ;;
    both)
        run_group with_cgrd
        # Board was rebooted at end of last with_cgrd run; wait before starting next group
        wait_after_reboot
        run_group without_cgrd
        ;;
esac

log "All done."
log "Results in $DATA_DIR/"
log "  with_cgrd/:    $DATA_DIR/with_cgrd/"
log "  without_cgrd/: $DATA_DIR/without_cgrd/"
