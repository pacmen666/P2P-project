#!/bin/bash
cd /mnt/d/Coding_Project/CS374/Project || exit 1
pkill -9 -f room_server || true
sleep 0.3
gcc -Wall -g -Iinclude -o room_server src/tracker/room_server.c src/common/log.c
gcc -Wall -g -Iinclude -o tests/probe_client_min tests/probe_client_min.c src/punch/udp_sock.c src/common/log.c
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
./tests/probe_client_min
EC=$?
kill -9 $RP 2>/dev/null || true
exit $EC