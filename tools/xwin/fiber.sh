#!/bin/bash
# Cross-compiles cutil for win64 and runs test-fiber.exe under wine (story 1 of
# the Defiant milestone, notes/cutil/fibers.md).  Host side: needs podman, rsync
# and wine.  Run from anywhere; the paths come from this script.
#
#   suite/tools/xwin/fiber.sh [scratch-dir]          (default /tmp/xwin-fiber)
#
# It uses the same container and the same imitation of MSYS2 as the runtime
# stack's run (m1-win.sh, m1-env.sh), builds cutil with its own Makefile, and
# runs every test in test-fiber.exe, rounding isolation included.  Exit status
# is test-fiber.exe's.
#
# What a green run means: the Windows branch of src/fiber.c compiles with the
# real Windows headers, and its two-fibers-different-rounding-modes test passes
# under wine's implementation of the Fiber API.  It does not mean the same on a
# Windows machine: whether FIBER_FLAG_FLOAT_SWITCH is needed there was decided
# by the test under wine, and wine is not Windows.  See notes/cutil/fibers.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-fiber}

for need in podman rsync; do
  command -v $need >/dev/null || { echo "fiber.sh: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "fiber.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "fiber.sh: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }

mkdir -p "$W/logs"
rsync -a --exclude build --exclude .git "$ROOT/libs/cutil/" "$W/cutil/" || exit 2
"$HERE/m1-win.sh" "$W" 'bash /tools/xwin/fiber-win.sh'
