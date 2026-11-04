# KV Cache Decode 跨节点卸载——总体设计文档

版本：v2.1 ｜ 日期：2026-11-02 ｜ 状态：两线合入全链一体闭环（任务 43，AnoProbe mode4，发射热路径 host 零指令）；项目正式命名 ANO（AIV NVMe-oF Offload），包 ano_offload_pkg

---

## 1. 项目目标

大模型推理 decode 阶段，KV cache 以页块形式存储于对端节点的 NVMe SSD。本项目实现：**NPU 推理大图内的 AIV 核直接发起跨节点读**——命令面经 UB（Unified Bus）链路送达对端 TGT，TGT 读盘后把 KV 数据经 RDMA WRITE 直写本端 NPU 的 HBM，回执同样直写 HBM 由 AIV 收割。

**核心收益**：decode 数据面全程不过 host CPU、不过 device AICPU——消除传统路径上的两次上下文切换与数据拷贝，稳态路径全部在图内闭环。

## 2. 形态演进与关键裁定

| 形态 | 裁定 | 依据（实证在案） |
|---|---|---|
| READ 直取（最初原型） | 退役 | TGT 不对外暴露地址，READ 填单三件套（rmtAddr/rmtObjId/token）凑不齐 |
| SEND 信封（命令载于报文载荷） | 不可达 | AIV 引擎 jetty 硬件不支持 SEND opcode（撞 CQE status=3）；AICPU 通道提交面锁于签名算子内；官方 asc-comm 断言 AIV 面仅支持 Write/WriteWithNotify/Read |
| **WRITE_WITH_NOTIFY（在役）** | **采用** | 官方 AIV 面原生支持；单边填单语义免接收预投递；端到端实证全绿 |

**项目边界定义**：真 TGT 能力边界既定（标准 NVMe-oF 惯例：回执写对端 host DRAM、命令收 SEND 带内、不向对端 HBM 写数据），不为本项目改变。所有缺口由**我方适配层**填补（软 TGT 即其原型）；真 TGT 对接＝适配层与 TGT 惯例的缝合点。

## 3. 总体逻辑拓扑

```
 节点 A（INI，推理侧）                                  节点 B（TGT，存储侧）
┌───────────────────────────────────┐              ┌────────────────────────────┐
│ NPU (950DT)                       │              │ TGT（软 TGT 原型=tgt_wn；   │
│ ┌─────────────────────────────┐   │   UB 链路    │  真 TGT=NVMe-oF Target）    │
│ │ AIV kernel（推理图内）       │   │ ═══════════  │                            │
│ │ ① 组装 96B NVMe SQE         │   │              │ ┌────────────────────────┐ │
│ │    (slba=slba_base          │   │              │ │ 命令收件箱 inbox        │ │
│ │     +idx×block_lbas)        │   │              │ │ 8 槽×128B              │ │
│ │ ② 组 128B 双块 WQE 上环      │   │              │ │ (8B 控制头+96B SQE)     │ │
│ │ ③ st_dev 敲门铃 dbVa        │───┼─►jetty 硬件──┼─WRITE 直写+notify 置位─►│ │
│ │ ④ 轮询回执槽 valid 收割      │   │  (opcode=5)  │ └────────────────────────┘ │
│ │    （HBM，图内闭环）         │   │              │ tgt_wn：轮询→解析 SQE→    │
│ └─────────────────────────────┘   │              │ 读本地 SSD→组回执           │
│        ▲                          │              │                            │
│ ┌──────┴────────────┐             │  RDMA WRITE  │ ┌────────────────────────┐ │
│ │ HBM 数据目标区     │◄────────────┼──────────────┤ │ KV 数据（直写 INI HBM） │ │
│ │ （KV 落点）        │             │              │ └────────────────────────┘ │
│ │ HBM 回执区         │◄────────────┼──────────────┤ 回执 WRITE（cqe+data）    │
│ │ cqe 32B+data 256B  │             │              └────────────────────────────┘
│ └───────────────────┘             │
│ host 侧：建链/注册/通告（一次性）；aclrtMemcpy 仅降级观察
└───────────────────────────────────┘
```

三条通道，全部单边 RDMA 语义，全部无需对端预投递：

| 通道 | 方向 | 语义 | 落点 |
|---|---|---|---|
| 命令面 | INI→TGT | WRITE_WITH_NOTIFY（opcode=5） | 对端收件箱槽位+notify 置 valid |
| 数据面 | TGT→INI | RDMA WRITE | 本端 HBM 数据目标区 |
| 回执面 | TGT→INI | RDMA WRITE | 本端 HBM 回执区（AIV 收割） |

## 4. 建链面（一次性，host 侧完成）

1. HcommEndpointCreate → channel 建链（COMM_ENGINE_AIV），ChannelEntity 快照取 sqVa/dbVa（device HBM jetty 环）；
2. 内存注册进 channel 共享域：本端 dbuf（数据目标+回执区）、对端 inbox；**token 三件套（addr/tokenId/tokenValue）经 channel remoteBuffer 交换**；
3. LBA 几何协商：取 namespace 扇区粒度/slba_base → 字段包焊接为编译常量 → 稳态寻址 `slba_base + idx × block_lbas` 单变量乘加，hov_xlate 图内翻译成对端地址；
4. 凭证体系：Token 高 24bit=内存域索引、低 8bit=key；建链期签发，稳态随 WQE 载。

## 5. 命令面：WRITE_WITH_NOTIFY 双块 WQE

```
WQEBB0 (64B)：ctl 48B（opcode=5, flag=0x2D URMA_DEFAULT_CFG 官方默认,
              token_en/nf/owner/tpID, rmtObjId/rmtTokenValue,
              rmtAddr=inbox_base+(cid%8)×128+8）+ NotifyCtx 前 16B
WQEBB1 (64B)：NotifyCtx 后 16B（notifyTokenId/Value/Addr,
              notifyData=恒 1）+ SGE{104B=8B 控制头+96B SQE, localTokenId, va}
```

- **发射纪律（双块原子性）**：slot0 写入→无害门铃（magic=0 占位）→slot1 写入→真门铃（head+2）；一发一收（commit=true+cqe=1）；
- **notify 实证语义**：硬件向 notifyAddr 写入 notifyData（u64）——notifyData 恒 1，写入动作即 valid 置位；**cid 等元信息不得放控制头**（与 notify 写入域共享，会被覆写），由槽位索引承载；
- **收割**：jetty CQ（HBM）owner-bit 相位轮询，CQE status=0 即发射成功。

## 6. 收件箱与回执区布局

```
对端 dbuf（HBM，注册共享）                本端 dbuf（HBM，注册共享）
inbox @ dbuf+0x800：                     cqe_ring：
┌────────┬──────────┬─────┐             ┌────────┬─────────────┬─────┐
│8B 控制头│96B SQE   │ pad │ ×8 槽/128B  │8B 控制头│256B 回执载荷 │ pad │
│=notifyAddr         │     │             │valid@b16│             │     │
└────────┴──────────┴─────┘             └────────┴─────────────┴─────┘
tgt_wn 轮询 valid→解析→清位复用           AIV 轮询 valid→比对→清位复用
cqe_va=dbuf+0x100(32B) data_va=dbuf+0x200(256B)，清位写点 cqe_va+16 u32
```

回执消费通道：AnoProbe AIV 算子（mode0=GetValue 轮询/分片读、mode1=st_dev 清位写；任务 43 起 mode4 全链一体：组 SQE→组 WQE→上环敲门→收割→回执消费单次 kernel 启动完成）——host 侧 aclrtMemcpy 仅降级观察帧，不作判据。

## 7. 端到端时序（单发）

```
AIV(INI)         jetty(UB)          tgt_wn(TGT)          SSD
  │─组 SQE+双块 WQE►│                  │                  │
  │─敲门铃─────────►│                  │                  │
  │                 │─WRITE 直写──────►│ 槽 i 落 104B      │
  │                 │─notify 写───────►│ valid=1           │
  │◄─CQE status=0──│                  │                  │
  │                 │                  │─轮询命中→解析 SQE │
  │                 │                  │────读盘─────────►│
  │                 │                  │◄──KV 数据────────│
  │ HBM 落 KV ◄────│──RDMA WRITE─────│                  │
  │ HBM 回执槽 ◄───│──RDMA WRITE─────│                  │
  │ AIV 收割/对账/清位（图内闭环）      │                  │
```

## 8. 实测基线（device 1↔2，2026-10-31/11-02）

| 项 | 结果 |
|---|---|
| 命令面（任务 39） | 单发+八连发全绿：CQE status=0×8，96B SQE 逐字节 MATCH，key MATCH×8 |
| 回执面（任务 40） | 单发+八连发全绿：8/8 valid=1，256B AIV 侧 MATCH，清位复用无丢失无错序 |
| 全链一体（任务 43） | **mode4 单次 kernel 启动完成全链**（组 SQE→组 WQE→上环敲门→收割→回执消费）：单发+八连发 8/8 全绿；发射热路径 host 零 H2D（代码审查+运行期双证据）；SQE 组帧=生产镜像（psdt/keyed type 两处偏差随合入修正） |
| 官方接口对照（任务 37） | asc-comm 官方样例 WriteNbi/ReadNbi 实跑 PASS（互证组帧正确性） |

## 9. 真 TGT 缝合点（对接时仅换两处）

1. **inbox 地址获取段**（tgt 侧 `inbox = rmt + 0x800` 约定）→ 替换为真 TGT 的地址通告面；
2. **回执落点段**（cqe_ring 偏移常量）→ 与真 TGT 惯例缝合（若 TGT 仅写 host DRAM，则由适配层转发 HBM，写路径归我侧）。

其余全部复用：SQE 载荷格式、LBA 编译常量、双块 WQE 构造、收割环、纪律面零改动。

## 10. 纪律与封存

- 禁改件（md5 双端自证）：hov_proto.h/hov_sm.cpp/hov_host.cpp/hov_xlate.h/hcomm_probe.cpp/demo.py；
- SEND 形态（hov_send/ 全线，历史命名按证据链纪律保留原名）只读封存——真 TGT 若原生支持 SEND 可复活对照；现役全链形态在 ano_full/（ano_wn/ 封存为基线参照；命名规范见《命名规范与历史映射》）；
- 分卡纪律：实跑仅 device 1↔2；device 0/3（故障/对照样本）严禁触碰。
