#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1
pkill -9 -f room_server || true
sleep 0.5
make room_server
mkdir -p logs
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
python3 tests/probe_ack_check.py
EC=$?
kill -9 $RP 2>/dev/null || true
exit $EC