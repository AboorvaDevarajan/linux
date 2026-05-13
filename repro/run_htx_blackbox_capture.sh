#!/bin/bash
# Run BTT black-box alias detector while executing an HTX command.
#
# Usage:
#   sudo ./repro/run_htx_blackbox_capture.sh "<htx command>"
# Example:
#   sudo ./repro/run_htx_blackbox_capture.sh "htxcmdline -run -mdt /path/to/mdt.hd"

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: run as root"
  exit 1
fi

if [ $# -lt 1 ]; then
  echo "Usage: $0 \"<htx command>\""
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BT_SCRIPT="$SCRIPT_DIR/btt_blackbox_alias.bt"
OUTDIR="/tmp/htx_blackbox_$(date +%Y%m%d_%H%M%S)"
HTX_CMD="$1"

mkdir -p "$OUTDIR"
echo "Output dir: $OUTDIR"
echo "BPF script: $BT_SCRIPT"
echo "HTX cmd:    $HTX_CMD"

# Start bpftrace
set +e
bpftrace "$BT_SCRIPT" > "$OUTDIR/bpftrace.log" 2>&1 &
BPID=$!
set -e

sleep 2
if ! kill -0 "$BPID" 2>/dev/null; then
  echo "ERROR: bpftrace failed to start. See $OUTDIR/bpftrace.log"
  exit 1
fi

echo "bpftrace pid: $BPID"
echo "=== Running HTX command ==="
set +e
bash -lc "$HTX_CMD" > "$OUTDIR/htx.log" 2>&1
RC=$?
set -e

echo "=== Stopping bpftrace ==="
kill -INT "$BPID" 2>/dev/null || true
wait "$BPID" 2>/dev/null || true

dmesg | tail -200 > "$OUTDIR/dmesg_tail.log" 2>/dev/null || true

echo ""
echo "=== Summary ==="
echo "HTX exit code: $RC"
echo "bpf log:       $OUTDIR/bpftrace.log"
echo "htx log:       $OUTDIR/htx.log"
echo "dmesg tail:    $OUTDIR/dmesg_tail.log"
echo ""
echo "--- Alias lines (if any) ---"
rg "ALIAS " "$OUTDIR/bpftrace.log" || true

exit "$RC"

