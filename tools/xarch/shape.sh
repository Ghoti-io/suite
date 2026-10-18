#!/bin/bash
#
# The shape family's cross-target gates (CAP-12; AD-2, AD-17 of the shape-family
# architecture): the ones that need a cross compiler, and so cannot run inside
# a library's own `make test`.  Run it in the ghoti-xarch image, from the
# workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 \
#     bash /work/suite/tools/xarch/shape.sh
#
# Three sections, each of which shows its gate failing:
#
#   1. The four copies of each gate script (math, shape-geometry, shape-scene,
#      shape-cpu) are byte-identical, because CONVENTIONS reuses one script text
#      per library; a copy that has drifted is named.  Planted: a changed copy.
#
#   2. The cross-target hash gate.  shape-hash-probe.c, built with
#      -ffp-contract=off and xarch_fp_cflags, must print the x86_64 hash on
#      every target in targets.sh (x86-64 as the reference, i686 with SSE, arm64,
#      and the big-endian s390x, powerpc, powerpc64 and sparc64).  Windows is
#      suite/tools/xwin/shape.sh.  Controls: i686 without the flags, and
#      aarch64 with contraction on, must print other hashes, which is what
#      shows the probe can see what the flags prevent.  Planted: one differing
#      bit on each target in turn; the gate must fail and name that target.
#
#   3. The no-x87 gate over the i686 build of each library's sources.  The gate
#      refuses x87 arithmetic and the rest, and allows the loads and stores the
#      i386 ABI needs to return a double in st(0) (see check-no-x87.sh), so a
#      control shows a double-returning function built with SSE, which does hold
#      an fld, passing.  Planted: the fixture built without the SSE flags has
#      x87 arithmetic and must be refused, and a hand-written faddp must be; an
#      arm64 object must be named as not x86 and refused when --require-x86
#      is given.
#
#   4. The math corpus hash gate (CAP-1, AD-17).  libs/math/tools/corpus-hash.c,
#      built with every libs/math/src/elementary/*.c (sin, cos, tan, atan2, exp,
#      log, pow and hypot) for each target (same flags,
#      xarch_fp_cflags), reads libs/math/tests/corpus and must print the hash
#      committed in libs/math/tests/corpus/trig.hash on every target.  The
#      controls are the same two (i686 without SSE, aarch64 with contraction
#      on) and so is the plant (one differing bit on each target in turn).
#
# Arm64 is run under qemu-user, not on an arm64 machine; the real Raspberry Pi
# 4 is `tools/evo --box pi4`.  A target that cannot be built or run fails the
# section, naming it.
#
# Sources of the libraries that follow a scaffold may include their
# dependencies' headers: put the -I flags in SHAPE_DEP_INCLUDES.
#
# Exit status 0 only if every section holds.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/targets.sh"

B=${B:-/tmp/xarch-shape}
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "shape.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in /*) ;; *) echo "shape.sh: B must be absolute, got '$B'" >&2; exit 2 ;; esac
rm -rf "$B"; mkdir -p "$B"

LIBS="math shape-geometry shape-scene shape-cpu"
status=0

#---------------------------------------------------------------------------
echo "1. the four copies of each gate script are identical"
SCRIPTS="check-edges.sh check-libm.sh check-no-x87.sh check-gates.sh check-fp-contract.sh check-stamps.py"
# same_text REFERENCE FILE... -> 0 if every file equals REFERENCE; names a differing one.
same_text() {
  local ref=$1 f rc=0
  shift
  for f in "$@"; do
    if ! cmp -s "$ref" "$f"; then
      echo "   differs: $f"
      rc=1
    fi
  done
  return $rc
}
sync_gate() {
  local rc=0 s l files
  for s in $SCRIPTS; do
    files=""
    for l in $LIBS; do files="$files $1/$l/tools/$s"; done
    for l in $LIBS; do [ -f "$1/$l/tools/$s" ] || { echo "   missing: $1/$l/tools/$s"; rc=1; }
    done
    # shellcheck disable=SC2086
    same_text "$1/math/tools/$s" $files || rc=1
  done
  return $rc
}
# The plant works on a copy of the four tools directories, so the real ones are
# never touched.
mkdir -p "$B/sync"
for l in $LIBS; do mkdir -p "$B/sync/$l"; cp -r "$ROOT/libs/$l/tools" "$B/sync/$l/"; done
if sync_gate "$ROOT/libs" > "$B/sync-real.txt" 2>&1; then
  echo "   ok    every script is the same text in $(echo $LIBS | wc -w) libraries"
else
  sed 's/^/   | /' "$B/sync-real.txt"
  echo "   FAIL  the gate scripts have drifted"
  status=1
fi
echo "# drift" >> "$B/sync/shape-cpu/tools/check-libm.sh"
if sync_gate "$B/sync" > "$B/sync-plant.txt" 2>&1; then
  echo "   FAIL  planted drift in shape-cpu/check-libm.sh was not caught"
  status=1
elif grep -q "sync/shape-cpu/tools/check-libm.sh" "$B/sync-plant.txt"; then
  echo "   ok    planted drift is caught, naming the file"
else
  cat "$B/sync-plant.txt"
  echo "   FAIL  the planted drift failed the gate without naming the file"
  status=1
fi

#---------------------------------------------------------------------------
echo
echo "2. the cross-target hash gate"
FP="-O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off"

# build_run TRIPLE CC QEMU TAG FLAGS... -> prints the hash; binary in $B/TAG-TRIPLE
build_run() {
  local triple=$1 cc=$2 qemu=$3 tag=$4
  shift 4
  "$cc" "$@" "$HERE/shape-hash-probe.c" -o "$B/$tag-$triple" -lm 2> "$B/$tag-$triple.err" || return 1
  xarch_run "$triple" "$qemu" "$B/$tag-$triple" > "$B/$tag-$triple.txt" 2>> "$B/$tag-$triple.err" || return 1
  awk '/^shape.hash/{print $2}' "$B/$tag-$triple.txt"
}

declare -A OK PLANT
reference=""
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  OK[$triple]=$(build_run "$triple" "$cc" "$qemu" ok $FP $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the probe (see $B/ok-$triple.err)"; status=1; OK[$triple]=""; }
  # shellcheck disable=SC2046
  PLANT[$triple]=$(build_run "$triple" "$cc" "$qemu" plant $FP -DSHAPE_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted probe"; status=1; PLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT
reference=${OK[x86_64-linux-gnu]:-}
[ -n "$reference" ] || { echo "   FAIL  no x86_64 reference hash"; status=1; }

# hash_gate PLANTED_TRIPLE -> 0 if every target's hash is the reference's.  The
# planted target contributes the hash of its planted build.
hash_gate() {
  local planted=$1 rc=0 triple cc qemu desc got
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${PLANT[$triple]:-}; else got=${OK[$triple]:-}; fi
    if [ -n "$got" ] && [ "$got" = "$reference" ]; then
      printf "   ok    %-22s %s\n" "$triple" "$got"
    else
      printf "   FAIL  %-22s %s  (want %s)\n" "$triple" "${got:-none}" "$reference"
      rc=1
    fi
  done <<EOT
$XARCH_TARGETS
EOT
  return $rc
}

echo " a. every target agrees with x86_64"
hash_gate "" || status=1

echo " b. the controls: the probe sees what the flags prevent"
# i686 without the SSE flags: excess precision.
bare=$(build_run i686-linux-gnu i686-linux-gnu-gcc qemu-i386 bare $FP) || bare=""
if [ -n "$bare" ] && [ "$bare" != "$reference" ]; then
  echo "   ok    i686 without -msse2 -mfpmath=sse prints $bare, not $reference"
else
  echo "   FAIL  i686 without the SSE flags printed '${bare:-nothing}': the probe cannot see x87"
  status=1
fi
# aarch64 with contraction on: a fused multiply-add.
fused=$(build_run aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 fused -O2 -std=gnu17 -ffp-contract=fast) || fused=""
if [ -n "$fused" ] && [ "$fused" != "$reference" ]; then
  echo "   ok    aarch64 with -ffp-contract=fast prints $fused, not $reference"
else
  echo "   FAIL  aarch64 with contraction on printed '${fused:-nothing}': the probe cannot see a fused multiply-add"
  status=1
fi

echo " c. planted: one differing bit on each target in turn must fail the gate, naming it"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if hash_gate "$triple" > "$B/planted-$triple.txt" 2>&1; then
    echo "   FAIL  the gate passed with a bit flipped on $triple: it checks nothing there"
    status=1
  elif grep -q "FAIL  $triple " "$B/planted-$triple.txt" \
    && [ "$(grep -c FAIL "$B/planted-$triple.txt")" -eq 1 ]; then
    echo "   ok    $triple: caught, and only $triple named"
  else
    sed 's/^/   | /' "$B/planted-$triple.txt"
    echo "   FAIL  the gate failed, but not on $triple alone"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

#---------------------------------------------------------------------------
echo
echo "3. no x87 instruction in the i686 build"
I686="i686-linux-gnu-gcc"
export OBJDUMP=i686-linux-gnu-objdump
SSE=$(xarch_fp_cflags i686-linux-gnu)
GATE="$ROOT/libs/math/tools/check-no-x87.sh"

for l in $LIBS; do
  g="$B/gen/$l"
  mkdir -p "$g" "$B/obj/$l"
  if ! make -s -C "$ROOT/libs/$l" libver-gen BUILD_DIR="$g" > "$B/gen-$l.log" 2>&1; then
    echo "   FAIL  $l: could not write libver_gen.h (see $B/gen-$l.log)"
    status=1
    continue
  fi
  objs=""
  ok=1
  while IFS= read -r src; do
    o="$B/obj/$l/$(echo "${src#"$ROOT/libs/$l/src/"}" | tr / _).o"
    # shellcheck disable=SC2086
    if ! $I686 -std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off $SSE \
      -fPIC -fvisibility=hidden -I"$ROOT/libs/$l/include" -I"$g/generated" ${SHAPE_DEP_INCLUDES:-} \
      -c "$src" -o "$o" 2> "$o.err"; then
      echo "   FAIL  $l: cannot build $src for i686 (see $o.err)"
      ok=0; status=1
    fi
    objs="$objs $o"
  done < <(find "$ROOT/libs/$l/src" -name '*.c' | sort)
  [ "$ok" -eq 1 ] || continue
  # shellcheck disable=SC2086
  if out=$("$ROOT/libs/$l/tools/check-no-x87.sh" --require-x86 $objs 2>&1); then
    echo "   ok    $l: $out"
  else
    echo "$out" | sed 's/^/   | /'
    echo "   FAIL  $l: an x87 instruction, or no i686 object"
    status=1
  fi
done

echo " controls and plants for the i686 gate"
# The probe with the SSE flags: clean.  Without: x87, and the gate must refuse it.
$I686 -std=gnu17 -O2 -ffp-contract=off $SSE -c "$HERE/shape-hash-probe.c" -o "$B/x87-sse.o" 2> "$B/x87-sse.err"
$I686 -std=gnu17 -O2 -ffp-contract=off -mfpmath=387 -c "$HERE/shape-hash-probe.c" -o "$B/x87-387.o" 2> "$B/x87-387.err"
printf 'void x87_asm(void) { __asm__ volatile("fld1\\n\\tfld1\\n\\tfaddp\\n\\tfstp %%%%st(0)" ::: "st", "st(1)"); }\n' > "$B/x87-asm.c"
$I686 -std=c17 -O2 -c "$B/x87-asm.c" -o "$B/x87-asm.o" 2> "$B/x87-asm.err"
# A function returning a double: the i386 ABI returns it in st(0), so even an SSE
# build holds a load of it, which the gate must allow (and shows the case is real).
printf 'double ret_double(double a, double b) { return a * b + a; }\n' > "$B/x87-ret.c"
$I686 -std=c17 -O2 -ffp-contract=off $SSE -c "$B/x87-ret.c" -o "$B/x87-ret.o" 2> "$B/x87-ret.err"
if i686-linux-gnu-objdump -d --no-show-raw-insn "$B/x87-ret.o" | grep -qE '\sfld[ls]?\s'; then
  echo "   ok    control: an SSE build of a double-returning function does hold an ABI fld"
else
  echo "   FAIL  control: the double-returning fixture holds no fld, so the ABI case is untested"
  status=1
fi
if out=$("$GATE" --require-x86 "$B/x87-ret.o" 2>&1); then
  echo "   ok    control: that ABI fld is allowed ($out)"
else
  echo "$out" | sed 's/^/   | /'
  echo "   FAIL  control: a double-returning function built with SSE was refused"
  status=1
fi
if out=$("$GATE" --require-x86 "$B/x87-sse.o" 2>&1); then
  echo "   ok    control: the fixture built with -msse2 -mfpmath=sse passes ($out)"
else
  echo "$out" | sed 's/^/   | /'
  echo "   FAIL  control: the SSE build of the fixture was refused"
  status=1
fi
if out=$("$GATE" --require-x86 "$B/x87-387.o" 2>&1); then
  echo "   FAIL  planted: the fixture built with -mfpmath=387 passed"
  status=1
elif echo "$out" | grep -q 'x87-387.o'; then
  echo "   ok    planted: the 387 build of the fixture is refused, naming the file"
else
  echo "$out" | sed 's/^/   | /'
  echo "   FAIL  planted: the 387 build was refused without naming the file"
  status=1
fi
if out=$("$GATE" --require-x86 "$B/x87-asm.o" 2>&1); then
  echo "   FAIL  planted: a hand-written faddp passed"
  status=1
elif echo "$out" | grep -q 'faddp'; then
  echo "   ok    planted: a hand-written faddp is refused, naming the instruction"
else
  echo "$out" | sed 's/^/   | /'
  echo "   FAIL  planted: faddp refused without naming the instruction"
  status=1
fi
# An arm64 object: not judged without the flag, refused with it.
aarch64-linux-gnu-gcc -std=gnu17 -O2 -c "$HERE/shape-hash-probe.c" -o "$B/arm.o" 2> "$B/arm.err"
if out=$(OBJDUMP=aarch64-linux-gnu-objdump "$GATE" "$B/arm.o" 2>&1) && echo "$out" | grep -q 'SKIPPED'; then
  echo "   ok    an arm64 object is named as not x86 and skipped, loudly"
else
  echo "$out" | sed 's/^/   | /'
  echo "   FAIL  an arm64 object was not reported as skipped"
  status=1
fi
if OBJDUMP=aarch64-linux-gnu-objdump "$GATE" --require-x86 "$B/arm.o" > "$B/arm-req.txt" 2>&1; then
  echo "   FAIL  --require-x86 passed an arm64 object"
  status=1
else
  echo "   ok    --require-x86 refuses a population with no x86 file"
fi

#---------------------------------------------------------------------------
echo
echo "4. the math corpus hash gate (libs/math: gmath_sin, cos, tan, atan2, exp, log, pow, hypot)"
MATH="$ROOT/libs/math"
CORPUS="$MATH/tests/corpus"
mg="$B/gen/math-hash"
mkdir -p "$mg"
if ! make -s -C "$MATH" libver-gen BUILD_DIR="$mg" > "$B/gen-math-hash.log" 2>&1; then
  echo "   FAIL  math: could not write libver_gen.h (see $B/gen-math-hash.log)"
  status=1
fi
committed_hash=$(awk '$1=="hash"{print $2}' "$CORPUS/trig.hash")
committed_count=$(awk '$1=="count"{print $2}' "$CORPUS/trig.hash")
if [ -z "$committed_hash" ] || [ -z "$committed_count" ]; then
  echo "   FAIL  no committed hash in $CORPUS/trig.hash"
  status=1
fi
MFLAGS="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGMATH_BUILD -I$MATH/include -I$mg/generated"

# math_run TRIPLE CC QEMU TAG FLAGS... -> prints "HASH COUNT"; binary in $B/math-TAG-TRIPLE
math_run() {
  local triple=$1 cc=$2 qemu=$3 tag=$4 out
  shift 4
  "$cc" $MFLAGS "$@" "$MATH/tools/corpus-hash.c" "$MATH"/src/elementary/*.c \
    -o "$B/math-$tag-$triple" 2> "$B/math-$tag-$triple.err" || return 1
  xarch_run "$triple" "$qemu" "$B/math-$tag-$triple" "$CORPUS" > "$B/math-$tag-$triple.txt" 2>> "$B/math-$tag-$triple.err" || return 1
  awk '/^math.trig.hash/{h=$2} /^math.trig.count/{c=$2} END{if (h != "") print h, c}' "$B/math-$tag-$triple.txt"
}

declare -A MOK MPLANT
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  MOK[$triple]=$(math_run "$triple" "$cc" "$qemu" ok $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the math hash probe (see $B/math-ok-$triple.err)"; status=1; MOK[$triple]=""; }
  # shellcheck disable=SC2046
  MPLANT[$triple]=$(math_run "$triple" "$cc" "$qemu" plant -DGMATH_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted math hash probe"; status=1; MPLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT

# math_gate PLANTED_TRIPLE -> 0 if every target prints the committed hash and count.
math_gate() {
  local planted=$1 rc=0 triple cc qemu desc got
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${MPLANT[$triple]:-}; else got=${MOK[$triple]:-}; fi
    if [ -n "$got" ] && [ "$got" = "$committed_hash $committed_count" ]; then
      printf "   ok    %-22s %s\n" "$triple" "$got"
    else
      printf "   FAIL  %-22s %s  (want %s %s)\n" "$triple" "${got:-none}" "$committed_hash" "$committed_count"
      rc=1
    fi
  done <<EOT
$XARCH_TARGETS
EOT
  return $rc
}

echo " a. every target prints the committed hash"
math_gate "" || status=1

echo " b. the controls: the probe sees what the flags prevent"
mbare=$(math_run i686-linux-gnu i686-linux-gnu-gcc qemu-i386 bare) || mbare=""
if [ -n "$mbare" ] && [ "$mbare" != "$committed_hash $committed_count" ]; then
  echo "   ok    i686 without -msse2 -mfpmath=sse prints $mbare, not the committed hash"
else
  echo "   FAIL  i686 without the SSE flags printed '${mbare:-nothing}': the hash cannot see x87"
  status=1
fi
mfused=$(math_run aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 fused -ffp-contract=fast) || mfused=""
if [ -n "$mfused" ] && [ "$mfused" != "$committed_hash $committed_count" ]; then
  echo "   ok    aarch64 with -ffp-contract=fast prints $mfused, not the committed hash"
else
  echo "   FAIL  aarch64 with contraction on printed '${mfused:-nothing}': the hash cannot see a fused multiply-add"
  status=1
fi

echo " c. planted: one differing bit on each target in turn must fail the gate, naming it"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if math_gate "$triple" > "$B/math-planted-$triple.txt" 2>&1; then
    echo "   FAIL  the math gate passed with a bit flipped on $triple: it checks nothing there"
    status=1
  elif grep -q "FAIL  $triple " "$B/math-planted-$triple.txt" \
    && [ "$(grep -c FAIL "$B/math-planted-$triple.txt")" -eq 1 ]; then
    echo "   ok    $triple: caught, and only $triple named"
  else
    sed 's/^/   | /' "$B/math-planted-$triple.txt"
    echo "   FAIL  the math gate failed, but not on $triple alone"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

echo
if [ $status -eq 0 ]; then
  echo "shape.sh: PASS"
else
  echo "shape.sh: FAIL"
fi
exit $status
