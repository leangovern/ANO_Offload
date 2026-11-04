#!/bin/bash
# build.sh — 编译入口（在案 g++ 命令封装；错误即停）
# 产物：bin/wn_full（src/ano_full/wn_full.cpp）+ bin/tgt_wn（src/tgt_wn/tgt_wn.cpp）
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh

C="$ASCEND_HOME_PATH"
V="$PKG_VENDOR"
K="$PKG_ROOT/src/aiv_probe/op_kernel"	# sqe96_build.h 链
INC="$PKG_ROOT/src/include"		# n2_udma_internal.h

for pair in "src/ano_full/wn_full.cpp:bin/wn_full" "src/tgt_wn/tgt_wn.cpp:bin/tgt_wn"; do
	srcf="${pair%%:*}"; outf="${pair##*:}"
	echo "[build] $srcf → $outf"
	g++ -std=c++17 -O2 -fpermissive -o "$outf" "$srcf" \
		-I"$C/include" -I"$C/aarch64-linux/include" -I/usr/include \
		-I"$V/op_api/include" -I"$K" -I"$INC" \
		-L"$C/aarch64-linux/lib64" -lhcomm -lascendcl -lruntime -lc_sec \
		-L/usr/lib64 -lurma \
		-L"$V/op_api/lib" -lcust_opapi \
		-L"$C/lib64" -lnnopbase \
		-Wl,-rpath,"$C/aarch64-linux/lib64" -Wl,-rpath,/usr/lib64 \
		-Wl,-rpath,"$V/op_api/lib" -Wl,-rpath,"$C/lib64"
done
echo "[build] 完成："
md5sum bin/wn_full bin/tgt_wn
