#!/bin/bash
# 任务 43：全链一体复验对（tgt_wn=ano_wn 封存件零改动 + wn_full mode4）
C=/usr/local/Ascend/cann-9.3.0
NB=/data/disk2/pyptouser/ssulijiawen/aiv_scratch/taskM42
V=$NB/opp_priv_ano/vendors/customize
export ASCEND_HOME_PATH=$C
export LD_LIBRARY_PATH=$C/aarch64-linux/lib64:$V/op_api/lib:$V/op_impl/ai_core/tbe/op_tiling/lib/linux/aarch64:$V/op_proto/lib/linux/aarch64:$LD_LIBRARY_PATH
export ASCEND_OPP_PATH=$NB/opp_merged_ano
export ASCEND_CUSTOM_OPP_PATH=$V
D=${TASK_DEVICE:-1}
N=${1:-1}
if [ "$D" != "1" ] && [ "$D" != "2" ]; then echo "[wn] 拒绝：device $D 不在白名单"; exit 64; fi
HS=/data/disk2/pyptouser/ssulijiawen/aiv_scratch/taskM43/ano_full
rm -f /tmp/hcomm_srv_* /tmp/hcomm_cli_* /tmp/hcomm_probe_stop_lijiawen /tmp/hcomm_fire_done_lijiawen /tmp/wn_tgt_done_lijiawen
echo "[wn] device=$D iters=$N 启动 tgt_wn wnrecv"
timeout 500 stdbuf -o0 /data/disk2/pyptouser/ssulijiawen/aiv_scratch/taskM42/ano_wn/tgt_wn wnrecv "$D" 0 "$D" "$N" > /tmp/wn_tgt_log_lijiawen.txt 2>&1 &
sleep 8
echo "[wn] 单发/连发 wn_host wnfire ×$N"
timeout 500 stdbuf -o0 $HS/wn_full wnfire "$D" 0 "$D" "$N"
CLI_RC=$?
for i in $(seq 1 240); do [ -f /tmp/wn_tgt_done_lijiawen ] && break; sleep 1; done
sleep 2
touch /tmp/hcomm_probe_stop_lijiawen
wait 2>/dev/null
echo "[wn] wn_host_rc=$CLI_RC"
grep -E "wnrecv" /tmp/wn_tgt_log_lijiawen.txt | head -12
