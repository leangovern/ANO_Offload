/*   SPDX-License-Identifier: BSD-3-Clause
 *
 *   Standalone, dependency-free description of the 96B HCOMM SQE wire
 *   layout + 352B slot.  This is an INDEPENDENT re-encoding of the wire
 *   format for the aiv_offload golden-reference harness: it does NOT
 *   include spdk/nvme_spec.h, lib/nvme/nvme_hcomm_sqe_96b.h, or any
 *   hcomm/acl symbol.  Field offsets and bit packing are transcribed
 *   from the authoritative layout anchored in
 *   lib/nvme/nvme_hcomm_sqe_96b.h (STATIC_ASSERTs) and
 *   include/spdk/nvme_spec.h (struct spdk_nvme_cmd /
 *   struct spdk_nvme_sgl_descriptor).
 *
 *   Wire layout (little-endian host), byte offsets:
 *     0  ~ 63  : standard NVMe command (struct spdk_nvme_cmd)
 *     64 ~ 87  : reserved, 6 dwords, always zero
 *     88       : routing word DW22 — bit 0~15 = ctrlid, bit 16~31 = sqid
 *     92       : DW23 — INI JFR jetty token (NVME_HCOMM_INI_JFR_TOKEN)
 *     96 ~ 351 : tail SGL descriptor array, 16 x 16B keyed data blocks
 *
 *   DPTR (offset 24) carries one spdk_nvme_sgl_descriptor:
 *     single segment  -> KEYED_DATA_BLOCK (type 0x4, subtype 0x0)
 *     multi segment   -> LAST_SEGMENT+OFFSET (type 0x3, subtype 0x1)
 *                        written via the UNKEYED view: full u32 length
 *                        at +8 = N*16 (the single count source; TGT
 *                        derives N = length/16), reserved[3] zero
 *     no payload      -> all zero (fill helper zeroes the whole slot)
 *
 *   Descriptor wire layout (struct spdk_nvme_sgl_descriptor):
 *     keyed view:    +0 address(u64) +8 length(u24) +11 key(u32) +15 nibbles
 *     unkeyed view:  +0 address(u64) +8 length(u32) +12 rsvd[3]  +15 nibbles
 *     (nvme_spec.h:614-637; LAST_SEGMENT descs are filled via the
 *      unkeyed union member, so length occupies bytes 8-11 as a u32)
 */

#ifndef AIV_OFFLOAD_SQE96_LAYOUT_H
#define AIV_OFFLOAD_SQE96_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQE96_SIZE		96u	/* NVME_HCOMM_SQE_96B_SIZE  */
#define SQE96_MAX_SGL		16u	/* NVME_HCOMM_MAX_SGL      */
#define SQE96_SLOT_SIZE		(SQE96_SIZE + SQE96_MAX_SGL * 16u) /* 352B */
#define SQE96_INI_JFR_TOKEN	0xACFEu	/* NVME_HCOMM_INI_JFR_TOKEN */

/* spdk_nvme_sgl_descriptor_type */
#define SGL_TYPE_LAST_SEGMENT		0x3u
#define SGL_TYPE_KEYED_DATA_BLOCK	0x4u
/* spdk_nvme_sgl_descriptor_subtype */
#define SGL_SUBTYPE_OFFSET		0x1u
#define SGL_SUBTYPE_ADDRESS		0x0u

/* spdk_nvme_psdt_value */
#define NVME_PSDT_SGL_MPTR_CONTIG	0x1u

#pragma pack(push, 1)
/* Mirror of struct spdk_nvme_sgl_descriptor (16B, LE), dual-view union
 * exactly like nvme_spec.h:614-637.  Keyed data blocks (tail array,
 * single-seg DPTR) are filled via the `keyed` member; the
 * LAST_SEGMENT+OFFSET count descriptor is filled via the `unkeyed`
 * member, where length is a FULL u32 at bytes 8-11 (reserved[3]
 * follows) — NOT the 24-bit keyed field. */
struct sqe96_sgl_desc {
	uint64_t	address;	/* +0  user VA (or 0 for LAST_SEGMENT) */
	union {
		struct {
			uint32_t	length;		/* +8  full u32 (unkeyed) */
			uint8_t		reserved[3];	/* +12 */
			uint8_t		subtype	: 4;	/* +15 bits 0..3 */
			uint8_t		type	: 4;	/* +15 bits 4..7 */
		} unkeyed;
		struct {
			uint64_t	length	: 24;	/* +8  bits 0..23 */
			uint64_t	key	: 32;	/* +11 bits 24..55 (tokenId) */
			uint64_t	subtype	: 4;	/* +15 bits 0..3 */
			uint64_t	type	: 4;	/* +15 bits 4..7 */
		} keyed;
	};
};

/* Mirror of struct spdk_nvme_cmd (64B).  Dwords 6~9 (offset 24) are the
 * data pointer; we expose only the SGL view. */
struct sqe96_nvme_cmd {
	uint16_t	opc	: 8;	/* DW0 bit  0..7   */
	uint16_t	fuse	: 2;	/* DW0 bit  8..9   */
	uint16_t	rsvd1	: 4;	/* DW0 bit 10..13  */
	uint16_t	psdt	: 2;	/* DW0 bit 14..15  */
	uint16_t	cid;		/* DW0 bit 16..31  */
	uint32_t	nsid;		/* DW1             */
	uint32_t	rsvd2;		/* DW2             */
	uint32_t	rsvd3;		/* DW3             */
	uint64_t	mptr;		/* DW4~5           */
	struct sqe96_sgl_desc dptr_sgl1; /* DW6~9, offset 24 */
	uint32_t	cdw10;		/* DW10            */
	uint32_t	cdw11;		/* DW11            */
	uint32_t	cdw12;		/* DW12            */
	uint32_t	cdw13;		/* DW13            */
	uint32_t	cdw14;		/* DW14            */
	uint32_t	cdw15;		/* DW15            */
};

/* The full 96B SQE + tail array = the 352B slot mirror. */
struct sqe96_slot {
	struct sqe96_nvme_cmd	cmd;		/* offset 0   */
	uint32_t		rsvd[6];	/* offset 64  */
	uint32_t		dword22;	/* offset 88  */
	uint32_t		jetty_token;	/* offset 92  */
	struct sqe96_sgl_desc	sgl[16];	/* offset 96  */
};
#pragma pack(pop)

/* Compile-time layout anchors — must match nvme_hcomm_sqe_96b.h. */
typedef char sqe96_assert_cmd_64[(sizeof(struct sqe96_nvme_cmd) == 64) ? 1 : -1];
typedef char sqe96_assert_desc_16[(sizeof(struct sqe96_sgl_desc) == 16) ? 1 : -1];
typedef char sqe96_assert_slot_352[(sizeof(struct sqe96_slot) == SQE96_SLOT_SIZE) ? 1 : -1];
typedef char sqe96_assert_dptr_off[(offsetof(struct sqe96_slot, cmd.dptr_sgl1) == 24) ? 1 : -1];
typedef char sqe96_assert_rsvd_64[(offsetof(struct sqe96_slot, rsvd) == 64) ? 1 : -1];
typedef char sqe96_assert_dw22_88[(offsetof(struct sqe96_slot, dword22) == 88) ? 1 : -1];
typedef char sqe96_assert_dw23_92[(offsetof(struct sqe96_slot, jetty_token) == 92) ? 1 : -1];
typedef char sqe96_assert_sgl_96[(offsetof(struct sqe96_slot, sgl) == 96) ? 1 : -1];

#ifdef __cplusplus
}
#endif

#endif /* AIV_OFFLOAD_SQE96_LAYOUT_H */
