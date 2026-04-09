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
COLLECT_SYSLOG=0    # set 1 via --syslog to capture /var/log/messages during transition
TIMING_ONLY=0       # set 1 via --timing-only to skip meminfo+cgroup, syslog timing only
REQUIRED_VERSION=4  # must match TOOLS_VERSION in test.sh / meminfo_sampler.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$TOOLS_DIR/.." && pwd)"
SESSION="$(date '+%Y%m%d_%H%M%S')"
DATA_DIR="$TOOLS_DIR/data/$SESSION"

# Load .env from repo root if present (credentials, Confluence config)
if [ -f "$REPO_DIR/.env" ]; then
    # shellcheck disable=SC1090
    . "$REPO_DIR/.env"
fi

REBOOT_INITIAL_WAIT=30   # seconds to wait before starting SSH polling after reboot
SSH_POLL_INTERVAL=5      # seconds between SSH attempts
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
  -s, --syslog          Collect /var/log/messages during YouTube→Netflix transition
  -t, --timing-only     Skip meminfo+cgroup, collect syslog timing only (implies --syslog)
  -h, --help            Show this help

Examples:
  $(basename "$0")                        # both groups, 5 runs each
  $(basename "$0") -n 3 -m with_cgrd      # 3 runs, cgrd group only
  $(basename "$0") -n 3 -s -t             # 3 runs, timing only (fast)
  $(basename "$0") -b 192.168.0.200 -n 1  # custom board IP, 1 run each
EOF
    exit 0
}

# ── argument parsing ──────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--board)  BOARD_IP="$2";   shift 2 ;;
        -u|--user)   BOARD_USER="$2"; shift 2 ;;
        -n|--runs)   RUNS="$2";       shift 2 ;;
        -m|--mode)   MODE="$2";       shift 2 ;;
        -s|--syslog)      COLLECT_SYSLOG=1; shift ;;
        -t|--timing-only) TIMING_ONLY=1; COLLECT_SYSLOG=1; shift ;;
        -h|--help)   usage ;;
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
    scp -r $SSH_OPTS "$BOARD:$1" "$2"
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

# ── deploy & version ─────────────────────────────────────────────────────────

deploy_tools() {
    log "Deploying tools v$REQUIRED_VERSION to board..."
    # shellcheck disable=SC2086
    ssh_cmd "mkdir -p '$BOARD_PROFILE_DIR/script' '$BOARD_PROFILE_DIR/syslog' \
        '$BOARD_PROFILE_DIR/log/meminfo_sampler' '$BOARD_PROFILE_DIR/log/cgroup_sampler' \
        '$BOARD_PROFILE_DIR/log/syslog'"
    if [ "$TIMING_ONLY" -eq 0 ]; then
        scp $SSH_OPTS \
            "$TOOLS_DIR/meminfo_sampler.sh" \
            "$TOOLS_DIR/cgroup_sampler.py" \
            "$BOARD:$BOARD_PROFILE_DIR/" || die "Failed to deploy tools to board"
    fi
    scp $SSH_OPTS \
        "$TOOLS_DIR/script/test.sh" \
        "$BOARD:$BOARD_PROFILE_DIR/script/" || die "Failed to deploy test.sh to board"
    scp $SSH_OPTS \
        "$TOOLS_DIR/syslog/syslog_collector.sh" \
        "$BOARD:$BOARD_PROFILE_DIR/syslog/" || die "Failed to deploy syslog_collector.sh to board"
    ssh_cmd "chmod +x \
        '$BOARD_PROFILE_DIR/script/test.sh' \
        '$BOARD_PROFILE_DIR/syslog/syslog_collector.sh'"
    if [ "$TIMING_ONLY" -eq 0 ]; then
        ssh_cmd "chmod +x '$BOARD_PROFILE_DIR/meminfo_sampler.sh'"
    fi
    log "Deployed."
}

verify_version() {
    REMOTE_VER=$(ssh_cmd "grep -m1 'TOOLS_VERSION=' '$BOARD_PROFILE_DIR/script/test.sh' 2>/dev/null | cut -d= -f2")
    if [ "$REMOTE_VER" != "$REQUIRED_VERSION" ]; then
        die "Version mismatch: board test.sh v${REMOTE_VER:-unknown}, required v$REQUIRED_VERSION"
    fi
    log "Version OK (v$REQUIRED_VERSION)."
}

# ── prereq check ──────────────────────────────────────────────────────────────

check_local_prereqs() {
    command -v ssh  >/dev/null 2>&1 || die "ssh not found"
    command -v scp  >/dev/null 2>&1 || die "scp not found"
    [ -f "$TOOLS_DIR/script/test.sh" ] || die "Local file not found: $TOOLS_DIR/script/test.sh"
    if [ "$TIMING_ONLY" -eq 0 ]; then
        for f in "$TOOLS_DIR/meminfo_sampler.sh" "$TOOLS_DIR/cgroup_sampler.py"; do
            [ -f "$f" ] || die "Local file not found: $f"
        done
    fi
}

check_board_prereqs() {
    GROUP="$1"
    log "Checking board prerequisites..."
    ssh_cmd "test -x '$BOARD_PROFILE_DIR/script/test.sh'" \
        || die "Board: test.sh not found/executable"
    if [ "$TIMING_ONLY" -eq 0 ]; then
        ssh_cmd "test -x '$BOARD_PROFILE_DIR/meminfo_sampler.sh'" \
            || die "Board: meminfo_sampler.sh not found/executable — run deploy_tools"
        ssh_cmd "command -v python3 >/dev/null 2>&1" \
            || die "Board: python3 not found"
    fi
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
    if [ "$TIMING_ONLY" -eq 0 ]; then
        mkdir -p "$DEST/meminfo" "$DEST/cgroup"
    else
        mkdir -p "$DEST"
    fi

    log "Collecting logs → $DEST"

    # Debug: show what's on board before collecting
    ssh_cmd "ls -la '$BOARD_PROFILE_DIR/log/' 2>/dev/null || echo '(log dir missing)'"

    if [ "$TIMING_ONLY" -eq 0 ]; then
        if ssh_cmd "test -d '$BOARD_PROFILE_DIR/log/meminfo_sampler'"; then
            scp_from_board "$BOARD_PROFILE_DIR/log/meminfo_sampler" "$DEST/" \
                && log "meminfo logs collected." \
                || log "WARN: meminfo scp failed"
        else
            log "WARN: meminfo log dir not found on board"
        fi

        if ssh_cmd "test -d '$BOARD_PROFILE_DIR/log/cgroup_sampler'"; then
            scp_from_board "$BOARD_PROFILE_DIR/log/cgroup_sampler" "$DEST/" \
                && log "cgroup logs collected." \
                || log "WARN: cgroup scp failed"
        else
            log "WARN: cgroup log dir not found on board"
        fi
    fi

    if [ "$COLLECT_SYSLOG" -eq 1 ]; then
        if ssh_cmd "test -d '$BOARD_PROFILE_DIR/log/syslog'"; then
            scp_from_board "$BOARD_PROFILE_DIR/log/syslog" "$DEST/" \
                && log "syslog collected." \
                || log "WARN: syslog scp failed"
        else
            log "WARN: syslog log dir not found on board"
        fi
    fi

    scp_from_board "/tmp/test_run.log" "$DEST/test_run.log" \
        || log "WARN: test_run.log not collected"
    # cgrd runs with --no-log during benchmarks; no cgrd.log to collect

    # Clean board logs after collection
    ssh_cmd "rm -rf '$BOARD_PROFILE_DIR/log/' /tmp/test_run.log"
    log "Logs saved to $DEST/ and cleaned from board."
}

# ── single run ────────────────────────────────────────────────────────────────

do_run() {
    GROUP="$1"
    RUN="$2"
    log "--- $GROUP run $RUN/$RUNS ---"

    # Ensure board log dir is clean before run (safety net if previous collect failed)
    ssh_cmd "rm -rf '$BOARD_PROFILE_DIR/log/' /tmp/test_run.log"

    if [ "$GROUP" = "with_cgrd" ]; then
        log "Starting cgrd ($CGRD_HEADSTART s head start)..."
        # --no-log: suppress log file I/O so cgrd writes don't perturb memory measurements
        ssh_cmd "nohup $BOARD_CGRD --no-log >'/dev/null' 2>&1 &"
        sleep "$CGRD_HEADSTART"
    else
        log "without_cgrd: waiting $CGRD_HEADSTART s (symmetric stabilization)..."
        sleep "$CGRD_HEADSTART"
    fi

    log "Running test.sh -c on board..."
    TEST_FLAGS=""
    [ "$TIMING_ONLY" -eq 0 ] && TEST_FLAGS="-c"
    [ "$COLLECT_SYSLOG" -eq 1 ] && TEST_FLAGS="$TEST_FLAGS -s"
    [ "$TIMING_ONLY" -eq 1 ]   && TEST_FLAGS="$TEST_FLAGS -t"
    ssh_cmd "cd '$BOARD_PROFILE_DIR' && sh script/test.sh $TEST_FLAGS >/tmp/test_run.log 2>&1"
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

# Initial clean reboot to ensure a known-good board state
log "Initial reboot to ensure clean board state..."
wait_for_ssh   # verify board is reachable first
ssh_cmd "reboot" 2>/dev/null || true
wait_after_reboot

# Deploy latest tools and verify version
deploy_tools
verify_version

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

# ── auto-generate memory/cgroup plots if profiling was collected ──────────────
if [ "$TIMING_ONLY" -eq 0 ]; then
    MEMINFO_PY="$TOOLS_DIR/meminfo_plot.py"
    CGROUP_PY="$TOOLS_DIR/cgroup_plot.py"
    REPORT_DIR="$TOOLS_DIR/report/$SESSION"
    mkdir -p "$REPORT_DIR"

    if [ -f "$MEMINFO_PY" ] && command -v python3 >/dev/null 2>&1; then
        log "Generating meminfo comparison plot..."
        if [ -d "$DATA_DIR/with_cgrd" ] && [ -d "$DATA_DIR/without_cgrd" ]; then
            python3 "$MEMINFO_PY" \
                --with-cgrd    "$DATA_DIR/with_cgrd" \
                --without-cgrd "$DATA_DIR/without_cgrd" \
                --out "$REPORT_DIR/meminfo_comparison.png" \
                && log "Saved: tools/report/$SESSION/meminfo_comparison.png" \
                || log "WARN: meminfo_plot.py failed (non-fatal)"
        else
            log "WARN: no meminfo data dirs found"
        fi
    fi

    if [ -f "$CGROUP_PY" ] && command -v python3 >/dev/null 2>&1; then
        log "Generating cgroup comparison plot..."
        if [ -d "$DATA_DIR/with_cgrd" ] && [ -d "$DATA_DIR/without_cgrd" ]; then
            python3 "$CGROUP_PY" \
                --with-cgrd    "$DATA_DIR/with_cgrd" \
                --without-cgrd "$DATA_DIR/without_cgrd" \
                --out "$REPORT_DIR/cgroup_comparison.png" \
                && log "Saved: tools/report/$SESSION/cgroup_comparison.png" \
                || log "WARN: cgroup_plot.py failed (non-fatal)"
        else
            log "WARN: no cgroup data dirs found"
        fi
    fi
fi
if [ "$COLLECT_SYSLOG" -eq 1 ]; then
    TIMELINE_PY="$TOOLS_DIR/syslog/log_timeline.py"
    if [ -f "$TIMELINE_PY" ] && command -v python3 >/dev/null 2>&1; then
        log "Generating syslog timeline comparison..."
        python3 "$TIMELINE_PY" --session "$SESSION" \
            && log "Timeline PNG saved to tools/report/$SESSION/timeline_comparison.png" \
            || log "WARN: log_timeline.py --session failed (non-fatal)"

        log "Generating syslog stats comparison..."
        python3 "$TIMELINE_PY" --session "$SESSION" --stats \
            && log "Stats PNG + report saved to tools/report/$SESSION/" \
            || log "WARN: log_timeline.py --stats failed (non-fatal)"
    fi
fi

# ── auto-upload to Confluence if credentials are set ─────────────────────────
CONFLUENCE_PY="$TOOLS_DIR/analyze/confluence_upload.py"
if [ -f "$CONFLUENCE_PY" ] && command -v python3 >/dev/null 2>&1 \
   && [ -n "$CONFLUENCE_USER" ] && [ -n "$CONFLUENCE_PASS" ]; then
    log "Uploading analysis to Confluence..."
    CONF_PARENT="${CONFLUENCE_PARENT_PAGE_ID:-3614431987}"
    python3 "$CONFLUENCE_PY" \
        --user "$CONFLUENCE_USER" \
        --password "$CONFLUENCE_PASS" \
        --session "$SESSION" \
        --parent-page-id "$CONF_PARENT" \
        --report-dir "$TOOLS_DIR/report" \
        --data-dir "$DATA_DIR" \
        && log "Confluence page created." \
        || log "WARN: Confluence upload failed (non-fatal)"
else
    log "Skipping Confluence upload (set CONFLUENCE_USER / CONFLUENCE_PASS to enable)"
fi
