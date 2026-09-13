/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 128-bit cmpxchg using lqarx/stqcx (ISA 2.07 / POWER8+).
 *
 * Must live in assembler: lqarx/stqcx require even-odd GPR pairs, and GCC
 * has no constraint for that. Inline asm that pins r4/r6-r9 fights the
 * ELFv2 u128 ABI (ptr in r3, old in r4/r5, new in r6/r7) and can livelock
 * SLUB's try_cmpxchg128() retry loop.
 *
 * ABI: r3 = ptr (16-byte aligned), r4/r5 = old, r6/r7 = new.
 * Return previous *ptr in r3/r4 (memory order).
 */

#include <linux/export.h>
#include <asm/ppc_asm.h>
#include <asm/ppc-opcode.h>

_GLOBAL(__cmpxchg_u128)
#ifdef CONFIG_SMP
	sync
#endif
1:	PPC_LQARX(8, 0, 3, 0)
	cmpld	cr0, r8, r4
	bne-	2f
	cmpld	cr1, r9, r5
	bne-	cr1, 2f
	PPC_STQCX(6, 0, 3)
	bne-	1b
#ifdef CONFIG_SMP
	sync
#endif
2:	mr	r3, r8
	mr	r4, r9
	blr
EXPORT_SYMBOL(__cmpxchg_u128)

_GLOBAL(__cmpxchg_u128_local)
1:	PPC_LQARX(8, 0, 3, 0)
	cmpld	cr0, r8, r4
	bne-	2f
	cmpld	cr1, r9, r5
	bne-	cr1, 2f
	PPC_STQCX(6, 0, 3)
	bne-	1b
2:	mr	r3, r8
	mr	r4, r9
	blr
EXPORT_SYMBOL(__cmpxchg_u128_local)
