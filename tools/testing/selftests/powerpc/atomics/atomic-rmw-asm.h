/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SELFTEST_PPC_ATOMIC_RMW_ASM_H
#define _SELFTEST_PPC_ATOMIC_RMW_ASM_H

/*
 * Userspace copies of the kernel fully-ordered LL/SC sequences so the
 * selftests can check correctness and compare hwsync vs lwsync+isync
 * without building a kernel module.
 */

#ifdef __powerpc64__
#define PPC_LLARX	"ldarx"
#define PPC_STLCX	"stdcx."
#define PPC_CMP	"cmpd"
typedef unsigned long atomic_word_t;
#else
#define PPC_LLARX	"lwarx"
#define PPC_STLCX	"stwcx."
#define PPC_CMP	"cmpw"
typedef unsigned int atomic_word_t;
#endif

enum atomic_barrier_kind {
	ATOMIC_BARRIER_HWSYNC = 0,
	ATOMIC_BARRIER_LWSYNC_ISYNC,
	ATOMIC_BARRIER_NONE,
};

static inline void atomic_entry_barrier(enum atomic_barrier_kind kind)
{
	switch (kind) {
	case ATOMIC_BARRIER_HWSYNC:
		asm volatile("sync" ::: "memory");
		break;
	case ATOMIC_BARRIER_LWSYNC_ISYNC:
		asm volatile("lwsync" ::: "memory");
		break;
	case ATOMIC_BARRIER_NONE:
		break;
	}
}

static inline void atomic_exit_barrier(enum atomic_barrier_kind kind)
{
	switch (kind) {
	case ATOMIC_BARRIER_HWSYNC:
		asm volatile("sync" ::: "memory");
		break;
	case ATOMIC_BARRIER_LWSYNC_ISYNC:
		asm volatile("isync" ::: "memory");
		break;
	case ATOMIC_BARRIER_NONE:
		break;
	}
}

/*
 * Fully ordered atomic_add_return: entry barrier, LL/SC add, exit
 * barrier. Matches arch_atomic*_add_return() built from a relaxed
 * loop plus PPC_ATOMIC_* fences.
 */
static inline atomic_word_t
atomic_add_return_barriers(atomic_word_t a, atomic_word_t *v,
			   enum atomic_barrier_kind kind)
{
	atomic_word_t t;

	atomic_entry_barrier(kind);
	asm volatile(
"1:	" PPC_LLARX "	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	" PPC_STLCX "	%0,0,%3\n"
"	bne-	1b\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "xer");
	atomic_exit_barrier(kind);

	return t;
}

/*
 * Fully ordered cmpxchg. The exit barrier is after a successful
 * stcx and before label 2, so a failed compare skips it — the same
 * layout as __cmpxchg_u64().
 */
static inline atomic_word_t
cmpxchg_barriers(atomic_word_t *p, atomic_word_t old, atomic_word_t new,
		 enum atomic_barrier_kind kind)
{
	atomic_word_t prev;

	if (kind == ATOMIC_BARRIER_LWSYNC_ISYNC) {
		asm volatile(
		"	lwsync\n"
"1:		" PPC_LLARX "	%0,0,%2\n"
"		" PPC_CMP "	0,%0,%3\n"
"		bne-	2f\n"
"		" PPC_STLCX "	%4,0,%2\n"
"		bne-	1b\n"
		"	isync\n"
"2:"
		: "=&r" (prev), "+m" (*p)
		: "r" (p), "r" (old), "r" (new)
		: "cc", "memory");
	} else if (kind == ATOMIC_BARRIER_HWSYNC) {
		asm volatile(
		"	sync\n"
"1:		" PPC_LLARX "	%0,0,%2\n"
"		" PPC_CMP "	0,%0,%3\n"
"		bne-	2f\n"
"		" PPC_STLCX "	%4,0,%2\n"
"		bne-	1b\n"
		"	sync\n"
"2:"
		: "=&r" (prev), "+m" (*p)
		: "r" (p), "r" (old), "r" (new)
		: "cc", "memory");
	} else {
		asm volatile(
"1:		" PPC_LLARX "	%0,0,%2\n"
"		" PPC_CMP "	0,%0,%3\n"
"		bne-	2f\n"
"		" PPC_STLCX "	%4,0,%2\n"
"		bne-	1b\n"
"2:"
		: "=&r" (prev), "+m" (*p)
		: "r" (p), "r" (old), "r" (new)
		: "cc", "memory");
	}

	return prev;
}

/*
 * One-asm-block form used by the microbenchmark so the compiler
 * cannot split the barriers away from the LL/SC loop.
 */
static inline atomic_word_t
atomic_add_return_hwsync(atomic_word_t a, atomic_word_t *v)
{
	atomic_word_t t;

	asm volatile(
	"	sync\n"
"1:	" PPC_LLARX "	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	" PPC_STLCX "	%0,0,%3\n"
"	bne-	1b\n"
	"	sync\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "memory", "xer");

	return t;
}

static inline atomic_word_t
atomic_add_return_lwsync_isync(atomic_word_t a, atomic_word_t *v)
{
	atomic_word_t t;

	asm volatile(
	"	lwsync\n"
"1:	" PPC_LLARX "	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	" PPC_STLCX "	%0,0,%3\n"
"	bne-	1b\n"
	"	isync\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "memory", "xer");

	return t;
}

static inline atomic_word_t
atomic_add_return_relaxed_loop(atomic_word_t a, atomic_word_t *v)
{
	atomic_word_t t;

	asm volatile(
"1:	" PPC_LLARX "	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	" PPC_STLCX "	%0,0,%3\n"
"	bne-	1b\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "xer");

	return t;
}

#endif /* _SELFTEST_PPC_ATOMIC_RMW_ASM_H */
