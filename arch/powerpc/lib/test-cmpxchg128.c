// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/printk.h>

static int __init test_cmpxchg128(void)
{
	static u128 val __aligned(16);
	u128 old, new, ret;
	int fails = 0;

	if (!system_has_cmpxchg128()) {
		pr_info("cmpxchg128: skipped (need ISA 2.07 / POWER8+)\n");
		return 0;
	}

	val = 0;
	old = 0;
	new = ((u128)0x2222222222222222ULL << 64) | 0x1111111111111111ULL;
	if (!try_cmpxchg128(&val, &old, new) || val != new || old != 0) {
		pr_err("cmpxchg128: success path failed\n");
		fails++;
	}

	old = 0;
	if (try_cmpxchg128(&val, &old, 0) || old != new) {
		pr_err("cmpxchg128: mismatch path failed\n");
		fails++;
	}

	old = new;
	ret = cmpxchg128(&val, old, 0);
	if (ret != old || val != 0) {
		pr_err("cmpxchg128: cmpxchg128() failed\n");
		fails++;
	}

	if (fails)
		pr_err("cmpxchg128: %d test(s) failed\n", fails);
	else
		pr_info("cmpxchg128: tests passed\n");

	return 0;
}
late_initcall(test_cmpxchg128);
