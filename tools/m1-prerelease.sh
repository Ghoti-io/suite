#!/bin/bash
# The runtime stack's checks that `make test` in a library cannot run: they
# need a foreign instruction set or a foreign operating system, and so they
# need containers, qemu and wine, which a library may not reach for.
#
#   suite/tools/m1-prerelease.sh [arm64|windows]     (default: both)
#
# arm64    suite/tools/xarch/jit-arm64.sh in its image: runtime-jit's suites and
#          lang-tang's JIT arm built for AArch64 and run under qemu-aarch64,
#          with an x86-64 control and a planted arm64 defect that must fail.
#          Story 17 found that a regression in grjit_compile's arm64 branch
#          (target selection, the instruction-cache call) passes every host
#          `make test` and fails only this.
# windows  suite/tools/xwin/m1-run.sh: the five libraries cross-built for win64 and
#          run under wine, then the probe and the controls that put each
#          Windows fix back to its old self.
#
# Run it before a release and after a change to a backend, the page provider
# or anything under a _WIN32 or an architecture branch. It exits 0 only if
# every part it ran did. It never writes outside /tmp and the build output of
# the scripts it calls.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
want=${1:-both}
IMAGE=ghoti-xarch-arm64-jit:deb13-gxx14-gtest
SCRATCH=${XARCH_SCRATCH:-/tmp/xarch-jit}
failed=0

case "$want" in both | arm64 | windows) ;; *) echo "usage: $0 [arm64|windows]" >&2; exit 2 ;; esac

if [ "$want" != windows ]; then
  echo "=== arm64 under qemu ==="
  command -v podman >/dev/null || { echo "m1-prerelease: podman is needed" >&2; exit 2; }
  if ! podman image exists "localhost/$IMAGE"; then
    podman build -t "$IMAGE" -f "$HERE/xarch/Containerfile.arm64-jit" "$HERE/xarch" || exit 2
  fi
  mkdir -p "$SCRATCH"
  podman run --rm -v "$ROOT:/work:ro,z" -v "$SCRATCH:/scratch:z" "$IMAGE" \
    bash /work/suite/tools/xarch/jit-arm64.sh || failed=1
fi

if [ "$want" != arm64 ]; then
  echo "=== Windows x86-64 under wine ==="
  "$HERE/xwin/m1-run.sh" || failed=1
fi

if [ $failed -eq 0 ]; then
  echo "m1-prerelease: every part passed"
else
  echo "m1-prerelease: FAILED" >&2
fi
exit $failed
