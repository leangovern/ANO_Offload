/* ============================================================================
 * n2_udma_internal.h — 任务 12-b：udma provider 内部件声明（挖掘用）
 * 来源：oss/umdk（atomgit openeuler/umdk @1717aef）逐字段拷贝，行号锚点：
 *   udma_u_common.h:48-53(doorbell) :133-172(jetty_queue) :219-225(jetty)
 *   :227-236(jfc) :249-256(segment) :304(TO_DEVICE_BARRIER)
 *   udma_u_jfs.h:17-51(jfs_sqe_ctl)  udma_u_jfc.h:24-53(jfc_cqe)
 *   kernel_headers/udma_abi.h:29(DOORBELL_OFFSET) :33(DB_SIZE) :139-145(mmap_type)
 * 风险：安装版 liburma 内部件可能与 umdk master 存在版本差——运行时校验
 *   （sq.idx==jetty_id.id、cq.idx==jfc_id.id、provider post 后 pi/db 自增、
 *   WQE 字节与字段映射一致），任一不符即停止上报，禁止硬用。
 * ========================================================================= */
#ifndef N2_UDMA_INTERNAL_H
#define N2_UDMA_INTERNAL_H

#include <stdint.h>
#include <pthread.h>
#include <ub/umdk/urma/urma_api.h>

#define N2_UDMA_DOORBELL_OFFSET 0x80	/* udma_abi.h:29 */
#define N2_UDMA_DB_SIZE         64U	/* udma_abi.h:33 */
#define N2_UDMA_JFS_WQEBB       64	/* udma_u_common.h:28 */
#define N2_UDMA_SGE_SIZE        16	/* udma_u_common.h:29 */

enum n2_db_mmap_type {			/* udma_abi.h:139-145 */
	N2_UDMA_MMAP_HUGEPAGE = 0,
	N2_UDMA_MMAP_JFC_PAGE,
	N2_UDMA_MMAP_JETTY_DSQE,
	N2_UDMA_MMAP_RESERVED_SQ,
	N2_UDMA_MMAP_KERNEL_BUF,
};

struct n2_udma_doorbell {		/* udma_u_common.h:48-52 */
	uint32_t id;
	enum n2_db_mmap_type type;
	void volatile *addr;
};

struct n2_udma_hugepage {
	void *va_start;
	uint32_t va_len;
	void *priv;
};

struct n2_udma_jetty_queue {		/* udma_u_common.h:133-172 */
	void *ctx;
	void *qbuf;
	void *qbuf_end;
	uint32_t qbuf_size;
	void *qbuf_curr;
	uint32_t pi;
	uint32_t ci;
	uint32_t idx;
	struct n2_udma_doorbell db;
	uint32_t baseblk_shift;
	uint32_t baseblk_cnt;
	uint32_t baseblk_mask;
	uintptr_t *wrid;
	pthread_spinlock_t lock;
	uint32_t max_inline_size;
	void *dwqe_addr;
	void *tjetty;
	uint32_t trans_mode;		/* urma_transport_mode_t */
	uint32_t sqe_bb_cnt;
	uint32_t max_sge_num;
	uint8_t flush_flag;
	uint32_t old_entry_idx;
	uint8_t lock_free;
	uint8_t cstm;
	uint8_t sq_reserved;
	struct n2_udma_hugepage *hugepage;
	uint8_t db_status;
	uint8_t need_ring_db;
	uint8_t pi_type;
	uint8_t dtu_en;
	uint32_t aligned_size;
	uint8_t is_jetty;
	uint8_t is_wqe_lock_buf_jetty;
};

struct n2_udma_jetty {			/* udma_u_common.h:219-225 */
	urma_jetty_t base;
	struct n2_udma_jetty_queue sq;
	void *jfr;
	void *jetty_grp;
	uint32_t jetty_type;
};

struct n2_udma_jfc {			/* udma_u_common.h:227-236 */
	urma_jfc_t base;
	struct n2_udma_jetty_queue cq;
	uint32_t *sw_db;
	uint8_t db_cstm;
	uint32_t arm_sn;
	uint32_t mode;
	uint32_t cq_shift;
	uint32_t ccu_flag;
};

struct n2_udma_segment {		/* udma_u_common.h:249-256 */
	urma_target_seg_t urma_tseg;
	urma_token_t token_value;
	uint8_t token_value_valid;
	uint64_t len;
	uint64_t va;
	uint32_t tid;
};

/* C/C++ 双兼容断言（C 探针 n2_probe.c 与 C++ host n2_aiv_host.cpp 共用本头） */
#ifdef __cplusplus
#define N2_STATIC_ASSERT static_assert
#else
#define N2_STATIC_ASSERT _Static_assert
#endif

/* 48B JFS SQE（udma_u_jfs.h:17-51；与 CANN PTO UrmaSqeCtx 逐字段同构，13-a 在案） */
struct n2_jfs_sqe_ctl {
	/* byte 0-4 */
	uint32_t sqe_bb_idx : 16;
	uint32_t flag : 7;
	uint32_t udf_flag : 1;
	uint32_t rsv0 : 3;
	uint32_t nf : 1;
	uint32_t token_en : 1;
	uint32_t rmt_jetty_type : 2;
	uint32_t owner : 1;
	/* byte 4-8 */
	uint32_t target_hint : 8;
	uint32_t opcode : 8;
	uint32_t rsv1 : 6;
	uint32_t inline_msg_len : 10;
	/* byte 8-12 */
	uint32_t tp_id : 24;
	uint32_t sge_num : 8;
	/* byte 12-16 */
	uint32_t rmt_jetty_or_seg_id : 20;
	uint32_t write_len : 10;
	uint32_t rsv2 : 2;
	/* byte 16-32 */
	uint8_t rmt_eid[16];
	/* byte 32-36 */
	uint32_t rmt_token_value;
	/* byte 36-40 */
	uint32_t udf_type : 8;
	uint32_t reduce_data_type : 4;
	uint32_t reduce_opcode : 4;
	uint32_t rsv3 : 16;
	/* byte 40-48 */
	uint32_t rmt_addr_l_or_token_id;
	uint32_t rmt_addr_h_or_token_value;
};
N2_STATIC_ASSERT(sizeof(struct n2_jfs_sqe_ctl) == 48, "sqe ctl layout");

struct n2_wqe_sge {			/* udma_u_common.h（udma_wqe_sge） */
	uint32_t length;
	uint32_t token_id;
	uint64_t va;
};
N2_STATIC_ASSERT(sizeof(struct n2_wqe_sge) == 16, "sge layout");

/* 64B JFC CQE（udma_u_jfc.h:24-53 = PTO UrmaJfcCqeCtx 同构） */
struct n2_jfc_cqe {
	/* DW0 */
	uint32_t s_r : 1;
	uint32_t is_jetty : 1;
	uint32_t owner : 1;
	uint32_t inline_en : 1;
	uint32_t opcode : 3;
	uint32_t fd : 1;
	uint32_t rsv : 8;
	uint32_t substatus : 8;
	uint32_t status : 8;
	/* DW1 */
	uint32_t entry_idx : 16;
	uint32_t local_num_l : 16;
	/* DW2 */
	uint32_t local_num_h : 4;
	uint32_t rmt_idx : 20;
	uint32_t rsv1 : 8;
	/* DW3 */
	uint32_t tpn : 24;
	uint32_t rsv2 : 8;
	/* DW4 */
	uint32_t byte_cnt;
	/* DW5 ~ DW6 */
	uint32_t user_data_l;
	uint32_t user_data_h;
	/* DW7 ~ DW10 */
	uint32_t rmt_eid[4];
	/* DW11 ~ DW12 */
	uint32_t data_l;
	uint32_t data_h;
	/* DW13 ~ DW15 */
	uint32_t inline_data[3];
};
N2_STATIC_ASSERT(sizeof(struct n2_jfc_cqe) == 64, "cqe layout");

#define N2_TO_DEVICE_BARRIER() { __asm__ __volatile__("dmb st" ::: "memory"); }	/* udma_u_common.h:304 */

#endif /* N2_UDMA_INTERNAL_H */
