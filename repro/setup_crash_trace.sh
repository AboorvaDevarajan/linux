#!/bin/bash
# Prepare the system for an HTX run with crash-surviving ftrace.
#
# This script:
#   1. Verifies we're running the -bttdebug kernel
#   2. Verifies kdump is active
#   3. Enlarges and enables the ftrace ring buffer
#   4. Prints instructions for starting HTX
#
# After the crash, use extract_ftrace_from_vmcore.sh to get the trace.
#
# Usage:
#   sudo ./repro/setup_crash_trace.sh [buffer_size_kb]
#
# Default buffer: 65536 KB (64 MB) per-cpu

set -e

TRACE="/sys/kernel/debug/tracing"
BUF_KB="${1:-65536}"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root"
    exit 1
fi

echo "=== BTT crash trace setup ==="
echo

KREL=$(uname -r)
echo "1. Kernel: $KREL"
if echo "$KREL" | grep -q bttdebug; then
    echo "   OK: -bttdebug kernel detected (trace_printk events available)"
else
    echo "   WARNING: Not a -bttdebug kernel. trace_printk events will NOT fire."
    echo "   Build with: ./repro/build_and_deploy.sh"
    echo "   Continue anyway? (y/n)"
    read -r ans
    [ "$ans" = "y" ] || exit 1
fi
echo

echo "2. kdump status:"
if systemctl is-active --quiet kdump 2>/dev/null; then
    echo "   OK: kdump is active"
else
    echo "   WARNING: kdump is NOT active"
    echo "   Enabling kdump..."
    systemctl enable --now kdump || {
        echo "   ERROR: Failed to enable kdump. Fix manually."
        exit 1
    }
    echo "   OK: kdump enabled"
fi

CRASH_PATH=$(grep -E "^path" /etc/kdump.conf 2>/dev/null | awk '{print $2}')
echo "   Crash dump path: ${CRASH_PATH:-/var/crash (default)}"
echo

echo "3. Setting up ftrace ring buffer:"
if [ ! -d "$TRACE" ]; then
    echo "   ERROR: debugfs/tracing not mounted"
    mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
    if [ ! -d "$TRACE" ]; then
        echo "   FATAL: Cannot access $TRACE"
        exit 1
    fi
fi

echo "   Disabling tracing..."
echo 0 > "$TRACE/tracing_on"

echo "   Setting buffer to ${BUF_KB} KB per-cpu..."
echo "$BUF_KB" > "$TRACE/buffer_size_kb" 2>/dev/null || {
    echo "   WARNING: Could not set buffer to ${BUF_KB} KB, trying 32768..."
    echo 32768 > "$TRACE/buffer_size_kb"
}
ACTUAL=$(cat "$TRACE/buffer_size_kb")
echo "   Actual buffer: ${ACTUAL} KB per-cpu"

NCPU=$(nproc)
TOTAL_MB=$(( ACTUAL * NCPU / 1024 ))
echo "   Total trace memory: ~${TOTAL_MB} MB (${NCPU} CPUs)"

echo "   Clearing old trace..."
echo nop > "$TRACE/current_tracer"
echo > "$TRACE/trace"

echo "   Enabling tracing..."
echo 1 > "$TRACE/tracing_on"

EVENTS_BEFORE=$(wc -l < "$TRACE/trace")
echo "   Trace is ON (${EVENTS_BEFORE} lines in buffer)"
echo

echo "4. Quick sanity check — do a test write to verify trace_printk fires:"
echo "   (If you have pmem device, do: dd if=/dev/zero of=/dev/pmemX.Xs bs=4k count=1)"
echo

echo "============================================"
echo "  READY. Start HTX now:"
echo ""
echo "    htxscreen"
echo "    # select pmem device, activate"
echo ""
echo "  When the kernel panics:"
echo "    - kdump captures vmcore to ${CRASH_PATH:-/var/crash}/"
echo "    - After reboot, run:"
echo "      sudo ./repro/extract_ftrace_from_vmcore.sh"
echo "      ./repro/analyze_trace.sh /tmp/btt_crash_*/btt-trace.log"
echo "============================================"
