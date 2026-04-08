#!/bin/sh
# /proc/meminfo sampler — TOOLS_VERSION=2
# ----------------------------------------
# Continuously samples MemFree, MemAvailable, Swap, and Anon/File page stats
# from /proc/meminfo.  Press Ctrl+C (or send SIGTERM) to stop — collected
# data is saved to a CSV and plotted with gnuplot automatically if available.
#
# Usage: meminfo_sampler.sh [-i SECONDS]

TOOLS_VERSION=2
INTERVAL=1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/log/meminfo_sampler"
mkdir -p "$LOGDIR"
OUTFILE="$LOGDIR/meminfo_samples_$$.csv"

usage() {
    printf "Usage: %s [-i SECONDS]\n" "$0"
    printf "  -i SECONDS  Sampling interval in seconds (default: 1)\n"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -i|--interval)
            INTERVAL="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            printf "Unknown option: %s\n" "$1" >&2
            usage
            ;;
    esac
done

printf "timestamp,phase,mem_free_kb,mem_available_kb,swap_free_kb,swap_total_kb,active_anon_kb,inactive_anon_kb,active_file_kb,inactive_file_kb\n" > "$OUTFILE"

STOP=0
PHASE=1
trap 'STOP=1' INT TERM
# SIGUSR1: advance to next phase (e.g. YouTube→Netflix boundary)
trap 'PHASE=$((PHASE+1)); printf "\n[phase] → phase %s\n" "$PHASE" >&2' USR1
printf "Sampling /proc/meminfo every %ss — press Ctrl+C to stop and save CSV.\n" "$INTERVAL"

while [ "$STOP" -eq 0 ]; do
    NOW=$(date '+%H:%M:%S')

    # Parse all fields in a single pass
    set -- $(awk '
        /^MemFree:/         { f=$2 }
        /^MemAvailable:/    { ma=$2 }
        /^SwapFree:/        { sf=$2 }
        /^SwapTotal:/       { st=$2 }
        /^Active\(anon\):/  { aa=$2 }
        /^Inactive\(anon\):/{ ia=$2 }
        /^Active\(file\):/  { af=$2 }
        /^Inactive\(file\):/{ iff=$2 }
        END { print f+0, ma+0, sf+0, st+0, aa+0, ia+0, af+0, iff+0 }
    ' /proc/meminfo)
    MEM_FREE=$1
    MEM_AVAIL=$2
    SWAP_FREE=$3
    SWAP_TOTAL=$4
    ACTIVE_ANON=$5
    INACTIVE_ANON=$6
    ACTIVE_FILE=$7
    INACTIVE_FILE=$8

    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
        "$NOW" "$PHASE" "$MEM_FREE" "$MEM_AVAIL" "$SWAP_FREE" "$SWAP_TOTAL" \
        "$ACTIVE_ANON" "$INACTIVE_ANON" "$ACTIVE_FILE" "$INACTIVE_FILE" >> "$OUTFILE"

    printf "[%s] phase=%s  MemFree: %7.1f MB  MemAvail: %7.1f MB  SwapUsed: %7.1f MB  Anon(A/I): %6.1f/%6.1f MB\n" \
        "$NOW" "$PHASE" \
        "$(awk "BEGIN {printf \"%.1f\", $MEM_FREE/1024}")" \
        "$(awk "BEGIN {printf \"%.1f\", $MEM_AVAIL/1024}")" \
        "$(awk "BEGIN {printf \"%.1f\", ($SWAP_TOTAL-$SWAP_FREE)/1024}")" \
        "$(awk "BEGIN {printf \"%.1f\", $ACTIVE_ANON/1024}")" \
        "$(awk "BEGIN {printf \"%.1f\", $INACTIVE_ANON/1024}")"

    sleep "$INTERVAL" &
    wait $!
done

printf "\nSampling stopped.\n"

N=$(awk 'NR>1 {n++} END {print n+0}' "$OUTFILE")

if [ "$N" -lt 2 ]; then
    printf "Need at least 2 samples to plot. Data saved to %s\n" "$OUTFILE"
    exit 0
fi

printf "Saved %d samples to %s\n" "$N" "$OUTFILE"

if command -v gnuplot >/dev/null 2>&1; then
    printf "Plotting with gnuplot...\n"
    gnuplot -persist <<EOF
set datafile separator ","
set xdata time
set timefmt "%H:%M:%S"
set format x "%H:%M:%S"
set title "/proc/meminfo — Memory Sampling"
set xlabel "Time"
set grid
set key top right

set multiplot layout 4,1 title "/proc/meminfo — Memory Sampling"

set ylabel "Free Memory (MB)"
set title "MemFree / MemAvailable"
plot "$OUTFILE" using 1:(\$3/1024) with lines lw 2 lc rgb "steelblue" title "MemFree", \
     "$OUTFILE" using 1:(\$4/1024) with lines lw 2 lc rgb "orange"    title "MemAvailable"

set ylabel "Swap (MB)"
set title "Swap"
plot "$OUTFILE" using 1:(\$5/1024) with lines lw 2 lc rgb "mediumseagreen" title "SwapFree", \
     "$OUTFILE" using 1:((\$6-\$5)/1024) with lines lw 2 lc rgb "tomato" title "SwapUsed"

set ylabel "Anon Pages (MB)"
set title "Active / Inactive Anon"
plot "$OUTFILE" using 1:(\$7/1024) with lines lw 2 lc rgb "violet"      title "Active(anon)", \
     "$OUTFILE" using 1:(\$8/1024) with lines lw 2 lc rgb "mediumpurple" title "Inactive(anon)"

set ylabel "File Cache (MB)"
set title "Active / Inactive File Cache"
plot "$OUTFILE" using 1:(\$9/1024) with lines lw 2 lc rgb "gold"   title "Active(file)", \
     "$OUTFILE" using 1:(\$10/1024) with lines lw 2 lc rgb "khaki"  title "Inactive(file)"

unset multiplot
EOF
else
    printf "gnuplot not found — data saved to %s\n" "$OUTFILE"
    printf "Plot with: gnuplot, python, or any CSV viewer.\n"
fi

printf "CSV saved to %s\n" "$OUTFILE"
