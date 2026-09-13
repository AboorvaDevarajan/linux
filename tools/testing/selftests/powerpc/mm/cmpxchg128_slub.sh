#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Userspace checks for powerpc 128-bit SLUB cmpxchg on a real POWER machine.
# Expects a kernel built with CONFIG_PPC_CMPXCHG128_SELFTEST=y.

ksft_skip=4
fails=0
skips=0

pass() { echo "ok: $*"; }
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
skip() { echo "SKIP: $*"; skips=$((skips + 1)); }

if ! grep -q '^processor' /proc/cpuinfo 2>/dev/null && \
   ! grep -qi power /proc/cpuinfo 2>/dev/null; then
	echo "not a PowerPC system" >&2
	exit "$ksft_skip"
fi

cpu=$(grep -m1 '^cpu' /proc/cpuinfo | sed 's/^cpu[[:space:]]*:[[:space:]]*//')
echo "cpu: ${cpu:-unknown}"
echo "uname: $(uname -a)"

# PPC_FEATURE2_ARCH_2_07 (ISA 2.07 / POWER8+)
hwcap2=0
if aux=$(LD_SHOW_AUXV=1 /bin/true 2>/dev/null | awk '/AT_HWCAP2/ {print $2; exit}'); then
	hwcap2=$((aux))
fi
isa207=0
if (( hwcap2 & 0x80000000 )); then
	isa207=1
	pass "AT_HWCAP2 has PPC_FEATURE2_ARCH_2_07 (POWER8+)"
else
	echo "AT_HWCAP2=0x$(printf '%x' "$hwcap2") (no ARCH_2_07)"
fi

if [[ "$isa207" -eq 0 ]] && echo "$cpu" | grep -qiE 'POWER(8|9|10|11)'; then
	isa207=1
	pass "cpu string looks like POWER8+"
fi

dmesg_out=""
if dmesg_out=$(dmesg 2>/dev/null); then
	:
elif [[ -r /dev/kmsg ]]; then
	dmesg_out=$(dd if=/dev/kmsg bs=4096 count=256 2>/dev/null || true)
else
	skip "cannot read dmesg (need CAP_SYSLOG); boot selftest not checked"
fi

if [[ -n "$dmesg_out" ]]; then
	if echo "$dmesg_out" | grep -q 'cmpxchg128: .*failed'; then
		fail "boot selftest reported failure"
		echo "$dmesg_out" | grep 'cmpxchg128:' >&2
	elif echo "$dmesg_out" | grep -q 'cmpxchg128: tests passed'; then
		pass "boot selftest: cmpxchg128: tests passed"
		if [[ "$isa207" -eq 0 ]]; then
			fail "selftest passed but CPU does not look like POWER8+"
		fi
	elif echo "$dmesg_out" | grep -q 'cmpxchg128: skipped'; then
		pass "boot selftest skipped (CPU lacks ISA 2.07)"
		if [[ "$isa207" -eq 1 ]]; then
			fail "selftest skipped on a POWER8+ CPU (feature gate wrong?)"
		fi
	else
		skip "no cmpxchg128 boot selftest in dmesg (enable CONFIG_PPC_CMPXCHG128_SELFTEST)"
	fi

	if echo "$dmesg_out" | grep -q 'slub_test:.*test_freelist_aba'; then
		if echo "$dmesg_out" | grep -E 'slub_test:.*test_freelist_aba' | grep -qi fail; then
			fail "kunit test_freelist_aba failed"
		else
			pass "kunit test_freelist_aba present in dmesg"
		fi
	fi
fi

# SLUB_STATS: /sys/kernel/slab/*/cmpxchg_double_fail
if [[ -d /sys/kernel/slab ]]; then
	found=0
	nonzero=0
	for f in /sys/kernel/slab/*/cmpxchg_double_fail; do
		[[ -e "$f" ]] || continue
		found=1
		val=$(cat "$f" 2>/dev/null || echo 0)
		if [[ "$val" != 0 ]]; then
			nonzero=$((nonzero + val))
		fi
	done
	if [[ "$found" -eq 1 ]]; then
		pass "cmpxchg_double_fail sysfs present (SLUB_STATS)"
		echo "cmpxchg_double_fail total=$nonzero (contention retries, not a failure)"
	else
		skip "no cmpxchg_double_fail sysfs (enable CONFIG_SLUB_STATS)"
	fi
else
	skip "/sys/kernel/slab missing"
fi

# Re-run slub_test via debugfs if available
kunit_run=/sys/kernel/debug/kunit/slub_test/run
kunit_res=/sys/kernel/debug/kunit/slub_test/results
if [[ -e "$kunit_run" ]]; then
	if echo rerun > "$kunit_run" 2>/dev/null; then
		if grep -q FAIL "$kunit_res" 2>/dev/null; then
			fail "kunit slub_test reported FAIL"
			grep -E 'FAIL|test_freelist_aba' "$kunit_res" >&2 || true
		else
			pass "kunit slub_test re-run ok"
			grep -E 'test_freelist_aba|not ok|ok' "$kunit_res" 2>/dev/null | tail -n 20
		fi
	else
		skip "cannot write $kunit_run (need root and CONFIG_KUNIT_DEBUGFS)"
	fi
else
	skip "no $kunit_run (CONFIG_KUNIT=y, CONFIG_SLUB_KUNIT_TEST=y, CONFIG_KUNIT_DEBUGFS=y)"
fi

dir=$(dirname "$0")
stress="$dir/cmpxchg128_slub_stress"
if [[ -x "$stress" ]]; then
	if "$stress"; then
		pass "cmpxchg128_slub_stress"
	else
		fail "cmpxchg128_slub_stress"
	fi
else
	skip "cmpxchg128_slub_stress binary not built"
fi

if [[ "$fails" -gt 0 ]]; then
	echo "$fails test(s) failed"
	exit 1
fi

if [[ "$skips" -gt 0 && "$fails" -eq 0 ]]; then
	echo "all executed checks passed ($skips skipped)"
fi

exit 0
