# codeagent 接手说明（新机器部署/编译）

> 致接手的 codeagent：本文档是你在新机器上部署、编译、验证本包的全部上下文。**先通读，再动手。**

## 0. 铁律（最高优先级，全程有效）

1. **个人环境原则**：所有操作在个人目录内进行；**禁止用 sudo 修改公共环境**（系统包、系统配置、公共目录）。安装/配置一律限定个人目录。
2. **个人环境标识**：新建的环境、目录、临时文件命名带 `lijiawen` 标识。
3. **零功能改动**：本包为验证定型交付。部署/编译过程中发现任何"需要改代码才能继续"的情况——**停手上报，不擅自改**。脚本封装、环境变量适配属允许范围。
4. **修改留痕纪律**：凡被迫做出的任何适配修改，必须逐处记录：文件路径 | 位置 | 类型（新增/删除/替换）| 修改前摘要 | 修改后完整文本 | 理由，随回传一并上报。
5. **证据纪律**：结论一律"证据→结论"两栏式；不确定的语义标"待确认"，禁止编造、禁止"顺手优化"。
6. **设备纪律**：仅占用被明确授权的设备号；占卡前确认空闲；他用户占用则排队等待，不抢。**本包源码继承白名单硬校验（仅 device 1/2）**——新机器上卡号语义不同，部署前必须由人确认可用卡号并以 `TASK_DEVICE` 覆盖，不得盲目沿用 1/2。

## 1. 本项目是什么

NVMe-oF 命令面下沉原型：AIV（AI Vector 核）内完成 96B SQE 生产、WRITE_WITH_NOTIFY 命令发射、回执 HBM 图内轮询收割，host 稳态零参与。包内为**软 TGT 自证形态**：tgt_wn 扮演对端闭环验证；真 TGT 对接只换两处缝合点（`docs/真TGT缝合点.md`）。代码↔产物↔功能映射见 `docs/代码与产物对应关系.md`。

## 2. 环境前置（部署前先核对）

| 项 | 要求 | 核对方式 |
|---|---|---|
| 架构 | aarch64 | `uname -m` |
| CANN | 9.3.0（`ASCEND_HOME_PATH` 指向，默认 `/usr/local/Ascend/cann-9.3.0`） | `cat $ASCEND_HOME_PATH/aarch64-linux/ascend_toolkit_install.info` |
| 驱动/固件 | 驱动 25.6.rc2.b023／固件 7.0.9.100.202（其他版本未验证） | `npu-smi info` |
| 设备 | 健康（Health=OK）、双卡/双端 UB 互通、空闲 | `npu-smi info`（容器内可能缺面，降级人工确认） |
| 编译（仅重编译时需要） | g++，参数 `-std=c++17 -O2 -fpermissive` | `g++ --version` |

**版本不符的处理**：CANN/驱动版本与上表不符时，deploy_check 会报硬失败——此时重编译是对的动作（见 §4），但运行行为是否等价**未验证**，如实上报实测结果，不承诺可用。

## 3. 部署三步（首选路径：预编译件直跑）

```bash
tar xzf ano_offload_pkg_YYYYMMDD.tar.gz && cd hov_offload_pkg
bash scripts/deploy_check.sh     # 五项自检，全 [ok] 且结论 [PASS] 才继续
bash scripts/run_direct.sh 1     # 单发；8 = 八连发稳定性
```

判读口径（唯一的全绿标准）：

```
[PASS] wnfire 回执面 AIV 图内闭环（N/N 发 CQE=0 + N/N 发回执 AIV 侧 MATCH）
```

辅助判读：tgt 侧日志 `/tmp/wn_tgt_log_lijiawen.txt` 应有 `key 校验 … MATCH` ×N、`回执 WRITE#1/#2 rc=0` ×N。

## 4. 重编译路径（仅当预编译件不可用）

触发条件：deploy_check 报 CANN/ABI 类硬失败、二进制起不来（ldd 符号缺失）、或缝合点适配需要。

```bash
bash scripts/build.sh            # 编译 src/ano_wn 两件 → bin/
```

注意：

- 重编译产物 md5 与包内预编译件**必然不同**（包内路径嵌入调试信息），这是正常现象，**不是**完整性问题；
- 重编译后 `md5sum -c MANIFEST.md5` 会对 bin/ 两件报 FAIL——预期内；此时身份基准转为"编译日志+单发行为判据"，在回传中声明"使用现场重编译件"并附新 md5；
- AnoProbe 算子（bin/opp_vendor/；历史名 N2AivProbe）**不要现场重建**——需要完整算子开发链，无收益；算子包与编译期路径解耦，直接用；
- 预编译件与重编译件的行为判据完全相同（§3 的 PASS 行），对错不由二进制 md5 判。

## 5. 已知坑位（全部实踩过，照单规避）

1. **npu-smi 在容器内可能报 -8005 缺面**——不是设备坏了，是容器限制；deploy_check 已降级为 warn，人工确认设备健康即可。
2. **轮询必须走 AIV 读，不走 host memcpy**：host `aclrtMemcpy` D2H 轮询 device 内存会命中陈旧缓存行造成假超时——包内所有轮询已走 AnoProbe mode0，你新增任何观测代码也必须走同一通道。
3. **notify 语义=数据写**：WRITE_WITH_NOTIFY 的 notify 动作是把 notifyData(u64) 写到 notifyAddr——收件箱槽位的 valid 位就是 notify 写置位的，不要再往槽控制头塞其他字段。
4. **发射纪律不可省**：双块 WQE 必须先写 slot0+无害门铃（magic=0）、slot1 写完才落真门铃——顺序反了硬件会消费半成品 WQE。
5. **timeout 包装层**：若你的执行环境有任务时限（如 300s），编排脚本收尾段（等对端退出）可能超时被杀——**判读以日志中的 PASS 行落盘为准**，包装层退出码不作业务判据。
6. **/tmp 标志文件**：编排握手用 `/tmp/hcomm_*` `/tmp/wn_*_lijiawen` 系列文件，多轮运行前脚本会自清；手工调试残留会导致假等待，先 `rm -f` 再跑。
7. **token 逐 channel 漂移**：每次建链 tokenId/tokenValue 都不同，属正常，不是异常。
8. **写文件类操作后要复核落盘**（heredoc/重定向截断是实踩过的坑），尤其生成脚本和清单时。

## 6. 验证 checklist（收工前逐项打勾）

- [ ] `md5sum -c MANIFEST.md5` 全 OK（或未重编译项全 OK）
- [ ] deploy_check 五项 [ok] + [PASS]
- [ ] run_direct 1：PASS 行 + tgt 侧 key MATCH
- [ ] run_direct 8：8/8 PASS（稳定性）
- [ ] 无残留进程（`ps aux | grep -E 'wn_host|tgt_wn'`）
- [ ] 适配修改零或已全部留痕（§0.4 格式）
- [ ] 设备白名单确认未误用（§0.6）

## 7. 回传要求

回传只需：①deploy_check 全量输出 ②单发+八连发日志关键行（PASS/FAIL 行+tgt 侧 key 校验行）③使用的二进制 md5（预编译件或现场重编译件）④修改留痕记录（零修改则声明"零修改"）⑤环境实测版本（CANN/驱动/固件）。**不要**回传代码、patch、二进制本体。
