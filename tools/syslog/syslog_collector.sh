#!/bin/sh
# TOOLS_VERSION=4
# syslog_collector.sh — /var/log/messages 전환 구간 수집기 (board-side, busybox ash)
#
# 동작:
#   YouTube → Netflix 전환 시점(SIGUSR1)부터 종료(SIGTERM)까지
#   /var/log/messages 를 tail -f 로 수집.
#   webOS syslogd 는 파일 크기 초과 시 rotate 하므로, 2초마다 파일 크기를 감시해
#   축소(rotate) 감지 시 tail 을 재시작하여 로그 단절 없이 수집.
#
# 사용법 (test.sh --syslog 에서 호출):
#   syslog_collector.sh &
#   SYSLOG_PID=$!
#   ... (YouTube 실행 중) ...
#   kill -USR1 $SYSLOG_PID    # 전환 구간 수집 시작 마킹
#   ... (Netflix 실행 중) ...
#   kill -TERM $SYSLOG_PID    # 수집 종료

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/../log/syslog"
SYSLOG="/var/log/messages"
OUTFILE=""
COLLECTING=0
STOP=0
READER_PID=""
ROTATE_POLL_SEC=2   # interval (s) to check for log rotation

# ── signal handlers ──────────────────────────────────────────────────────────
handle_usr1() {
    COLLECTING=1
    TS="$(date '+%Y%m%d_%H%M%S')"
    OUTFILE="$LOGDIR/syslog_${TS}.log"
    printf "[syslog_collector] transition start at %s -> %s\n" "$TS" "$OUTFILE" >&2
    printf "# syslog_collector: transition_start=%s\n" "$TS" >> "$OUTFILE"
}

handle_term() {
    STOP=1
}

trap 'handle_usr1' USR1
trap 'handle_term' TERM INT

# ── setup ────────────────────────────────────────────────────────────────────
mkdir -p "$LOGDIR"

PREBUF_LINES=50

printf "[syslog_collector] started (PID $$), waiting for SIGUSR1 (transition marker)\n" >&2

# ── rotation-aware tail ───────────────────────────────────────────────────────
# Starts tail -f on $SYSLOG, polls every ROTATE_POLL_SEC seconds for rotation
# (file shrinks = rotated). Restarts tail on new file transparently.
start_tail() {
    tail -n 0 -f "$SYSLOG" >> "$OUTFILE" 2>/dev/null &
    READER_PID=$!
}

# ── main loop ────────────────────────────────────────────────────────────────
while [ "$STOP" -eq 0 ]; do
    if [ "$COLLECTING" -eq 1 ] && [ -n "$OUTFILE" ]; then
        # Save pre-transition context (snapshot, not affected by rotation)
        printf "# === pre-transition context (last %d lines) ===\n" "$PREBUF_LINES" >> "$OUTFILE"
        tail -n "$PREBUF_LINES" "$SYSLOG" >> "$OUTFILE" 2>/dev/null
        printf "# === live collection start ===\n" >> "$OUTFILE"

        start_tail
        PREV_SIZE=$(wc -c < "$SYSLOG" 2>/dev/null || echo 0)

        while [ "$STOP" -eq 0 ]; do
            sleep "$ROTATE_POLL_SEC" &
            wait $!
            [ "$STOP" -eq 1 ] && break

            CUR_SIZE=$(wc -c < "$SYSLOG" 2>/dev/null || echo 0)
            if [ "$CUR_SIZE" -lt "$PREV_SIZE" ]; then
                # /var/log/messages was rotated — restart tail on new file
                kill "$READER_PID" 2>/dev/null
                wait "$READER_PID" 2>/dev/null
                printf "# [syslog_collector] rotation detected, restarting tail\n" >> "$OUTFILE"
                start_tail
            fi
            PREV_SIZE="$CUR_SIZE"
        done

        if [ -n "$READER_PID" ]; then
            kill "$READER_PID" 2>/dev/null
            wait "$READER_PID" 2>/dev/null
        fi

        TS_END="$(date '+%Y%m%d_%H%M%S')"
        printf "# syslog_collector: collection_end=%s\n" "$TS_END" >> "$OUTFILE"
        printf "[syslog_collector] collection ended at %s\n" "$TS_END" >&2
        break
    fi

    sleep 1 &
    wait $!
done

printf "[syslog_collector] exiting.\n" >&2
