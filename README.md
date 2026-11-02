# ano_offload_pkg — ANO：NVMe-oF 命令面 AIV 卸载原型部署包

**ANO = AIV NVMe-oF Offload**。命名规范与历史名（hov_*／N2AivProbe）映射见 `docs/命名规范与历史映射.md`。

## 用途

NVMe-oF 命令面下沉原型的可部署交付：AIV 核内完成 96B SQE 生产、WRITE_WITH_NOTIFY 命令发射（2-WQEBB）、回执 HBM 图内轮询收割，host 稳态零参与。本包为软 TGT 自证形态——软 TGT（tgt_wn）扮演对端做闭环验证；真 TGT 对接只换两处缝合点（见 `docs/真TGT缝合点.md`）。

## 前置

- AArch64 目标机，CANN 9.3.0（ASCEND_HOME_PATH，默认 `/usr/local/Ascend/cann-9.3.0`，可用环境变量覆盖）
- 驱动 25.6.rc2.b023／固件 7.0.9.100.202（实测通过面；其他版本未验证）
- 双卡 UB 互通（本包验证面为同机双卡；真 TGT 形态为主机↔TGT 卡）
- **设备号须人工确认**：包内默认 TASK_DEVICE=1 仅是开发机验证面，新机器上必须确认健康空闲卡号后用环境变量覆盖
- g++（仅重编译时需要；包内自带预编译二进制）

## 三步部署

```bash
tar xzf ano_offload_pkg_YYYYMMDD.tar.gz && cd ano_offload_pkg
bash scripts/deploy_check.sh      # ①健康 ②CANN 版本 ③MANIFEST 校验 ④opp 农场重建 ⑤dry-run 建链
bash scripts/run_direct.sh 1      # 单发联跑；8 = 八连发稳定性
```

判读口径：日志出现 `[PASS] wnfire 回执面 AIV 图内闭环（N/N 发 CQE=0 + N/N 发回执 AIV 侧 MATCH）` 即全绿。对端（软 TGT）明细在 `/tmp/wn_tgt_log_lijiawen.txt`。

## 自检

- 完整性：`md5sum -c MANIFEST.md5`（deploy_check.sh 已内含）
- 重编译自证（可选）：`bash scripts/build.sh` 编译通过即构建面自证（注意：重编译产物含包内路径调试信息，md5 与预编译件不同属正常；**运行时以 MANIFEST 钉死的预编译件为准**，重编译后请从 MANIFEST 校验或重新拷回预编译件）

## 目录

```
bin/        预编译二进制（wn_host/tgt_wn）+ opp_vendor/（AnoProbe 算子 vendor 包）
src/        ano_wn/（两件源码）· aiv_probe/（AnoProbe 算子源）· include/（线格式布局头文件）
scripts/    env.sh · build.sh · run_direct.sh · deploy_check.sh · ano_pair.sh（在案原样存档）
third_party/DEPENDENCIES.md   依赖清单（版本钉死值）
docs/       命名规范与历史映射.md · 真TGT缝合点.md · 代码与产物对应关系.md · codeagent接手说明.md
            （设计文档.md / 调试文档.md：Govern 侧提供，放入即可）
```

## 注意

- `scripts/ano_pair.sh` 为开发机在案编排的**原样存档**（内部硬编码开发机路径），部署联跑请用 `run_direct.sh`。
- 本包不含任何内部过程文档；问题反馈只需回传部署机上的 run 日志与 `md5sum -c` 输出。
