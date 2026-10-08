#!/bin/bash
# Cross-compile security for win64 and run it under wine.  Host side: needs
# podman (for the toolchain) and wine (to run the result).  Run from the
# workspace root, or anywhere - the path is derived from this script.
#
#   ./suite/tools/xwin/run-security.sh
#
# Exits 0 only when the probe passes AND the control fails.  A probe that
# cannot fail is not evidence, so both halves are checked.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
IMAGE=${IMAGE:-localhost/ghoti-cross-mingw64:deb13}
OUT=${OUT:-$(mktemp -d)}
export WINEPREFIX=${WINEPREFIX:-$OUT/wineprefix}
export WINEDEBUG=${WINEDEBUG:--all}

for t in podman wine; do
  command -v "$t" >/dev/null || { echo "xwin: $t is not installed" >&2; exit 2; }
done
podman image exists "$IMAGE" || {
  echo "xwin: no $IMAGE -- see notes/suite/CONTAINERS.md section 6" >&2; exit 2; }

mkdir -p "$WINEPREFIX"
echo "=== cross-compiling for win64 in $IMAGE ==="
podman run --rm -v "$ROOT:/src:ro,z" -v "$OUT:/out:rw,z" "$IMAGE" \
    bash "/src/${HERE#"$ROOT"/}/build-security.sh" || exit 1

echo
echo "=== the probe, under wine ==="
wine "$OUT/probe.exe"; probe=$?

echo
echo "=== the control: the same probe over a BCryptGenRandom that must fail ==="
wine "$OUT/probe_mut.exe" > "$OUT/control.log" 2>&1; control=$?
grep -E '^(32 bytes returns|two draws differ)' "$OUT/control.log" || true

echo
if [ "$probe" -eq 0 ] && [ "$control" -ne 0 ]; then
  echo "xwin: PASS - the probe passes and the control fails"
  status=0
elif [ "$probe" -ne 0 ]; then
  echo "xwin: FAIL - the probe reported failures (exit $probe)"
  status=1
else
  echo "xwin: FAIL - the control passed, so the probe cannot see a broken" \
       "BCryptGenRandom and a green run means nothing"
  status=1
fi
echo "artifacts in $OUT"
exit $status
