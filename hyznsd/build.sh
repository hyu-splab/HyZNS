#!/bin/bash
# build.sh - build the daemon (hyznsd) and the dm module (dm-hyzns.ko).
#   bash build.sh          # both
#   bash build.sh daemon   # hyznsd only
#   bash build.sh dm       # dm-hyzns.ko only
# Runtime flags live in hyznsd.conf (no rebuild needed); changing dm #defines
# requires a dm rebuild and a module reload to take effect.
set -e
cd "$(dirname "$0")"
what=${1:-all}

if [[ $what == all || $what == daemon ]]; then
  echo "== hyznsd =="
  make
  ls -la --time-style=+%H:%M:%S hyznsd | awk '{print "   built:", $6, $7}'
fi
if [[ $what == all || $what == dm ]]; then
  echo "== dm-hyzns.ko =="
  ../dm-hyzns/scripts/build.sh | tail -1
fi
echo "done."
