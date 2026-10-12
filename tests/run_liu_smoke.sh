#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1
pkill -f './room_server' 2>/dev/null || true
sleep 0.3
make room_server client
mkdir -p logs
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
( sleep 0.2; echo 'join detA localhost'; sleep 1.2; echo probe; sleep 1.2; echo quit ) | ./client >logs/client_run.log 2>&1
kill $RP 2>/dev/null
wait $RP 2>/dev/null
echo DONE
tail -20 logs/client.log
