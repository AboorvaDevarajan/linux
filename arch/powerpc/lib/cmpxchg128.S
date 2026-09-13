/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 128-bit cmpxchg using lqarx/stqcx (ISA 2.07 / POWER8+).
 *
 * Even-odd GPR pair required. On little-endian the even register is the
 * doubleword at EA+8 and the odd register is EA (see emulate_lq()).
 * ELFv2 passes u128 as lo/hi in consecutive GPRs, so LE swaps for
 * compare, store, and return.
 *
 * ABI: r3 = ptr (16-byte aligned), r4/r5 = old (lo/hi), r6/r7 = new.
 * Return previous *ptr in r3/r4 (lo/hi).
 */

#include <linux/export.h>
#include <asm/ppc_asm.h>
#include <asm/ppc-opcode.h>

.macro CMPXCHG128_BODY barriers
	.if \barriers
#ifdef CONFIG_SMP
	sync
#endif
	.endif
1:	PPC_LQARX(8, 0, 3, 0)
#ifdef CONFIG_CPU_LITTLE_ENDIAN
	cmpld	cr0, r9, r4		/* lo at EA */
	bne-	2f
	cmpld	cr1, r8, r5		/* hi at EA+8 */
	bne-	cr1, 2f
	mr	r10, r7			/* even RS = hi */
	mr	r11, r6			/* odd RS+1 = lo */
	PPC_STQCX(10, 0, 3)
#else
	cmpld	cr0, r8, r4
	bne-	2f
	cmpld	cr1, r9, r5
	bne-	cr1, 2f
	PPC_STQCX(6, 0, 3)
#endif
	bne-	1b
	.if \barriers
#ifdef CONFIG_SMP
	sync
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

_GLOBAL(__cmpxchg_u128)
	CMPXCHG128_BODY 1
_ASM_NOKPROBE_SYMBOL(__cmpxchg_u128)
EXPORT_SYMBOL_GPL(__cmpxchg_u128)

_GLOBAL(__cmpxchg_u128_local)
	CMPXCHG128_BODY 0
_ASM_NOKPROBE_SYMBOL(__cmpxchg_u128_local)
EXPORT_SYMBOL_GPL(__cmpxchg_u128_local)
