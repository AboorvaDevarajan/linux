#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Run the atomic-barrier litmus pack. Skip if herd7 is not installed.

set -eu

ksft_skip=4

dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
litmus_dir=$dir/litmus

find_mm_cfg()
{
	d=$dir
	i=0
	while [ "$i" -lt 8 ]; do
		d=$(dirname "$d")
		if [ -f "$d/tools/memory-model/linux-kernel.cfg" ]; then
			printf '%s\n' "$d/tools/memory-model/linux-kernel.cfg"
			return 0
		fi
		i=$((i + 1))
	done
	return 1
}

pass()
{
	printf '[PASS] %s\n' "$1"
}

if ! command -v herd7 >/dev/null 2>&1; then
	echo "skip: herd7 not installed (https://github.com/herd/herdtools7)"
	exit $ksft_skip
fi

if ! mm_cfg=$(find_mm_cfg); then
	echo "skip: tools/memory-model/linux-kernel.cfg not found"
	exit $ksft_skip
fi

mm_dir=$(dirname "$mm_cfg")
failed=0

expect_of()
{
	sed -n 's/^ \* Result: //p' "$1" | head -1
}

observation_of()
{
	sed -n 's/^Observation [^ ]* \([^ ]*\).*/\1/p' | head -1
}

run_one()
{
	file=$1
	kind=$2
	base=$(basename "$file")
	want=$(expect_of "$file")
	if [ -z "$want" ]; then
		printf '[FAIL] %s: missing Result: comment\n' "$base" >&2
		return 1
	fi

	if [ "$kind" = lkmm ]; then
		# linux-kernel.cfg names .cat/.def relatively. herd7 also
		# resolves macros from cwd, so run from tools/memory-model
		# with an explicit -macros (herd7 7.58+, variant lkmmv2).
		rel=$(realpath --relative-to="$mm_dir" "$file")
		out=$(cd "$mm_dir" && herd7 -conf linux-kernel.cfg \
			-macros linux-kernel.def -I . "$rel" 2>&1) || {
			printf '%s\n' "$out" >&2
			printf '[FAIL] %s: herd7 LKMM failed (need herd7 7.58+)\n' "$base" >&2
			return 1
		}
	else
		out=$(herd7 -unroll 2 "$file" 2>&1) || {
			printf '%s\n' "$out" >&2
			printf '[FAIL] %s: herd7 PPC failed\n' "$base" >&2
			return 1
		}
	fi

	got=$(printf '%s\n' "$out" | observation_of)
	if [ -z "$got" ]; then
		printf '%s\n' "$out" >&2
		printf '[FAIL] %s: no Observation line\n' "$base" >&2
		return 1
	fi
	if [ "$got" != "$want" ]; then
		printf '%s\n' "$out" >&2
		printf '[FAIL] %s: expected %s, got %s\n' "$base" "$want" "$got" >&2
		return 1
	fi
	pass "$base ($got)"
}

echo "LKMM (linux-kernel.cfg) from $mm_dir"
for f in "$litmus_dir"/SB+atomicincmb+atomicincmb.litmus \
	 "$litmus_dir"/SB+atomicincrel+atomicincacq.litmus \
	 "$litmus_dir"/MP+atomicincmb.litmus; do
	run_one "$f" lkmm || failed=1
done

echo "PPC ISA model"
for f in "$litmus_dir"/SB+lwsyncs.litmus \
	 "$litmus_dir"/SB+syncs.litmus \
	 "$litmus_dir"/SB+lwsync-lwarx-stwcx-isync.litmus; do
	run_one "$f" ppc || failed=1
done

if [ "$failed" -ne 0 ]; then
	exit 1
fi
echo "ok: litmus"
exit 0
