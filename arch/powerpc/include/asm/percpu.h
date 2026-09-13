/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_PERCPU_H_
#define _ASM_POWERPC_PERCPU_H_
#ifdef __powerpc64__

/*
 * Same as asm-generic/percpu.h, except that we store the per cpu offset
 * in the paca. Based on the x86-64 implementation.
 */

#ifdef CONFIG_SMP

#define __my_cpu_offset local_paca->data_offset

#endif /* CONFIG_SMP */
#endif /* __powerpc64__ */

#if defined(CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK) && defined(CONFIG_SMP)
#include <linux/jump_label.h>
DECLARE_STATIC_KEY_FALSE(__percpu_first_chunk_is_paged);

#define percpu_first_chunk_is_paged	\
		(static_key_enabled(&__percpu_first_chunk_is_paged.key))
#else
#define percpu_first_chunk_is_paged	false
#endif

#ifndef __ASSEMBLER__
#include <linux/preempt.h>
#include <asm/cmpxchg.h>

/*
 * Keep the per-CPU address stable across the lwarx/stwcx sequence.
 * The RMW itself is irq/NMI-safe; do not use irq-disable plus a
 * non-atomic load/store (the generic fallback).
 */
#define arch_this_cpu_cmpxchg(pcp, oval, nval)				\
({									\
	typeof(pcp) *ptr__;						\
	typeof(pcp) ret__;						\
									\
	preempt_disable_notrace();					\
	ptr__ = raw_cpu_ptr(&(pcp));					\
	ret__ = arch_cmpxchg_relaxed(ptr__, oval, nval);		\
	preempt_enable_notrace();					\
	ret__;								\
})

#define this_cpu_cmpxchg_1(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg_2(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg_4(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#ifdef CONFIG_PPC64
#define this_cpu_cmpxchg_8(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg64(pcp, o, n)	this_cpu_cmpxchg_8(pcp, o, n)
#endif
#endif /* __ASSEMBLER__ */

#include <asm-generic/percpu.h>

#include <asm/paca.h>

#endif /* _ASM_POWERPC_PERCPU_H_ */
