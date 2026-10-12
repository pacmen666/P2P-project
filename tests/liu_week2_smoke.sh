#!/bin/bash
set -e
cd "$(dirname "$0")/.." || exit 1
mkdir -p logs
make room_server client
pkill -9 -f room_server 2>/dev/null || true
pkill -9 -f './client' 2>/dev/null || true
sleep 0.5
: > logs/client.log
./room_server >logs/room_run.log 2>&1 &
RP=$!
sleep 1
(
  sleep 0.2
  echo 'join 侦探 localhost'
  sleep 3
  echo 'markhave 1 测试源'
  sleep 0.5
  echo 'redraw'
  sleep 0.5
  echo 'quit'
) | ./client >logs/client_run.log 2>&1
kill -9 "$RP" 2>/dev/null || true
grep -qE 'PROBE_ACK|PEER_LIST' logs/client.log logs/client_run.log
grep -q '已有' logs/client_run.log
echo 'liu_week2_smoke: OK'
