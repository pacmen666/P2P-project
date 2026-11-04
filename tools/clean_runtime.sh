#!/usr/bin/env bash
# 清理本机运行缓存（不影响源码与 make 产物）。在仓库根目录执行：
#   bash tools/clean_runtime.sh
# 可选保留演示案卷：bash tools/clean_runtime.sh --keep-demo
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

KEEP_DEMO=0
[[ "${1:-}" == "--keep-demo" ]] && KEEP_DEMO=1

# 误建目录名 logs + 行尾 U+000D（Windows/WSL 混用 echo 时偶发）
find . -maxdepth 1 -type d -name $'logs\r' -exec rm -rf {} + 2>/dev/null || true

rm -rf .cache
mkdir -p logs
find logs -mindepth 1 -delete 2>/dev/null || rm -rf logs/* 2>/dev/null || true

if [[ "$KEEP_DEMO" -eq 1 ]]; then
  find . -maxdepth 1 -type d -name 'session_*' ! -name 'session_1e22ca8e' -exec rm -rf {} +
else
  rm -rf session_*/
fi

echo "runtime cache cleared (logs/, .cache/, session_* per mode). Rebuild case: python3 tools/gen_case.py"
