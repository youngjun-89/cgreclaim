#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Start memory profiler in background; it will write a phase-tagged CSV
"$SCRIPT_DIR/meminfo_sampler.sh" &
SAMPLER_PID=$!
printf "meminfo_sampler started (PID %s)\n" "$SAMPLER_PID"

luna-send -n 1 -f luna://com.webos.service.preloadmanager/setPreloadPolicy '{"application" : {"id": "netflix","isEnabled" : false}, "permanent" : true}'

sleep 3

luna-send -n 1 -f luna://com.webos.applicationManager/closeAllApps '{}'

sleep 3

# phase 1: youtube launching
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{ "id":"youtube.leanback.v4", "params" : { "contentTarget": "https://www.youtube.com/tv#/browse-sets?v=HmZKgaHa3Fg&resume"} }'

sleep 80

# YouTube phase ended — signal profiler to advance to phase 2
kill -USR1 "$SAMPLER_PID"

# phase 2: netflix execution
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"netflix"}'

sleep 20

# Stop profiler → triggers CSV save + scp
kill -INT "$SAMPLER_PID"
wait "$SAMPLER_PID"
printf "Done.\n"
