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
	}

	n2_gm_store((__gm__ uint8_t *)out, &o, sizeof(o));
	n2_flush_gm((__gm__ uint8_t *)out);
}
