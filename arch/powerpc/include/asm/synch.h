/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_SYNCH_H 
#define _ASM_POWERPC_SYNCH_H 
#ifdef __KERNEL__

#include <asm/cputable.h>
#include <asm/feature-fixups.h>
#include <asm/ppc-opcode.h>

#ifndef __ASSEMBLER__
extern unsigned int __start___lwsync_fixup, __stop___lwsync_fixup;
extern void do_lwsync_fixups(unsigned long value, void *fixup_start,
			     void *fixup_end);

static inline void eieio(void)
{
	if (IS_ENABLED(CONFIG_BOOKE))
		__asm__ __volatile__ ("mbar" : : : "memory");
	else
		__asm__ __volatile__ ("eieio" : : : "memory");
}

static inline void isync(void)
{
	__asm__ __volatile__ ("isync" : : : "memory");
}

static inline void ppc_after_tlbiel_barrier(void)
{
	asm volatile("ptesync": : :"memory");
	/*
	 * POWER9, POWER10 need a cp_abort after tlbiel to ensure the copy is
	 * invalidated correctly. If this is not done, the paste can take data
	 * from the physical address that was translated at copy time.
	 *
	 * POWER9 in practice does not need this, because address spaces with
	 * accelerators mapped will use tlbie (which does invalidate the copy)
	 * to invalidate translations. It's not possible to limit POWER10 this
	 * way due to local copy-paste.
	 *
	 * POWER12 does not need it.
	 */
	asm volatile(ASM_FTR_IF(PPC_CP_ABORT, "", %0, %1) :
		: "i" (CPU_FTR_ARCH_31|CPU_FTR_ARCH_32), "i" (CPU_FTR_ARCH_31)
		: "memory");
}
#endif /* __ASSEMBLER__ */

#if defined(__powerpc64__)
#    define LWSYNC	lwsync
#elif defined(CONFIG_PPC_E500)
#    define LWSYNC					\
	START_LWSYNC_SECTION(96);			\
	sync;						\
	MAKE_LWSYNC_SECTION_ENTRY(96, __lwsync_fixup);
#else
#    define LWSYNC	sync
#endif

#ifdef CONFIG_SMP
#define __PPC_ACQUIRE_BARRIER				\
	START_LWSYNC_SECTION(97);			\
	isync;						\
	MAKE_LWSYNC_SECTION_ENTRY(97, __lwsync_fixup);
#define PPC_ACQUIRE_BARRIER	 "\n" stringify_in_c(__PPC_ACQUIRE_BARRIER)
#define PPC_RELEASE_BARRIER	 stringify_in_c(LWSYNC) "\n"
#ifdef __powerpc64__
/*
 * Fully ordered RMWs: lwsync ; larx/stcx. ; isync on POWER9+
 * (CPU_FTR_ARCH_300). POWER8 and below keep sync/sync.
 *
 * One ppc64 binary boots P7 through P11, so this is a CPU feature
 * alternative, not #ifdef __powerpc64__. POWER7 is where
 * b97021f855 found that isync after stwcx. did not make the store
 * globally visible. Nest hwsync cost (the reason to drop two
 * hwsyncs) is a POWER8+ problem; this RFC stops at POWER9+ until
 * P8 is re-checked.
 *
 * This is not smp_store_release() + smp_load_acquire(). Those do not
 * implement a full barrier. Boqun's 2015 change switched the entry
 * side from lwsync to sync for that reason.
 *
 * The extra ingredient is stcx. itself (Nicholas Piggin, April 2024):
 *
 * - lwsync does not order store-load, so larx may run before a prior
 *   store is globally visible. That is OK: stcx. is a store, so
 *   lwsync orders it against all prior accesses, and stcx. fails if
 *   the line changed after larx. When larx executes is not a concern.
 * - isync after a successful stcx. stops later instructions until
 *   stcx. has executed. A completed stcx. is visible to the system,
 *   so later loads/stores cannot become visible ahead of the RMW or
 *   of earlier stores already ordered by lwsync.
 *
 * smp_mb() remains hwsync. Failed cmpxchg still branches to label 2
 * before the exit barrier.
 *
 * Nested FTR labels (80/81) because ENTRY and EXIT share one asm.
 * IF/ELSE are both 4-byte ops (lwsync vs sync, isync vs sync).
 * 0x20000 is CPU_FTR_ARCH_300; gas cannot take the UL C token.
 */
#define PPC_ATOMIC_ENTRY_BARRIER					\
	"\n" stringify_in_c(BEGIN_FTR_SECTION_NESTED(80))		\
	"lwsync\n"							\
	stringify_in_c(FTR_SECTION_ELSE_NESTED(80))			\
	"sync\n"							\
	stringify_in_c(ALT_FTR_SECTION_END_NESTED_IFSET(0x20000, 80))
#define PPC_ATOMIC_EXIT_BARRIER						\
	"\n" stringify_in_c(BEGIN_FTR_SECTION_NESTED(81))		\
	"isync\n"							\
	stringify_in_c(FTR_SECTION_ELSE_NESTED(81))			\
	"sync\n"							\
	stringify_in_c(ALT_FTR_SECTION_END_NESTED_IFSET(0x20000, 81))
#ifndef __ASSEMBLER__
#if CPU_FTR_ARCH_300 != 0x0000000000020000UL
#error PPC_ATOMIC FTR mask 0x20000 must match CPU_FTR_ARCH_300
#endif
#endif
#else
/* 32-bit keeps two hwsyncs until the isync cost is measured. */
#define PPC_ATOMIC_ENTRY_BARRIER "\n" stringify_in_c(sync) "\n"
#define PPC_ATOMIC_EXIT_BARRIER	 "\n" stringify_in_c(sync) "\n"
#endif
#else
#define PPC_ACQUIRE_BARRIER
#define PPC_RELEASE_BARRIER
#define PPC_ATOMIC_ENTRY_BARRIER
#define PPC_ATOMIC_EXIT_BARRIER
#endif

#endif /* __KERNEL__ */
#endif	/* _ASM_POWERPC_SYNCH_H */
