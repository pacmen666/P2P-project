#!/usr/bin/env bash
# 端到端联调：验证 C 核心运行时，Web 案情板能读到真实状态（logs/ui_state.json）。
# 用法（在 WSL Ubuntu 内）：bash tests/e2e_ui_link.sh
set -u
cd "$(dirname "$0")/.."

echo "[1/4] 清理旧进程与产物"
pkill -f 'room_server' 2>/dev/null
pkill -f 'relay_server' 2>/dev/null
pkill -f '[.]/client' 2>/dev/null
sleep 1
rm -f logs/ui_state.json logs/room_server.log logs/relay_server.log

echo "[2/4] 启动 room_server + relay_server"
./room_server > logs/room_server.log 2>&1 &
ROOM_PID=$!
./relay_server > logs/relay_server.log 2>&1 &
RELAY_PID=$!
sleep 1

echo "[3/4] 房主进房（host），侦探进房（玩家）"
# 房主 JSON 写到 /tmp，避免覆盖侦探视角（Web 轮询的是侦探的 logs/ui_state.json）
( echo "join 房主 localhost host"; sleep 8; echo quit ) \
    | UI_JSON_PATH=/tmp/host_ui.json ./client > /dev/null 2>&1 &
HOST_PID=$!
sleep 2
( echo "join 侦探 localhost"; sleep 6; echo quit ) \
    | ./client > /dev/null 2>&1 &
DET_PID=$!

sleep 4

echo "[4/4] 侦探视角 logs/ui_state.json（Web 案情板每秒轮询的就是它）"
cat logs/ui_state.json

echo ""
echo "--- 房主视角 /tmp/host_ui.json ---"
cat /tmp/host_ui.json 2>/dev/null

kill "$ROOM_PID" "$RELAY_PID" 2>/dev/null
wait 2>/dev/null
echo ""
echo "[done] room_server / relay_server 已停止"
