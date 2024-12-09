/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A select_cpu_n scheduler.
 *
 * By default, it operates as a select_cpu_n global weighted vtime scheduler and can
 * be switched to FIFO scheduling. It also demonstrates the following niceties.
 *
 * - Statistics tracking how many tasks are queued to local and global dsq's.
 * - Termination notification for userspace.
 *
 * While very select_cpu_n, this scheduler should work reasonably well on CPUs with a
 * uniform L3 cache topology. While preemption is not implemented, the fact that
 * the scheduling queue is shared across all CPUs means that whatever is at the
 * front of the queue is likely to be executed fairly quickly given enough
 * number of CPUs. The FIFO scheduling mode may be beneficial to some workloads
 * but comes with the usual problems with FIFO scheduling where saturating
 * threads can easily drown out interactive ones.
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

s32 BPF_STRUCT_OPS(select_cpu_n_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	return 0;
}

SCX_OPS_DEFINE(select_cpu_n_ops,
	       .select_cpu		= (void *)select_cpu_n_select_cpu,
	       .name			= "select_cpu_n");
