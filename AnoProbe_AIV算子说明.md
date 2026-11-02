# AnoProbe AIV 算子说明（现役 v2：含 mode4 全链一体）

版本：2026-11-02　源：`taskM42/AnoProbeBuild/op_kernel/ano_probe.cpp`（md5 `9457e48d…`）　aclnn 面：`aclnnAnoProbe*`

## 1. 算子是什么、为什么存在

AnoProbe 是项目自研的 **AIV（AI Vector 核）通用通道算子**——host 经 aclnn 接口把一段参数交给它，它在 AIV 核内执行一组"贴硬件"的动作（GM 读写、WQE 上环、敲 MMIO 门铃、轮询收割），把结果写回。

它存在的根本原因：**hcomm/urma 的用户态库只提供 host API，AIV 核内没有任何官方途径直接操作 UB jetty**（SQ 环、dbVa 门铃、CQ 都在 device 地址空间）。AnoProbe 就是那条"从 AIV 侧够到硬件"的自研通道。全部动作形态都有实证先例锚点（prod2 的 GM 访问、12-c 的发射面、HovSm 的 spin 收割）。

**调用面**（host 侧）：`aclnnAnoProbeGetWorkspaceSize → aclnnAnoProbe(NULL, ws, exec, stream) → aclrtSynchronizeStream`。输入一个 256B 参数区（`{addr, addr2, addr3, magic, len, mode, wqe[8]}`），输出一个 256B 结果区（`{done_magic, mode_echo, len_echo, data[64]}`）。

## 2. 算子族谱与分工

| 算子 | 状态 | 作用 |
|---|---|---|
| **AnoProbe**（历史名 N2AivProbe，任务 42 改名重建） | **现役** | 五模式通道：mode0 读 / mode1 写 / mode2 遗留 / mode3 发射 / mode4 全链一体 |
| HovSm（任务 15-a，六件之一 `c24c33af…`） | 封存钉住 | 交接状态机（IDLE→ARMED→ACTIVE→DRAINED）：host 建链后把凭证交接给 AIV 持证发射的协议机——**真 TGT 迁移环 1 的复活资产** |

## 3. AnoProbe 五个 mode

| mode | 名称 | 作用 | 现状 |
|---|---|---|---|
| 0 | 读探针 | AIV 从任意 GM 地址读 ≤64B → out.data（CQ 收割、槽 valid 轮询的通道） | 现役（ano_wn 线用） |
| 1 | 写探针 | `st_dev` 写 u32 到任意 GM 地址（清 valid 位、无害门铃） | 现役（ano_wn 线用） |
| 2 | 直驱发射（SW 门铃） | WQE 上环 + `db+0x80` 普通 store 门铃 | 遗留（12-b 形态，mode3 取代） |
| 3 | channel 发射 | WQE 上环 → dcci+屏障+dsb → `st_dev` 双写（dbVa+head 影子） | 现役（ano_wn 线 hp_fire） |
| 4 | **全链一体** | 单次启动完成 组 SQE→组 WQE→上环敲门→CQ 收割→回执消费+清位 | **现役主力（ano_full）** |

## 4. 部署拓扑图

```
host 进程（wn_full）                     device（AIV 核 + HBM + UB jetty）
┌────────────────────────┐   aclnn    ┌──────────────────────────────────────────┐
│ prep：建链/物化 prep 包 │ ─────────► │ AnoProbe kernel（AIV 核内执行）           │
│ 热路径：启动+等待+观察   │            │   │读 prep 包/fire_idx（GM）              │
└────────────────────────┘            │   ▼                                      │
                                      │ 组 SQE（sqe96_build 生产镜像）            │
                                      │   │                                      │
        HBM（device 内存，全部 GM 可达）│   ▼                                      │
        ┌──────────────────────┐      │ 组 128B 双块 WQE                          │
        │ dbuf 4KB：            │ ◄─── │   │写 staging（dbuf+0x400）               │
        │  +0x100 回执 cqe_buf  │      │   ▼                                      │
        │  +0x200 回执数据      │      │ 写 SQ 环槽 slot0/slot1 ──► UB jetty SQ    │
        │  +0x400 载荷 staging  │      │ st_dev 敲门（dbVa/head 影子）──► 硬件消费  │
        │  +0x500 fire_idx      │      │   │                          │           │
        │  +0x600 prep 包       │      │   ▼spin（dcci+ld_dev 逐轮）   ▼ UB 网络   │
        │ SQ 环 / CQ 环 / dbVa  │ ◄─── │ 收割 CQ owner-bit           │           │
        └──────────────────────┘      │   ▼                                      │
                                      │ 轮询回执 valid→读 256B 比对→清位          │
                                      └──────────────────────────────────────────┘
                                                           ▲        │ UB WRITE_WITH_NOTIFY
                                                           │        ▼
                                      对端（软 TGT tgt_wn / 未来真 TGT）：收件箱槽环
                                      （解析 SQE→key 校验→取数→两发 WRITE 回执直写本端 HBM）
```

要点：①参数/表格/队列/回执全部在 HBM，AIV 经 GM 地址直读直写；②门铃是 MMIO，必须 `st_dev` 设备写语义（普通缓存写不触发硬件，12-c 已实证）；③对端回执是 UB WRITE 直写本端 HBM，AIV 轮询即可见（dcci+ld_dev 逐轮保证读到新值）。

## 5. mode4 执行时序（单次 aclnn 调用 = 一发完整 I/O）

```
步骤  动作                                                  访存/屏障语义
────  ──────────────────────────────────────────────────  ─────────────────────
 1    读 prep 包（200B）+ fire_idx（8B）自 GM               dcci + GetValue
 2    cid=fire_idx%8；head=head0+fire_idx×2                 纯计算
 3    sqe96_build(cmd, route, seg) → 96B SQE                纯计算（寄存器/本地）
 4    104B 载荷（8B 控制头+96B SQE）→ staging(dbuf+0x400)   store + dcci(CACHELINE_OUT)
 5    组 128B WQE（双块：WRITE 填单+NotifyCtx）             纯计算
 6    slot0（前 64B）写 SQ 环槽                             store + dcci
 7    PipeBarrier + dsb(DSB_DDR)                            全屏障（写先于门铃可见）
 8    st_dev(dbVa, 0) + st_dev(headAddr, 0)                 无害门铃（硬件不消费）
 9    slot1（后 64B）写 SQ 环槽                             store + dcci
10    PipeBarrier + dsb                                     全屏障
11    st_dev(dbVa, head+2) + st_dev(headAddr, head+2)       真门铃 → 硬件发射
12    spin 收割 CQ：dcci + ld_dev(cq_slot)，≤20M 轮          owner-bit 命中读 status
13    spin 回执 valid：dcci + ld_dev(cqe_va+16)，≤40M 轮     等对端 UB WRITE 落 HBM
14    读回执数据 256B（4×64B）逐 u32 比对递增图案            dcci + GetValue
15    清 valid（cqe_va+16 写 8B 0）                         store + dcci（槽位复用）
16    fire_idx+1 回写 GM                                    store + dcci
17    结果（ok_cqe/status/substatus/ok_rcpt/ok_match/cid）   store + dcci → host D2H 观察
      写 out.data
```

时序要点：
- **步骤 7-8 vs 10-11 的"两跳敲门"纪律**（任务 39 在案）：slot0 先上环但只敲无害门铃（值 0=无新工作），slot1 写完才敲真门铃——硬件永远不会消费半成品双块 WQE；
- **步骤 12-13 的 spin 读必须是 dcci+ld_dev**（设备读语义）：同 kernel 内 GetValue 缓存读看不到硬件/对端新写（HovSm 已证的坑）；
- **步骤 13 是唯一的对端等待点**：tgt 处理时延（秒级）决定 spin 时长，上限 40M 轮（HovSm 在案值）。

## 6. 支撑 mode 时序（ano_wn 线遗留形态，mode4 之外仍在用）

**mode0（读）**：`读 params → GetValue 读 addr≤64B → 写 out`——每轮 host 调用一次（CQ owner-bit、槽 valid 的轮询通道；UB 写后可见性多轮实证）。

**mode3（发射）**：`WQE 64B 上环 → dcci → PipeBarrier → dsb → st_dev(dbVa,head+1) + st_dev(head影子,head+1)`——单块 64B 发射原语（任务 12-c 形态；WRITE_WITH_NOTIFY 双块需调两次，正是 mode4 把这两跳+组帧合并进单 kernel 的动机）。

**mode1（写）**：`st_dev(addr, magic)`——清回执 valid 位（写 0）与无害门铃共用通道。

## 7. HovSm（封存，环 1 复活资产）

任务 15-a 交接状态机算子（六件之一，钉住不改）：kernel 内跑 IDLE→ARMED→ACTIVE→DRAINED 四状态机——host 建链后把凭证（sqVa/dbVa/token/SGL 表）发布到交接描述符，AIV 侧 double-read 校验后持证发射、每 WR 现场读 token（禁缓存），DRAIN 信号到即停手确认。**真 TGT 迁移环 1（stock 建链+凭证交接 AIV）的协议机制就是它**，届时以 AnoSm 正式名接入，本体零改动。

## 8. 一致性语义备忘（实踩过的坑）

| 动作 | 正确原语 | 反例（已踩） |
|---|---|---|
| 写 GM 让硬件/对端可见 | store + `DataCacheCleanAndInvalid<CACHELINE_OUT>` | 只 store 不 flush（硬件读旧值） |
| 敲 MMIO 门铃 | `st_dev` 设备写 | GlobalTensor SetValue 普通缓存写（12-c 首轮不触发） |
| spin 轮询硬件/对端新写 | 每轮 dcci + `ld_dev` | 同 kernel 内 GetValue（命中陈旧缓存行，假超时） |
| host 轮询 device 内存 | 走 AIV（mode0） | host aclrtMemcpy D2H 轮询（陈旧缓存行假超时，任务 39） |
