#!/bin/bash
# Run HTX alongside the BTT flight recorder (bpftrace alias detector).
#
# This script is designed for the UNPATCHED kernel where the bug reproduces.
# It starts the bpftrace alias detector in the background, then lets you
# run HTX. When HTX crashes (sysrq panic) or you stop it, the bpftrace
# output is preserved.
#
# Usage:
#   sudo ./repro/run_htx_with_alias_detector.sh [options]
#
# Options:
#   -o DIR     Output directory (default: /root/btt_flight_<timestamp>)
#   -b FILE    bpftrace script (default: repro/btt_blackbox_alias.bt)
#   -n         No-HTX mode: just start bpftrace, user runs HTX manually
#
# The script saves:
#   - bpf_alias.log    — full bpftrace output (alias events + heartbeats)
#   - dmesg_pre.log    — dmesg snapshot before HTX
#   - dmesg_post.log   — dmesg snapshot after HTX (if we survive the crash)
#
# If the kernel panics (sysrq from HTX miscompare):
#   - bpf_alias.log on disk will have all data flushed up to the crash
#     (stdbuf -oL ensures line-buffered output)
#   - kdump vmcore will NOT contain bpftrace data (it's userspace)
#   - After reboot, check /root/btt_flight_*/bpf_alias.log

set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
BT_SCRIPT="${SCRIPTDIR}/btt_blackbox_alias.bt"
OUTDIR=""
NO_HTX=0

while getopts "o:b:n" opt; do
    case $opt in
        o) OUTDIR="$OPTARG" ;;
        b) BT_SCRIPT="$OPTARG" ;;
        n) NO_HTX=1 ;;
        *) echo "Usage: $0 [-o outdir] [-b bpftrace_script] [-n]"; exit 1 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root"
    exit 1
fi

if [ -z "$OUTDIR" ]; then
    OUTDIR="/root/btt_flight_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUTDIR"

if [ ! -r "$BT_SCRIPT" ]; then
    echo "ERROR: bpftrace script not found: $BT_SCRIPT"
    exit 1
fi

if ! command -v bpftrace >/dev/null 2>&1; then
    echo "ERROR: bpftrace not installed"
    exit 1
fi

echo "=== BTT Flight Recorder ==="
echo "Kernel:    $(uname -r)"
echo "Script:    $BT_SCRIPT"
echo "Output:    $OUTDIR"
echo

# Pre-flight checks
echo "--- Pre-flight ---"

# Check nd_btt module is loaded
if ! grep -q btt_submit_bio /proc/kallsyms 2>/dev/null; then
    echo "WARNING: btt_submit_bio not in kallsyms. Is nd_btt loaded?"
    echo "  Try: modprobe nd_btt"
fi

# Check pmem devices exist
if ls /dev/pmem* >/dev/null 2>&1; then
    echo "pmem devices: $(ls /dev/pmem* 2>/dev/null | tr '\n' ' ')"
else
    echo "WARNING: No /dev/pmem* devices found"
fi

# Snapshot dmesg before
dmesg > "$OUTDIR/dmesg_pre.log" 2>/dev/null || true
echo "dmesg pre-snapshot: $OUTDIR/dmesg_pre.log"
echo

# Start bpftrace in background with line-buffered output
echo "--- Starting bpftrace flight recorder ---"
LOGFILE="$OUTDIR/bpf_alias.log"

stdbuf -oL bpftrace "$BT_SCRIPT" > "$LOGFILE" 2>&1 &
BPF_PID=$!

sleep 2

if ! kill -0 "$BPF_PID" 2>/dev/null; then
    echo "ERROR: bpftrace failed to start. Check $LOGFILE"
    cat "$LOGFILE"
    exit 1
fi

echo "bpftrace PID: $BPF_PID"
echo "Log file:     $LOGFILE"
echo

# Sync to disk frequently
(
    while kill -0 "$BPF_PID" 2>/dev/null; do
        sync
        sleep 5
    done
) &
SYNC_PID=$!

cleanup() {
    echo ""
    echo "--- Shutting down ---"

    # Kill the sync loop
    kill "$SYNC_PID" 2>/dev/null || true
    wait "$SYNC_PID" 2>/dev/null || true

    # Stop bpftrace gracefully (SIGINT triggers END block)
    if kill -0 "$BPF_PID" 2>/dev/null; then
        echo "Stopping bpftrace (PID $BPF_PID)..."
        kill -INT "$BPF_PID" 2>/dev/null || true
        # Wait up to 10 seconds for graceful shutdown
        for i in $(seq 1 10); do
            kill -0 "$BPF_PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$BPF_PID" 2>/dev/null || true
        wait "$BPF_PID" 2>/dev/null || true
    fi

    # Post-crash dmesg
    dmesg > "$OUTDIR/dmesg_post.log" 2>/dev/null || true

    # Summary
    echo ""
    echo "=== Flight Recorder Results ==="
    echo "Output dir: $OUTDIR"
    echo ""

    if [ -s "$LOGFILE" ]; then
        ALIAS_N=$(grep -c "^!!! ALIAS" "$LOGFILE" 2>/dev/null || echo 0)
        WRITES=$(grep "map_write events:" "$LOGFILE" | tail -1 || echo "unknown")
        echo "Alias detections: $ALIAS_N"
        echo "Last summary:     $WRITES"

        if [ "$ALIAS_N" -gt 0 ]; then
            echo ""
            echo "--- ALIAS events found! ---"
            grep -A2 "^!!! ALIAS" "$LOGFILE"
            echo ""
            echo "Full log: $LOGFILE"
        else
            echo "No aliases detected in this run."
        fi
    else
        echo "WARNING: Log file is empty (bpftrace may have crashed)"
    fi

    echo ""
    echo "Files:"
    ls -lh "$OUTDIR/"
}

trap cleanup EXIT INT TERM

if [ "$NO_HTX" -eq 1 ]; then
    echo "============================================"
    echo "  bpftrace is running. Start HTX manually:"
    echo ""
    echo "    htxscreen"
    echo ""
    echo "  Press Ctrl-C here when done."
    echo "============================================"
    echo ""

    # Monitor bpftrace — tail the log and wait
    tail -f "$LOGFILE" &
    TAIL_PID=$!
    wait "$BPF_PID" 2>/dev/null || true
    kill "$TAIL_PID" 2>/dev/null || true
else
    echo "============================================"
    echo "  bpftrace is running in background."
    echo "  Starting htxscreen now..."
    echo "============================================"
    echo ""

    # Run HTX interactively
    htxscreen || true
fi
