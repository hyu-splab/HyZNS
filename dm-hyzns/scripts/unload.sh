#!/bin/bash
#
# Tear down hyzns0 (if any) and remove the module.
# Usage:
#   sudo ./scripts/unload.sh
set -euo pipefail

NAME="${NAME:-hyzns0}"

if dmsetup info "${NAME}" >/dev/null 2>&1; then
    echo "==> dmsetup remove ${NAME}"
    dmsetup remove "${NAME}"
fi

if lsmod | awk '{print $1}' | grep -qx dm_hyzns; then
    echo "==> rmmod dm_hyzns"
    rmmod dm_hyzns
fi

echo "==> done"
