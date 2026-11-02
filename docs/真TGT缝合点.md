# 真 TGT 缝合点指引

本包为软 TGT 自证形态：tgt_wn 扮演对端（收命令→解析→取数→写回执）。真 TGT 到位后**整层退役 tgt_wn 的扮演逻辑**，发射侧（wn_host）只需替换以下两处地址来源，其余（96B SQE 生产、双块 WQE 构造、发射纪律、AIV 回执收割）一行不动。

## 缝合点 ①：命令收件箱（inbox）地址获取段

**位置**：`src/ano_wn/wn_host.cpp` wnfire 段（:518-525）

```c
uint32_t rtid  = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenId;      /* :519 */
uint32_t rtval = rb.bufferInfo.rma.protectionInfo.memInfo.ub.tokenValue;   /* :520 */
uint64_t rmt   = rb.bufferInfo.rma.addr;                                   /* :521 */
uint64_t inbox = rmt + 0x800;   /* :523 对端命令收件箱槽环基址（软 TGT 约定：8 槽×128B） */
```

**现行语义**：软 TGT 把自有 dbuf 注册进 channel，发射侧经 `HcclChannelGetRemoteMems` 拿到 `rb`（对端缓冲三件套：addr/tokenId/tokenValue），inbox = 对端 dbuf+0x800 的约定偏移。

**真 TGT 替换接口**：真 TGT 须以等效方式通告"命令收件箱基址 + token 三件套"。替换形态——将上述四行的取值来源改为真 TGT 通告面（连接协商阶段的地址通告，先例：stock INI→TGT 的 cqe_buf 地址+tokenid 经 88B conn_req 通告，nvme_hcomm.c:2169 锚点）。替换后 WQE 填单段（:560-583，rmtObjId/rmtTokenValue/rmtAddr/notifyAddr 全由 rtid/rtval/inbox 派生）零改动。

**替换接口签名建议**：

```c
/* 真 TGT 通告面：返回对端命令收件箱描述。返回值 0=成功。 */
int tgt_inbox_acquire(uint64_t *inbox_addr, uint32_t *token_id, uint32_t *token_value,
                      uint32_t *slot_cnt /* 期望 >=8，槽=128B */);
```

**约束**：收件箱槽位布局（128B/槽：8B 控制头 valid@byte0 + 96B SQE 载荷区 + 24B pad）是真 TGT 必须遵守的线协议；notify 语义 = 向槽控制头地址写 notifyData(u64)=1 置 valid（任务 39 实证）。

## 缝合点 ②：回执落点段（cqe_ring 偏移常量）

**位置 A（消费侧）**：`src/ano_wn/wn_host.cpp` wnfire 段（:524-525）

```c
uint64_t cqe_va  = lva + 0x100;   /* 回执 cqe_buf：32B（cpl16B + valid@byte16） */
uint64_t data_va = lva + 0x200;   /* 回执数据落区：256B */
```

**位置 B（对端写侧）**：`src/ano_wn/tgt_wn.cpp` wnrecv 回执段（:598/:605）

```c
dst = rmt + 0x200;   /* :598 回执数据 256B → client dbuf+0x200 */
dst = rmt + 0x100;   /* :605 回执 cqe_buf 32B → client dbuf+0x100（valid@byte16，数据先行 valid 殿后） */
```

**现行语义**：回执区落在**发起侧（client）注册的 dbuf**（HBM）固定偏移；tgt 经 WRITE WQE 直写；消费侧 AIV 图内轮询 valid。

**真 TGT 缝合**：真 TGT 惯例为"TGT RDMA 直写发起侧 cqe_buf"（stock 原生语义，nvme_hcomm.c:3480 段 HcommMemReg 注册+export 锚点）——与本形态天然同构。缝合只需双方约定：①cqe_buf 基址+三件套由发起侧在连接协商时通告真 TGT（同 stock conn_req 通道）；②槽布局 32B=16B cpl+valid@byte16 与真 TGT CQE 写格式对齐（若真 TGT 写格式不同，改消费侧解析，不改运输面）。

**真 TGT 协调项（D1 在案挂账）**：真 TGT 是否提供命令收件箱（缝合点①的地址源）属跨端协调事项；若真 TGT 无收件箱概念，则由其暴露等效"命令投递缓冲"注册面替代。

## 退役边界

真 TGT 到位后删除范围：`tgt_wn` 二进制的 wnrecv 扮演段（:518-622）及 `run_direct.sh` 中 tgt 启动行。发射侧 `wn_host` 全部保留。
