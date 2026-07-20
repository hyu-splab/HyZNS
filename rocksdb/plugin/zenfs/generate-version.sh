#!/bin/bash
set -e
REPO_ROOT=$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )
cd $REPO_ROOT

# 'git describe --abbrev=7 --dirty' will output a version in that looks like "v0.1.0-12-g3456789-dirty".
# VERSION=$(git describe --abbrev=7 --dirty)
# Fall back to a default version when git is unavailable or this is not a repo.
if git rev-parse --git-dir > /dev/null 2>&1; then
    VERSION=$(git describe --abbrev=7 --dirty 2>/dev/null || echo "unknown-version")
else
    VERSION="manual-build"
fi

updateVersionFile () {
  if [ "${#VERSION}" -gt 63 ]; then
    echo "The version is longer than 63 chars, using truncated version" >&2
    VERSION="${VERSION:0:63}"
  fi
  printf '#pragma once\n#define ZENFS_VERSION "%s"\n' \
    $VERSION > $REPO_ROOT/fs/version.h
  return 0
}

RET=0
if [ -f "$REPO_ROOT/fs/version.h" ]; then
  PERSISTED_VERSION=$(cat $REPO_ROOT/fs/version.h | grep ' ZENFS_VERSION ' | grep -o '".*"' | sed 's/"//g' 2>/dev/null || echo "")
  if [ "$PERSISTED_VERSION" != "$VERSION" ]; then
    RET=$(updateVersionFile)
  fi
else
  RET=$(updateVersionFile)
fi
exit $RET
