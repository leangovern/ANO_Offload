#!/bin/bash
# run_direct.sh — v5 联跑编排裸跑变体（软 TGT 自验环境干净性用）
# 编排逻辑与在案 full_pair.sh 一致：tgt 后台先起 → wn 前台连发 → 等 tgt 完成 → 收尾。
# 用法：TASK_DEVICE=<卡号> bash scripts/run_direct.sh [N发数]
set -u
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh

D="$TASK_DEVICE"
N="${1:-1}"
rm -f /tmp/hcomm_srv_* /tmp/hcomm_cli_* "$ANO_STOPF" "$ANO_FIRE_DONE" "$ANO_TGT_DONE"

echo "[run] device=$D iters=$N 启动 tgt_wn wnrecv（后台）"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/tgt_wn" wnrecv "$D" 0 "$D" "$N" > "$ANO_TGT_LOG" 2>&1 &
sleep 8
echo "[run] wn_full wnfire ×$N（前台，mode4 全链一体）"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/wn_full" wnfire "$D" 0 "$D" "$N"
CLI_RC=$?
for i in $(seq 1 240); do [ -f "$ANO_TGT_DONE" ] && break; sleep 1; done
sleep 2
touch "$ANO_STOPF"
wait 2>/dev/null
echo "[run] wn_full_rc=$CLI_RC"
grep -E "wnrecv" "$ANO_TGT_LOG" | head -12
