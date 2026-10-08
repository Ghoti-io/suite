#!/bin/bash
# Host side of http-arm64.sh: stages what the container cannot get for itself and
# runs the aarch64 gtest run (libs/http, story 3c of the Defiant milestone).
#
#   suite/tools/xarch/http-arm64-host.sh [scratch-dir]      (default /tmp/xarch-http-arm)
#
# It stages, under <scratch-dir>:
#
#   deps/{cutil,security,compress}   `git archive HEAD` of each library, NOT the
#       working tree: another session may be mid-edit in one of them, and what is
#       tested is libghttp against the dependencies as they are committed (the
#       same rule suite/tools/xwin/http.sh follows for cutil).
#   hostlist/counts.txt   "<test binary> <tests listed>" for every test binary
#       of libs/http, from a HOST build of the library's working tree (the tree
#       under test, which is what the aarch64 build copies). The aarch64 run
#       requires its passed count to equal each of these.
#
# Environment:
#   HOST_PREFIX  the PREFIX= of the host build (rpath target; default $ROOT/.local);
#                the .pc files are found through PKG_CONFIG_PATH if it is set, else
#                here (a prefix of copies, like .local-http, has no share/pkgconfig).
#   HOST_BUILD   the BUILD= name of the host build tree (default xc), so that
#                this does not touch the tree another session is using.
#   JOBS         parallelism.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
D=${1:-/tmp/xarch-http-arm}
HOST_PREFIX=${HOST_PREFIX:-$ROOT/.local}
HOST_BUILD=${HOST_BUILD:-xc}
JOBS=${JOBS:-$(nproc)}
IMAGE=localhost/ghoti-xarch-arm64-jit:deb13-gxx14-gtest
case "$D" in
  ""|/|/tmp|/tmp/|"$ROOT"|"$ROOT"/*|"$HOME"|"$HOME"/) echo "http-arm64-host: refusing scratch dir '$D'" >&2; exit 2 ;;
esac
case "$D" in /*) ;; *) echo "http-arm64-host: the scratch dir must be absolute" >&2; exit 2 ;; esac
podman image exists "$IMAGE" || { echo "http-arm64-host: no image $IMAGE (suite/tools/xarch/Containerfile.arm64-jit)" >&2; exit 2; }

rm -rf "$D/deps" "$D/hostlist"
mkdir -p "$D/deps" "$D/hostlist" || exit 2
for l in cutil security compress; do
  mkdir -p "$D/deps/$l" && git -C "$ROOT/libs/$l" archive HEAD | tar -x -C "$D/deps/$l" ||
    { echo "http-arm64-host: cannot archive libs/$l" >&2; exit 2; }
done

# The host's listing, through the library's own Makefile.
"$HERE/http-host-counts.sh" "$D/hostlist/counts.txt" || exit 2

mkdir -p "$D/scratch"
podman run --rm -v "$ROOT:/work:ro,z" -v "$D/deps:/deps:ro,z" -v "$D/hostlist:/hostlist:ro,z" \
  -v "$D/scratch:/scratch:z" -e JOBS="$JOBS" -e ALLOW_NO_HOSTLIST=0 \
  "$IMAGE" bash /work/suite/tools/xarch/http-arm64.sh
