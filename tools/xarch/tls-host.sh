#!/bin/bash
# Host side of tls.sh: stages cutil, security, certificate and tls as
# COMMITTED (git archive HEAD: another session may be mid-edit in one of them,
# and what is being tested is tls against the others as they are) and runs the
# check in the ghoti-xarch image.
#
#   suite/tools/xarch/tls-host.sh [scratch-dir]       (default /tmp/tls-xarch)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
D=${1:-/tmp/tls-xarch}
rm -rf "$D/deps" && mkdir -p "$D/deps" || exit 2
for lib in cutil security certificate tls; do
  mkdir -p "$D/deps/$lib" && git -C "$ROOT/libs/$lib" archive HEAD | tar -x -C "$D/deps/$lib" || exit 2
done
exec podman run --rm -v "$ROOT:/work:ro,z" -v "$D/deps:/deps:ro,z" \
  localhost/ghoti-xarch:deb13 bash /work/suite/tools/xarch/tls.sh
