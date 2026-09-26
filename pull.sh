#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Bring every cloned library up to date with its remote master.
#
# clone.sh leaves a checkout that is already there alone. This is the
# update: fetch, then fast-forward master. A master with local commits the
# remote does not have is reported and left alone. A branch other than
# master is not checked out or merged; master is moved, and the branch is
# reported if it is behind.
#
# Usage:
#   ./pull.sh          fetch and fast-forward each master
#   ./pull.sh -n       fetch, and show what each would take in

set -u

DRY_RUN=0
if [ "${1:-}" = "-n" ] || [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
fi

SUITE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SUITE/.." && pwd)
LIBS="$ROOT/libs"
MANIFEST="$SUITE/libraries.txt"

if [ ! -f "$MANIFEST" ]; then
  echo "pull.sh: no libraries.txt beside this script" >&2
  exit 1
fi

publish_remote() {
  if git -C "$1" remote get-url upstream >/dev/null 2>&1; then
    printf '%s\n' upstream
  elif git -C "$1" remote get-url origin >/dev/null 2>&1; then
    printf '%s\n' origin
  fi
}

failed=""
updated=""
current=""
behind=""
missing=""

while read -r name remote branch deps; do
  case "$name" in ''|\#*) continue ;; esac
  [ -n "$remote" ] && [ -n "$branch" ] || continue
  : "$deps"

  if [ ! -d "$LIBS/$name/.git" ]; then
    printf '  %-10s not cloned\n' "$name"
    missing="$missing $name"
    continue
  fi

  printf '\n=== %s ===\n' "$name"

  repo="$LIBS/$name"
  remote_name=$(publish_remote "$repo")
  if [ -z "$remote_name" ]; then
    echo "  no upstream or origin remote; skipping"
    failed="$failed $name"
    continue
  fi

  if ! git -C "$repo" fetch --quiet "$remote_name"; then
    echo "  FETCH FAILED"
    failed="$failed $name"
    continue
  fi

  checked_out=$(git -C "$repo" rev-parse --abbrev-ref HEAD 2>/dev/null)
  incoming=$(git -C "$repo" rev-list --count "master..$remote_name/master" 2>/dev/null || echo '?')
  local_only=$(git -C "$repo" rev-list --count "$remote_name/master..master" 2>/dev/null || echo '?')

  if [ "$incoming" = "0" ]; then
    echo "  master is up to date with $remote_name/master"
    current="$current $name"
  else
    echo "  $incoming incoming commit(s):"
    git -C "$repo" log --oneline "master..$remote_name/master" | sed 's/^/    /'

    if [ "$local_only" != "0" ]; then
      echo "  master has $local_only local commit(s) $remote_name does not; not fast-forwardable"
      failed="$failed $name"
    elif [ "$DRY_RUN" -eq 1 ]; then
      echo "  dry run: not updating"
    elif [ "$checked_out" = "master" ]; then
      if git -C "$repo" merge --ff-only --quiet "$remote_name/master"; then
        updated="$updated $name"
      else
        echo "  FAST-FORWARD FAILED (uncommitted changes in the way?)"
        failed="$failed $name"
      fi
    else
      if git -C "$repo" fetch --quiet "$remote_name" master:master; then
        echo "  master fast-forwarded; '$checked_out' is checked out and was not touched"
        updated="$updated $name"
      else
        echo "  FAST-FORWARD OF master FAILED"
        failed="$failed $name"
      fi
    fi
  fi

  if [ "$checked_out" != "master" ] && [ "$checked_out" != "HEAD" ]; then
    lag=$(git -C "$repo" rev-list --count "$checked_out"..master 2>/dev/null || echo '?')
    if [ "$lag" != "0" ]; then
      echo "  '$checked_out' is $lag commit(s) behind master; merge when ready"
      behind="$behind $name"
    fi
  fi
done < "$MANIFEST"

printf '\n=== summary ===\n'
[ -n "$updated" ] && echo "  updated:  $updated"
[ -n "$current" ] && echo "  current:  $current"
[ -n "$missing" ] && echo "  missing:  $missing"
[ -n "$behind" ]  && echo "  branches behind master:$behind"
[ -n "$failed" ]  && echo "  failed:   $failed"

[ -z "$failed" ] || exit 1
exit 0
