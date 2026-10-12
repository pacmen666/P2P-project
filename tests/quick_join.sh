#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1
pkill -9 -f room_server || true
pkill -9 -f './client' || true
sleep 0.5
fuser -k 37401/tcp 37402/udp 2>/dev/null || true
sleep 0.3
make room_server client
mkdir -p logs
: > logs/client.log
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
( sleep 0.2; echo 'join detA localhost'; sleep 2; echo quit ) | ./client >logs/client_run.log 2>&1
kill -9 $RP 2>/dev/null || true
echo '--- client.log ---'
cat logs/client.log
echo '--- room ---'
tail -8 logs/room.log