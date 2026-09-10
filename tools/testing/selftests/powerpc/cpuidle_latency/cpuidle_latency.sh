#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Report the wakeup latency of every cpuidle state on this system, once
# for a wakeup by IPI and once for a wakeup by timer.
#
# Every state is measured twice on every target CPU, back to back. Once
# with the CPU kept busy, which gives a reference for the cost of the
# measurement itself, and once with the CPU left to idle in the state
# under test. The two runs are identical in every other way, including how
# long the timer test sleeps, so their difference, reported as the wakeup
# cost, is what leaving that state costs and nothing else. That is the
# number to compare against the exit latency the platform advertises in
# sysfs. A wakeup cost at or below zero means the state was no more
# expensive to leave than being awake.
#
# What a state costs to leave is not a property of the state alone: it also
# depends on how long the CPU has been sitting in it. Where the state is
# managed by a hypervisor, which may hand the CPU to someone else once it
# has been idle a while, the difference between leaving a state entered a
# moment ago and one held for several seconds can be tenfold. So every
# figure here belongs to one idle duration, the one -w sets, and the report
# prints it alongside the results. Comparing runs taken with different -w
# values, or against the single exit latency sysfs advertises, means keeping
# that in mind.
#
# Single wakeups are noisy enough that one of them says very little, so
# every measurement is a median of several samples, and the summary is the
# median of those across CPUs. The spread behind each figure is in the
# detailed log. A CPU only contributes to a state's row once its usage
# counter shows it went through that state on every one of those samples,
# so a governor that quietly picked something else cannot end up reported
# as this state.
#
# A state that measures far from what it advertises is misinforming the
# idle governor either way round, so the report remarks on a wide gap in
# either direction. Costing more than advertised can break a latency
# constraint the governor thought it was honouring. Costing far less is
# the commoner fault and the quieter one: the governor is told the state
# is dear, so it passes over a state it could have afforded, and nothing
# looks wrong from the outside. Since sysfs carries one figure per state
# and the real cost moves with how long the CPU was idle, a remark either
# way says the advertised number does not match what was measured at this
# idle duration, which is worth knowing without being proof on its own
# that the platform has it wrong.
#
# Only the first of those can fail the test, and only when asked with -e,
# because how much room to allow over an advertised figure is a judgement
# that wants checking against real hardware first, and a test that fails
# on an unproven threshold is worse than one that reports and lets the
# reader decide. A state that advertises more than it costs is the
# platform being careful rather than the kernel being wrong, so that one
# is only ever reported.
#
# The timer wakeup is measured entirely in userspace by timer_wakeup,
# which sleeps on a pinned thread. Only the IPI wakeup needs help from
# the kernel, from the test_cpuidle_latency module, because nothing in
# userspace can aim a single IPI at one specific idle CPU. Either half
# runs without the other.
#
# Author: Pratik R. Sampat <psampat@linux.ibm.com>
# Author: Aboorva Devarajan <aboorvad@linux.ibm.com>

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

shopt -s nullglob

CPU_SYSFS="/sys/devices/system/cpu"
DEBUGFS="/sys/kernel/debug/powerpc/latency_test"
MODULE="test_cpuidle_latency"
MODULE_PATH="/lib/modules/$(uname -r)/kernel/arch/powerpc/kernel/$MODULE.ko"
TIMER_PROG="$(dirname "$0")/timer_wakeup"

LOG="cpuidle_latency.log"
IPI_SRC_CPU=0
ALL_THREADS=0
RUN_TIMER_TESTS=1
ENFORCE_TOLERANCE=0

# How long to leave a CPU alone before measuring it. Long enough for the
# governor to settle on the state under test, and, because what a state
# costs to leave grows with how long it has been held, part of what the
# result means rather than a detail of how it was taken.
SETTLE_SEC=1
# Samples per CPU per state. Enough to have a median worth reporting
# without making a run on a large machine take all day.
SAMPLES=20
# Added to a target residency so the timer outlasts the state.
RESIDENCY_SLACK_NS=1000
# Shortest sleep to ask for. A state with a tiny target residency would
# otherwise be measured with a sleep so short that the CPU may not reach
# the idle loop at all, and a sample that never idled is no use here. Only
# the state under test is enabled while it is measured, so a longer sleep
# cannot land the CPU in some deeper state instead.
TIMER_MIN_NS=100000
# Longest sleep timer_wakeup accepts.
TIMER_MAX_NS=1000000000
# How far a measured wakeup cost may sit from the advertised exit latency,
# in either direction, before the test remarks on it. A factor rather than
# a fixed amount because the advertised figures span three orders of
# magnitude, and loose on purpose: this is here to catch a state the
# governor is badly misinformed about, not to hold hardware to an exact
# number. The floor keeps small numbers from being reported on ratio alone,
# and means something different in each direction, described where it is
# used.
TOLERANCE_FACTOR=2
TOLERANCE_FLOOR_NS=10000

module_loaded=0
saved_states=""
busy_pid=""
num_states=0
online_cpus=()
target_cpus=()
ipi_targets=()
measured=0
attributed=0
violations=0
overstated=0
report_cpu=0
declare -A src_sibling=()

usage()
{
	cat <<-EOF
	Usage: ${0##*/} [options]

	  -s CPU   send the IPIs from CPU (default: $IPI_SRC_CPU)
	  -m PATH  path to $MODULE.ko (default: under /lib/modules)
	  -o FILE  write the detailed log to FILE (default: $LOG)
	  -c N     samples per CPU per state (default: $SAMPLES)
	  -w SEC   how long to hold a CPU idle before measuring it, which is
	           part of what the result means (default: $SETTLE_SEC)
	  -v       measure every CPU thread, not one per core
	  -n       skip the timer tests
	  -e       fail, rather than warn, when a state costs more than it
	           advertises
	  -h       show this help
	EOF
}

skip()
{
	printf 'cpuidle_latency: %s [SKIP]\n' "$*"
	exit $ksft_skip
}

fail()
{
	printf 'cpuidle_latency: %s [FAIL]\n' "$*"
	exit 1
}

report()
{
	printf "$@" | tee -a "$LOG"
}

detail()
{
	printf "$@" >> "$LOG"
}

# Both halves log the same shape of row, and both log the spread behind
# every figure that reaches the summary, so that a median resting on wildly
# scattered samples can be told from one resting on tight ones. They differ
# only in what the first two columns identify.
detail_header()
{
	detail '%-8s %10s %6s %12s %12s %12s %8s\n' "$1" "$2" "PHASE" \
		"MIN(ns)" "MEDIAN(ns)" "MAX(ns)" "ENTRIES"
}

detail_row()
{
	detail '%-8s %10s %6s %12s %12s %12s %8s\n' "$1" "$2" "$3" \
		"$measured_min" "$measured_med" "$measured_max" "$4"
}

# A CPU that cannot be offlined, the boot CPU in particular, has no
# "online" file, so treat a missing one as online.
cpu_is_online()
{
	local online="$CPU_SYSFS/cpu$1/online"

	[ -d "$CPU_SYSFS/cpu$1" ] || return 1
	[ -f "$online" ] || return 0
	[ "$(cat "$online")" -eq 1 ]
}

collect_online_cpus()
{
	local dir cpu

	for dir in "$CPU_SYSFS"/cpu[0-9]*; do
		cpu="${dir##*/cpu}"
		cpu_is_online "$cpu" && printf '%s\n' "$cpu"
	done | sort -n
}

# Count the state directories rather than the contents of cpuidle/, which
# also holds the governor files.
count_idle_states()
{
	local states=("$CPU_SYSFS/cpu$1/cpuidle"/state[0-9]*)

	printf '%s\n' "${#states[@]}"
}

state_attr()
{
	cat "$CPU_SYSFS/cpu$1/cpuidle/state$2/$3"
}

# The summary has one row per state index, so the CPUs being measured have
# to agree on what each index means. A machine where they do not would need
# a different report, so say so instead of quietly mixing states together.
check_states_agree()
{
	local cpu state count name ref

	for cpu in "${target_cpus[@]}"; do
		count="$(count_idle_states "$cpu")"
		[ "$count" -eq "$num_states" ] ||
			fail "CPU $cpu has $count idle states, CPU $report_cpu has $num_states"
		for ((state = 0; state < num_states; state++)); do
			name="$(state_attr "$cpu" "$state" name)"
			ref="$(state_attr "$report_cpu" "$state" name)"
			[ "$name" = "$ref" ] ||
				fail "CPU $cpu state$state is $name, CPU $report_cpu has $ref"
		done
	done
}

# Times a CPU has entered one idle state. Comparing this across a
# measurement is what shows the result describes the state it is
# reported under, and not whatever the governor picked instead.
state_usage()
{
	local file="$CPU_SYSFS/cpu$1/cpuidle/state$2/usage"

	# A CPU that does not have the state can never enter it, so hand
	# back a count that cannot change.
	[ -r "$file" ] || { printf '0\n'; return; }
	cat "$file"
}

# Entries into any idle state, used to confirm a CPU stayed awake.
total_usage()
{
	local file total=0 value

	for file in "$CPU_SYSFS/cpu$1/cpuidle"/state[0-9]*/usage; do
		read -r value < "$file"
		total=$((total + value))
	done
	printf '%s\n' "$total"
}

# Column headings for the per-state summary, kept next to the row format
# below so the two stay lined up.
report_header()
{
	report '#\n'
	report '# %-14s %12s %12s %14s %14s\n' "state" "measured" "busy ref" \
		"wakeup cost" "advertised"
}

# One summary line per state. The wakeup cost is the measured value with
# the busy reference taken out, which is the part that is the cost of
# leaving the state rather than the cost of measuring it. A state nothing
# entered is called out rather than left to look like a free wakeup.
report_state()
{
	local name="$1" busy="$2" advertised="$3" missed="$4" med cost

	shift 4
	if 	[ "$#" -eq 0 ]; then
		report '# %-14s %12s %9s ns %14s %11s ns   no CPU entered it on every sleep\n' \
			"$name" "-" "$busy" "-" "$advertised"
		return
	fi

	med="$(median "$@")"
	cost=$((med - busy))
	attributed=$((attributed + 1))
	report '# %-14s %9s ns %9s ns %11s ns %11s ns' "$name" "$med" \
		"$busy" "$cost" "$advertised"
	[ "$missed" -gt 0 ] &&
		report '   not entered on every sleep on %s of %s CPUs' \
			"$missed" "$((missed + $#))"
	report '\n'
	check_tolerance "$name" "$cost" "$advertised"
}

# Compare what leaving the state actually cost against what the platform
# tells the governor it costs, and remark on a gap either way. Only a state
# that advertises something can be checked, which leaves out the polling
# state every platform advertises as free.
check_tolerance()
{
	local name="$1" cost="$2" advertised="$3"

	[ "$advertised" -gt 0 ] || return 0

	# What is too small a gap to remark on depends on which way it runs.
	# Costing more than advertised is only worth saying when the excess is
	# a real amount of time, whatever the ratio, because a state that
	# advertises little has little to be wrong about. Costing less is
	# worth saying whenever the ratio is large, as long as the state
	# advertises a real amount of time to begin with, because it is the
	# advertised figure that decides whether the governor rules the state
	# out and the measured one may be near zero.
	if [ "$cost" -gt $((advertised * TOLERANCE_FACTOR)) ]; then
		[ $((cost - advertised)) -gt "$TOLERANCE_FLOOR_NS" ] || return 0
		report '#   %s cost %s ns to leave after %s s idle, over %sx\n' \
			"$name" "$cost" "$SETTLE_SEC" "$TOLERANCE_FACTOR"
		report '#   its advertised %s ns\n' "$advertised"
		violations=$((violations + 1))
	elif [ $((cost * TOLERANCE_FACTOR)) -lt "$advertised" ]; then
		[ "$advertised" -gt "$TOLERANCE_FLOOR_NS" ] || return 0
		report '#   %s advertises %s ns but cost %s ns after %s s idle,\n' \
			"$name" "$advertised" "$cost" "$SETTLE_SEC"
		report '#   so the governor is told it is dearer than it is here\n'
		overstated=$((overstated + 1))
	fi
}

expand_cpu_list()
{
	local range first last cpu

	for range in ${1//,/ }; do
		if [[ $range == *-* ]]; then
			first="${range%-*}"
			last="${range#*-}"
			for ((cpu = first; cpu <= last; cpu++)); do
				printf '%s\n' "$cpu"
			done
		else
			printf '%s\n' "$range"
		fi
	done
}

sibling_cpus()
{
	local attr file

	for attr in core_cpus_list thread_siblings_list; do
		file="$CPU_SYSFS/cpu$1/topology/$attr"
		if [ -r "$file" ]; then
			expand_cpu_list "$(cat "$file")"
			return
		fi
	done
	printf '%s\n' "$1"
}

# Measuring one thread per core is enough by default: siblings share the
# idle state, so the extra threads only make the run longer.
select_target_cpus()
{
	local cpu sibling
	declare -A covered=()

	for cpu in "${online_cpus[@]}"; do
		if [ "$ALL_THREADS" -eq 1 ]; then
			target_cpus+=("$cpu")
			continue
		fi
		[ -n "${covered[$cpu]}" ] && continue
		target_cpus+=("$cpu")
		for sibling in $(sibling_cpus "$cpu"); do
			covered[$sibling]=1
		done
	done

	for sibling in $(sibling_cpus "$IPI_SRC_CPU"); do
		src_sibling[$sibling]=1
	done

	# An IPI has to cross to another core to say anything useful, so
	# note which targets can serve as one.
	for cpu in "${target_cpus[@]}"; do
		[ -n "${src_sibling[$cpu]}" ] && continue
		ipi_targets+=("$cpu")
	done
}

# Used both for the samples taken on one CPU and for combining CPUs, so
# that one unlucky CPU cannot drag a whole row the way a mean would.
median()
{
	local sorted mid

	if [ "$#" -eq 0 ]; then
		printf '0\n'
		return
	fi
	mapfile -t sorted < <(printf '%s\n' "$@" | sort -n)
	mid=$(( ${#sorted[@]} / 2 ))
	if [ $(( ${#sorted[@]} % 2 )) -eq 1 ]; then
		printf '%s\n' "${sorted[mid]}"
	else
		printf '%s\n' $(( (sorted[mid - 1] + sorted[mid]) / 2 ))
	fi
}

# Only the IPI test needs the module, so report failure to the caller and
# let it carry on with the timer test rather than giving up here.
load_module()
{
	[ -d "$DEBUGFS" ] && return 0

	if modprobe "$MODULE" 2>/dev/null; then
		module_loaded=1
	elif [ -f "$MODULE_PATH" ] && insmod "$MODULE_PATH" 2>/dev/null; then
		module_loaded=1
	else
		return 1
	fi

	[ -d "$DEBUGFS" ]
}

unload_module()
{
	[ "$module_loaded" -eq 1 ] || return 0
	rmmod "$MODULE" 2>/dev/null
	module_loaded=0
}

# Enabling and disabling idle states is visible system wide, so put the
# original values back when the test is done.
save_idle_states()
{
	local file

	saved_states="$(mktemp)"
	for file in "$CPU_SYSFS"/cpu[0-9]*/cpuidle/state[0-9]*/disable; do
		printf '%s %s\n' "$file" "$(cat "$file")"
	done > "$saved_states"
}

# Runs from the exit trap, so report what could not be put back rather
# than bailing out and leaving the rest of the states untouched.
restore_idle_states()
{
	local file value

	[ -n "$saved_states" ] || return 0
	while read -r file value; do
		{ printf '%s\n' "$value" > "$file"; } 2>/dev/null ||
			printf 'cpuidle_latency: could not restore %s\n' "$file"
	done < "$saved_states"
	rm -f "$saved_states"
	saved_states=""
}

# Silently skipping a write here would leave the wrong state enabled and
# the numbers below would describe something other than the state named
# in the report, so treat a failure as fatal.
set_state_disable()
{
	local state="$1" value="$2" cpu file

	for cpu in "${online_cpus[@]}"; do
		file="$CPU_SYSFS/cpu$cpu/cpuidle/state$state/disable"
		# CPUs are not obliged to expose the same set of states.
		[ -e "$file" ] || continue
		{ printf '%s\n' "$value" > "$file"; } 2>/dev/null ||
			fail "cannot write $file"
	done
}

# The test enables one state at a time, so check up front that it is
# allowed to, instead of failing part way through a run.
check_state_control()
{
	local file="$CPU_SYSFS/cpu$report_cpu/cpuidle/state0/disable"

	{ printf '%s\n' "$(cat "$file")" > "$file"; } 2>/dev/null ||
		skip "cannot write $file, idle states cannot be controlled"
}

disable_all_states()
{
	local state

	for ((state = 0; state < num_states; state++)); do
		set_state_disable "$state" 1
	done
}

# SCHED_IDLE keeps the CPU out of the idle loop without getting in the way
# of the thread being measured, which preempts it as soon as it wakes.
#
# A spinning CPU has nothing to settle into, so rather than waiting a fixed
# time, wait only until the load is actually runnable. On a large machine
# that is the difference between a run of minutes and one of an hour.
start_busy_load()
{
	local waited=0 state

	chrt --idle 0 taskset -c "$1" bash -c 'while :; do :; done' &
	busy_pid=$!

	while [ "$waited" -lt 100 ]; do
		state="$(awk '/^State:/ { print $2 }' \
			"/proc/$busy_pid/status" 2>/dev/null)"
		[ "$state" = "R" ] && return 0
		sleep 0.01
		waited=$((waited + 1))
	done

	fail "the busy load did not start on CPU $1"
}

stop_busy_load()
{
	[ -n "$busy_pid" ] || return 0
	kill "$busy_pid" 2>/dev/null
	wait "$busy_pid" 2>/dev/null
	busy_pid=""
}

# Everything downstream does arithmetic on the measured values, and bash
# quietly reads a non-numeric one as zero, so a bad read has to be caught
# where it happens instead of becoming a plausible looking result.
require_number()
{
	[[ "$2" =~ ^-?[0-9]+$ ]] || fail "$1 gave '$2' instead of a number"
}

# Each write blocks until the module has taken one measurement, so the
# result files are ready as soon as it returns and the samples are just a
# loop. The target goes back to idle in the gap between them.
measure_ipi()
{
	local cpu="$1" i src value sorted values=()

	for ((i = 0; i < SAMPLES; i++)); do
		taskset -c "$IPI_SRC_CPU" \
			bash -c "echo $cpu > $DEBUGFS/ipi_cpu_dest" ||
			fail "sending an IPI to CPU $cpu failed"

		# The module reports the CPU it was driven from, so a caller
		# that did not end up where it asked to be is caught here
		# rather than reported as a latency for the wrong CPU.
		src="$(cat "$DEBUGFS/ipi_cpu_src")"
		[ "$src" -eq "$IPI_SRC_CPU" ] ||
			fail "IPI came from CPU $src, expected CPU $IPI_SRC_CPU"

		value="$(cat "$DEBUGFS/ipi_latency_ns")"
		require_number "$DEBUGFS/ipi_latency_ns" "$value"
		values+=("$value")
	done

	mapfile -t sorted < <(printf '%s\n' "${values[@]}" | sort -n)
	measured_min="${sorted[0]}"
	measured_max="${sorted[-1]}"
	measured_med="$(median "${values[@]}")"
}

# timer_wakeup binds itself to the CPU, sleeps to an absolute deadline and
# prints the smallest, middle and largest overshoot of several such sleeps,
# so it both stimulates and measures the wakeup without any help from the
# kernel.
measure_timer()
{
	local out

	out="$("$TIMER_PROG" "$1" "$2" "$SAMPLES")" ||
		fail "timer_wakeup failed on CPU $1"
	read -r measured_min measured_med measured_max <<< "$out"
	require_number "$TIMER_PROG" "$measured_min"
	require_number "$TIMER_PROG" "$measured_med"
	require_number "$TIMER_PROG" "$measured_max"
}

state_timer_ns()
{
	local ns

	ns=$(( $(state_attr "$1" "$2" residency) * 1000 + RESIDENCY_SLACK_NS ))
	[ "$ns" -lt "$TIMER_MIN_NS" ] && ns="$TIMER_MIN_NS"
	[ "$ns" -gt "$TIMER_MAX_NS" ] && ns="$TIMER_MAX_NS"
	printf '%s\n' "$ns"
}

run_ipi_tests()
{
	local state name expected cpu before entries
	local samples=() busy=() missed=()

	report '\n# --- IPI wakeup latency ---\n'

	# Measuring an IPI to a thread on the source core would only time
	# the test keeping itself awake, so there is nothing to do here on
	# a single core system. The timer test still works on one core.
	if [ "${#ipi_targets[@]}" -eq 0 ]; then
		report '# skipped, no online CPU outside the source core to send an IPI to\n'
		return
	fi
	if ! load_module; then
		report '# skipped, %s is not available, build it with\n' "$MODULE"
		report '#          CONFIG_CPUIDLE_LATENCY_SELFTEST=m and mount debugfs\n'
		return
	fi
	measured=1
	disable_all_states
	report_header

	for ((state = 0; state < num_states; state++)); do
		name="$(state_attr "$report_cpu" "$state" name)"
		expected=$(( $(state_attr "$report_cpu" "$state" latency) * 1000 ))
		samples=() busy=() missed=()

		set_state_disable "$state" 0
		detail '\n# idle state %s, advertised exit latency %s ns\n' \
			"$name" "$expected"
		detail_header "SRC" "TARGET"
		for cpu in "${ipi_targets[@]}"; do
			# Reference for this state, taken here rather than once
			# up front so that it differs from the measurement below
			# in one thing only: whether the target is awake.
			start_busy_load "$cpu"
			before="$(total_usage "$cpu")"
			measure_ipi "$cpu"
			entries=$(( $(total_usage "$cpu") - before ))
			stop_busy_load
			detail_row "$IPI_SRC_CPU" "$cpu" busy "$entries"
			# An idle entry means the load did not hold the CPU
			# awake, so this is no reference.
			[ "$entries" -eq 0 ] && busy+=("$measured_med")

			# The same again with the target left to idle.
			sleep "$SETTLE_SEC"
			before="$(state_usage "$cpu" "$state")"
			measure_ipi "$cpu"
			entries=$(( $(state_usage "$cpu" "$state") - before ))
			detail_row "$IPI_SRC_CPU" "$cpu" idle "$entries"
			# One idle entry per sample is what a CPU that really
			# spent every wait in this state looks like. Fewer means
			# some of the samples are timing something else, which
			# would quietly drag the median away from the state it
			# is reported under.
			if [ "$entries" -lt "$SAMPLES" ]; then
				missed+=("$cpu")
				continue
			fi
			samples+=("$measured_med")
		done
		set_state_disable "$state" 1

		[ "${#busy[@]}" -gt 0 ] ||
			fail "no usable busy reference, the load is not keeping targets awake"
		report_state "$name" "$(median "${busy[@]}")" "$expected" \
			"${#missed[@]}" "${samples[@]}"
	done
}

run_timer_tests()
{
	local state name expected timeout cpu before entries
	local samples=() busy=() missed=()

	report '\n# --- timer wakeup latency ---\n'

	if [ ! -x "$TIMER_PROG" ]; then
		report '# skipped, %s was not built\n' "$TIMER_PROG"
		return
	fi
	measured=1
	disable_all_states
	report_header

	for ((state = 0; state < num_states; state++)); do
		name="$(state_attr "$report_cpu" "$state" name)"
		expected=$(( $(state_attr "$report_cpu" "$state" latency) * 1000 ))
		samples=() busy=() missed=()

		set_state_disable "$state" 0
		detail '\n# idle state %s\n' "$name"
		detail_header "TARGET" "SLEEP(ns)"
		for cpu in "${target_cpus[@]}"; do
			# Residency is a property of the CPU being measured, so
			# ask that CPU rather than assuming the machine is
			# uniform.
			timeout="$(state_timer_ns "$cpu" "$state")"

			# The reference has to use this state's sleep length,
			# not a fixed one, or the two numbers being subtracted
			# would not describe the same amount of sleeping.
			start_busy_load "$cpu"
			before="$(total_usage "$cpu")"
			measure_timer "$cpu" "$timeout"
			entries=$(( $(total_usage "$cpu") - before ))
			stop_busy_load
			detail_row "$cpu" "$timeout" busy "$entries"
			[ "$entries" -eq 0 ] && busy+=("$measured_med")

			sleep "$SETTLE_SEC"
			before="$(state_usage "$cpu" "$state")"
			measure_timer "$cpu" "$timeout"
			entries=$(( $(state_usage "$cpu" "$state") - before ))
			detail_row "$cpu" "$timeout" idle "$entries"
			# See run_ipi_tests: every sample has to have gone
			# through this state, not just one of them.
			if [ "$entries" -lt "$SAMPLES" ]; then
				missed+=("$cpu")
				continue
			fi
			samples+=("$measured_med")
		done
		set_state_disable "$state" 1

		[ "${#busy[@]}" -gt 0 ] ||
			fail "no usable busy reference, the load is not keeping targets awake"
		report_state "$name" "$(median "${busy[@]}")" "$expected" \
			"${#missed[@]}" "${samples[@]}"
	done
}

cleanup()
{
	stop_busy_load
	restore_idle_states
	unload_module
}

while getopts 's:m:o:c:w:vneh' opt; do
	case "$opt" in
	s) IPI_SRC_CPU="$OPTARG" ;;
	m) MODULE_PATH="$OPTARG" ;;
	o) LOG="$OPTARG" ;;
	c) SAMPLES="$OPTARG" ;;
	w) SETTLE_SEC="$OPTARG" ;;
	v) ALL_THREADS=1 ;;
	n) RUN_TIMER_TESTS=0 ;;
	e) ENFORCE_TOLERANCE=1 ;;
	h) usage; exit 0 ;;
	*) usage; exit 1 ;;
	esac
done

[ "$(id -u)" -eq 0 ] || skip "must be run as root"
command -v taskset > /dev/null || skip "taskset is not available"
command -v chrt > /dev/null || skip "chrt is not available"

[[ "$IPI_SRC_CPU" =~ ^[0-9]+$ ]] || fail "-s wants a CPU number, got '$IPI_SRC_CPU'"
[[ "$SAMPLES" =~ ^[0-9]+$ ]] && [ "$SAMPLES" -gt 0 ] ||
	fail "-c wants a sample count above zero, got '$SAMPLES'"
[[ "$SETTLE_SEC" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
	fail "-w wants a number of seconds, got '$SETTLE_SEC'"
cpu_is_online "$IPI_SRC_CPU" || fail "CPU $IPI_SRC_CPU is not online"

mapfile -t online_cpus < <(collect_online_cpus)
[ "${#online_cpus[@]}" -gt 0 ] || fail "found no online CPUs"
select_target_cpus

# Everything in the summary is named after this CPU's view of the states,
# having checked that the CPUs being measured share it.
report_cpu="${target_cpus[0]}"
[ -d "$CPU_SYSFS/cpu$report_cpu/cpuidle" ] || skip "cpuidle is not enabled"
num_states="$(count_idle_states "$report_cpu")"
[ "$num_states" -gt 0 ] || skip "CPU $report_cpu has no idle states"
check_states_agree
check_state_control

trap cleanup EXIT
save_idle_states

: > "$LOG"

state_names=()
for ((i = 0; i < num_states; i++)); do
	state_names+=("$(state_attr "$report_cpu" "$i" name)")
done

report '# cpuidle latency selftest, kernel %s\n' "$(uname -r)"
report '# IPI source CPU: %s\n' "$IPI_SRC_CPU"
report '# target CPUs: %s\n' "${target_cpus[*]}"
report '# idle states: %s\n' "${state_names[*]}"
report '# samples per CPU per state: %s\n' "$SAMPLES"
report '# idle held before each measurement: %s s\n' "$SETTLE_SEC"

run_ipi_tests
if [ "$RUN_TIMER_TESTS" -eq 1 ]; then
	run_timer_tests
fi

report '\n# per-CPU results logged to %s\n' "$LOG"
[ "$measured" -eq 1 ] || skip "no part of the test could run on this system"

# Every row came back empty, so the test ran but measured nothing. Passing
# on that would report success for a machine where no idle state was ever
# held long enough to time.
[ "$attributed" -gt 0 ] ||
	skip "no idle state was entered on every sample, nothing was measured"

if [ "$overstated" -gt 0 ]; then
	report '# %s result(s) advertise far more than they cost, which keeps the\n' \
		"$overstated"
	report '# governor off a state it could have afforded\n'
fi

if [ "$violations" -gt 0 ]; then
	[ "$ENFORCE_TOLERANCE" -eq 1 ] &&
		fail "$violations result(s) cost more to leave than advertised"
	report '# %s result(s) over tolerance, pass -e to treat that as a failure\n' \
		"$violations"
fi
exit 0
