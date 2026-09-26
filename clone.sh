#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Clone every library named in libraries.txt into the sibling libs/ directory.
#
# Run this from a checkout of this repository. The libraries land beside it,
# not inside it:
#
#     mkdir ghoti.io
#     cd ghoti.io
#     git clone https://github.com/Ghoti-io/suite.git
#     cd suite
#     ./clone.sh
#
# A repository that is already there is left alone. ./pull.sh brings those
# up to date. Every repository is attempted even if an earlier one fails.
#
# Usage:
#   ./clone.sh          clone whatever is not here yet
#   ./clone.sh -n       say what would be cloned, without cloning

set -u

DRY_RUN=0
if [ "${1:-}" = "-n" ] || [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
fi

# Paths come from this file, so the shell can be in any directory.
SUITE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SUITE/.." && pwd)
LIBS="$ROOT/libs"
mkdir -p "$LIBS"

# A sibling script, spelled so the line can be run from the current directory.
# A relative path that climbs out of this directory is harder to read than
# the absolute one, so that case uses the absolute path.
beside() {
  rel=$(realpath --relative-to="$PWD" "$SUITE/$1" 2>/dev/null) || rel=""
  case "$rel" in
    ""|..|../*) printf '%s\n' "$SUITE/$1" ;;
    *) printf './%s\n' "$rel" ;;
  esac
}

MANIFEST="$SUITE/libraries.txt"
if [ ! -f "$MANIFEST" ]; then
  echo "clone.sh: no libraries.txt beside this script" >&2
  exit 1
fi

cloned=""
present=""
failed=""

while read -r name remote branch deps; do
  case "$name" in ''|\#*) continue ;; esac
  [ -n "$remote" ] && [ -n "$branch" ] || continue
  : "$deps"

  if [ -d "$LIBS/$name/.git" ]; then
    printf '  %-10s already here\n' "$name"
    present="$present $name"
    continue
  fi

  if [ -e "$LIBS/$name" ]; then
    printf '  %-10s EXISTS but is not a git repository; skipping\n' "$name"
    failed="$failed $name"
    continue
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    printf '  %-10s would clone %s (%s)\n' "$name" "$remote" "$branch"
    cloned="$cloned $name"
    continue
  fi

  printf '  %-10s cloning...' "$name"
  if git clone --quiet --origin upstream --branch "$branch" "$remote" "$LIBS/$name" 2>/dev/null; then
    printf '\r  %-10s cloned %s\n' "$name" "$remote"
    cloned="$cloned $name"
  else
    printf '\r  %-10s CLONE FAILED: %s\n' "$name" "$remote"
    failed="$failed $name"
  fi
done < "$MANIFEST"

printf '\n=== summary ===\n'
[ -n "$cloned" ]  && echo "  cloned:   $cloned"
[ -n "$present" ] && echo "  present:  $present"
[ -n "$failed" ]  && echo "  failed:   $failed"

if [ -n "$present" ]; then
  echo
  echo "The ones already here were left as they are. To update them:"
  echo "  $(beside pull.sh)"
fi

if [ -z "$failed" ] && [ "$DRY_RUN" -eq 0 ]; then
  echo
  echo "Next: $(beside install.sh)"
  echo "      $(beside docs.sh) --container"
fi

[ -z "$failed" ] || exit 1
exit 0
