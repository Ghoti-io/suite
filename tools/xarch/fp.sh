#!/bin/bash
#
# A gate that the i686 target computes `double` the way the others do
# (AD-17, AD-26 of the shape family).
#
# Run it in the ghoti-xarch image, from the workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 \
#     bash /work/suite/tools/xarch/fp.sh
#
# fp-probe.c computes a sum whose double result differs between x87 (80-bit
# registers, two roundings) and SSE.  The gate asserts three things:
#
#   1. with xarch_fp_cflags every target prints the x86_64 bits;
#   2. WITHOUT the flags i686 prints other bits and a FLT_EVAL_METHOD of 2:
#      the probe can see the difference it exists to see.  If this ever stops
#      being so (a different default, a changed probe), the gate says so
#      rather than passing on a probe that is blind;
#   3. the gate fails when the flags are withheld from i686 (a planted
#      violation, run in-process: the gate logic is run a second time with
#      the flag function replaced by one that returns nothing, and that run
#      must fail).
#
# It checks the arithmetic, not the instructions: that no x87 instruction is
# in a build is a separate gate, and not this one.
#
# Exit status 0 only if all three hold.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

B=${B:-/tmp/xarch-fp}
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "fp.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in /*) ;; *) echo "fp.sh: B must be absolute, got '$B'" >&2; exit 2 ;; esac
rm -rf "$B"; mkdir -p "$B"

# probe TRIPLE CC QEMU TAG FLAGS...  ->  prints "bits eval_method"
probe() {
  local triple=$1 cc=$2 qemu=$3 tag=$4
  shift 4
  # shellcheck disable=SC2086
  "$cc" -O2 -std=gnu17 -Wall -Wextra -Werror "$@" "$HERE/fp-probe.c" \
    -o "$B/$tag-$triple" || return 1
  xarch_run "$triple" "$qemu" "$B/$tag-$triple" > "$B/$tag-$triple.txt" || return 1
  awk '/^fp.sum_bits/{b=$2} /^fp.eval_method/{e=$2} END{print b, e}' "$B/$tag-$triple.txt"
}

# gate  ->  0 if every target, built with xarch_fp_cflags, matches x86_64.
gate() {
  local rc=0 want got triple cc qemu desc
  want=$(probe x86_64-linux-gnu gcc "" gate $(xarch_fp_cflags x86_64-linux-gnu)) \
    || { echo "   FAIL  cannot build or run the x86_64 probe"; return 1; }
  echo "   x86_64-linux-gnu       $want  (reference)"
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
    qemu=$(echo "$qemu" | tr -d ' ')
    [ -z "$triple" ] && continue
    [ "$triple" = x86_64-linux-gnu ] && continue
    # The expansion is deliberately unquoted: it is a list of flags.
    # shellcheck disable=SC2046
    got=$(probe "$triple" "$cc" "$qemu" gate $(xarch_fp_cflags "$triple")) \
      || { printf "   FAIL  %-22s cannot build or run\n" "$triple"; rc=1; continue; }
    # eval_method is allowed to differ by target; the bits are the claim.
    if [ "${got%% *}" = "${want%% *}" ]; then
      printf "   ok    %-22s %s  flags: [%s]\n" "$triple" "$got" "$(xarch_fp_cflags "$triple")"
    else
      printf "   FAIL  %-22s %s  (want bits %s)  flags: [%s]\n" "$triple" "$got" \
        "${want%% *}" "$(xarch_fp_cflags "$triple")"
      rc=1
    fi
  done <<EOT
$XARCH_TARGETS
EOT
  return $rc
}

status=0
echo "1. every target, with xarch_fp_cflags, reproduces the x86_64 bits"
gate || status=1

echo
echo "2. the control: i686 WITHOUT the flags must show excess precision"
want=$(awk '/^fp.sum_bits/{print $2}' "$B/gate-x86_64-linux-gnu.txt")
bare=$(probe i686-linux-gnu i686-linux-gnu-gcc qemu-i386 bare) \
  || { echo "   FAIL  cannot build or run the unflagged i686 probe"; status=1; bare=""; }
if [ -n "$bare" ]; then
  echo "   i686 without flags:   $bare   (x86_64: $want)"
  if [ "${bare%% *}" != "$want" ] && [ "${bare##* }" = 2 ]; then
    echo "   ok    the probe sees x87 (FLT_EVAL_METHOD 2, different bits)"
  else
    echo "   FAIL  the unflagged i686 build matches x86_64 or does not report x87:"
    echo "         the probe cannot tell the two apart, so section 1 proves nothing"
    status=1
  fi
fi

echo
echo "3. planted violation: the gate must FAIL if i686 is built without the flags"
# Redefine the flag function in this shell only; the gate then runs unchanged.
xarch_fp_cflags_real=$(declare -f xarch_fp_cflags)
xarch_fp_cflags() { echo ""; }
if gate > "$B/planted.txt" 2>&1; then
  sed 's/^/   | /' "$B/planted.txt"
  echo "   FAIL  the gate passed with the flags withheld: it checks nothing"
  status=1
else
  grep -E 'FAIL' "$B/planted.txt" | sed 's/^/   | /'
  echo "   ok    the gate fails without the flags"
fi
eval "$xarch_fp_cflags_real"

echo
if [ $status -eq 0 ]; then
  echo "fp.sh: PASS - with the flags every target agrees; without them i686 does not"
else
  echo "fp.sh: FAIL"
fi
exit $status
