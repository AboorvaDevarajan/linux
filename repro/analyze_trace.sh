#!/bin/bash
# Analyze a btt-trace.log produced by run_btt_debug.sh and look for the
# "BTT map/freelist dual-ownership" pattern: a single physical block
# (postmap) referenced by two different premaps simultaneously, AND/OR
# a freelist entry pointing at a postmap that is still present in a
# live map entry.
#
# Usage:
#   ./repro/analyze_trace.sh /tmp/btt_debug_<ts>/btt-trace.log
#
# Output is written to <log>.analysis.txt next to the trace.

set -e

LOG="${1:?Usage: $0 <btt-trace.log>}"
[ -r "$LOG" ] || { echo "ERROR: cannot read $LOG"; exit 1; }

OUT="${LOG}.analysis.txt"
exec > "$OUT" 2>&1

echo "=== BTT trace analysis for $LOG ==="
echo "Generated: $(date)"
echo

echo "--- 1. Top-level event counts ---"
for ev in BTT_WRITE BTT_READ_MISMATCH BTT_FLOG; do
    n=$(grep -c "$ev" "$LOG" || true)
    printf "  %-20s %s\n" "$ev" "$n"
done
echo

echo "--- 2. First 20 BTT_READ_MISMATCH events ---"
grep BTT_READ_MISMATCH "$LOG" | head -20 || echo "(none)"
echo

echo "--- 3. Per-(premap,postmap) mismatch count ---"
echo "  count premap     postmap    data_lba"
grep BTT_READ_MISMATCH "$LOG" | \
    sed -nE 's/.*premap=(0x[0-9a-f]+) postmap=(0x[0-9a-f]+) data_lba=(0x[0-9a-f]+).*/\1 \2 \3/p' | \
    sort | uniq -c | sort -rn | head -30
echo

echo "--- 4. For each unique mismatched postmap, find all BTT_WRITE events that wrote to it ---"
mapfile -t POSTMAPS < <(grep BTT_READ_MISMATCH "$LOG" | \
    sed -nE 's/.*postmap=(0x[0-9a-f]+).*/\1/p' | sort -u | head -10)

for pm in "${POSTMAPS[@]}"; do
    echo
    echo "  ## postmap=$pm"
    echo "  - BTT_WRITE events whose new_post=$pm (the writes that placed data there):"
    grep "BTT_WRITE" "$LOG" | grep "new_post=$pm" | head -10
    echo "  - BTT_WRITE events whose old_post=$pm (writes that displaced it back to freelist):"
    grep "BTT_WRITE" "$LOG" | grep "old_post=$pm" | head -10
    echo "  - BTT_FLOG events whose free_block=$pm (freelist recycling):"
    grep "BTT_FLOG" "$LOG" | grep "free_block=$pm" | head -10
done
echo

echo "--- 5. For each mismatched (premap, data_lba), check if some BTT_WRITE wrote (data_lba -> postmap) ---"
echo "    If yes: the read got data from a writer who used that physical block"
echo "    for a *different* premap. That is the dual-ownership smoking gun."
echo
grep BTT_READ_MISMATCH "$LOG" | head -10 | while read -r line; do
    pm=$(echo "$line" | sed -nE 's/.*premap=(0x[0-9a-f]+).*/\1/p')
    pp=$(echo "$line" | sed -nE 's/.*postmap=(0x[0-9a-f]+).*/\1/p')
    dl=$(echo "$line" | sed -nE 's/.*data_lba=(0x[0-9a-f]+).*/\1/p')
    echo "  Mismatch: premap=$pm postmap=$pp data_lba=$dl"
    # The data on disk says it belongs to LBA=data_lba. Find a write
    # that put data_lba's content into postmap pp.
    hits=$(grep "BTT_WRITE" "$LOG" | grep "premap=$dl" | grep "new_post=$pp" | head -3 || true)
    if [ -n "$hits" ]; then
        echo "    SMOKING GUN: a BTT_WRITE put premap=$dl into the same postmap $pp:"
        echo "$hits" | sed 's/^/      /'
    else
        echo "    (no direct write of premap=$dl to postmap=$pp found in window)"
    fi
done
echo

echo "--- 6. BTT_FLOG events grouped by (free_block) -- look for the same physical block being freed multiple times in close succession ---"
grep BTT_FLOG "$LOG" | \
    sed -nE 's/.*free_block=(0x[0-9a-f]+).*/\1/p' | \
    sort | uniq -c | sort -rn | head -20

echo
echo "=== Analysis complete: $OUT ==="
