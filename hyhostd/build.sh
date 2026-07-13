#!/bin/bash
# build.sh - build the daemon (hyhostd) and the dm module (dm-hyhost.ko).
#   bash build.sh          # both
#   bash build.sh daemon   # hyhostd only
#   bash build.sh dm       # dm-hyhost.ko only
# Runtime flags live in hyhostd.conf (no rebuild needed); changing dm #defines
# requires a dm rebuild and a module reload to take effect.
set -e
cd "$(dirname "$0")"
what=${1:-all}

if [[ $what == all || $what == daemon ]]; then
  echo "== hyhostd =="
  make
  ls -la --time-style=+%H:%M:%S hyhostd | awk '{print "   built:", $6, $7}'
fi
if [[ $what == all || $what == dm ]]; then
  echo "== dm-hyhost.ko =="
  ../dm-hyhost/scripts/build.sh | tail -1
fi
echo "done."
