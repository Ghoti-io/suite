#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Render the shape gallery on this machine, for when the EVO-X2 cannot be
# reached; from the workspace root:
#
#   suite/tools/shape-gallery/run-local.sh
#
# The same as run-evo.sh (the committed trees staged by common.sh, render.sh
# in ghoti-build:gcc16, a run directory with its manifest), but under podman
# here, kept light: this workstation has 15 GB shared with Claude Desktop and
# several sessions, and has been taken down by a heavy run before.  So the
# container is held to 3 GB and two make jobs, the whole thing runs niced
# under tools/memcap, and it refuses to start when less than
# SHAPE_GALLERY_MIN_FREE_MB (default 6000) MB is available.  Expect it to take
# several times as long as the EVO-X2.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/common.sh"

MIN_FREE=${SHAPE_GALLERY_MIN_FREE_MB:-6000}
avail=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
[ "$avail" -ge "$MIN_FREE" ] || die "only $avail MB available, want $MIN_FREE; use run-evo.sh, or wait"
command -v podman > /dev/null || die "no podman"
podman image exists "localhost/$IMAGE" || die "no localhost/$IMAGE; build it from suite/Containerfile.build (suite/install.sh does)"

START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mkdir -p "$RUNS"
STAGE=$(mktemp -d "$RUNS/.stage-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
trap 'exit 130' INT TERM

stage "$STAGE"
warn_dirty
NAME=$(run_name "$STAGE")
RUN=$RUNS/$NAME
mkdir -p "$RUN" "$STAGE/prefix" "$STAGE/tmp"
echo "shape-gallery: libraries: $LIBS"

rc=0
# shellcheck disable=SC2086 # LIBS is a word list
nice -n 10 "$ROOT/tools/memcap" -m 3G -- \
  podman run --rm --memory=3g --memory-swap=3g --userns=keep-id \
  -v "$STAGE:$STAGE:z" -v "$RUN:$RUN:z" -w "$STAGE" -e TMPDIR="$STAGE/tmp" "localhost/$IMAGE" \
  sh suite/tools/shape-gallery/render.sh "$STAGE" "$STAGE/prefix" "$RUN" 2 $LIBS || rc=$?

{
  echo "host: $(hostname)"
  echo "image: $IMAGE"
  echo "image_id: sha256:$(podman image inspect --format '{{.Id}}' "localhost/$IMAGE")"
} > "$RUN/host.txt"
write_manifest "$STAGE" "$RUN" "this workstation, podman"
cat "$RUN/manifest.txt"
[ "$rc" -eq 0 ] || { echo "shape-gallery: the render failed ($rc); see $RUN/logs/" >&2; echo "$RUN"; exit 1; }
echo "$RUN"
