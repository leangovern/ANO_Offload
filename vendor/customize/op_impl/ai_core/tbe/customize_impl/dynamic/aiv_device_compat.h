/*   SPDX-License-Identifier: BSD-3-Clause
 *
 *   AIV offload: device-compilation compatibility shim (drop 5).
 *
 *   Single adjudication point for the four device-mode findings from the
 *   on-site ccec attempt (task 4R):
 *
 *     (a) sqe96_build.c / kernel helpers need the __aicore__ attribute
 *         when compiled by ccec for the vector core;
 *     (b) aicore code has no libc — memset/memcpy do not exist there;
 *     (c) __gm__ pointers may not be cast to generic pointers (handled in
 *         aiv_sqe96_kernel.cpp via the GlobalTensor staging layer — not
 *         in this header);
 *     (d) cache-flush primitive spelling is version-dependent (handled in
 *         aiv_sqe96_kernel.cpp behind aiv_flush_gm — not in this header).
 *
 *   Strategy for (a)+(b): call-site text is UNCHANGED.  In aicore mode
 *   this header provides hand-rolled memset/memcpy replacements and
 *   macro-aliases the standard names onto them, plus SQE96_AICORE_FN for
 *   the function attribute.  In host mode it simply pulls in <string.h>
 *   and SQE96_AICORE_FN expands to nothing — the host build (gcc/g++)
 *   is byte-for-byte identical in behaviour to before.
 *
 *   __CCE_AICORE__ is predefined by ccec when compiling aicore code.
 */

#ifndef AIV_OFFLOAD_AIV_DEVICE_COMPAT_H
#define AIV_OFFLOAD_AIV_DEVICE_COMPAT_H

#ifdef __CCE_AICORE__

/* ---- aicore mode: no libc, hand-rolled memory ops ---- */

#include <stdint.h>

#define SQE96_AICORE_FN		__aicore__

/* Word-at-a-time memset: dst is naturally aligned at every call site in
 * sqe96_build.c (struct members / 352B slot buffer), but we stay safe by
 * doing a byte head/tail around a uint64_t body. */
static inline __aicore__ void *
aiv_memset_device(void *dst, int c, uint32_t n)
{
	uint8_t *p = (uint8_t *)dst;
	uint8_t v = (uint8_t)c;
	uint32_t i = 0;

	while (i < n && ((uintptr_t)(p + i) & 7u) != 0) {
		p[i] = v;
		i++;
	}
	{
		uint64_t w = v;
		uint64_t *q;

		w |= w << 8;
		w |= w << 16;
		w |= w << 32;
		q = (uint64_t *)(p + i);
		while (i + 8 <= n) {
			*q++ = w;
			i += 8;
		}
	}
	while (i < n) {
		p[i] = v;
		i++;
	}
	return dst;
}

/* Word-at-a-time memcpy with byte head/tail.  Call sites never overlap
 * (slot buffer <- locals), so no memmove semantics are needed. */
static inline __aicore__ void *
aiv_memcpy_device(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	uint32_t i = 0;

	if ((((uintptr_t)d | (uintptr_t)s) & 7u) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		while (i + 8 <= n) {
			*dw++ = *sw++;
			i += 8;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (i < n) {
		*d++ = *s++;
		i++;
	}
	return dst;
}

/* Macro-alias the libc names: the sqe96_build.c / kernel call sites keep
 * their original text; only the binding changes.  Guarded so a site that
 * truly wants the libc symbol (none in device code) can #undef first. */
#ifndef memset
#define memset(d, c, n)		aiv_memset_device((d), (c), (uint32_t)(n))
#endif
#ifndef memcpy
#define memcpy(d, s, n)		aiv_memcpy_device((d), (s), (uint32_t)(n))
#endif

#else /* !__CCE_AICORE__ — host mode (gcc / g++ incl. CPU emulation) */

#include <string.h>

#define SQE96_AICORE_FN

#endif /* __CCE_AICORE__ */

#endif /* AIV_OFFLOAD_AIV_DEVICE_COMPAT_H */
