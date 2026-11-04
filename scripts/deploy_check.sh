#!/bin/bash
# deploy_check.sh — v5 部署自检（①设备健康 ②CANN 版本 ③MANIFEST 校验 ④merged opp 重建 ⑤dry-run 建链）
# dry-run 只建链+快照，不发射任何数据面 WQE。任一硬失败即非零退出。
set -u
cd "$(dirname "$0")/.." || exit 1
source scripts/env.sh
FAIL=0

echo "===== ① 设备健康 ====="
if command -v npu-smi > /dev/null 2>&1 && npu-smi info > /dev/null 2>&1; then
	npu-smi info | head -20
else
	echo "[warn] npu-smi 不可用（容器内常见）——跳过，不判死；请人工确认 device $TASK_DEVICE 健康空闲"
fi

echo "===== ② CANN 版本 ====="
VINFO="$ASCEND_HOME_PATH/aarch64-linux/ascend_toolkit_install.info"
if [ -f "$VINFO" ]; then
	grep -E '^(package_name|version|innerversion)=' "$VINFO"
	grep -q '^version=9.3.0$' "$VINFO" \
		&& echo "[ok] CANN 9.3.0 与实测通过面一致" \
		|| echo "[warn] CANN 版本非 9.3.0——未验证面，如实提示（不判死）"
else
	echo "[warn] $VINFO 不存在——请确认 ASCEND_HOME_PATH 指向 CANN 根"
fi

echo "===== ③ MANIFEST.md5 全量校验 ====="
if md5sum -c MANIFEST.md5 > /tmp/ano_manifest_check.log 2>&1; then
	echo "[ok] MANIFEST 全量校验通过（$(grep -c ': OK' /tmp/ano_manifest_check.log) 项）"
else
	echo "[FAIL] MANIFEST 校验不符项："
	grep -v ': OK' /tmp/ano_manifest_check.log | head -10
	FAIL=1
fi

echo "===== ④ merged opp 农场重建 ====="
SYSOPP="$ASCEND_HOME_PATH/opp"
M="$PKG_ROOT/opp_merged"
if [ -d "$SYSOPP" ]; then
	rm -rf "$M"
	mkdir -p "$M/op_impl" "$M/op_proto"
	for d in Ascend bin built-in conf include lib64 scene.info; do
		ln -sfn "$SYSOPP/$d" "$M/$d"
	done
	ln -sfn "$SYSOPP/built-in/op_impl" "$M/op_impl/built-in"
	ln -sfn "$PKG_VENDOR/op_impl" "$M/op_impl/custom"
	ln -sfn "$SYSOPP/built-in/op_proto" "$M/op_proto/built-in"
	ln -sfn "$PKG_VENDOR/op_proto" "$M/op_proto/custom"
	echo "[ok] merged opp 农场已重建（$M → 系统 $SYSOPP + 包内 customize）"
else
	echo "[FAIL] 系统 opp 不存在：$SYSOPP"
	FAIL=1
fi

echo "===== ⑤ dry-run 建链（不发射数据面） ====="
if [ "$FAIL" = "0" ]; then
	D="$TASK_DEVICE"
	rm -f /tmp/hcomm_srv_* /tmp/hcomm_cli_* "$ANO_STOPF"
	timeout 120 stdbuf -o0 "$PKG_ROOT/bin/tgt_wn" server "$D" 0 "$D" > /tmp/ano_dry_srv.log 2>&1 &
	SRVPID=$!
	sleep 8
	timeout 120 stdbuf -o0 "$PKG_ROOT/bin/wn_full" client "$D" 0 "$D" > /tmp/ano_dry_cli.log 2>&1 &
	CLIPID=$!
	OK=0
	for i in $(seq 1 60); do
		if grep -q '建链+快照完成' /tmp/ano_dry_srv.log 2>/dev/null && grep -q '建链+快照完成' /tmp/ano_dry_cli.log 2>/dev/null; then
			OK=1; break
		fi
		sleep 1
	done
	touch "$ANO_STOPF"
	sleep 2
	kill $SRVPID $CLIPID 2>/dev/null
	wait 2>/dev/null
	if [ "$OK" = "1" ]; then
		echo "[ok] dry-run 双端建链+快照完成（未发射任何 WQE）"
		grep -E 'localBuf|remoteBuf|sqVa' /tmp/ano_dry_cli.log | head -4
	else
		echo "[FAIL] dry-run 建链 60s 内未完成——日志：/tmp/ano_dry_srv.log /tmp/ano_dry_cli.log"
		FAIL=1
	fi
fi

echo "===== 自检结论 ====="
if [ "$FAIL" = "0" ]; then
	echo "[PASS] 部署自检通过——可执行 bash scripts/run_direct.sh 1 做单发联跑"
else
	echo "[FAIL] 存在硬失败项，见上"
fi
exit $FAIL
