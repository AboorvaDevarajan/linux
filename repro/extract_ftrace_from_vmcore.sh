#!/bin/bash
# Extract the ftrace ring buffer from a kdump vmcore.
#
# Tries multiple methods in order of reliability:
#   1. crash utility with 'trace' extension
#   2. trace-cmd extract
#   3. crash 'log' command (dmesg, which may contain trace_printk output)
#
# Usage:
#   sudo ./repro/extract_ftrace_from_vmcore.sh [vmcore_path] [vmlinux_path]
#
# Defaults:
#   vmcore:  latest in /var/crash/
#   vmlinux: /boot/vmlinux-$(uname -r) or build tree vmlinux

set -e

# Find latest vmcore if not specified
if [ -n "$1" ]; then
    VMCORE="$1"
else
    CRASH_DIR=$(ls -td /var/crash/*/ 2>/dev/null | head -1)
    if [ -z "$CRASH_DIR" ]; then
        echo "ERROR: No crash dumps found in /var/crash/"
        echo "Usage: $0 [vmcore_path] [vmlinux_path]"
        exit 1
    fi
    VMCORE="${CRASH_DIR}vmcore"
fi

if [ ! -r "$VMCORE" ]; then
    echo "ERROR: Cannot read $VMCORE"
    exit 1
fi

# Find vmlinux
if [ -n "$2" ]; then
    VMLINUX="$2"
else
    KREL=$(uname -r)
    for candidate in \
        "/boot/vmlinux-${KREL}" \
        "/usr/lib/debug/lib/modules/${KREL}/vmlinux" \
        "$(dirname "$0")/../vmlinux"; do
        if [ -r "$candidate" ]; then
            VMLINUX="$candidate"
            break
        fi
    done
fi

if [ -z "$VMLINUX" ] || [ ! -r "$VMLINUX" ]; then
    echo "ERROR: Cannot find vmlinux. Specify path as second argument."
    echo "Searched: /boot/vmlinux-$(uname -r), debug modules dir, build tree"
    exit 1
fi

OUTDIR="/tmp/btt_crash_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"

echo "=== BTT crash ftrace extraction ==="
echo "vmcore:  $VMCORE"
echo "vmlinux: $VMLINUX"
echo "output:  $OUTDIR"
echo

# Method 1: crash 'log' — always works, gets dmesg + trace_printk output
echo "--- Extracting kernel log (dmesg + trace_printk) via crash ---"
if command -v crash >/dev/null 2>&1; then
    crash -s "$VMLINUX" "$VMCORE" <<'CRASHEOF' > "$OUTDIR/crash_log.txt" 2>&1
log
quit
CRASHEOF
    if [ -s "$OUTDIR/crash_log.txt" ]; then
        echo "  Saved: $OUTDIR/crash_log.txt ($(wc -l < "$OUTDIR/crash_log.txt") lines)"
        grep -c "BTT_" "$OUTDIR/crash_log.txt" 2>/dev/null && \
            echo "  (contains BTT trace events)" || \
            echo "  (no BTT_ events in dmesg log)"
    fi
else
    echo "  SKIP: 'crash' not installed"
fi
echo

# Method 2: crash 'trace' extension — extracts full ftrace buffer
echo "--- Extracting ftrace ring buffer via crash 'trace' extension ---"
if command -v crash >/dev/null 2>&1; then
    crash -s "$VMLINUX" "$VMCORE" <<'CRASHEOF' > "$OUTDIR/crash_ftrace.txt" 2>&1
extend /usr/lib64/crash/extensions/trace.so
trace show > /dev/stdout
quit
CRASHEOF
    if [ -s "$OUTDIR/crash_ftrace.txt" ] && \
       ! grep -q "extend:.*not found" "$OUTDIR/crash_ftrace.txt"; then
        echo "  Saved: $OUTDIR/crash_ftrace.txt ($(wc -l < "$OUTDIR/crash_ftrace.txt") lines)"
    else
        echo "  SKIP: crash 'trace' extension not available"
        rm -f "$OUTDIR/crash_ftrace.txt"
    fi
else
    echo "  SKIP: 'crash' not installed"
fi
echo

# Method 3: trace-cmd
echo "--- Extracting via trace-cmd ---"
if command -v trace-cmd >/dev/null 2>&1; then
    trace-cmd dump --input "$VMCORE" > "$OUTDIR/tracecmd_dump.txt" 2>&1 || true
    if [ -s "$OUTDIR/tracecmd_dump.txt" ]; then
        echo "  Saved: $OUTDIR/tracecmd_dump.txt"
    else
        echo "  SKIP: trace-cmd dump produced no output"
        rm -f "$OUTDIR/tracecmd_dump.txt"
    fi
else
    echo "  SKIP: 'trace-cmd' not installed"
fi
echo

# Pick the best trace file and filter BTT events
TRACE_FILE=""
for candidate in "$OUTDIR/crash_ftrace.txt" "$OUTDIR/tracecmd_dump.txt" "$OUTDIR/crash_log.txt"; do
    if [ -s "$candidate" ] && grep -q "BTT_" "$candidate" 2>/dev/null; then
        TRACE_FILE="$candidate"
        break
    fi
done

if [ -n "$TRACE_FILE" ]; then
    echo "=== Filtering BTT events from $TRACE_FILE ==="
    grep "BTT_" "$TRACE_FILE" > "$OUTDIR/btt-trace.log"
    echo "  BTT events: $(wc -l < "$OUTDIR/btt-trace.log")"
    echo "  Saved: $OUTDIR/btt-trace.log"
    echo ""
    echo "Run analysis with:"
    echo "  ./repro/analyze_trace.sh $OUTDIR/btt-trace.log"
else
    echo "WARNING: No BTT trace events found in any extraction method."
    echo "The ftrace buffer may have been overwritten or tracing was not enabled."
    echo ""
    echo "Files extracted to: $OUTDIR/"
    ls -la "$OUTDIR/"
fi
