#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Check images.sh against a fake engine. Nothing here talks to the real
# store: a prefix mistake would print stock names, and an rmi of an image
# id or a --force retry would drop a tag the prefix check does not protect.
#
# Usage: suite/tools/check-images.sh

set -eu

SUITE=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=$SUITE/images.sh
WORKDIR=$(mktemp -d)
LOG=$WORKDIR/calls
trap 'rm -rf "$WORKDIR"' EXIT

mkdir -p "$WORKDIR/bin"
cat > "$WORKDIR/bin/podman" << 'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$FAKE_LOG"
case "$1" in
  images)
    if [ "${FAKE_RMI_FAIL:-0}" -eq 1 ]; then
      printf '%s\n' 'localhost/ghoti-rmi-fail:probe'
      exit 0
    fi
    printf '%s\n' \
      'localhost/ghoti-build:gcc16' \
      'localhost/ghoti-docs:doxygen-1.9.8' \
      'docker.io/library/ghoti-build:gcc16' \
      'localhost:5000/team/ghoti-xarch:deb13' \
      'localhost/ghoti-cap6-decoy:probe' \
      'localhost/ghoti-dangling:<none>' \
      '<none>:<none>' \
      'docker.io/library/debian:bookworm' \
      'docker.io/library/gcc:14' \
      'docker.io/library/python:3.12' \
      'localhost/gotool:latest'
    ;;
  rmi)
    for arg in "$@"; do
      if [ "$arg" = --force ] || [ "$arg" = -f ]; then
        echo "force was passed" >&2
        exit 2
      fi
    done
    if [ "${FAKE_RMI_FAIL:-0}" -eq 1 ]; then
      echo "refused" >&2
      exit 1
    fi
    ;;
  *)
    echo "unexpected $*" >&2
    exit 3
    ;;
esac
EOF
chmod +x "$WORKDIR/bin/podman"
ln -s /usr/bin/grep "$WORKDIR/bin/grep"
export FAKE_LOG=$LOG
unset FAKE_RMI_FAIL || true

fail() {
  printf 'check-images: %s\n' "$1" >&2
  exit 1
}

run() {
  : > "$LOG"
  PATH="$WORKDIR/bin" /bin/sh "$SCRIPT" "$@"
}

echo "list"
out=$(run)
printf '%s\n' "$out" | grep -F 'localhost/ghoti-build:gcc16' >/dev/null
printf '%s\n' "$out" | grep -F 'ghoti-docs:doxygen-1.9.8' >/dev/null
printf '%s\n' "$out" | grep -F 'ghoti-xarch:deb13' >/dev/null
printf '%s\n' "$out" | grep -F 'ghoti-cap6-decoy:probe' >/dev/null
printf '%s\n' "$out" | while IFS= read -r line; do
  [ -n "$line" ] || fail "blank list line"
  tag=${line##*:}
  repo=${line%:*}
  name=${repo##*/}
  case "$name" in
    ghoti-*) ;;
    *) fail "listed $line" ;;
  esac
  [ "$tag" != '<none>' ] || fail "listed untagged $line"
done
if printf '%s\n' "$out" | grep -E 'debian|python|gotool|gcc:14|<none>' >/dev/null; then
  fail "list leaked a stock name or an untagged image"
fi

echo "narrow misses"
for word in gotool python debian; do
  run --apply "$word" >/dev/null
  if grep -q '^rmi ' "$LOG"; then
    fail "--apply $word called rmi"
  fi
done

echo "decoy"
run --apply ghoti-cap6-decoy >/dev/null
rmi_lines=$(grep '^rmi ' "$LOG" || true)
[ "$rmi_lines" = "rmi localhost/ghoti-cap6-decoy:probe" ] || fail "decoy rmi was: $rmi_lines"
if grep -q -- '--force' "$LOG"; then
  fail "decoy rmi used --force"
fi

echo "newline"
nl=$(printf '\nX')
nl=${nl%X}
set +e
run --apply "$nl" >/dev/null 2>"$WORKDIR/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "newline narrow exited $rc"
grep -F 'newline' "$WORKDIR/err" >/dev/null || fail "newline narrow did not say why"
if grep -q '^rmi ' "$LOG"; then
  fail "newline narrow called rmi"
fi

echo "unknown"
set +e
run --foo >/dev/null 2>"$WORKDIR/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "--foo exited $rc"
grep -F -- '--foo' "$WORKDIR/err" >/dev/null || fail "--foo was not named"

echo "extra arguments"
set +e
run foo bar baz >/dev/null 2>"$WORKDIR/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "three args exited $rc"
grep -F 'unknown argument foo.' "$WORKDIR/err" >/dev/null || fail "three args named the wrong one: $(cat "$WORKDIR/err")"

echo "no engine"
set +e
PATH=/nonexistent /bin/sh "$SCRIPT" >/dev/null 2>"$WORKDIR/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "no engine exited $rc"
grep -F podman "$WORKDIR/err" >/dev/null || fail "no engine did not name podman"
grep -F docker "$WORKDIR/err" >/dev/null || fail "no engine did not name docker"

echo "rmi refuses"
: > "$LOG"
set +e
FAKE_RMI_FAIL=1 PATH="$WORKDIR/bin" /bin/sh "$SCRIPT" --apply ghoti-rmi-fail >/dev/null 2>"$WORKDIR/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "refused rmi exited $rc"
[ "$(grep -c '^rmi ' "$LOG")" -eq 1 ] || fail "refused rmi was retried"
if grep -q -- '--force' "$LOG"; then
  fail "refused rmi used --force"
fi

echo "check-images: ok"
