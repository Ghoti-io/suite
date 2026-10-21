#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Render the shape gallery on the EVO-X2, from the workspace root:
#
#   suite/tools/shape-gallery/run-evo.sh
#
# Stages the committed tree (HEAD) of suite and of every library the gallery
# needs (common.sh), ships it through tools/evo as one job, and runs render.sh
# there in ghoti-build:gcc16 under the job's memory cap and time limit; then
# records the box's image id and name, waits for the job, fetches it, cleans it
# off the box, and leaves the run in runs/<UTC time>_<commits>/ with its
# manifest.  Prints that directory last.  Compare it with the run before:
#
#   python3 -I suite/tools/shape-gallery/compare.py
#
# tools/evo ships a library's working tree; it is pointed at the stage instead
# by running it through a link inside the stage, since it takes its workspace
# from where the script sits.  Its fetched copy lands in the stage's .evo/.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/common.sh"

[ -x "$ROOT/tools/evo" ] || die "no tools/evo under $ROOT"
START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mkdir -p "$RUNS"
STAGE=$(mktemp -d "$RUNS/.stage-XXXXXX")
JOB=
cleanup() {
  if [ -n "$JOB" ]; then
    # A job that got this far is fetched before it is cleaned, so `clean` never needs --force.
    "$STAGE/tools/evo" fetch "$JOB" > /dev/null 2>&1 || true
    "$STAGE/tools/evo" clean "$JOB" || echo "shape-gallery: could not clean $JOB; run tools/evo clean $JOB" >&2
  fi
  rm -rf "$STAGE"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

stage "$STAGE"
warn_dirty
mkdir -p "$STAGE/tools"
ln -s "$ROOT/tools/evo" "$STAGE/tools/evo"
cp "$ROOT/tools/memcap" "$STAGE/tools/memcap"
NAME=$(run_name "$STAGE")
RUN=$RUNS/$NAME
echo "shape-gallery: libraries: $LIBS"

# shellcheck disable=SC2086 # LIBS is a word list
JOB=$("$STAGE/tools/evo" new --ws shape-gallery $LIBS)
echo "shape-gallery: job $JOB"
"$STAGE/tools/evo" run "$JOB" -m 8G -t 1h \
  --host-after 'sh ws/suite/tools/shape-gallery/render.sh --host-info docker "$RESULTS"' \
  -- "sh ws/suite/tools/shape-gallery/render.sh \"\$JOB/ws\" \"\$PREFIX\" \"\$RESULTS\" 16 $LIBS"

# Poll the job's state; the time limit above bounds the wait.
state= misses=0
while :; do
  state=$("$STAGE/tools/evo" status "$JOB" 2> /dev/null | awk -v j="$JOB" '$1 == j { print $2, $3 }') || state=
  case "$state" in
  exited* | lost*) break ;;
  "")
    # A dropped connection is not a lost job; four misses in a row are.
    misses=$((misses + 1))
    [ "$misses" -lt 4 ] || die "job $JOB is not listed by tools/evo status"
    ;;
  *) misses=0 ;;
  esac
  sleep 15
done
echo "shape-gallery: job $state"

"$STAGE/tools/evo" fetch "$JOB" > /dev/null
mkdir -p "$RUN"
cp -R "$STAGE/.evo/$JOB/results/." "$RUN/"
cp "$STAGE/.evo/$JOB/log" "$RUN/logs/job.log" 2> /dev/null || true
write_manifest "$STAGE" "$RUN" "EVO-X2, tools/evo job $JOB"
cat "$RUN/manifest.txt"
case "$state" in
"exited 0") ;;
*) echo "shape-gallery: the job failed; see $RUN/logs/job.log" >&2; echo "$RUN"; exit 1 ;;
esac
echo "$RUN"
