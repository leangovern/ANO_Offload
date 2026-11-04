#!/bin/bash
# full_pair.sh — v5 包内编排（开发机在案 full_pair.sh 的包内适配版；差异=全部路径指向包内资产）
# 用法：TASK_DEVICE=<卡号> bash scripts/full_pair.sh [N发数]
# 与 run_direct.sh 等价，保留在案命名以便对照；原开发机版（硬编码绝对路径）不入包。
C="$ASCEND_HOME_PATH"
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh
D="$TASK_DEVICE"
N="${1:-1}"
rm -f /tmp/hcomm_srv_* /tmp/hcomm_cli_* "$ANO_STOPF" "$ANO_FIRE_DONE" "$ANO_TGT_DONE"
echo "[wn] device=$D iters=$N 启动 tgt_wn wnrecv"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/tgt_wn" wnrecv "$D" 0 "$D" "$N" > "$ANO_TGT_LOG" 2>&1 &
sleep 8
echo "[wn] 单发/连发 wn_full wnfire ×$N"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/wn_full" wnfire "$D" 0 "$D" "$N"
CLI_RC=$?
for i in $(seq 1 240); do [ -f "$ANO_TGT_DONE" ] && break; sleep 1; done
sleep 2
touch "$ANO_STOPF"
wait 2>/dev/null
echo "[wn] wn_full_rc=$CLI_RC"
grep -E "wnrecv" "$ANO_TGT_LOG" | head -12
