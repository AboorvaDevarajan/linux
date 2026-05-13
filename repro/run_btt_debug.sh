#!/bin/bash
# Run the BTT reproducer.  Kernel ftrace is enabled only if the running
# kernel is the -bttdebug build; otherwise we just run the reproducer
# and rely on its own miscompare reports.
#
# Usage:
#   sudo ./repro/run_btt_debug.sh /dev/pmem0.4s namespace0.4 [sleep_seconds]
#
# The script will:
#   1. Build htx_btt_repro if missing or stale.
#   2. Recreate the BTT namespace (ndctl create-namespace --force).
#   3. If running a -bttdebug kernel: enlarge ftrace ring buffer, clear,
#      enable; otherwise skip tracing.
#   4. Run htx_btt_repro (phase 1: bwrc, phase 2: sleep, phase 3: RC).
#   5. Stop tracing on exit and copy the trace if it was enabled.
#   6. Save miscompare reports + last 200 lines of dmesg.

set -e

DEV="${1:-/dev/pmem0.4s}"
NS="${2:-namespace0.4}"
PHASE2_SLEEP="${3:-5}"
OUTDIR="/tmp/btt_debug_$(date +%Y%m%d_%H%M%S)"
SRCDIR="$(cd "$(dirname "$0")" && pwd)"
TRACE_DIR="/sys/kernel/debug/tracing"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (needs ndctl, optionally ftrace)"
    exit 1
fi

mkdir -p "$OUTDIR"
echo "Output dir: $OUTDIR"

KREL=$(uname -r)
echo "Running kernel: $KREL"

USE_TRACE=0
if echo "$KREL" | grep -q bttdebug && [ -d "$TRACE_DIR" ]; then
    USE_TRACE=1
    echo "Kernel tracing: ENABLED (trace_printk events available)"
else
    echo "Kernel tracing: SKIPPED (not a -bttdebug kernel; only userland reports)"
fi

# Build reproducer if needed
if [ ! -x "$SRCDIR/htx_btt_repro" ] || \
   [ "$SRCDIR/htx_btt_repro.c" -nt "$SRCDIR/htx_btt_repro" ]; then
    echo "=== Building htx_btt_repro ==="
    gcc -O2 -Wall -pthread "$SRCDIR/htx_btt_repro.c" \
        -o "$SRCDIR/htx_btt_repro"
fi

# Recreate namespace (best-effort)
echo "=== Recreating BTT namespace $NS ==="
ndctl create-namespace --force -m sector -e "$NS" || {
    echo "WARNING: ndctl create-namespace failed; continuing with existing $DEV"
}
sleep 1

if [ ! -b "$DEV" ]; then
    echo "ERROR: $DEV not found after namespace creation"
    exit 1
fi

if [ "$USE_TRACE" = "1" ]; then
    echo "=== Setting up kernel tracing ==="
    echo 0 > "$TRACE_DIR/tracing_on"
    echo 32768 > "$TRACE_DIR/buffer_size_kb" 2>/dev/null || \
        echo "(could not resize trace buffer; using default)"
    echo nop > "$TRACE_DIR/current_tracer"
    echo > "$TRACE_DIR/trace"
    echo 1 > "$TRACE_DIR/tracing_on"
fi

echo "=== Running htx_btt_repro ==="
echo "Device: $DEV"
echo "Sleep:  $PHASE2_SLEEP seconds"
echo ""

set +e
"$SRCDIR/htx_btt_repro" -t "$(nproc)" -i 50000 -s "$PHASE2_SLEEP" \
    -E 20 -L "$OUTDIR" -v "$DEV"
RC=$?
set -e

if [ "$USE_TRACE" = "1" ]; then
    echo 0 > "$TRACE_DIR/tracing_on"
    cp "$TRACE_DIR/trace" "$OUTDIR/btt-trace.log"
fi

dmesg | tail -200 > "$OUTDIR/dmesg.log" 2>/dev/null || true

echo ""
echo "=== Results ==="
echo "Exit code:  $RC"
[ "$USE_TRACE" = "1" ] && \
    echo "Trace log:  $OUTDIR/btt-trace.log ($(stat -c%s "$OUTDIR/btt-trace.log") bytes)"
echo "Dmesg:      $OUTDIR/dmesg.log"
echo "Reports:    $OUTDIR/htxbtt_mis_*"
echo ""

if [ "$RC" -ne 0 ]; then
    echo "FAIL detected."
    if [ "$USE_TRACE" = "1" ]; then
        echo ""
        echo "--- BTT_READ_MISMATCH events (first 20) ---"
        grep BTT_READ_MISMATCH "$OUTDIR/btt-trace.log" | head -20 || \
            echo "(none found in trace)"
    fi
    echo ""
    echo "--- Miscompare report summary (first 5) ---"
    n=0
    for f in "$OUTDIR"/htxbtt_mis_*.txt; do
        [ -f "$f" ] || continue
        head -12 "$f"
        echo "---"
        n=$((n+1))
        [ $n -ge 5 ] && break
    done
    echo ""
    echo "Next steps:"
    echo "  - Share the directory: $OUTDIR"
fi

exit $RC
