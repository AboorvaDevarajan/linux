#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Wrapper around p9-core-hotplug-spr.sh: run whole-core offline/online and
# collect logs for later inference about whether the P9 hotplug path actually
# reached a SPR-losing stop, and whether skipping C-level SPR restore survived.
#
# Usage:
#   p9-hotplug-spr-collect.sh [--out DIR] [--cpu N] [--iters N] [--hold SEC]
#                             [--restore 0|1|both]
#   p9-hotplug-spr-collect.sh --bisect [--bisect-from N] [--bisect-to N] [...]
#
# --bisect walks skip_spr=N one at a time with spr_restore on. If a number
# hangs the AP ("Processor X is stuck"), reboot and continue:
#   --bisect --bisect-from $((N+1))
#
# Numbered map: cat /sys/kernel/debug/powerpc/pnv_hotplug_idle/sprs
#   1-4 KUAP, 5 ptcr, 6 rpr, 7 tscr, 8 lpcr, ... 21 sprg3
#
# Default OUT is ./p9-hp-spr-<timestamp> under the current directory.
# Remaining flags are passed to p9-core-hotplug-spr.sh. Default restore=both
# so restore=1 is captured even if restore=0 then hangs.
#
# Layout of $OUT:
#   meta.txt                 uname, cmdline, cpu model
#   topology.txt             present/online, sibling map, target core
#   idle-boot.txt            deepest stop / SPR-loss / TB-loss boot lines
#   dmesg-before.txt
#   run-restore-{0,1}.log    inner-script stdout
#   dmesg-after-restore-*.txt
#   pls-after-restore-*.txt  debugfs snapshot
#   cycles/                  per-cycle debugfs + hotplug dmesg
#   dmesg-hotplug.txt        all "cpu N hotplug:" lines
#   SUMMARY.txt              extracted numbers + inference
#
# Inference in SUMMARY.txt:
#   SHALLOW_ONLY   max_pls < deep_spr_loss — skip was a no-op, rerun with
#                  a whole core and a longer --hold
#   DEEP_RESTORE_ON_OK / DEEP_RESTORE_OFF_SURVIVED / DEEP_RESTORE_OFF_FAILED

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
INNER=$HERE/p9-core-hotplug-spr.sh
OUT=""
CPU=""
ITERS=3
HOLD=2
RESTORE=both
BISECT=0
BISECT_FROM=1
BISECT_TO=21
DBG=/sys/kernel/debug/powerpc/pnv_hotplug_idle
SYSFS=/sys/devices/system/cpu

usage() {
	sed -n '2,32p' "$0" | sed 's/^# \?//'
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
	--out) OUT=$2; shift 2 ;;
	--cpu) CPU=$2; shift 2 ;;
	--iters) ITERS=$2; shift 2 ;;
	--hold) HOLD=$2; shift 2 ;;
	--restore) RESTORE=$2; shift 2 ;;
	--bisect) BISECT=1; shift ;;
	--bisect-from) BISECT_FROM=$2; shift 2 ;;
	--bisect-to) BISECT_TO=$2; shift 2 ;;
	-h|--help) usage ;;
	*) echo "unknown arg: $1" >&2; usage ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root" >&2
	exit 1
fi

if [ ! -x "$INNER" ]; then
	echo "missing inner script: $INNER" >&2
	exit 1
fi

if [ -z "$OUT" ]; then
	OUT=$(pwd)/p9-hp-spr-$(date +%Y%m%d-%H%M%S)
fi
mkdir -p "$OUT/cycles"
OUT=$(cd "$OUT" && pwd)

dump_meta() {
	{
		echo "date $(date -Is)"
		echo "host $(hostname)"
		echo "uname $(uname -a)"
		echo "kernel $(uname -r)"
		echo "cmdline $(cat /proc/cmdline)"
		echo
		echo "=== /proc/cpuinfo (first cpu) ==="
		awk 'BEGIN{RS=""} NR==1{print}' /proc/cpuinfo
		echo
		if command -v lscpu >/dev/null 2>&1; then
			echo "=== lscpu ==="
			lscpu
		fi
	} > "$OUT/meta.txt"
}

dump_topology() {
	{
		echo "present=$(cat $SYSFS/present)"
		echo "online=$(cat $SYSFS/online)"
		echo "offline=$(cat $SYSFS/offline 2>/dev/null || true)"
		echo
		echo "=== thread siblings ==="
		for d in "$SYSFS"/cpu[0-9]*; do
			c=${d##*cpu}
			[ -f "$d/topology/thread_siblings_list" ] || continue
			echo "cpu$c siblings=$(cat "$d/topology/thread_siblings_list") core=$(cat "$d/topology/core_id" 2>/dev/null || echo ?) pkg=$(cat "$d/topology/physical_package_id" 2>/dev/null || echo ?)"
		done
		if command -v ppc64_cpu >/dev/null 2>&1; then
			echo
			echo "=== ppc64_cpu --info ==="
			ppc64_cpu --info 2>/dev/null || true
		fi
	} > "$OUT/topology.txt"
}

dump_idle_boot() {
	{
		echo "=== dmesg idle / stop ==="
		dmesg -t | grep -E 'Deepest stop|lose SPRs|lose timebase|cpuidle-powernv|powernv: failed to register hotplug' || true
		echo
		echo "=== $DBG ==="
		if [ -d "$DBG" ]; then
			echo "spr_restore=$(cat "$DBG/spr_restore" 2>/dev/null || echo missing)"
			echo "skip_spr=$(cat "$DBG/skip_spr" 2>/dev/null || echo missing)"
			echo
			cat "$DBG/sprs" 2>/dev/null || true
			echo
			cat "$DBG/pls" 2>/dev/null || echo "(pls empty — no hotplug wakes yet)"
		else
			echo "MISSING $DBG — not the probe kernel, or debugfs not mounted"
			mount | grep debugfs || true
		fi
		if [ -f /sys/firmware/opal/msglog ]; then
			echo
			echo "=== opal msglog (stop/idle) ==="
			grep -Ei 'stop|idle|psscr|power-mgt' /sys/firmware/opal/msglog | tail -n 80 || true
		fi
	} > "$OUT/idle-boot.txt"
}

run_one() {
	local mode=$1
	local extra=()
	[ -n "$CPU" ] && extra+=(--cpu "$CPU")

	echo "==== collect restore=$mode iters=$ITERS hold=${HOLD}s out=$OUT ===="
	COLLECT_DIR="$OUT/cycles" "$INNER" \
		--restore "$mode" --iters "$ITERS" --hold "$HOLD" \
		"${extra[@]}" \
		> >(tee "$OUT/run-restore-${mode}.log") \
		2> >(tee -a "$OUT/run-restore-${mode}.log" >&2) || {
			echo "inner script restore=$mode exited $?" | tee -a "$OUT/run-restore-${mode}.log"
			return 1
		}

	dmesg > "$OUT/dmesg-after-restore-${mode}.txt"
	if [ -f "$DBG/pls" ]; then
		cat "$DBG/pls" > "$OUT/pls-after-restore-${mode}.txt"
	fi
	return 0
}

run_skip() {
	local n=$1
	local extra=()
	[ -n "$CPU" ] && extra+=(--cpu "$CPU")

	echo "==== collect skip_spr=$n iters=$ITERS hold=${HOLD}s out=$OUT ===="
	COLLECT_DIR="$OUT/cycles" "$INNER" \
		--restore 1 --skip-spr "$n" --iters "$ITERS" --hold "$HOLD" \
		"${extra[@]}" \
		> >(tee "$OUT/run-skip-${n}.log") \
		2> >(tee -a "$OUT/run-skip-${n}.log" >&2) || {
			echo "inner script skip_spr=$n exited $?" | tee -a "$OUT/run-skip-${n}.log"
			return 1
		}

	dmesg > "$OUT/dmesg-after-skip-${n}.txt"
	if [ -f "$DBG/pls" ]; then
		cat "$DBG/pls" > "$OUT/pls-after-skip-${n}.txt"
	fi
	if [ -f "$DBG/stage" ]; then
		cat "$DBG/stage" > "$OUT/stage-after-skip-${n}.txt"
	fi
	return 0
}

write_summary() {
	local hp=$OUT/dmesg-hotplug.txt
	local summary=$OUT/SUMMARY.txt
	dmesg -t | grep 'cpu .* hotplug:' > "$hp" || true

	python3 - "$OUT" "$hp" "$summary" << 'PY'
import os, re, sys

out, hp_path, summary_path = sys.argv[1], sys.argv[2], sys.argv[3]

def read(path):
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return ""

idle = read(os.path.join(out, "idle-boot.txt"))
meta = read(os.path.join(out, "meta.txt"))
topo = read(os.path.join(out, "topology.txt"))
hp = read(hp_path)

deep_loss = None
tb_loss = None
deepest = None
m = re.search(r"First stop level that may lose SPRs = (0x[0-9a-fA-F]+|\d+)", idle)
if m:
    deep_loss = int(m.group(1), 0)
m = re.search(r"First stop level that may lose timebase = (0x[0-9a-fA-F]+|\d+)", idle)
if m:
    tb_loss = int(m.group(1), 0)
m = re.search(r"Deepest stop: psscr = (0x[0-9a-fA-F]+)", idle)
if m:
    deepest = m.group(1)

# debugfs header overrides dmesg if present
for name in os.listdir(out):
    if name.startswith("pls-after-restore-"):
        hdr = read(os.path.join(out, name)).splitlines()[:1]
        if hdr:
            mm = re.search(r"deep_spr_loss_state (0x[0-9a-fA-F]+)", hdr[0])
            if mm:
                deep_loss = int(mm.group(1), 0)
            mm = re.search(r"first_tb_loss (0x[0-9a-fA-F]+)", hdr[0])
            if mm:
                tb_loss = int(mm.group(1), 0)
            mm = re.search(r"deepest_psscr (0x[0-9a-fA-F]+)", hdr[0])
            if mm:
                deepest = mm.group(1)

line_re = re.compile(
    r"cpu (\d+) hotplug: wakes=(\d+) last_pls=(\d+) max_pls=(\d+) "
    r"deep_spr_loss=(0x[0-9a-fA-F]+|\d+) restore=(\d+) skipped=(\d+) "
    r"deep=(\d+) srr1\(noloss/gpr/hv\)=(\d+)/(\d+)/(\d+) pls:(.*)$"
)

by_restore = {0: [], 1: []}
for line in hp.splitlines():
    m = line_re.search(line)
    if not m:
        continue
    rec = {
        "cpu": int(m.group(1)),
        "wakes": int(m.group(2)),
        "last_pls": int(m.group(3)),
        "max_pls": int(m.group(4)),
        "deep_spr_loss": int(m.group(5), 0),
        "restore": int(m.group(6)),
        "skipped": int(m.group(7)),
        "deep": int(m.group(8)),
        "noloss": int(m.group(9)),
        "gprloss": int(m.group(10)),
        "hvloss": int(m.group(11)),
        "pls": m.group(12).strip(),
        "raw": line,
    }
    if deep_loss is None:
        deep_loss = rec["deep_spr_loss"]
    by_restore.setdefault(rec["restore"], []).append(rec)

survived = {
    1: os.path.exists(os.path.join(out, "run-restore-1.log")),
    0: os.path.exists(os.path.join(out, "run-restore-0.log"))
    and "inner script restore=0 exited" not in read(os.path.join(out, "run-restore-0.log")),
}
# if restore=0 log exists and ends with "done.", it survived
for mode in (0, 1):
    log = read(os.path.join(out, f"run-restore-{mode}.log"))
    if log:
        survived[mode] = "done. core" in log or log.strip().endswith("back online.")

lines = []
def p(s=""):
    lines.append(s)

p("P9 hotplug SPR-restore probe — SUMMARY")
p("out: " + out)
p(meta.splitlines()[0] if meta else "")
p(meta.splitlines()[1] if meta and len(meta.splitlines()) > 1 else "")
p()
p("idle gates:")
p(f"  deep_spr_loss_state = {deep_loss}")
p(f"  first_tb_loss_level = {tb_loss}")
p(f"  deepest_psscr       = {deepest}")
p()
p("topology (head):")
for row in topo.splitlines()[:4]:
    p("  " + row)
p()

def summarise(mode, recs, did_survive):
    p(f"restore={mode}  survived={did_survive}  n_print={len(recs)}")
    if not recs:
        p("  (no hotplug PLS prints — CPU never came back, or not the probe kernel)")
        p()
        return None, None
    max_pls = max(r["max_pls"] for r in recs)
    last_pls = max(r["last_pls"] for r in recs)
    deep = sum(r["deep"] for r in recs)
    skipped = sum(r["skipped"] for r in recs)
    hv = sum(r["hvloss"] for r in recs)
    gpr = sum(r["gprloss"] for r in recs)
    noloss = sum(r["noloss"] for r in recs)
    p(f"  max_pls={max_pls} last_pls_max={last_pls} deep_wakes={deep} skipped={skipped}")
    p(f"  srr1 noloss={noloss} gprloss={gpr} hvloss={hv}")
    for r in recs[-8:]:
        p(f"  cpu{r['cpu']}: last_pls={r['last_pls']} max_pls={r['max_pls']} "
          f"deep={r['deep']} skipped={r['skipped']} pls:{r['pls']}")
    p()
    return max_pls, deep

max1, deep1 = summarise(1, by_restore.get(1, []), survived.get(1, False))
max0, deep0 = summarise(0, by_restore.get(0, []), survived.get(0, False))

p("INFERENCE")
gate = deep_loss if deep_loss is not None else 16
reached = False
for mx in (max1, max0):
    if mx is not None and mx >= gate:
        reached = True

if not reached:
    p("  SHALLOW_ONLY: max_pls never reached deep_spr_loss_state.")
    p("  Skipping SPR restore did not exercise the deep path. Rerun with a")
    p("  whole core offlined and a longer --hold. Check idle-boot.txt that")
    p("  Deepest stop is a LOSE_FULL_CONTEXT state.")
elif max0 is None and not survived.get(0, False) and os.path.exists(os.path.join(out, "run-restore-1.log")):
    p("  DEEP_RESTORE_OFF_FAILED: core did go deep with restore=1, but the")
    p("  restore=0 run did not finish (hang/crash, or no PLS print after online).")
    p("  That is evidence the hotplug path needs the C-level SPR restore.")
elif max0 is not None and max0 >= gate and survived.get(0, False):
    p("  DEEP_RESTORE_OFF_SURVIVED: PLS reached the SPR-loss level with")
    p("  restore=0 and the CPUs came back. Hotplug C-level SPR restore was")
    p("  not required for this stop level on this machine — still check")
    p("  skipped= vs deep= in the restore=0 lines.")
elif max1 is not None and max1 >= gate:
    p("  DEEP_RESTORE_ON_OK: whole-core offline reached a SPR-losing PLS")
    p("  with restore=1. restore=0 was not conclusive.")
else:
    p("  INCONCLUSIVE: see dmesg-hotplug.txt and run-restore-*.log")

p()
p("files: meta.txt topology.txt idle-boot.txt run-restore-*.log")
p("       dmesg-after-restore-*.txt pls-after-restore-*.txt cycles/ SUMMARY.txt")

text = "\n".join(lines) + "\n"
with open(summary_path, "w") as f:
    f.write(text)
print(text)
PY
}

dump_meta
dump_topology
dump_idle_boot
dmesg > "$OUT/dmesg-before.txt"

echo "collecting into $OUT"

rc=0
if [ "$BISECT" = 1 ]; then
	if [ -f "$DBG/sprs" ]; then
		cp "$DBG/sprs" "$OUT/sprs.txt"
	fi
	echo "bisect skip_spr=$BISECT_FROM..$BISECT_TO (spr_restore=Y, omit one)"
	for n in $(seq "$BISECT_FROM" "$BISECT_TO"); do
		name=""
		if [ -f "$DBG/sprs" ]; then
			name=$(awk -v n="$n" '$1==n {print $2}' "$DBG/sprs" | head -1)
		fi
		echo
		echo "########## skip_spr=$n ${name:+($name)} ##########"
		if ! run_skip "$n"; then
			rc=1
			echo "$n ${name:-}" > "$OUT/HUNG_AT"
			[ -f "$DBG/stage" ] && cat "$DBG/stage" | tee "$OUT/stage-hung.txt"
			echo
			echo "HUNG or failed omitting SPR #$n ${name:+($name)}"
			echo "Those CPUs stay dead until reboot."
			echo "After reboot, continue with:"
			echo "  $0 --bisect --bisect-from $((n+1)) --hold $HOLD --iters $ITERS ${CPU:+--cpu $CPU}"
			break
		fi
		echo "OK skip_spr=$n ${name:+($name)} — CPUs came back"
	done
	if [ "$rc" -eq 0 ]; then
		echo "bisect complete: skip_spr $BISECT_FROM..$BISECT_TO all survived"
	fi
else
	case "$RESTORE" in
	both) MODES="1 0" ;;
	0|1) MODES=$RESTORE ;;
	*) echo "--restore must be 0, 1, or both" >&2; exit 1 ;;
	esac

	for mode in $MODES; do
		if ! run_one "$mode"; then
			rc=1
			echo "restore=$mode failed; writing summary with what we have"
			break
		fi
	done
fi

write_summary || true

echo
echo "logs: $OUT"
echo "read: $OUT/SUMMARY.txt"
exit $rc
