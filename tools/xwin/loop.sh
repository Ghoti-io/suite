#!/bin/bash
# Cross-compiles cutil for win64 and runs test-socket.exe and test-loop.exe
# under wine (story 2 of the Defiant milestone, notes/cutil/event-loop.md).
# Host side: needs podman, rsync and wine.  Run from anywhere.
#
#   suite/tools/xwin/loop.sh [scratch-dir]          (default /tmp/xwin-loop)
#
# It uses the same container and the same imitation of MSYS2 as fiber.sh and
# the runtime stack's run (m1-win.sh, m1-env.sh), builds cutil with its own
# Makefile, and runs every test of both executables.  Exit status is 0 only
# when both pass and each planted defect is caught.
#
# What a green run means: the Windows arm of src/socket.c and src/loop.c (the
# I/O completion port, AcceptEx, ConnectEx) compiles against the real Windows
# headers, and the tests that drive it on Linux pass against wine's Winsock and
# completion-port implementation: echo, a write larger than the socket buffer,
# timers, cancel, a post from another thread, a fiber waiting on a read,
# datagrams, destroy with work outstanding.  It does not mean the same on a
# Windows machine: wine is not Windows.  See notes/cutil/event-loop.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${1:-/tmp/xwin-loop}

for need in podman rsync; do
  command -v $need >/dev/null || { echo "loop.sh: $need is needed on the host" >&2; exit 2; }
done
[ -d /usr/lib/wine ] || { echo "loop.sh: the host needs wine in /usr/lib/wine" >&2; exit 2; }
[ -d /usr/src/googletest ] || { echo "loop.sh: the host needs googletest sources in /usr/src/googletest" >&2; exit 2; }

mkdir -p "$W/logs"
rsync -a --delete --exclude build --exclude .git "$ROOT/libs/cutil/" "$W/cutil/" || exit 2
"$HERE/m1-win.sh" "$W" 'bash /tools/xwin/loop-win.sh'
