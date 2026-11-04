/*   SPDX-License-Identifier: BSD-3-Clause
 *
 *   Independent 96B HCOMM SQE reference builder for the aiv_offload
 *   golden-reference harness.
 *
 *   Design contract (from the task): this constructor must be a PURE
 *   function with NO host global state and field-level portability, so
 *   the same logic can be reproduced verbatim on the AIV core (Ascend C)
 *   once the host sq_buf staging buffer is removed.  Accordingly:
 *
 *     - no globals, no static buffers, no allocation;
 *     - all inputs arrive through the parameter block (command fields +
 *       a segment table of {addr, len});
 *     - every SQE byte is derived solely from those inputs plus the
 *       constants in sqe96_layout.h;
 *     - the builder never touches mem_map / HcommMemReg / tokenId pools
 *       — the caller supplies per-segment tokenIds exactly as the host
 *       submit path would have obtained them (nvme_hcomm_build_contig_
 *       request / nvme_hcomm_sgl_add_segment, lib/nvme/nvme_hcomm.c).
 *
 *   It mirrors lib/nvme/nvme_hcomm.c, line-by-line, for the observable
 *   wire bytes:
 *     - nvme_hcomm_sqe_96b_fill       (header, lib/nvme/nvme_hcomm_sqe_96b.h:155)
 *     - nvme_hcomm_qpair_fill_sqe     (nvme_hcomm.c:4850) — incl. PSDT set at :4860
 *     - nvme_hcomm_build_contig_      (nvme_hcomm.c:4521) — single keyed desc in DPTR
 *       request
 *     - nvme_hcomm_sgl_add_segment    (nvme_hcomm.c:4602) — tail desc fill
 *     - nvme_hcomm_sgl_set_count      (nvme_hcomm.c:4652) — DPTR LAST_SEGMENT / collapse
 *
 *   Caller API: see sqe96_build() below.
 */

#ifndef AIV_OFFLOAD_SQE96_BUILD_H
#define AIV_OFFLOAD_SQE96_BUILD_H

#include "sqe96_layout.h"
#include "aiv_device_compat.h"	/* SQE96_AICORE_FN + device memset/memcpy shim */

#ifdef __cplusplus
extern "C" {
#endif

/* Command fields that survive to the wire.  The transport copies
 * req->cmd verbatim (nvme_hcomm_sqe_96b_fill memcpy), then sets
 * req->cmd.cid (reserve_cid) and req->cmd.psdt (fill_sqe) just before. */
struct sqe96_cmd_in {
	uint8_t		opc;
	uint8_t		fuse;
	uint8_t		psdt;		/* PSDT_SGL_MPTR_CONTIG when payload_size>0 */
	uint16_t	cid;
	uint32_t	nsid;
	uint64_t	mptr;
	uint32_t	cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

/* One payload segment (keyed data block).  token_id is what the host
 * path obtains from HcommMemExport via the mem_map cache; address is the
 * user VA, length the segment byte count. */
struct sqe96_seg {
	uint64_t	address;
	uint32_t	length;
	uint32_t	token_id;
};

/* Routing / qpair attributes (DW22/DW23). */
struct sqe96_route_in {
	uint16_t	ctrlid;		/* hctrlr->ctrlr.cntlid  */
	uint16_t	sqid;		/* qpair->id            */
	uint32_t	jetty_token;	/* NVME_HCOMM_INI_JFR_TOKEN */
};

/* Build result: the 352B slot image plus the variable SEND length
 * (base 96 + nsgl*16), which mirrors nvme_hcomm_send_sqe's len. */
struct sqe96_build_out {
	uint8_t		slot[352];
	uint32_t	send_len;	/* 96 + nsgl*16 */
	uint32_t	nsgl;		/* tail descriptors sent (0 for single-seg collapse) */
};

/*
 * Build one 96B SQE + tail SGL array into out->slot[352].
 *
 *   cmd    : NVMe command fields (cmd.cid and cmd.psdt are applied here,
 *            mirroring reserve_cid + fill_sqe — pass them already set or
 *            let nsgl==0/payload presence overwrite psdt as the transport
 *            does at nvme_hcomm.c:4860).
 *   route  : ctrlid / sqid / jetty_token.
 *   segs   : payload segment table (may be NULL when nsegs==0).
 *   nsegs  : 0 (no payload / metadata-only) | 1 (CONTIG single) | >=2 (SGL).
 *
 * Returns 0 on success, -EINVAL when nsegs > SQE96_MAX_SGL or a segment
 * length exceeds the 24-bit keyed field (1<<24 - 1) — the same guard as
 * NVME_HCOMM_MAX_KEYED_SGL_LENGTH (nvme_hcomm.c:191) /
 * nvme_hcomm_sgl_add_segment (nvme_hcomm.c:4612).
 */
SQE96_AICORE_FN int sqe96_build(const struct sqe96_cmd_in *cmd,
		const struct sqe96_route_in *route,
		const struct sqe96_seg *segs, uint32_t nsegs,
		struct sqe96_build_out *out);

#ifdef __cplusplus
}
#endif

#endif /* AIV_OFFLOAD_SQE96_BUILD_H */
