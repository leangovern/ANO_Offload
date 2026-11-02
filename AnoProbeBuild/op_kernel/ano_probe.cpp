/* ============================================================================
 * op_kernel/ano_probe.cpp — 任务 12-b：N2 AIV 可达性/直驱探针 kernel
 * mode（params.mode）：
 *   0 = 读探针：从 params.addr 读 ≤64B → out.data（heap/sq.qbuf/cq.qbuf/db MMIO）
 *   1 = 写探针：向 params.addr 写 8B magic → flush → 读回 → out.data
 *   2 = AIV 直驱发射：把 params.wqe[64B] 写到 params.addr（SQ 环槽位）→
 *       Dcci(CACHELINE_OUT) + PipeBarrier → 向 params.addr2+0x80 写 u32
 *       params.db_val（SW/MMIO 门铃，udma_update_sq_db 等价语义，
 *       umdk udma_u_jfs.h:160-163 在案）
 * GM 访问形态：GlobalTensor GetValue/SetValue（prod2 实证通道，任务 6-f
 * op_kernel/aiv_sqe96_build_prod2.cpp:24-70 同源）；flush 形态
 * DataCacheCleanAndInvalid<CACHELINE_OUT>（同文件 aiv_flush_gm:59-70 同源）。
 * 屏障顶替标注：PTO 用 dcci+dsb(DSB_DDR)（urma_async_post.hpp:103-112），
 * 本 kernel 用 AscendC 面 DataCacheCleanAndInvalid + PipeBarrier<PIPE_ALL>
 * 顶替（语义等价非实现等价，修改记录在案）。
 * ========================================================================= */
#include "kernel_operator.h"
#include "ano_probe_tiling.h"	/* REGISTER_TILING_DEFAULT 所需 */
/* ---- 任务 43：组帧链单源（自 taskM6f/AivSqe96BuildAll/op_kernel 拷贝，md5
 * 对账：sqe96_build.c 0b84cbc1…/sqe96_build.h bd53ccb0…/sqe96_layout.h
 * 93b44740…/aiv_device_compat.h 39859541…；禁止本地改动，与 prod2:16 同款
 * include 形态） ---- */
#include "sqe96_build.h"
#include "sqe96_build.c"

/* ---- 任务 43 mode4：全链一体（组 SQE→组 WQE→上环敲门→收割→回执消费+清位） ----
 * params.addr = prep 包 GM 地址（prep 一次性 H2D；热路径 d_params 复用零 H2D）。
 * prep 包布局与 ano_full/wn_full.cpp host 侧镜像（DO1 裁定书 §5）。 */
struct ano_full_prep {
	uint64_t sqVa, dbVa, headAddr, scqVa;
	uint64_t sqDepth, cqDepth, head0;
	uint64_t inbox;			/* 对端命令收件箱基址（8 槽×128B） */
	uint32_t rtid, rtval, ltid, tpID;
	uint8_t rmtEID[16];
	uint64_t staging_va;	/* 104B 载荷 staging（dbuf+0x400） */
	uint64_t cqe_va;		/* 回执 cqe_buf（dbuf+0x100，valid@byte16） */
	uint64_t data_va;		/* 回执数据（dbuf+0x200，256B） */
	uint64_t fire_idx_va;	/* GM 发射序号计数器（prep 置 0，kernel 自增） */
	struct sqe96_cmd_in cmd;
	struct sqe96_route_in route;
	struct sqe96_seg seg;
};

#define ANO_M4_CQ_SPINS		20000000u	/* HovSm HOV_CQ_SPINS 在案值 */
#define ANO_M4_RCPT_SPINS	40000000u	/* HovSm HOV_MAX_SPINS 在案值 */

struct n2_probe_params {
	uint64_t addr;		/* 被测地址（heap VA / sq.qbuf / cq.qbuf / db 页） */
	uint64_t addr2;		/* mode2：门铃页基址；mode3：门铃直写地址（channel dbVa，无偏移） */
	uint64_t addr3;		/* mode3：SQ head 影子地址（PTO UrmaPostSendUpdateInfo 双写形态，intrin.hpp:78-82） */
	uint64_t magic;		/* mode1：写入值；mode2：门铃值（低 32 位有效） */
	uint32_t len;		/* mode0：读字节数（≤64） */
	uint32_t mode;
	uint64_t wqe[8];	/* mode2：64B WQE（SQE 48B + SGE 16B） */
};

struct n2_probe_out {
	uint64_t done_magic;	/* 完成标记（host 预置 0） */
	uint32_t mode_echo;
	uint32_t len_echo;
	uint8_t data[64];
};

static __aicore__ inline void
n2_gm_load(void *dst, __gm__ uint8_t *src, uint32_t bytes)
{
	AscendC::GlobalTensor<uint64_t> g;
	uint32_t words = (bytes + 7u) / 8u;
	g.SetGlobalBuffer((__gm__ uint64_t *)src);
	for (uint32_t k = 0; k < words; k++) {
		uint64_t w = g.GetValue(k);
		for (uint32_t b = 0; b < 8 && k * 8 + b < bytes; b++)
			((uint8_t *)dst)[k * 8 + b] = (uint8_t)(w >> (b * 8));
	}
}

static __aicore__ inline void
n2_gm_store(__gm__ uint8_t *dst, const void *src, uint32_t bytes)
{
	AscendC::GlobalTensor<uint64_t> g;
	uint32_t words = (bytes + 7u) / 8u;
	g.SetGlobalBuffer((__gm__ uint64_t *)dst);
	for (uint32_t k = 0; k < words; k++) {
		uint64_t w = 0;
		for (uint32_t b = 0; b < 8; b++)
			w |= (uint64_t)((const uint8_t *)src)[k * 8 + b] << (b * 8);
		g.SetValue(k, w);
	}
}

static __aicore__ inline void
n2_flush_gm(__gm__ uint8_t *addr)
{
	AscendC::GlobalTensor<uint8_t> t;
	t.SetGlobalBuffer(addr);
	AscendC::DataCacheCleanAndInvalid<uint8_t,
		AscendC::CacheLine::ENTIRE_DATA_CACHE,
		AscendC::DcciDst::CACHELINE_OUT>(t);
}

/* spin 内读原语：ld_dev 设备读语义（hov_sm.cpp:33-37 hov_ld32 逐字形态——
 * GetValue 缓存读在同 kernel spin 内看不到硬件/对端新写，HovSm 已证） */
static __aicore__ inline uint32_t
m4_ld32(__gm__ uint8_t *addr)
{
	return ld_dev(reinterpret_cast<__gm__ uint32_t *>(addr), 0);
}

/* 块读：先 dcci 再 GetValue（hov_desc_verify 逐轮形态，hov_sm.cpp:95-98） */
static __aicore__ inline void
m4_load(void *dst, __gm__ uint8_t *src, uint32_t bytes)
{
	n2_flush_gm(src);
	n2_gm_load(dst, src, bytes);
}

extern "C" __global__ __aicore__ void
ano_probe(GM_ADDR params, GM_ADDR out, GM_ADDR workspace, GM_ADDR tiling)
{
	REGISTER_TILING_DEFAULT(AnoProbeTilingData);
	struct n2_probe_params p;
	struct n2_probe_out o;

	n2_gm_load(&p, (__gm__ uint8_t *)params, 40);
	if (p.mode == 2 || p.mode == 3)
		n2_gm_load(p.wqe, (__gm__ uint8_t *)params + 40, 64);

	o.done_magic = 0x4E324149565030ull;	/* 'N2AIVP0' */
	o.mode_echo = p.mode;
	o.len_echo = p.len;
	for (uint32_t i = 0; i < 64; i++)
		o.data[i] = 0;

	if (p.mode == 0) {
		uint32_t len = p.len > 64 ? 64 : p.len;
		n2_gm_load(o.data, (__gm__ uint8_t *)(uintptr_t)p.addr, len);
	} else if (p.mode == 1) {
		/* 无害写改 st_dev（MMIO 面；12-c 阶段 0 用） */
		AscendC::PipeBarrier<PIPE_ALL>();
		dsb(DSB_DDR);
		st_dev((uint32_t)p.magic, reinterpret_cast<__gm__ uint32_t *>((uintptr_t)p.addr), 0);
		uint32_t rv = ld_dev(reinterpret_cast<__gm__ uint32_t *>((uintptr_t)p.addr), 0);
		o.data[0] = (uint8_t)(rv & 0xFF);
		o.data[1] = (uint8_t)((rv >> 8) & 0xFF);
		o.data[2] = (uint8_t)((rv >> 16) & 0xFF);
		o.data[3] = (uint8_t)((rv >> 24) & 0xFF);
	} else if (p.mode == 2) {
		/* WQE 上环 → flush + 全屏障 → 门铃（u32 store，liburma db+0x80 口径） */
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)p.addr, p.wqe, 64);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)p.addr);
		AscendC::PipeBarrier<PIPE_ALL>();
		AscendC::GlobalTensor<uint32_t> db;
		db.SetGlobalBuffer((__gm__ uint32_t *)(uintptr_t)(p.addr2 + 0x80));
		uint32_t dbv = (uint32_t)p.magic;
		db.SetValue(0, dbv);
	} else if (p.mode == 3) {
		/* 任务 12-c：hcomm channel 发射——WQE 上环 → dcci + 屏障 + dsb →
		 * st_dev 双写（PTO UrmaPostSendUpdateInfo 逐指令形态，
		 * urma_async_post.hpp:106-113/intrin.hpp:78-82）。
		 * 修正（首轮 GlobalTensor SetValue 普通缓存写未触发硬件消费——
		 * MMIO 门铃必须 st_dev 设备写语义） */
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)p.addr, p.wqe, 64);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)p.addr);
		AscendC::PipeBarrier<PIPE_ALL>();
		dsb(DSB_DDR);
		uint32_t dbv = (uint32_t)p.magic;
		st_dev(dbv, reinterpret_cast<__gm__ uint32_t *>((uintptr_t)p.addr2), 0);
		st_dev(dbv, reinterpret_cast<__gm__ uint32_t *>((uintptr_t)p.addr3), 0);
	} else if (p.mode == 4) {
		/* 任务 43 mode4：全链一体——AIV 读 prep 包与 fire_idx（GM）→
		 * sqe96_build 现组装 96B SQE → 现组装 128B 双块 WQE →
		 * slot0/slot1 上环（无害门铃→真门铃）→ kernel 内 CQ owner-bit
		 * 收割 → 回执 valid 轮询+256B 比对+清位 → fire_idx 自增。
		 * 结果经 out.data 回 host（观察面）：{ok_cqe,status,substatus,ok_rcpt,ok_match,cid} */
		struct ano_full_prep pp;
		struct sqe96_build_out sb;
		uint8_t pay[104];
		uint8_t wqe[128];
		uint32_t *wd = (uint32_t *)wqe;
		uint64_t fire_idx = 0, head, cq_slot, slot_off, slotVa0, slotVa1, z;
		uint32_t cid, owner, exp_owner, i, spins;
		uint8_t ok_cqe = 0, ok_rcpt = 0, ok_match = 0;
		uint32_t cqe_status = 0xFF, cqe_substatus = 0xFF;

		m4_load(&pp, (__gm__ uint8_t *)(uintptr_t)p.addr, sizeof(pp));
		m4_load(&fire_idx, (__gm__ uint8_t *)(uintptr_t)pp.fire_idx_va, 8);
		cid = (uint32_t)(fire_idx & 7u);
		head = pp.head0 + (uint32_t)fire_idx * 2u;

		/* 1) AIV 现组装 96B SQE（sqe96_build 生产镜像构造器，线 B 合入点） */
		pp.cmd.cid = (uint16_t)cid;
		if (sqe96_build(&pp.cmd, &pp.route, &pp.seg, 1, &sb) != 0) {
			o.data[0] = 0xEE;	/* 组装失败标记（如实上报） */
		} else {
		/* 2) 104B 载荷（8B 控制头+96B SQE）→ staging */
		for (i = 0; i < 8; i++) pay[i] = 0;
		pay[0] = 1; pay[1] = (uint8_t)cid;
		for (i = 0; i < 96; i++) pay[8 + i] = sb.slot[i];
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)pp.staging_va, pay, 104);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)pp.staging_va);

		/* 3) AIV 现组装 128B 双块 WQE（任务 36 对拍表字段；wn_host.cpp:561-583 移植） */
		for (i = 0; i < 128; i++) wqe[i] = 0;
		slot_off = pp.inbox + (uint64_t)cid * 128u;
		owner = ((head & pp.sqDepth) == 0u) ? 1u : 0u;
		wd[0] = (uint32_t)((head + 2) & 0xFFFFu) | (0x2Du << 16) | (1u << 28) | (1u << 29) | (owner << 31);
		wd[1] = (5u << 8);
		wd[2] = pp.tpID | (1u << 24);
		wd[3] = pp.rtid & 0xFFFFFu;
		for (i = 0; i < 16; i++) wqe[16 + i] = pp.rmtEID[i];
		wd[8] = pp.rtval;
		wd[10] = (uint32_t)(slot_off & 0xFFFFFFFFull);
		wd[11] = (uint32_t)(slot_off >> 32);
		wd[12] = pp.rtid & 0xFFFFFu;
		wd[13] = pp.rtval;
		wd[14] = wd[10]; wd[15] = wd[11];
		wd[16] = 1; wd[17] = 0;
		wd[20] = 104;
		wd[21] = pp.ltid;
		wd[22] = (uint32_t)(pp.staging_va & 0xFFFFFFFFull);
		wd[23] = (uint32_t)(pp.staging_va >> 32);

		/* 4) 上环+敲门（纪律同任务 39：slot0+无害门铃 → slot1+真门铃 head+2） */
		slotVa0 = pp.sqVa + (uint64_t)(head % pp.sqDepth) * 64u;
		slotVa1 = pp.sqVa + (uint64_t)((head + 1) % pp.sqDepth) * 64u;
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)slotVa0, wqe, 64);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)slotVa0);
		AscendC::PipeBarrier<PIPE_ALL>();
		dsb(DSB_DDR);
		st_dev(0u, reinterpret_cast<__gm__ uint32_t *>((uintptr_t)pp.dbVa), 0);
		st_dev(0u, reinterpret_cast<__gm__ uint32_t *>((uintptr_t)pp.headAddr), 0);
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)slotVa1, wqe + 64, 64);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)slotVa1);
		AscendC::PipeBarrier<PIPE_ALL>();
		dsb(DSB_DDR);
		st_dev((uint32_t)(head + 2), reinterpret_cast<__gm__ uint32_t *>((uintptr_t)pp.dbVa), 0);
		st_dev((uint32_t)(head + 2), reinterpret_cast<__gm__ uint32_t *>((uintptr_t)pp.headAddr), 0);

		/* 5) kernel 内 CQ owner-bit 收割（hov_sm.cpp:176-186 逐轮 dcci+ld_dev 形态） */
		cq_slot = pp.scqVa + (uint64_t)((pp.head0 + (uint32_t)fire_idx) % pp.cqDepth) * 64u;
		exp_owner = ((((pp.head0 + (uint32_t)fire_idx) / pp.cqDepth) % 2u) == 0u) ? 1u : 0u;
		for (spins = 0; spins < ANO_M4_CQ_SPINS; spins++) {
			uint32_t dw0;
			n2_flush_gm((__gm__ uint8_t *)(uintptr_t)cq_slot);
			dw0 = m4_ld32((__gm__ uint8_t *)(uintptr_t)cq_slot);
			if (((dw0 >> 2) & 1u) == exp_owner) {
				ok_cqe = 1;
				cqe_status = (dw0 >> 24) & 0xFFu;
				cqe_substatus = (dw0 >> 16) & 0xFFu;
				break;
			}
		}

		/* 6) kernel 内回执消费：valid spin（dcci+ld_dev）→ 256B 比对 → 清位 */
		for (spins = 0; spins < ANO_M4_RCPT_SPINS; spins++) {
			uint32_t v0;
			n2_flush_gm((__gm__ uint8_t *)(uintptr_t)pp.cqe_va);
			v0 = m4_ld32((__gm__ uint8_t *)(uintptr_t)(pp.cqe_va + 16));
			if ((v0 & 0xFFu) == 1u) { ok_rcpt = 1; break; }
		}
		if (ok_rcpt) {
			uint8_t gd[64];
			ok_match = 1;
			for (i = 0; i < 4; i++) {
				m4_load(gd, (__gm__ uint8_t *)(uintptr_t)(pp.data_va + (uint64_t)i * 64u), 64);
				for (uint32_t k = 0; k < 16; k++)
					if (((uint32_t *)gd)[k] != i * 16u + k) { ok_match = 0; break; }
				if (!ok_match) break;
			}
			z = 0;
			n2_gm_store((__gm__ uint8_t *)(uintptr_t)(pp.cqe_va + 16), &z, 8);
			n2_flush_gm((__gm__ uint8_t *)(uintptr_t)(pp.cqe_va + 16));
		}

		/* 7) fire_idx 自增回写 + 结果出参 */
		z = fire_idx + 1;
		n2_gm_store((__gm__ uint8_t *)(uintptr_t)pp.fire_idx_va, &z, 8);
		n2_flush_gm((__gm__ uint8_t *)(uintptr_t)pp.fire_idx_va);
		o.data[0] = ok_cqe;
		o.data[1] = (uint8_t)cqe_status;
		o.data[2] = (uint8_t)cqe_substatus;
		o.data[3] = ok_rcpt;
		o.data[4] = ok_match;
		o.data[5] = (uint8_t)cid;
		*(uint32_t *)(o.data + 8) = (uint32_t)fire_idx;
		}
	}

	n2_gm_store((__gm__ uint8_t *)out, &o, sizeof(o));
	n2_flush_gm((__gm__ uint8_t *)out);
}
