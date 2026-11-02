# 第三方依赖清单（DEPENDENCIES）

| # | 名称 | 在案版本（钉死值） | 用途 | 获取方式 |
|---|---|---|---|---|
| 1 | CANN 工具链 | **cann-9.3.0**（`$ASCEND_HOME_PATH/version.info` 中 `Version=9.3.0`） | 编译（g++ 头文件/库）与运行（acl 运行时、hcomm、urma） | 目标机预装；`ASCEND_HOME_PATH` 环境变量指向 |
| 2 | asc-comm 公开仓 | commit **c9ec6c9**（gitcode.com/cann/asc-comm，master） | 官方 AIV 直驱参考实现（WriteWithNotify 字段对拍依据） | **不编译进包**，仅文档引用；`git clone --depth 1` 后 `git checkout c9ec6c9` |
| 3 | 驱动/固件基线 | 驱动 **25.6.rc2.b023**／固件 **7.0.9.100.202** | UB fabric 运行底座 | 目标机预装；其他版本未验证，如实标注 |
| 4 | 系统依赖 | g++（在案编译参数 `-std=c++17 -O2 -fpermissive`）、acl 运行时（libascendcl/libruntime/libc_sec，随 CANN）、liburma（`/usr/lib64`）、libnnopbase（随 CANN）、libhcomm（`$ASCEND_HOME_PATH/aarch64-linux/lib64`） | 编译链接与运行 | 随 CANN/系统；重编译需 g++ |
| 5 | 部署前置 | device 健康（npu-smi 全绿；容器内 npu-smi 可能缺面，降级人工确认）、双卡/双机 UB 互通、设备白名单机制 | 运行前提 | 目标机环境 |

## 编排层说明

开发机联跑经 `task-submit --device <N> --run`（占卡调度+300s 时限）。目标机若无该机制，使用包内 `scripts/run_direct.sh`（裸跑变体，编排逻辑与在案 `wn_pair.sh` 一致，仅路径参数化+去调度层）。`scripts/ano_pair.sh` 为在案原样存档件，内部硬编码开发机路径，不用于部署。

## 包内自带（非第三方，列出以便区分）

- AnoProbe 算子（`bin/opp_vendor/` 产物 + `src/aiv_probe/` 源；历史名 N2AivProbe，映射见 docs/命名规范与历史映射.md）：AIV 读（mode0）/写（mode1）/发射（mode3）通道，项目自研。
- wn_host / tgt_wn（`bin/` 产物 + `src/ano_wn/` 源）：命令面/回执面原型本体，项目自研。
