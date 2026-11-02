# ANO 现役代码交付文档（任务 43 全链一体形态）

版本：v1.0（2026-11-02）　交付方：codeagent（lijiawen 个人环境）　接收方：项目 Owner / 后续接手 codeagent

## 1. 交付范围

本次交付为 ANO（AIV NVMe-oF Offload）现役主线代码及其验证证据，形态为**本机双卡软 TGT 自证版**。核心能力：NVMe-oF 读取命令的 SQE 生产、UB WRITE_WITH_NOTIFY 发射、CQE 回执收割全链在 AIV 核内闭环——**发射热路径 host 零指令（prep 一次性物化除外）**。

不含：真 TGT 对接（环 1 建链接线、环 3 厂商收件箱协调，见 §6）。

## 2. 交付清单与指纹

| # | 件 | 路径（~/aiv_scratch/ 相对） | md5 |
|---|---|---|---|
| 1 | 发射侧源/二进制 | taskM43/ano_full/wn_full.cpp / wn_full | `0dfd62e8fd44fb5df752f074edf4d76a` / `3931835e3aa038cfe1a64c26ebf18131` |
| 2 | 联跑编排 | taskM43/ano_full/full_pair.sh | `ecb93ab6e4e5e3d679a6922643aec543` |
| 3 | 使用说明 | taskM43/ano_full/README.md | 以实测为准 |
| 4 | AnoProbe 算子源（含 mode4+组帧链） | taskM42/AnoProbeBuild/（op_host/op_kernel/构建三件套） | kernel `9457e48d7870644df06af3777a2cf763` |
| 5 | 组帧链四件（任务 6-f 拷贝对账） | AnoProbeBuild/op_kernel/sqe96_build.{c,h}+sqe96_layout.h+aiv_device_compat.h | `0b84cbc1…`/`bd53ccb0…`/`93b44740…`/`39859541…` |
| 6 | AnoProbe vendor 安装态 | taskM42/opp_priv_ano/vendors/customize/ | libcust_opapi.so `d9c074f2076e2147418d5d65adb89dc2` |
| 7 | 软 TGT（封存复用，零改动） | taskM42/ano_wn/tgt_wn（源 tgt_wn.cpp） | `81b6ac50ed1d6243f353644f2010670e` / `cc115380d653632e83aed048b6ab5395` |
| 8 | 验证证据（单发+八连发，含 tgt 侧） | taskM43/full_run1{,_tgt}.log、full_run8{,_tgt}.log | `4518d7fe…`/`e0c7d264…`/`35d559af…`/`5d231994…` |
| 9 | 任务报告与裁定书 | taskM43/REPORT.md、DO1_合入形态裁定书.md | `41d556c2…` / `06e66cec…` |
| 10 | 全景总览（决策层读物） | taskM44P/ANO_项目全景框架与流程总览.md | `94316956…` |
| 11 | 资产归档包（68 件全量） | taskM44P/ano_archive_20261102.tar.gz | `8340eae…`→`8340e4eae386ca9e409c88db29a7da20` |

## 3. 运行环境要求

- 硬件：aarch64，Ascend 950DT×2（UB 互通；本交付验证面=同机 device 1↔2）
- 软件：CANN 9.3.0（`ASCEND_HOME_PATH`）、驱动 25.6.rc2.b023、固件 7.0.9.100.202（其他版本未验证）
- 依赖库：libhcomm/libascendcl/libruntime/libc_sec/liburma/libnnopbase（随 CANN/系统）；AnoProbe vendor（交付件 #6 自带）

## 4. 部署与验证（三步）

```bash
# ① 算子 vendor 就位（若用交付环境原样，已装于 opp_priv_ano；新机器则 .run 重装）
# ② 运行时环境：full_pair.sh 已封装 ASCEND_OPP_PATH/ASCEND_CUSTOM_OPP_PATH/LD_LIBRARY_PATH
# ③ 联跑验证
task-submit --timeout 3600 --device 1 --run "bash .../ano_full/full_pair.sh 1"   # 单发
task-submit --timeout 3600 --device 1 --run "bash .../ano_full/full_pair.sh 8"   # 八连发
```

**验收判据**（唯一全绿口径）：`[PASS] wnfire 全链一体闭环（N/N 发 CQE=0 + N/N 发回执 AIV 侧 MATCH）`；tgt 侧 key 校验 MATCH×N、keyed word `type=0x40`、回执 WRITE rc=0×2N。已交付证据（#8）即本判据的八连发全绿实录。

## 5. 架构要点（一分钟版）

- **prep（host，一次性）**：建链（hcomm channel，COMM_ENGINE_AIV）→ dbuf 注册（HBM 4KB）→ prep 包（凭证+SQE 模板+地址面）与 fire_idx 物化落 GM；
- **热路径（AIV kernel mode4，每 I/O）**：读 fire_idx → sqe96_build 组 96B SQE → 组 128B 双块 WQE → 上环敲门（无害门铃→真门铃）→ spin 收割 CQ → spin 回执 valid → 256B 比对 → 清位 → fire_idx+1；
- **软 TGT（tgt_wn）**：AIV 轮询收件箱槽 → 解析 SQE → key 校验 → 模拟取数 → 两发 WRITE 回执（数据先行 valid 殿后）。真 TGT 到位后此层整层退役，发射侧一行不动（缝合点见 ano_offload_pkg `docs/真TGT缝合点.md`）。

## 6. 已知边界与开口项

1. **静态载荷用例**：当前 slba=8 常量（经 prep 包 cmd.cdw10 注入）；运行期每 I/O 变参（段表/LUT 全形态，任务 3 资产）未接入——真 TGT 阶段工作；
2. **真 TGT 迁移未开工**：环 1（stock 建链+15-a 交接机制）与环 3（厂商收件箱，D1 协调项挂账）；
3. **环境异常在案**：status=5（device 0/3，不触碰）与本环境 SEND 面不可用（D3 问项⑩，P1）——均不阻塞本交付形态（WRITE_WITH_NOTIFY 路径）；
4. **语义待厂商 3 项**：notify 数据宽度、status=3 官方语义空档、channel JFR 预投递面（D3_问项清单.md）。

## 7. 修改与维护纪律（接手必读）

- 封存件（ano_wn 五件、六件、hov_send 六件、n2_udma_internal.h）只读；现役改动落 ano_full/AnoProbeBuild 并逐处留痕（文件/位置/新旧 md5/理由）；
- device 白名单 1/2，排队不抢；device 0/3 严禁发射/复位/触碰；不重启机器；
- 验证必须单发+八连发双过才算绿（单发不构成证据）；
- 证据纪律：结论=证据→结论两栏式；无语义来源标"待厂商"，禁止编造。
