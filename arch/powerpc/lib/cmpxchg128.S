/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 128-bit cmpxchg using lqarx/stqcx (ISA 2.07 / POWER8+).
 *
 * Even-odd GPR pair required. On little-endian the even register is the
 * doubleword at EA+8 and the odd register is EA (see emulate_lq()).
 *
 * cmpxchg ABI (ELFv2 u128 args): r3 = ptr (16-byte aligned),
 * r4/r5 = old (lo/hi), r6/r7 = new. Return previous *ptr in r3/r4 (lo/hi).
 *
 * try ABI (pointer + u128): r3 = ptr, r4 = oldp, r5/r6 = new
 * (LE lo/hi, BE hi/lo). Return 1 in r3 on success; on failure store the
 * loaded value to *oldp and return 0.
 *
 * LE remaps *new* into ISA even/odd order before the reservation, not
 * between lqarx and stqcx.
 */

#include <linux/export.h>
#include <asm/ppc_asm.h>
#include <asm/ppc-opcode.h>

.macro CMPXCHG128_BODY barriers
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	mr	r10, r6			/* lo */
	mr	r6, r7			/* even RS = hi */
	mr	r7, r10			/* odd RS+1 = lo */
#endif
	.if \barriers
#ifdef CONFIG_SMP
	sync				/* PPC_ATOMIC_ENTRY_BARRIER */
#endif
	.endif
1:	PPC_LQARX(8, 0, 3, 0)
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	cmpld	cr0, r9, r4		/* lo at EA */
	bne-	2f
	cmpld	cr1, r8, r5		/* hi at EA+8 */
	bne-	cr1, 2f
#else
	cmpld	cr0, r8, r4
	bne-	2f
	cmpld	cr1, r9, r5
	bne-	cr1, 2f
#endif
	PPC_STQCX(6, 0, 3)
	bne-	1b
	.if \barriers
#ifdef CONFIG_SMP
	sync				/* PPC_ATOMIC_EXIT_BARRIER */
#endif
	.endif
2:
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	mr	r3, r9
	mr	r4, r8
#else
	mr	r3, r8
	mr	r4, r9
#endif
	blr
.endm

.macro TRY_CMPXCHG128_BODY barriers
	mr	r12, r4			/* oldp */
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	mr	r7, r5			/* new lo → odd; r6 already hi */
#else
	mr	r7, r6			/* new lo */
	mr	r6, r5			/* new hi */
#endif
	ld	r4, 0(r12)
	ld	r5, 8(r12)
	.if \barriers
#ifdef CONFIG_SMP
	sync
#endif
	.endif
1:	PPC_LQARX(8, 0, 3, 0)
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	cmpld	cr0, r9, r4
	bne-	2f
	cmpld	cr1, r8, r5
	bne-	cr1, 2f
#else
	cmpld	cr0, r8, r4
	bne-	2f
	cmpld	cr1, r9, r5
	bne-	cr1, 2f
#endif
	PPC_STQCX(6, 0, 3)
	bne-	1b
	.if \barriers
#ifdef CONFIG_SMP
	sync
#endif
	.endif
	li	r3, 1
	blr
2:
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	std	r9, 0(r12)
	std	r8, 8(r12)
#else
	std	r8, 0(r12)
	std	r9, 8(r12)
#endif
	li	r3, 0
	blr
.endm

_GLOBAL(__cmpxchg_u128)
	CMPXCHG128_BODY 1
_ASM_NOKPROBE_SYMBOL(__cmpxchg_u128)
EXPORT_SYMBOL_GPL(__cmpxchg_u128)

_GLOBAL(__cmpxchg_u128_local)
	CMPXCHG128_BODY 0
_ASM_NOKPROBE_SYMBOL(__cmpxchg_u128_local)
EXPORT_SYMBOL_GPL(__cmpxchg_u128_local)

_GLOBAL(__try_cmpxchg_u128)
	TRY_CMPXCHG128_BODY 1
_ASM_NOKPROBE_SYMBOL(__try_cmpxchg_u128)
EXPORT_SYMBOL_GPL(__try_cmpxchg_u128)

_GLOBAL(__try_cmpxchg_u128_local)
	TRY_CMPXCHG128_BODY 0
_ASM_NOKPROBE_SYMBOL(__try_cmpxchg_u128_local)
EXPORT_SYMBOL_GPL(__try_cmpxchg_u128_local)
