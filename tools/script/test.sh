#!/bin/sh
# TOOLS_VERSION=4
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR/.."

# ── prerequisite check ────────────────────────────────────────────────────────
check_prereqs() {
    FAILED=0
    if [ "$USE_MEMINFO" -eq 1 ] && [ ! -f "$TOOLS_DIR/meminfo_sampler.sh" ]; then
        printf "ERROR: required file not found: %s\n" "$TOOLS_DIR/meminfo_sampler.sh" >&2
        FAILED=1
    fi
    if [ "$USE_CGROUP" -eq 1 ]; then
        if [ ! -f "$TOOLS_DIR/cgroup_sampler.py" ]; then
            printf "ERROR: required file not found: %s\n" "$TOOLS_DIR/cgroup_sampler.py" >&2
            FAILED=1
        fi
        if ! command -v python3 >/dev/null 2>&1; then
            printf "ERROR: python3 not found (required for -c/--cgroup)\n" >&2
            FAILED=1
        fi
    fi
    if [ "$USE_SYSLOG" -eq 1 ]; then
        if [ ! -f "$TOOLS_DIR/syslog/syslog_collector.sh" ]; then
            printf "ERROR: syslog_collector.sh not found: %s\n" "$TOOLS_DIR/syslog/syslog_collector.sh" >&2
            FAILED=1
        fi
        if [ ! -r "$SYSLOG_SRC" ]; then
            printf "WARN: %s not readable — syslog collection may fail\n" "$SYSLOG_SRC" >&2
        fi
    fi
    if ! command -v luna-send >/dev/null 2>&1; then
        printf "ERROR: luna-send not found — are you running on a webOS board?\n" >&2
        FAILED=1
    fi
    [ "$FAILED" -eq 1 ] && exit 1
}

USE_MEMINFO=1
USE_CGROUP=0
USE_SYSLOG=0
SYSLOG_SRC="/var/log/messages"
SYSLOG_PID=""
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--cgroup)       USE_CGROUP=1; shift ;;
        -s|--syslog)       USE_SYSLOG=1; shift ;;
        -t|--timing-only)  USE_MEMINFO=0; USE_CGROUP=0; shift ;;
        -h|--help)
            printf "Usage: %s [-c|--cgroup] [-s|--syslog] [-t|--timing-only]\n" "$0"
            printf "  -c, --cgroup        Also run cgroup_sampler.py (saves to log/cgroup_sampler/)\n"
            printf "  -s, --syslog        Collect /var/log/messages during YouTube→Netflix transition\n"
            printf "  -t, --timing-only   Skip meminfo + cgroup profiling (syslog timing only)\n"
            exit 0
            ;;
        *) printf "Unknown option: %s\n" "$1" >&2; exit 1 ;;
    esac
done

check_prereqs

# Start memory profiler in background; it will write a phase-tagged CSV
if [ "$USE_MEMINFO" -eq 1 ]; then
    "$TOOLS_DIR/meminfo_sampler.sh" &
    SAMPLER_PID=$!
    printf "meminfo_sampler started (PID %s)\n" "$SAMPLER_PID"
fi

if [ "$USE_CGROUP" -eq 1 ]; then
    python3 "$TOOLS_DIR/cgroup_sampler.py" --outdir "$TOOLS_DIR/log/cgroup_sampler" &
    CGROUP_PID=$!
    # Give it a moment to start; if it exits immediately python3/cgroup not available
    sleep 1
    if ! kill -0 "$CGROUP_PID" 2>/dev/null; then
        printf "WARN: cgroup_sampler failed to start (python3 or cgroup not available)\n" >&2
        CGROUP_PID=""
    else
        printf "cgroup_sampler started (PID %s)\n" "$CGROUP_PID"
    fi
fi

if [ "$USE_SYSLOG" -eq 1 ]; then
    sh "$TOOLS_DIR/syslog/syslog_collector.sh" &
    SYSLOG_PID=$!
    sleep 1
    if ! kill -0 "$SYSLOG_PID" 2>/dev/null; then
        printf "WARN: syslog_collector failed to start\n" >&2
        SYSLOG_PID=""
    else
        printf "syslog_collector started (PID %s)\n" "$SYSLOG_PID"
    fi
fi

luna-send -n 1 -f luna://com.webos.service.preloadmanager/setPreloadPolicy '{"application" : {"id": "netflix","isEnabled" : false}, "permanent" : true}'

sleep 3

luna-send -n 1 -f luna://com.webos.applicationManager/closeAllApps '{}'

sleep 3

# phase 1: youtube launching
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{ "id":"youtube.leanback.v4", "params" : { "contentTarget": "https://www.youtube.com/tv#/browse-sets?v=HmZKgaHa3Fg&resume"} }'

sleep 70

# YouTube phase ended — signal profilers to advance to phase 2
if [ -n "$SAMPLER_PID" ]; then
    kill -USR1 "$SAMPLER_PID"
fi
# Signal syslog_collector: YouTube→Netflix transition starts NOW
if [ -n "$SYSLOG_PID" ]; then
    kill -USR1 "$SYSLOG_PID" 2>/dev/null
fi

# phase 2: netflix execution
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"netflix"}'

sleep 30

# Stop profilers — use TERM (more reliable on busybox ash than INT)
if [ -n "$SAMPLER_PID" ]; then
    kill -TERM "$SAMPLER_PID" 2>/dev/null
    wait "$SAMPLER_PID" 2>/dev/null
fi
if [ -n "$CGROUP_PID" ]; then
    kill -TERM "$CGROUP_PID" 2>/dev/null
    wait "$CGROUP_PID" 2>/dev/null
fi
if [ -n "$SYSLOG_PID" ]; then
    kill -TERM "$SYSLOG_PID" 2>/dev/null
    wait "$SYSLOG_PID" 2>/dev/null
fi
printf "Done.\n"

