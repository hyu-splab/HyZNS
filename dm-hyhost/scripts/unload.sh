#!/bin/bash
#
# Tear down hyhost0 (if any) and remove the module.
# Usage:
#   sudo ./scripts/unload.sh
set -euo pipefail

NAME="${NAME:-hyhost0}"

if dmsetup info "${NAME}" >/dev/null 2>&1; then
    echo "==> dmsetup remove ${NAME}"
    dmsetup remove "${NAME}"
fi

if lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    echo "==> rmmod dm_hyhost"
    rmmod dm_hyhost
fi

echo "==> done"
