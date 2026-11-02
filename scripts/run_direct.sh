#!/bin/bash
# run_direct.sh — ANO 联跑编排裸跑变体（无 task-submit/taskqueue 环境用）
# 编排逻辑与 scripts/ano_pair.sh（在案原样存档件）一致：tgt 后台先起 → wn 前台连发 → 等 tgt 完成 → 收尾。
# 用法：bash scripts/run_direct.sh [N发数]     （设备用 TASK_DEVICE 环境变量选择，默认 1）
set -u
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh

D="$TASK_DEVICE"
N="${1:-1}"
if [ "$D" != "1" ] && [ "$D" != "2" ]; then echo "[run] 拒绝：device $D 不在白名单（1/2）；新机器请确认卡号后用 TASK_DEVICE 覆盖并同步修改本白名单"; exit 64; fi

rm -f /tmp/hcomm_srv_* /tmp/hcomm_cli_* "$ANO_STOPF" "$ANO_FIRE_DONE" "$ANO_TGT_DONE"

echo "[run] device=$D iters=$N 启动 tgt_wn wnrecv（后台）"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/tgt_wn" wnrecv "$D" 0 "$D" "$N" > "$ANO_TGT_LOG" 2>&1 &
sleep 8
echo "[run] wn_host wnfire ×$N（前台）"
timeout 500 stdbuf -o0 "$PKG_ROOT/bin/wn_host" wnfire "$D" 0 "$D" "$N"
CLI_RC=$?
for i in $(seq 1 240); do [ -f "$ANO_TGT_DONE" ] && break; sleep 1; done
sleep 2
touch "$ANO_STOPF"
wait 2>/dev/null
echo "[run] wn_host_rc=$CLI_RC"
grep -E "wnrecv" "$ANO_TGT_LOG" | head -12
