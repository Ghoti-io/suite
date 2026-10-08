#!/bin/bash
#
# Trials of the gates in jit-arm64.sh themselves, on the host and in seconds, with no container: each gate is
# run on input it must pass (its control) and on input it must fail, and the verdicts are required to be
# the expected ones, so that a gate that passes everything cannot pass here.
#
#   - the benchmark's check words: right sums pass, one wrong sum fails, a missing case fails;
#   - the skip accounting: a plain name, a parameterized name with slashes, and a line with a timing, which
#     is not a summary line and is not counted;
#   - the per-binary timeout: a test binary that finishes passes, one that hangs fails the script with a
#     verdict that says it did not finish (and does not take the time a hang would).
#
# Run from the workspace root:  suite/tools/xarch/jit-arm64-selftest.sh
# The test binaries of the timeout trial are built with the host's compiler; the script needs `gcc`,
# `readelf` and `file`, which jit-arm64.sh needs in any case.

set -u
set -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export JIT_ARM64_SOURCE_ONLY=1
# shellcheck disable=SC1091
source "$HERE/jit-arm64.sh"
set +e  # jit-arm64.sh sets -u and pipefail; the trials expect failures and read them

FAILED=0
ok() { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; FAILED=1; }

# `fail` of jit-arm64.sh exits the shell: a trial runs the gate in a subshell and reads its status and message.
verdict() { # <command...> -> sets V_RC and V_OUT
  V_OUT=$("$@" 2>&1)
  V_RC=$?
}

echo "== the benchmark's check words =="
call_word=$(printf '%016x' $((499500 ^ 1000)))
good_out="runtime-jit 0.0, smoke run
loop-plain   best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 0000000000079f2c)
loop-poll    best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 0000000000079f2c)
loop-call    best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check $call_word)
loop-compiled-call best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 00000000000003e8)
loop-tail-call best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 00000000000003e8)
loop-helper-gc best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 00000000000003e8)
loop-native  best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 00000000000003e8)
loop-native-status best 1.000 ns/op  median 1.000 ns/op  (1000 ops x 1, check 00000000000003e8)"
verdict bench_check_sums x86_64 "$good_out"
[ $V_RC -eq 0 ] && ok "right sums pass (the control)" || bad "right sums were refused: $V_OUT"
for victim in loop-plain loop-poll loop-call loop-compiled-call loop-tail-call loop-helper-gc loop-native loop-native-status; do
  # one case's check word off by one
  wrong=$(awk -v v="$victim" '
    $1 == v { sub(/check [0-9a-f]*\)/, "check ffffffffffffffff)") } { print }' <<<"$good_out")
  verdict bench_check_sums x86_64 "$wrong"
  if [ $V_RC -ne 0 ] && [[ "$V_OUT" == *"$victim"* && "$V_OUT" == *"sum is wrong"* ]]; then
    ok "a wrong sum in $victim fails and names it"
  else
    bad "a wrong sum in $victim was not refused (rc $V_RC): $V_OUT"
  fi
done
missing=$(grep -v '^loop-native-status' <<<"$good_out")
verdict bench_check_sums x86_64 "$missing"
[ $V_RC -ne 0 ] && [[ "$V_OUT" == *"loop-native-status did not run"* ]] &&
  ok "a case that did not run fails" || bad "a missing case was not refused: $V_OUT"
# The expected words themselves, against arithmetic done here another way.
[ "$(bench_expected_check loop-call 10)" = "$(printf '%016x' $((45 ^ 10)))" ] &&
  ok "the call loop's word is the sum xor the helper's call count" || bad "loop-call's expected word"

echo "== the skip accounting =="
sample='[==========] 5 tests ran.
[  PASSED  ] 3 tests.
[  SKIPPED ] 2 tests, listed below:
[  SKIPPED ] Suite.Plain
[  SKIPPED ] Prefix/Suite.Param/3
[  SKIPPED ] Suite.Timed (0 ms)'
got=$(printf '%s\n' "$sample" | skipped_names)
want="[  SKIPPED ] Suite.Plain
[  SKIPPED ] Prefix/Suite.Param/3"
[ "$got" = "$want" ] && ok "a plain name and a parameterized name with slashes are seen, the count and timed lines are not" ||
  bad "skipped_names gave: $got"
old_got=$(printf '%s\n' "$sample" | grep '^\[  SKIPPED \] [A-Za-z0-9_]*\.[A-Za-z0-9_]*$' || true)
[ "$old_got" = "[  SKIPPED ] Suite.Plain" ] && ok "the old pattern misses the parameterized name (the trial can see the difference)" ||
  bad "the old pattern gave: $old_got"

echo "== the per-binary timeout =="
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SRC="$T/src"; P="$T/prefix"; S="$T/scratch"
mkdir -p "$SRC/fake/build/linux/release/apps" "$P/lib/ghoti.io" "$S"
cat > "$T/hang.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
int main(void) { sleep(60); puts("[  PASSED  ] 1 test."); return 0; }
EOC
cat > "$T/ok.c" <<'EOC'
#include <stdio.h>
int main(void) {
  puts("[  PASSED  ] 3 tests.");
  puts("[  SKIPPED ] 1 test, listed below:");
  puts("[  SKIPPED ] Prefix/Suite.Param/3");
  return 0;
}
EOC
gcc -O2 "$T/hang.c" -o "$SRC/fake/build/linux/release/apps/testHang" || exit 2
gcc -O2 "$T/ok.c" -o "$SRC/fake/build/linux/release/apps/testOk" || exit 2
ARCH_NOW=x86_64
APP_SUBDIR=release; RUN_PREFIX=""; PHASE_LABEL=""
BINARY_TIMEOUT=3
SKIP_SEEN=(); RAN=0; PASSED_TESTS=0; SKIPPED_TESTS=0; RAN_LIST=()
verdict run_binary x86_64 fake testOk
if [ $V_RC -eq 0 ]; then ok "a binary that finishes passes (the control)"; else bad "the control binary failed: $V_OUT"; fi
# run_binary ran in a subshell above (verdict), so its counters did not move; run it here for the skip.
run_binary x86_64 fake testOk >/dev/null
[ "${#SKIP_SEEN[@]}" -eq 1 ] && [[ "${SKIP_SEEN[0]}" == *"Prefix/Suite.Param/3"* ]] &&
  ok "run_binary counts the parameterized skip by its whole name" || bad "run_binary's skips: ${SKIP_SEEN[*]:-none}"
start=$SECONDS
verdict run_binary x86_64 fake testHang
took=$((SECONDS - start))
if [ $V_RC -ne 0 ] && [[ "$V_OUT" == *"did not finish in 3s"* ]]; then
  ok "a binary that hangs fails the script, saying so"
else
  bad "a hanging binary was not refused (rc $V_RC): $V_OUT"
fi
[ "$took" -lt 30 ] && ok "and it took ${took}s, not the minute the hang would" || bad "the hang took ${took}s to be refused"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "jit-arm64-selftest: OK"
else
  echo "jit-arm64-selftest: FAIL"
  exit 1
fi
