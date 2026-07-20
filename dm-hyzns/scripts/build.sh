#!/bin/bash
#
# Build dm-hyzns.ko in the directory above this script.
# Usage:
#   ./scripts/build.sh           # build against running kernel
#   KDIR=/path/to/linux ./scripts/build.sh
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -z "${KDIR:-}" ]]; then
    KVER="$(uname -r)"
    KDIR="/lib/modules/${KVER}/build"
fi

if [[ ! -d "${KDIR}" ]]; then
    echo "ERROR: kernel build dir not found at ${KDIR}"
    echo "Install matching linux-headers, or set KDIR explicitly."
    exit 1
fi

echo "==> building dm-hyzns.ko against ${KDIR}"
make KDIR="${KDIR}"
echo "==> done: $(ls -l dm-hyzns.ko)"
