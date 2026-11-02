#!/bin/bash
# build.sh — 编译入口（在案编译命令原样封装；错误即停）
# 用法：bash scripts/build.sh        → 编译 src/ano_wn 两件 → bin/wn_host bin/tgt_wn
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh

C="$ASCEND_HOME_PATH"
V="$PKG_VENDOR"
INC_PROBE="$PKG_ROOT/src/include"	# n2_udma_internal.h（UB 线格式布局在案头文件，历史名沿用）

for f in wn_host tgt_wn; do
	echo "[build] $f.cpp → bin/$f"
	g++ -std=c++17 -O2 -fpermissive -o "bin/$f" "src/ano_wn/$f.cpp" \
		-I"$C/include" -I"$C/aarch64-linux/include" -I/usr/include \
		-I"$V/op_api/include" -I"$INC_PROBE" \
		-L"$C/aarch64-linux/lib64" -lhcomm -lascendcl -lruntime -lc_sec \
		-L/usr/lib64 -lurma \
		-L"$V/op_api/lib" -lcust_opapi \
		-L"$C/lib64" -lnnopbase \
		-Wl,-rpath,"$C/aarch64-linux/lib64" -Wl,-rpath,/usr/lib64 \
		-Wl,-rpath,"$V/op_api/lib" -Wl,-rpath,"$C/lib64"
done
echo "[build] 完成："
md5sum bin/wn_host bin/tgt_wn
