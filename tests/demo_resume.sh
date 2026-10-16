#!/bin/bash
# 自动演示陈第三周任务之「断点续传」：接收端重启后继续使用已完成分块。
# 流程：起房间+房主 → 侦探限速下载 1.x 秒 → 杀掉 → 同 --data-dir 重启 → 比对两次日志。
# 预期：重启日志出现 RESUME_BLOCK（复用已完成块），且 PIECE_OK 数远小于第一次（不必重传）。
cd "$(dirname "$0")/.." || exit 1
pkill -9 -f room_server 2>/dev/null || true
pkill -9 -f client_transfer 2>/dev/null || true
sleep 0.5
[ -x ./room_server ] || make room_server client test_client
[ -x ./client_transfer ] || make client_transfer
mkdir -p logs
SESSION=$(python3 tools/gen_case.py 2>&1 | grep -oE 'session_[0-9a-f]+' | head -1)
CACHE=.resume_demo
rm -rf "$CACHE"

./room_server >logs/room_run.log 2>&1 &
RP=$!
./client_transfer --host --session "$SESSION" --room localhost --nick host >logs/host_run.log 2>&1 &
sleep 1

echo "=== 第一次下载（--delay-ms 200 限速，1.2 秒后杀掉）==="
./client_transfer --room localhost --nick A --auto --data-dir "$CACHE" --delay-ms 200 >logs/detective1.log 2>&1 &
DP=$!
sleep 1.2
kill -9 $DP 2>/dev/null || true
echo "已杀死侦探；已完成块落在 $CACHE"

echo "=== 重启侦探（同 --data-dir，应只补缺失块）==="
./client_transfer --room localhost --nick A --auto --data-dir "$CACHE" --delay-ms 200 >logs/detective2.log 2>&1

echo
echo "--- 第一次日志 PIECE_OK 数（实际下载块数）---"; grep -c PIECE_OK logs/detective1.log || true
echo "--- 重启日志 RESUME_BLOCK 数（应 >0，证明复用已完成块）---"; grep -c RESUME_BLOCK logs/detective2.log || true
echo "--- 重启日志 PIECE_OK 数（应明显小于第一次，证明不必重传）---"; grep -c PIECE_OK logs/detective2.log || true

kill -9 $RP 2>/dev/null || true
pkill -9 -f client_transfer 2>/dev/null || true
echo "演示结束。完整日志： logs/detective1.log / logs/detective2.log"
