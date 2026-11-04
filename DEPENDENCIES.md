# 第三方依赖清单（DEPENDENCIES，ano_offload_pkg v5）

| # | 名称 | 在案版本（钉死值） | 用途 | 获取方式 |
|---|---|---|---|---|
| 1 | CANN 工具链 | **cann-9.3.0**（`aarch64-linux/ascend_toolkit_install.info` 中 `version=9.3.0`） | 编译与运行（acl/aclnn/hcomm/urma 头件与库、ccec 算子工具链） | 目标机预装；`ASCEND_HOME_PATH` 指向 |
| 2 | 驱动/固件 | 驱动 **25.6.rc2.b023**／固件 **7.0.9.100.202**（实测通过面；其他版本未验证） | UB fabric 运行底座 | 目标机预装 |
| 3 | 系统运行库 | liburma（`/usr/lib64`）、libhcomm（`$ASCEND_HOME_PATH/aarch64-linux/lib64`）、libascendcl/libruntime/libc_sec/libnnopbase（随 CANN） | 链接与运行 | 随 CANN/系统 |
| 4 | 编译器 | g++（在案参数 `-std=c++17 -O2 -fpermissive`） | 仅重编译时需要 | 目标机系统 |
| 5 | 内核/线格式头件 | `src/include/n2_udma_internal.h`（包内自带，项目在案布局头） | 编译期 WQE/CQE 布局定义 | **包内** |
| 6 | 任务调度（可选） | task-submit（开发机占卡调度） | 编排 | 目标机若无则用 `scripts/run_direct.sh` 裸跑 |

## 包内自带（项目自研，非第三方）

- AnoProbe 算子（`vendor/customize/` 安装态 + `src/aiv_probe/` 工程源）：AIV 五模式通道（mode0 读/1 写/2 遗留/3 发射/4 全链一体）；
- wn_full（`bin/` + `src/ano_full/`）：发射侧原型（mode4 全链一体形态）；
- tgt_wn（`bin/` + `src/tgt_wn/`）：软 TGT 对端扮演（环境自验用；真 TGT 到位后退役）。

## 版本不符处理

CANN/驱动与钉死值不符 → deploy_check 报 warn/FAIL → 重编译（build.sh）是对的动作，但行为等价性未验证，如实上报实测。
