#!/bin/bash
#
# Reshape the R/S boundary on a hyzns dm target (suspend-aware wrapper).
#
# Usage:
#   sudo ./scripts/set_r_end.sh [TARGET] <NEW_R_END_LBA>
#
# Default TARGET=hyzns0.
#
# `dmsetup message ... set_r_end` itself drives the full reshape: it
# emits a REQ_OP_ZONE_MODIFY bio (ZSA 0x20, new ABA in SLBA) to the
# backing device and, on a shrink, force-GCs the boundary R-lines before
# committing the dm-side L2P / free ring. This wrapper only adds dm
# suspend/resume around the message so concurrent IO is drained - invoke
# `dmsetup message` directly if you know there is no concurrent IO.
set -euo pipefail

if [[ $# -eq 1 ]]; then
    TARGET=hyzns0
    NEW=$1
elif [[ $# -eq 2 ]]; then
    TARGET=$1
    NEW=$2
else
    echo "Usage: $0 [TARGET] <NEW_R_END_LBA>" >&2
    exit 2
fi

if ! dmsetup info "${TARGET}" >/dev/null 2>&1; then
    echo "ERROR: dm target '${TARGET}' not found" >&2
    exit 1
fi
if (( NEW % 8 != 0 )); then
    echo "ERROR: NEW_R_END_LBA must be page-aligned (multiple of 8)" >&2
    exit 1
fi

echo "==> reshape target=${TARGET} new_r_end=${NEW}"

dmsetup suspend "${TARGET}"
trap 'dmsetup resume "${TARGET}" 2>/dev/null || true' EXIT

if ! dmsetup message "${TARGET}" 0 set_r_end "${NEW}"; then
    echo "ERROR: set_r_end failed; boundary unchanged" >&2
    exit 1
fi

trap - EXIT
dmsetup resume "${TARGET}"

echo "==> done"
dmsetup status "${TARGET}"
