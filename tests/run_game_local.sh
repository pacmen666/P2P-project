#!/bin/bash
# 一键在本地开一局：房间(Tracker) + 房主(加载一局并供块) + 侦探(自动下载) + 刘界面。
# 用法：
#   bash tests/run_game_local.sh                 # 随机生成一局并开局
#   bash tests/run_game_local.sh session_xxxx     # 加载已有 session 目录（如 AI 生图局）
#
# 环境：Ubuntu / WSL2。有 tmux 时开成 4 个窗格并自动 attach；
#       无 tmux 时房间+房主后台启动，侦探/刘界面按提示手动开。
#
# 注意：侦探使用独立缓存目录（.cache/A、.cache/B），不要指向房主的 session_<id>，
#       否则会扫到房主那份完整案卷而“全部已有”、不下发（也演示不了断点续传）。
set -e
cd "$(dirname "$0")/.." || exit 1

pkill -9 -f room_server 2>/dev/null || true
pkill -9 -f client_transfer 2>/dev/null || true
pkill -9 -f './client' 2>/dev/null || true
sleep 0.5

# 1) 确保二进制齐全（主项目默认 make 不含 client_transfer）
[ -x ./room_server ] || make room_server relay_server client test_client
[ -x ./client_transfer ] || make client_transfer
mkdir -p logs
rm -rf .cache

# 2) 案卷：用给定目录，否则随机生成一局
if [ $# -ge 1 ] && [ -d "$1" ]; then
  SESSION="$1"
  echo "加载已有案卷: $SESSION"
else
  LINE=$(python3 tools/gen_case.py 2>&1)
  SESSION=$(echo "$LINE" | grep -oE 'session_[0-9a-f]+' | head -1)
  echo "已随机生成案卷: $SESSION"
fi
[ -n "$SESSION" ] || { echo "无法确定 session 目录，退出"; exit 1; }

# 3) 启动
if command -v tmux >/dev/null 2>&1; then
  tmux kill-session -t frag 2>/dev/null || true
  tmux new-session  -d -s frag -n game "./room_server"
  tmux split-window -t frag -h "./client_transfer --host --session $SESSION --room localhost --nick host"
  tmux split-window -t frag -v "./client_transfer --room localhost --nick A --auto --data-dir .cache/A"
  tmux split-window -t frag -v "./client"
  tmux select-layout -t frag tiled
  echo "已用 tmux 开好 4 个窗格（会话名 frag）。"
  echo "  - 房主窗：自动进房并上传清单"
  echo "  - 侦探A窗：--auto 自动进房、逐块下载校验，块落 .cache/A"
  echo "  - 刘界面窗：输入  join 侦探 localhost  进房看板"
  echo "断点续传演示：在侦探A窗 Ctrl-C 杀掉，另开终端重跑"
  echo "  ./client_transfer --room localhost --nick A --auto --data-dir .cache/A"
  echo "  重启应打印 RESUME_BLOCK（复用已完成块），只补缺失块。"
  echo "退出整局： tmux kill-session -t frag"
  tmux attach -t frag
else
  echo "未检测到 tmux，退化为后台启动（房间 + 房主 + 侦探A），跑完自动统计生图。"
  ./room_server >logs/room_run.log 2>&1 &
  ROOM_PID=$!
  ./client_transfer --host --session "$SESSION" --room localhost --nick host --run-seconds 33 >logs/host_run.log 2>&1 &
  HOST_PID=$!
  ./client_transfer --room localhost --nick A --auto --data-dir .cache/A --run-seconds 30 >logs/detectiveA.log 2>&1 &
  DET_PID=$!
  echo "三端已在后台运行，侦探A 30 秒后自动退出并打印 TRANSFER_STATS。"
  echo "看日志： tail -f logs/room_run.log logs/host_run.log logs/detectiveA.log"
  echo "另可手动开刘界面： ./client   （然后输入 join 侦探 localhost）"
  wait "$DET_PID"
  wait "$HOST_PID"
  kill "$ROOM_PID" 2>/dev/null || true
  # 统计生图：解析 logs/*.log 里的 TRANSFER_STATS 行（房主/其他节点上传、中继字节、完成时间、校验结果）
  python3 tests/plot_stats.py --dir logs --out logs/stats.png --title "Local game transfer stats" || true
  echo "统计图已生成： logs/stats.png"
fi
