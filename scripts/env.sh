#!/bin/bash
# env.sh — ano_offload_pkg 环境变量集中（被 build.sh / run_direct.sh / deploy_check.sh source）
# 所有路径自包内定位；目标机差异一律用环境变量覆盖，不改脚本。

PKG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PKG_ROOT

# CANN 工具链根（目标机若非此路径，部署前 export ASCEND_HOME_PATH=<实际路径> 再 source 本文件）
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.3.0}"

# AnoProbe 算子 vendor 包（包内自带）
export PKG_VENDOR="$PKG_ROOT/bin/opp_vendor/vendors/customize"
export ASCEND_CUSTOM_OPP_PATH="$PKG_VENDOR"

# merged opp 农场（deploy_check.sh 重建；ASCEND_OPP_PATH 可被外部环境覆盖）
export ASCEND_OPP_PATH="${ASCEND_OPP_PATH:-$PKG_ROOT/opp_merged}"

export LD_LIBRARY_PATH="$ASCEND_HOME_PATH/aarch64-linux/lib64:$PKG_VENDOR/op_api/lib:$PKG_VENDOR/op_impl/ai_core/tbe/op_tiling/lib/linux/aarch64:$PKG_VENDOR/op_proto/lib/linux/aarch64:$LD_LIBRARY_PATH"

# 设备选择（新机器上卡号须人工确认后覆盖，不得盲目沿用默认）
export TASK_DEVICE="${TASK_DEVICE:-1}"

# 联跑用 /tmp 标志文件（双进程编排握手；路径硬编码于二进制内，仅变量名引用）
export ANO_STOPF="/tmp/hcomm_probe_stop_lijiawen"
export ANO_TGT_DONE="/tmp/wn_tgt_done_lijiawen"
export ANO_TGT_LOG="/tmp/wn_tgt_log_lijiawen.txt"
export ANO_FIRE_DONE="/tmp/hcomm_fire_done_lijiawen"
