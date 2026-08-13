#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Offline and online every thread of a POWER9 core, and dump the hotplug-path
# PSSCR[PLS] histogram printed by the kernel after each online.
#
# The C-level SPR restore in power9_idle_stop() can be left on (stock) or
# turned off for the hotplug path only. cpuidle wakeup is never gated.
#
# Usage:
#   p9-core-hotplug-spr.sh [--cpu N] [--iters N] [--hold SEC] [--restore 0|1|both]
#                         [--skip-spr N]
#   p9-hotplug-spr-collect.sh [--bisect] [--bisect-from N] [--bisect-to N] ...
#
# --skip-spr N  restore all C-level SPRs except number N (spr_restore forced on).
#               cat /sys/kernel/debug/powerpc/pnv_hotplug_idle/sprs for the map.
#               0 = omit none. 5 = ptcr (first suspect).
#
# Defaults: last present CPU whose core does not include CPU 0, 3 iters,
# 2s hold while offline, restore=both (stock then skip).
#
# How to read the dmesg line after online:
#   max_pls < deep_spr_loss  hardware never lost SPRs; skip is a no-op
#   max_pls >= deep_spr_loss && restore=0 && the CPU comes back
#                            hotplug did not need the C-level restore
#   restore=0 && crash/hang  restore is required at that PLS
#
# Offline the whole core. Per-core SPRs (PTCR/RPR/TSCR) are restored by the
# first thread out; a live sibling doing cpuidle would skip that work if we
# offlined only one thread.

set -euo pipefail

CPU=""
ITERS=3
HOLD=2
RESTORE=both
SKIP_SPR=0
SYSFS=/sys/devices/system/cpu
DBG=/sys/kernel/debug/powerpc/pnv_hotplug_idle

usage() {
	sed -n '2,24p' "$0" | sed 's/^# \?//'
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
	--cpu) CPU=$2; shift 2 ;;
	--iters) ITERS=$2; shift 2 ;;
	--hold) HOLD=$2; shift 2 ;;
	--restore) RESTORE=$2; shift 2 ;;
	--skip-spr) SKIP_SPR=$2; shift 2 ;;
	-h|--help) usage ;;
	*) echo "unknown arg: $1" >&2; usage ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root" >&2
	exit 1
fi

if [ ! -d "$SYSFS" ]; then
	echo "cpu sysfs missing" >&2
	exit 1
fi

expand_list() {
	local spec=$1 tok
	IFS=',' read -ra toks <<< "$spec"
	for tok in "${toks[@]}"; do
		if [[ "$tok" == *-* ]]; then
			local a=${tok%-*} b=${tok#*-} i
			for ((i=a; i<=b; i++)); do
				echo "$i"
			done
		else
			echo "$tok"
		fi
	done
}

siblings_of() {
	cat "$SYSFS/cpu$1/topology/thread_siblings_list"
}

core_has_cpu0() {
	expand_list "$(siblings_of "$1")" | grep -qx 0
}

pick_cpu() {
	local c
	for c in $(ls -d "$SYSFS"/cpu[0-9]* | sed 's/.*cpu//' | sort -n | tac); do
		[ -f "$SYSFS/cpu$c/online" ] || continue
		core_has_cpu0 "$c" && continue
		echo "$c"
		return 0
	done
	echo "no hotpluggable core that excludes CPU 0" >&2
	exit 1
}

cpu_set() {
	local cpu=$1 val=$2
	if [ ! -f "$SYSFS/cpu$cpu/online" ]; then
		return 0
	fi
	echo "$val" > "$SYSFS/cpu$cpu/online"
}

wait_state() {
	local cpu=$1 want=$2 i
	for i in $(seq 1 50); do
		if [ "$(cat "$SYSFS/cpu$cpu/online")" = "$want" ]; then
			return 0
		fi
		sleep 0.1
	done
	echo "cpu$cpu did not reach online=$want" >&2
	return 1
}

set_restore() {
	local val=$1
	if [ ! -f "$DBG/spr_restore" ]; then
		echo "missing $DBG/spr_restore (need DEBUG_FS and this probe kernel)" >&2
		exit 1
	fi
	echo "$val" > "$DBG/spr_restore"
	if [ -f "$DBG/skip_spr" ]; then
		echo "$SKIP_SPR" > "$DBG/skip_spr"
		echo "skip_spr=$(cat "$DBG/skip_spr")"
	fi
	echo "spr_restore=$(cat "$DBG/spr_restore")"
}

reset_hist() {
	[ -f "$DBG/reset" ] && echo 1 > "$DBG/reset" || true
}

dump_hist() {
	echo "---- $DBG/pls ----"
	cat "$DBG/pls" || true
	echo "---- $DBG/stage ----"
	cat "$DBG/stage" 2>/dev/null || true
	echo "---- dmesg (hotplug pls) ----"
	dmesg -t | grep 'cpu .* hotplug:' | tail -n 40 || true
	if [ -n "${COLLECT_DIR:-}" ]; then
		mkdir -p "$COLLECT_DIR"
		cat "$DBG/pls" > "$COLLECT_DIR/pls-restore${mode}-cycle${i}.txt" 2>/dev/null || true
		dmesg -t | grep 'cpu .* hotplug:' \
			> "$COLLECT_DIR/dmesg-hotplug-restore${mode}-cycle${i}.txt" 2>/dev/null || true
	fi
}

offline_core() {
	local cpu
	# Reverse order so the primary thread goes last; not required, just tidy.
	for cpu in $(echo "$THREADS" | sort -nr); do
		echo "offline cpu$cpu"
		cpu_set "$cpu" 0
		wait_state "$cpu" 0
	done
}

online_core() {
	local cpu
	for cpu in $(echo "$THREADS" | sort -n); do
		echo "online cpu$cpu"
		cpu_set "$cpu" 1
		wait_state "$cpu" 1
	done
}

try_pin() {
	local cpu=$1
	taskset -p -c "$cpu" $$ >/dev/null 2>&1 && return 0
	taskset -c "$cpu" -p $$ >/dev/null 2>&1 && return 0
	return 1
}

# Run on a CPU that is not in the core we are about to offline.
pin_self() {
	local cpu
	for cpu in $(expand_list "$(cat $SYSFS/online)"); do
		if echo "$THREADS" | grep -qx "$cpu"; then
			continue
		fi
		if try_pin "$cpu"; then
			echo "pinned to cpu$cpu"
			return 0
		fi
	done
	echo "warning: could not pin affinity; do not offline this shell's CPU" >&2
}

if [ "$SKIP_SPR" != 0 ]; then
	RESTORE=1
	echo "omit SPR #$SKIP_SPR only (spr_restore forced on)"
fi

if [ -z "$CPU" ]; then
	CPU=$(pick_cpu)
fi

if [ ! -d "$SYSFS/cpu$CPU" ]; then
	echo "cpu$CPU does not exist" >&2
	exit 1
fi

SIBS=$(siblings_of "$CPU")
THREADS=$(expand_list "$SIBS")

echo "target cpu$CPU core threads: $SIBS"
echo "present=$(cat $SYSFS/present) online=$(cat $SYSFS/online)"
dmesg | grep -E 'Deepest stop|lose SPRs|lose timebase' || true

pin_self

if echo "$THREADS" | grep -qx 0; then
	echo "refusing to offline CPU 0's core" >&2
	exit 1
fi

case "$RESTORE" in
0|1) MODES=$RESTORE ;;
both) MODES="1 0" ;;
*) echo "--restore must be 0, 1, or both" >&2; exit 1 ;;
esac

trap 'echo "attempting to online core $SIBS"; online_core || true' EXIT

for mode in $MODES; do
	echo
	echo "========== restore=$mode iters=$ITERS hold=${HOLD}s =========="
	set_restore "$mode"
	reset_hist
	for i in $(seq 1 "$ITERS"); do
		echo
		echo "----- cycle $i/$ITERS restore=$mode -----"
		reset_hist
		offline_core
		sleep "$HOLD"
		online_core
		dump_hist
	done
done

trap - EXIT
echo
echo "done. core $SIBS back online."
