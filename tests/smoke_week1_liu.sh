#!/bin/bash
set -e
cd "$(dirname "$0")/.."
mkdir -p logs
make room_server client
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
printf 'join detA localhost\nprobe\nredraw\nquit\n' | ./client >logs/client_run.log 2>&1 || true
sleep 1
kill "$RP" 2>/dev/null || true
wait "$RP" 2>/dev/null || true
echo '--- client.log ---'
tail -40 logs/client.log || true
echo '--- room.log ---'
tail -40 logs/room.log || true
