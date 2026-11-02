/*   SPDX-License-Identifier: BSD-3-Clause
 *
 *   Independent 96B HCOMM SQE reference builder — implementation.
 *   See sqe96_build.h for the design contract (pure, stateless,
 *   field-portable; no hcomm/acl symbols).
 *
 *   Wire-byte mirror of the host path in lib/nvme/nvme_hcomm.c:
 *
 *     nvme_hcomm_qpair_fill_sqe  (:4850)
 *       if (req->payload_size > 0) req->cmd.psdt = PSDT_SGL_MPTR_CONTIG (:4860)
 *       nvme_hcomm_sqe_96b_fill(sqe96, &req->cmd, ctrlid, sqid, token) (:4864)
 *         memset(slot, 0, sizeof)  -> cmd memcpy verbatim            (hdr:160,163)
 *         dword22.bs.ctrlid = ctrlid; dword22.bs.sqid = sqid        (hdr:166,167)
 *         jetty_token = token                                        (hdr:170)
 *
 *     payload paths (only when payload_size > 0, :5065):
 *       CONTIG  -> nvme_hcomm_build_contig_request (:4521)
 *                  single keyed descriptor {type,subtype,key,length,addr}
 *                  written to cmd.dptr.sgl1 (:4556-4560, :4564)
 *       SGL     -> nvme_hcomm_build_sgl_request (:4740)
 *                  walk segments -> nvme_hcomm_sgl_add_segment (:4602):
 *                    desc->keyed = {type KDB, subtype ADDRESS, key,
 *                                   length, address} at tail sgl[i] (:4637-4641)
 *                  nvme_hcomm_sgl_set_count (:4652):
 *                    count==1 -> DPTR = tail[0], return 0 (collapse) (:4659-4663)
 *                    count>=2 -> DPTR = LAST_SEGMENT+OFFSET (unkeyed view,
 *                                subtype=0x1, length=count*16 as a full u32,
 *                                rsvd=0, address=0), return count     (:4663-4668)
 *
 *   The 64B NVMe command is reproduced field-by-field (little-endian
 *   writes on the host) so the builder stays usable as a verbatim AIV
 *   reference without depending on struct spdk_nvme_cmd's exact layout.
 */

#include "sqe96_build.h"	/* brings aiv_device_compat.h: string.h (host) or
				 * device memset/memcpy aliases (__CCE_AICORE__) */

#define SQE96_MAX_KEYED_LEN	((1u << 24u) - 1u)	/* NVME_HCOMM_MAX_KEYED_SGL_LENGTH */

/* Assemble the keyed-view 8B word by value (drop 7, task 4R-f finding B):
 * +8 length(u24, bits 0..23) | +11 key(u32, bits 24..55) |
 * +15 subtype(bits 56..59) | type(bits 60..63) — one u64, one store,
 * no bitfield RMW.  Caller stores it with a single memcpy over d->keyed. */
static SQE96_AICORE_FN uint64_t
sqe96_keyed_word(uint32_t length, uint32_t token_id)
{
	uint64_t kw = 0;

	kw |= (uint64_t)(length & SQE96_MAX_KEYED_LEN);
	kw |= (uint64_t)token_id << 24;
	kw |= (uint64_t)SGL_SUBTYPE_ADDRESS << 56;
	kw |= (uint64_t)SGL_TYPE_KEYED_DATA_BLOCK << 60;
	return kw;
}

SQE96_AICORE_FN int
sqe96_build(const struct sqe96_cmd_in *cmd,
	    const struct sqe96_route_in *route,
	    const struct sqe96_seg *segs, uint32_t nsegs,
	    struct sqe96_build_out *out)
{
	struct sqe96_slot *slot;
	uint32_t i;
	uint16_t dw0;
	if (cmd == NULL || route == NULL || out == NULL ||
	    nsegs > SQE96_MAX_SGL) {
		return -1; /* -EINVAL */
	}
	if (nsegs > 0 && segs == NULL) {
		return -1;
	}
	for (i = 0; i < nsegs; i++) {
		if (segs[i].length > SQE96_MAX_KEYED_LEN) {
			return -1;
		}
	}

	memset(out->slot, 0, sizeof(out->slot));
	slot = (struct sqe96_slot *)out->slot;

	/* ---- dword 0~15: standard NVMe command (field-by-field) ---- */
	/* PSDT: the transport forces PSDT_SGL_MPTR_CONTIG whenever a
	 * payload is present, unconditionally overwriting the caller's
	 * value (nvme_hcomm_qpair_fill_sqe, nvme_hcomm.c:4859-4861).
	 * No payload -> PSDT is copied verbatim from req->cmd (which is 0
	 * for e.g. Identify). */
	if (nsegs > 0) {
		dw0 = (uint16_t)cmd->opc;
		dw0 |= (uint16_t)(cmd->fuse & 0x3u) << 8;
		dw0 |= (uint16_t)(NVME_PSDT_SGL_MPTR_CONTIG & 0x3u) << 14;
	} else {
		dw0 = (uint16_t)cmd->opc;
		dw0 |= (uint16_t)(cmd->fuse & 0x3u) << 8;
		dw0 |= (uint16_t)(cmd->psdt & 0x3u) << 14;
	}
	/* DW0 low half (opc|fuse|rsvd1|psdt) is 16 bits; cid is the high
	 * half — packed as two 16-bit halves exactly like spdk_nvme_cmd.
	 * (cmd is at slot offset 0, so writing via the raw slot bytes keeps
	 * us off the bit-fields, which cannot be addressed.) */
	memcpy(&out->slot[0], &dw0, sizeof(dw0));
	memcpy(&out->slot[2], &cmd->cid, sizeof(cmd->cid));

	slot->cmd.nsid = cmd->nsid;
	/* DW2/DW3 (rsvd2/rsvd3) stay zero — memset above. */
	slot->cmd.mptr = cmd->mptr;
	/* DW6~9 DPTR: left zero; filled below by the payload paths. */
	slot->cmd.cdw10 = cmd->cdw10;
	slot->cmd.cdw11 = cmd->cdw11;
	slot->cmd.cdw12 = cmd->cdw12;
	slot->cmd.cdw13 = cmd->cdw13;
	slot->cmd.cdw14 = cmd->cdw14;
	slot->cmd.cdw15 = cmd->cdw15;

	/* ---- dword 16~21: reserved, zero (memset) ---- */

	/* ---- dword 22: routing word (ctrlid low 16 | sqid high 16) ---- */
	slot->dword22 = (uint32_t)route->ctrlid |
			((uint32_t)route->sqid << 16);

	/* ---- dword 23: INI JFR jetty token ---- */
	slot->jetty_token = route->jetty_token;

	/* ---- payload keyed SGL ---- */
	out->nsgl = 0;
	if (nsegs == 0) {
		/* DPTR stays zeroed from the fill (nvme_hcomm.c:5064). */
		out->send_len = SQE96_SIZE;
		return 0;
	}

	if (nsegs == 1) {
		/* CONTIG / collapsed single segment: one keyed data block in
		 * the DPTR (nvme_hcomm_build_contig_request / set_count count==1).
		 * Keyed word assembled by value and stored once (drop 7, task
		 * 4R-f finding B): the previous read-modify-write bitfield
		 * sequence lost the key/length lanes under opc. */
		struct sqe96_sgl_desc *d = &slot->cmd.dptr_sgl1;
		uint64_t kw;

		kw = sqe96_keyed_word(segs[0].length, segs[0].token_id);
		memset(&d->keyed, 0, sizeof(d->keyed));
		memcpy(&d->keyed, &kw, sizeof(kw));
		d->address       = segs[0].address;
		out->send_len = SQE96_SIZE;	/* single-seg sends base only */
		return 0;
	}

	/* nsegs >= 2: N keyed descriptors in the tail array, DPTR carries
	 * the LAST_SEGMENT+OFFSET count descriptor (length = N*16 — the
	 * single count source; TGT derives N = length/16).  The descriptor
	 * is written via the UNKEYED view (nvme_hcomm_sgl_set_count,
	 * nvme_hcomm.c:4663-4668): length is a full u32 at +8,
	 * reserved[3] zero — not the 24-bit keyed field. */
	for (i = 0; i < nsegs; i++) {
		struct sqe96_sgl_desc *d = &slot->sgl[i];
		uint64_t kw;

		/* Value-assembled keyed word, single 8B store (drop 7, task
		 * 4R-f finding B): on-device (opc) the former bitfield RMW
		 * sequence lost the length(u24) and key(u32) lanes of the
		 * LAST tail descriptor while address and the type nibble
		 * landed — the byte-diff signature was exactly
		 * length[1]/key[0..1] == 0 (write_max16seg). */
		kw = sqe96_keyed_word(segs[i].length, segs[i].token_id);
		memset(&d->keyed, 0, sizeof(d->keyed));
		memcpy(&d->keyed, &kw, sizeof(kw));
		d->address       = segs[i].address;
	}

	{
		struct sqe96_sgl_desc *d = &slot->cmd.dptr_sgl1;
		memset(&d->unkeyed, 0, sizeof(d->unkeyed));
		d->unkeyed.type    = SGL_TYPE_LAST_SEGMENT;
		d->unkeyed.subtype = SGL_SUBTYPE_OFFSET;
		d->unkeyed.length  = nsegs * 16u;
		d->address         = 0;
	}

	out->nsgl = nsegs;
	out->send_len = SQE96_SIZE + nsegs * 16u;
	return 0;
}
