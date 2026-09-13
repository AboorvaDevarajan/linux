#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# End-to-end runner for the powerpc SLUB 128-bit cmpxchg series on a real
# POWER machine (POWER8 / POWER9 / POWER10 / POWER11). Run after booting a
# kernel that includes this series.
#
# Usage (as root, on the DUT):
#   make -C tools/testing/selftests TARGETS=powerpc/mm
#   sudo ./tools/testing/selftests/powerpc/mm/run_cmpxchg128_e2e.sh
#
# Optional:
#   STRESS_NG_SECS=30  also run stress-ng --malloc if installed
#   SKIP_STRESS=1      skip the pthread malloc hammer
#
# Kernel config expected on the DUT:
#   CONFIG_PPC64=y
#   CONFIG_PPC_CMPXCHG128_SELFTEST=y
#   CONFIG_KUNIT=y
#   CONFIG_KUNIT_DEBUGFS=y
#   CONFIG_SLUB_KUNIT_TEST=y
#   CONFIG_SLUB_STATS=y          # optional, cmpxchg_double_fail sysfs
#   # Do NOT select CONFIG_ARCH_SUPPORTS_INT128 (GCC ICE on ppc64le).

set -u

ksft_skip=4
dir=$(cd "$(dirname "$0")" && pwd)

echo "=== powerpc cmpxchg128 / SLUB freelist e2e ==="
echo "time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "host: $(uname -a)"
echo
echo "--- /proc/cpuinfo (first cpu) ---"
awk 'BEGIN{RS=""; ORS="\n\n"} NR==1' /proc/cpuinfo 2>/dev/null || cat /proc/cpuinfo | head -n 20
echo

if [[ "$(id -u)" -ne 0 ]]; then
	echo "warning: not root; dmesg and kunit re-run may be skipped" >&2
fi

echo "--- kselftest: cmpxchg128_slub.sh ---"
if [[ -x "$dir/cmpxchg128_slub.sh" ]]; then
	"$dir/cmpxchg128_slub.sh"
	rc=$?
	if [[ "$rc" -eq "$ksft_skip" ]]; then
		echo "cmpxchg128_slub.sh skipped"
	elif [[ "$rc" -ne 0 ]]; then
		echo "cmpxchg128_slub.sh failed (rc=$rc)" >&2
		exit "$rc"
	fi
else
	echo "cmpxchg128_slub.sh missing" >&2
	exit 1
fi

if [[ "${SKIP_STRESS:-0}" != 1 ]]; then
	echo
	echo "--- extra malloc stress (${STRESS_SECS:-10}s wall hint) ---"
	if [[ -x "$dir/cmpxchg128_slub_stress" ]]; then
		"$dir/cmpxchg128_slub_stress" || exit 1
	else
		echo "build the stress binary: make -C tools/testing/selftests TARGETS=powerpc/mm"
	fi
fi

if [[ -n "${STRESS_NG_SECS:-}" ]] && command -v stress-ng >/dev/null; then
	echo
	echo "--- stress-ng --malloc ${STRESS_NG_SECS}s ---"
	stress-ng --malloc 0 --timeout "${STRESS_NG_SECS}s" --metrics-brief || {
		echo "stress-ng failed" >&2
		exit 1
	}
elif [[ -n "${STRESS_NG_SECS:-}" ]]; then
	echo "stress-ng not installed; skip extra malloc stress"
fi

echo
echo "--- post-stress dmesg (oops / cmpxchg128) ---"
if dmesg_out=$(dmesg 2>/dev/null); then
	echo "$dmesg_out" | grep -E 'cmpxchg128:|Oops:|BUG:|slub_test' | tail -n 40 || true
	if echo "$dmesg_out" | grep -qE 'Oops:|Unable to handle kernel'; then
		echo "FAIL: kernel oops in dmesg" >&2
		exit 1
	fi
fi

echo
echo "e2e completed"
exit 0
