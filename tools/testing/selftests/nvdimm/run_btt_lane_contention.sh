#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Setup a sector-mode namespace, run the BTT lane contention test, and clean up.
# Requires: ndctl, root privileges, at least one pmem region.

set -e

NPROCS=${1:-16}
ITERS=${2:-100}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_BIN="${SCRIPT_DIR}/btt_lane_contention"
NAMESPACE=""
BLOCKDEV=""
RC=0

cleanup() {
	if [ -n "$NAMESPACE" ]; then
		echo "Cleaning up namespace $NAMESPACE..."
		ndctl disable-namespace "$NAMESPACE" 2>/dev/null || true
		ndctl destroy-namespace "$NAMESPACE" 2>/dev/null || true
	fi
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: must be root"
	exit 4  # kselftest SKIP
fi

if ! command -v ndctl >/dev/null 2>&1; then
	echo "SKIP: ndctl not found"
	exit 4
fi

if [ ! -x "$TEST_BIN" ]; then
	echo "SKIP: $TEST_BIN not built (run make first)"
	exit 4
fi

REGION=$(ndctl list -Ri | grep -o '"dev":"region[0-9]*"' | head -1 | cut -d'"' -f4)
if [ -z "$REGION" ]; then
	echo "SKIP: no idle pmem region found"
	exit 4
fi

echo "Creating sector-mode namespace on $REGION..."
NS_JSON=$(ndctl create-namespace -r "$REGION" -m sector -f 2>&1) || {
	echo "SKIP: failed to create namespace on $REGION"
	exit 4
}

NAMESPACE=$(echo "$NS_JSON" | grep -o '"dev":"namespace[0-9.]*"' | cut -d'"' -f4)
BLOCKDEV=$(echo "$NS_JSON" | grep -o '"blockdev":"[^"]*"' | cut -d'"' -f4)

if [ -z "$BLOCKDEV" ]; then
	echo "SKIP: could not determine block device"
	exit 4
fi

echo "Namespace: $NAMESPACE"
echo "Block device: /dev/$BLOCKDEV"
echo "Running: $TEST_BIN /dev/$BLOCKDEV $NPROCS $ITERS"
echo "---"

set +e
"$TEST_BIN" "/dev/$BLOCKDEV" "$NPROCS" "$ITERS"
RC=$?
set -e

exit $RC
