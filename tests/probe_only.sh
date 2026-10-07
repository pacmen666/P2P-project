#!/bin/bash
cd /mnt/d/Coding_Project/CS374/Project || exit 1
pkill -9 -f room_server || true
sleep 0.5
gcc -Wall -g -Iinclude -o room_server src/tracker/room_server.c src/common/log.c || exit 1
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
python3 tests/probe_ack_check.py
EC=$?
kill -9 $RP 2>/dev/null || true
exit $EC