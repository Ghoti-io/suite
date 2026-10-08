#!/bin/bash
# Host side of http.sh: writes the differential's corpus and runs the check in
# the ghoti-xarch image.
#
#   suite/tools/xarch/http-host.sh [scratch-dir]       (default /tmp/http-xarch)
#
# The corpus is the one `make check-oracle` draws from, at a size that runs in a
# few minutes under qemu-user (the gate's own size is 250000 draws).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
D=${1:-/tmp/http-xarch}
COUNT=${COUNT:-20000}
mkdir -p "$D" || exit 2
python3 "$ROOT/libs/http/tools/oracle/corpus.py" --dump "$D/corpus.tsv" --count "$COUNT" || exit 2
# The HTTP/2 corpus (libs/http story 3b): the same kind of file, for h2_probe.
COUNT_H2=${COUNT_H2:-3000}
python3 "$ROOT/libs/http/tools/oracle/diff_h2.py" --probe /bin/true --dump "$D/corpus_h2.tsv" --count "$COUNT_H2" >/dev/null || exit 2
# The WebSocket corpus (libs/http story 3c), dumped the same way as the HTTP/2
# one by libs/http/tools/oracle/diff_ws.py. Its absence is an error.
COUNT_WS=${COUNT_WS:-3000}
WSDUMP="$ROOT/libs/http/tools/oracle/diff_ws.py"
rm -f "$D/corpus_ws.tsv"
[ -f "$WSDUMP" ] || { echo "http-host: $WSDUMP does not exist" >&2; exit 2; }
python3 "$WSDUMP" --probe /bin/true --dump "$D/corpus_ws.tsv" --count "$COUNT_WS" >/dev/null || exit 2
# The dependencies as committed, for the container (see http.sh).
rm -rf "$D/deps"; mkdir -p "$D/deps" || exit 2
for l in cutil security compress; do
  mkdir -p "$D/deps/$l" && git -C "$ROOT/libs/$l" archive HEAD | tar -x -C "$D/deps/$l" || exit 2
done
mkdir -p "$D/out" && rm -f "$D/out/probe.out" "$D/out/write.out" "$D/out/h2probe.out" "$D/out/wsprobe.out"
podman run --rm -v "$ROOT:/work:ro,z" -v "$D:/corpus:ro,z" -v "$D/deps:/deps:ro,z" -v "$D/out:/out:rw,z" \
  localhost/ghoti-xarch:deb13 bash /work/suite/tools/xarch/http.sh
rc=$?
# The chain that matters: the host build the cross builds were compared with
# answers as the probe `make check-oracle` runs against the references does.
# The Makefile's probe is used if it has been built; the check is skipped, and
# says so, otherwise.
P=$ROOT/libs/http/build/linux/release/apps/oracle/http_probe
if [ -x "$P" ] && [ -s "$D/out/probe.out" ]; then
  if "$P" < "$D/corpus.tsv" | cmp -s - "$D/out/probe.out"; then
    echo "the container's host build answers as $P does"
  else
    echo "FAIL: the container's host build does NOT answer as $P does" >&2; rc=1
  fi
else
  echo "(no $P to compare with: run make oracle-probe in libs/http)"
fi
P2=$ROOT/libs/http/build/linux/release/apps/oracle/h2_probe
if [ -x "$P2" ] && [ -s "$D/out/h2probe.out" ]; then
  if "$P2" < "$D/corpus_h2.tsv" | cmp -s - "$D/out/h2probe.out"; then
    echo "the container's host build answers as $P2 does (HTTP/2)"
  else
    echo "FAIL: the container's host build does NOT answer as $P2 does (HTTP/2)" >&2; rc=1
  fi
else
  echo "(no $P2 to compare with: run make oracle-probe in libs/http)"
fi
P3=$ROOT/libs/http/build/linux/release/apps/oracle/ws_probe
if [ -x "$P3" ] && [ -s "$D/out/wsprobe.out" ]; then
  if "$P3" < "$D/corpus_ws.tsv" | cmp -s - "$D/out/wsprobe.out"; then
    echo "the container's host build answers as $P3 does (WebSocket)"
  else
    echo "FAIL: the container's host build does NOT answer as $P3 does (WebSocket)" >&2; rc=1
  fi
else
  echo "FAIL: no $P3 or no WebSocket answers to compare (run make oracle-probe in libs/http)" >&2; rc=1
fi
exit $rc
