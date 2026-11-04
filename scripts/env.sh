#!/bin/bash
# env.sh — ano_offload_pkg v5 环境变量集中（被 build.sh / run_direct.sh / deploy_check.sh source）
PKG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PKG_ROOT
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.3.0}"
export PKG_VENDOR="$PKG_ROOT/vendor/customize"
export ASCEND_CUSTOM_OPP_PATH="$PKG_VENDOR"
export ASCEND_OPP_PATH="${ASCEND_OPP_PATH:-$PKG_ROOT/opp_merged}"
export LD_LIBRARY_PATH="$ASCEND_HOME_PATH/aarch64-linux/lib64:$PKG_VENDOR/op_api/lib:$PKG_VENDOR/op_impl/ai_core/tbe/op_tiling/lib/linux/aarch64:$PKG_VENDOR/op_proto/lib/linux/aarch64:$LD_LIBRARY_PATH"
# 设备号新环境须人工确认后覆盖（本包不预设白名单卡号）
export TASK_DEVICE="${TASK_DEVICE:-0}"
# 联跑 /tmp 标志文件（二进制内硬编码路径的变量引用）
export ANO_STOPF="/tmp/hcomm_probe_stop_lijiawen"
export ANO_TGT_DONE="/tmp/wn_tgt_done_lijiawen"
export ANO_TGT_LOG="/tmp/wn_tgt_log_lijiawen.txt"
export ANO_FIRE_DONE="/tmp/hcomm_fire_done_lijiawen"
