#!/bin/bash
# The cross scripts exit non-zero when a target fails. This runs that
# contract without a cross compiler: a callback's status reaches the caller,
# and smoke.sh, murmur3.sh, and check.sh exit 1 when every compile fails.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
fail() { printf 'check-status: %s\n' "$1" >&2; exit 1; }

# xarch_each runs the callback in this shell and returns its failure.
status=0
seen=
. "$HERE/targets.sh"
XARCH_TARGETS='
one |cc1| |first
two |cc2| |second
'
mark() {
  seen="$seen $1"
  status=1
  [ "$1" = two ] && return 1
  return 0
}
xarch_each mark
rc=$?
[ "$rc" -eq 1 ] || fail "a failing callback returned $rc"
[ "$status" -eq 1 ] || fail "the callback's status did not reach this shell"
[ "$seen" = " one two" ] || fail "stopped early or skipped a target: $seen"

XARCH_TARGETS='
one |cc1| |first
'
mark_ok() { seen="$seen ok"; return 0; }
seen=
xarch_each mark_ok
rc=$?
[ "$rc" -eq 0 ] || fail "a passing callback returned $rc"
[ "$seen" = " ok" ] || fail "a passing callback did not run: $seen"

# A compiler that exists and fails, and no cross compiler beside it.
bin=$(mktemp -d)
cat > "$bin/gcc" << 'EOF'
#!/bin/sh
echo 'forced failure' >&2
exit 1
EOF
chmod +x "$bin/gcc"
saved=$(mktemp -d)
if [ -d /tmp/xarch-out ]; then
  cp -a /tmp/xarch-out/. "$saved/" 2>/dev/null || true
  restore=1
else
  restore=0
fi
cleanup() {
  rm -rf "$bin"
  rm -rf /tmp/xarch-out
  if [ "$restore" -eq 1 ]; then
    mkdir -p /tmp/xarch-out
    cp -a "$saved/." /tmp/xarch-out/
  fi
  rm -rf "$saved"
}
trap cleanup EXIT

out=$(PATH="$bin:/usr/bin:/bin" bash "$HERE/smoke.sh" 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "smoke.sh exited $rc"
printf '%s\n' "$out" | grep -q 'BUILD FAILED' || fail "smoke.sh did not report the failed host compile"
printf '%s\n' "$out" | grep -q 'MISSING COMPILER' || fail "smoke.sh did not report a missing cross compiler"

src=$(mktemp)
printf 'int x;\n' > "$src"
out=$(PATH="$bin:/usr/bin:/bin" bash "$HERE/murmur3.sh" "$src" 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "murmur3.sh exited $rc"
printf '%s\n' "$out" | grep -q 'BUILD FAILED' || fail "murmur3.sh did not report the failed compile"
[ ! -e /tmp/xarch-out/x86_64-linux-gnu.txt ] || fail "a failed build left an output file"

# A stale answer must not survive the failed build into a pass.
mkdir -p /tmp/xarch-out
printf 'verify.x B0F57EE3\nverify.y B3ECE62A\nverify.z 6384BA69\n' > /tmp/xarch-out/x86_64-linux-gnu.txt
out=$(PATH="$bin:/usr/bin:/bin" bash "$HERE/check.sh" "$src" 2>&1)
rc=$?
rm -f "$src"
[ "$rc" -eq 1 ] || fail "check.sh exited $rc"
printf '%s\n' "$out" | grep -q 'PASS:' && fail "check.sh passed after a failed build"

echo "check-status: ok"
