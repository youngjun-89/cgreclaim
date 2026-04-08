#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$SCRIPT_DIR/.."

# ── prerequisite check ────────────────────────────────────────────────────────
check_prereqs() {
    FAILED=0
    for f in "$TOOLS_DIR/meminfo_sampler.sh" "$TOOLS_DIR/cgroup_sampler.py"; do
        if [ ! -f "$f" ]; then
            printf "ERROR: required file not found: %s\n" "$f" >&2
            FAILED=1
        fi
    done
    if [ "$USE_CGROUP" -eq 1 ] && ! command -v python3 >/dev/null 2>&1; then
        printf "ERROR: python3 not found (required for -c/--cgroup)\n" >&2
        FAILED=1
    fi
    if ! command -v luna-send >/dev/null 2>&1; then
        printf "ERROR: luna-send not found — are you running on a webOS board?\n" >&2
        FAILED=1
    fi
    [ "$FAILED" -eq 1 ] && exit 1
}

USE_CGROUP=0
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--cgroup) USE_CGROUP=1; shift ;;
        -h|--help)
            printf "Usage: %s [-c|--cgroup]\n" "$0"
            printf "  -c, --cgroup   Also run cgroup_sampler.py (saves to log/cgroup_sampler/)\n"
            exit 0
            ;;
        *) printf "Unknown option: %s\n" "$1" >&2; exit 1 ;;
    esac
done

check_prereqs

# Start memory profiler in background; it will write a phase-tagged CSV
"$TOOLS_DIR/meminfo_sampler.sh" &
SAMPLER_PID=$!
printf "meminfo_sampler started (PID %s)\n" "$SAMPLER_PID"

if [ "$USE_CGROUP" -eq 1 ]; then
    python3 "$TOOLS_DIR/cgroup_sampler.py" --outdir "$TOOLS_DIR/log/cgroup_sampler" &
    CGROUP_PID=$!
    printf "cgroup_sampler started (PID %s)\n" "$CGROUP_PID"
fi

luna-send -n 1 -f luna://com.webos.service.preloadmanager/setPreloadPolicy '{"application" : {"id": "netflix","isEnabled" : false}, "permanent" : true}'

sleep 3

luna-send -n 1 -f luna://com.webos.applicationManager/closeAllApps '{}'

sleep 3

# phase 1: youtube launching
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{ "id":"youtube.leanback.v4", "params" : { "contentTarget": "https://www.youtube.com/tv#/browse-sets?v=HmZKgaHa3Fg&resume"} }'

sleep 70

# YouTube phase ended — signal profiler to advance to phase 2
kill -USR1 "$SAMPLER_PID"

# phase 2: netflix execution
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"netflix"}'

sleep 15

# Stop profiler → triggers CSV save + scp
kill -INT "$SAMPLER_PID"
if [ "$USE_CGROUP" -eq 1 ]; then
    kill -INT "$CGROUP_PID"
    wait "$CGROUP_PID"
fi
wait "$SAMPLER_PID"
printf "Done.\n"

