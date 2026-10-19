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
# Eight sections, each of which shows its gate failing:
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
#      log, pow, hypot, sqrt, floor, ceil, trunc, round, rint, fabs, copysign,
#      scalbn, frexp and ilogb) for each target (same flags,
#      xarch_fp_cflags), reads libs/math/tests/corpus and must print the hash
#      committed in libs/math/tests/corpus/trig.hash on every target.  The
#      controls are the same two (i686 without SSE, aarch64 with contraction
#      on) and so is the plant (one differing bit on each target in turn).
#      Story 4a (AD-17, AD-22): gmath_sqrt has two variants, the target's
#      instruction and the portable method, and every variant must be
#      bit-identical, so the probe is built twice for every target, as it is
#      (the instruction, where the target has one; the probe says whether it did)
#      and with -DGMATH_SQRT_PORTABLE, and both must print the committed hash;
#      the probe itself also runs both variants over the corpus and exits 1 when
#      they differ.  The plant is run on the portable build as well.
#
#   5. The libm gate on every target (AD-22): the sources of each library are
#      built for each target of the matrix at -O0, -O2 and -Os, and
#      check-libm.sh (which refuses every libm symbol) reads the objects with the
#      target's own nm: a compiler that turns a builtin into a call on one target
#      only is seen.  A fixture calling __builtin_sin on each target must be
#      refused, naming sin.
#
#   6. The shape-geometry hash gate (CAP-2, CAP-13, AD-17).  libs/shape-geometry/
#      tools/corpus-hash.c, built with every src/*/*.c of the library and the
#      cutil allocator stand-in in shape-stubs, must print the hash committed in
#      libs/shape-geometry/tests/corpus/geometry.hash on every target.  It
#      evaluates and bounds 3,000 curves of every kind in 2D and 3D, builds 80
#      paths and hashes it all in the library's own hash spelling, so a fused
#      multiply-add, x87 excess precision or a byte-order slip shows.  The
#      controls and the plant are the same as in section 4.  The stand-in
#      header must declare what cutil's does (checked here).
#
#   7. The shape-cpu hash gate (CAP-6, CAP-13, AD-17).  libs/shape-cpu/tools/
#      corpus-hash.c, built with every src/*/*.c of shape-cpu, of shape-geometry
#      (the flattener and the device path it fills) and of math, and the cutil
#      allocator stand-in, must print the hash committed in libs/shape-cpu/tests/
#      corpus/coverage.hash on every target.  It fills random polygons under both
#      fill rules, random curves, rectangles with an open subpath, and shapes
#      moved to the corners of the 26.6 range, and hashes every coverage in the
#      library's own spelling, so a byte-order slip, a 32-bit size assumption or
#      a host-dependent division shows.  The coverage path has no floating point,
#      so there is no x87 or fused-multiply-add control; the control is a probe
#      that folds its results in the host's byte order, which must print the
#      committed hash on a little-endian target and another on a big-endian one.
#      The plant (one differing bit on each target in turn) is as in section 4.
#
#   8. The shape-scene hash gate (CAP-13, CAP-16, AD-17).  libs/shape-scene/tools/
#      corpus-hash.c, built with every src/*/*.c of shape-scene, of shape-geometry
#      and of math, and the cutil allocator stand-in, must print the hash
#      committed in libs/shape-scene/tests/corpus/scene.hash on every target.  It
#      interns 64 random paths (each twice), 24 glyph runs of both sources and
#      builds 40 lists of every kind of item, a third of them frozen into
#      pictures that later lists instance, and hashes every resource hash, list
#      hash, count, expansion and picture depth and the store's counters in
#      shape-geometry's spelling.  The maps are the composition of two
#      general maps with entries over 1000, so their entries are sums of two
#      products of inexact doubles:
#      the two controls of section 4 (i686 without SSE, aarch64 with contraction
#      on) apply, and so does the plant (one differing bit on each target in
#      turn).  The library uses cutil's mutex and atomic counts, so this probe (and
#      the section 3 and 5 builds of the scene's sources) is built against cutil's
#      real headers, generated libver_gen.h, allocator.c and atomic.c, never the
#      stand-in.
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
# Dependencies' headers for the libraries' sources: cutil's allocator is a stand-in
# (shape-stubs), checked against the real header in section 6.
# math's headers (shape-geometry's rotations call gmath_sin and gmath_cos) are the library's
# own, with the generated header section 3 writes into $B/gen/math (math is the first of LIBS).

B=${B:-/tmp/xarch-shape}
SHAPE_DEP_INCLUDES=${SHAPE_DEP_INCLUDES--I$HERE/shape-stubs -I$ROOT/libs/math/include -I$B/gen/math/generated -I$ROOT/libs/shape-geometry/include -I$B/gen/shape-geometry/generated}
# shape-scene uses cutil's mutex and atomic counts (Corey, 2026-10-19: cutil is the suite's cross-platform layer), so its
# sources are built against cutil's real headers, with the generated header that section 3 writes into $B/gen/cutil, and its
# probe links cutil's atomic.c and allocator.c.  The other libraries keep the allocator stand-in (shape-stubs): the two must
# not meet in one translation unit, because the real headers rename the stand-in's names.
SCENE_DEP_INCLUDES="-I$ROOT/libs/cutil/include -I$B/gen/cutil/include -I$ROOT/libs/math/include -I$B/gen/math/generated -I$ROOT/libs/shape-geometry/include -I$B/gen/shape-geometry/generated"
dep_includes() { if [ "$1" = shape-scene ]; then echo "$SCENE_DEP_INCLUDES"; else echo "${SHAPE_DEP_INCLUDES:-}"; fi; }
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "shape.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in /*) ;; *) echo "shape.sh: B must be absolute, got '$B'" >&2; exit 2 ;; esac
rm -rf "$B"; mkdir -p "$B"
make -s -C "$ROOT/libs/cutil" BUILD_DIR="$B/gen/cutil" "$B/gen/cutil/include/ghoti.io/cutil/libver_gen.h" > "$B/gen-cutil.log" 2>&1 \
  || { echo "shape.sh: could not write cutil's libver_gen.h (see $B/gen-cutil.log)" >&2; exit 2; }

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
      -fPIC -fvisibility=hidden -I"$ROOT/libs/$l/include" -I"$g/generated" $(dep_includes "$l") \
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
echo "4. the math corpus hash gate (libs/math: nineteen functions, sqrt in both variants)"
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

declare -A MOK MPLANT MPOK MPPLANT
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
  # shellcheck disable=SC2046
  MPOK[$triple]=$(math_run "$triple" "$cc" "$qemu" port -DGMATH_SQRT_PORTABLE $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the math hash probe with the portable square root (see $B/math-port-$triple.err)"; status=1; MPOK[$triple]=""; }
  # shellcheck disable=SC2046
  MPPLANT[$triple]=$(math_run "$triple" "$cc" "$qemu" portplant -DGMATH_SQRT_PORTABLE -DGMATH_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted portable math hash probe"; status=1; MPPLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT

# math_gate KIND PLANTED_TRIPLE -> 0 if every target prints the committed hash and count.  KIND is
# "default" or "port" (the build with -DGMATH_SQRT_PORTABLE).
math_gate() {
  local kind=$1 planted=$2 rc=0 triple cc qemu desc got hw
  local -n okarr=MOK plantarr=MPLANT
  if [ "$kind" = port ]; then
    local -n okarr=MPOK plantarr=MPPLANT
  fi
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${plantarr[$triple]:-}; else got=${okarr[$triple]:-}; fi
    hw=""
    if [ "$kind" = default ] && [ -f "$B/math-ok-$triple.txt" ]; then
      hw=$(awk '/^math.sqrt_hardware.available/{print ($2 == 1) ? "sqrt: instruction" : "sqrt: portable only"}' "$B/math-ok-$triple.txt")
    fi
    if [ -n "$got" ] && [ "$got" = "$committed_hash $committed_count" ]; then
      printf "   ok    %-22s %s  %s\n" "$triple" "$got" "$hw"
    else
      printf "   FAIL  %-22s %s  (want %s %s)\n" "$triple" "${got:-none}" "$committed_hash" "$committed_count"
      rc=1
    fi
  done <<EOT
$XARCH_TARGETS
EOT
  return $rc
}

echo " a. every target prints the committed hash (the square root as the target has it)"
math_gate default "" || status=1

echo " a2. the same, built with -DGMATH_SQRT_PORTABLE: the portable square root is bit-identical on every target"
math_gate port "" || status=1

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

for kind in default port; do
  echo " c. planted ($kind build): one differing bit on each target in turn must fail the gate, naming it"
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if math_gate "$kind" "$triple" > "$B/math-planted-$kind-$triple.txt" 2>&1; then
      echo "   FAIL  the math gate passed with a bit flipped on $triple: it checks nothing there"
      status=1
    elif grep -q "FAIL  $triple " "$B/math-planted-$kind-$triple.txt" \
      && [ "$(grep -c FAIL "$B/math-planted-$kind-$triple.txt")" -eq 1 ]; then
      echo "   ok    $triple: caught, and only $triple named"
    else
      sed 's/^/   | /' "$B/math-planted-$kind-$triple.txt"
      echo "   FAIL  the math gate failed, but not on $triple alone"
      status=1
    fi
  done <<EOT
$XARCH_TARGETS
EOT
done

echo " d. the variants: the probe runs both square roots and ldexp over the corpus on every target"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if [ -f "$B/math-ok-$triple.txt" ] && grep -q '^math.sqrt_hardware.hash' "$B/math-ok-$triple.txt" \
    && [ "$(awk '$1=="math.sqrt.hash"{print $2}' "$B/math-ok-$triple.txt")" = "$(awk '$1=="math.sqrt_portable.hash"{print $2}' "$B/math-ok-$triple.txt")" ] \
    && [ "$(awk '$1=="math.sqrt.hash"{print $2}' "$B/math-ok-$triple.txt")" = "$(awk '$1=="math.sqrt_hardware.hash"{print $2}' "$B/math-ok-$triple.txt")" ] \
    && [ "$(awk '$1=="math.scalbn.hash"{print $2}' "$B/math-ok-$triple.txt")" = "$(awk '$1=="math.ldexp.hash"{print $2}' "$B/math-ok-$triple.txt")" ]; then
    printf "   ok    %-22s sqrt = sqrt_portable = sqrt_hardware, scalbn = ldexp  (%s)\n" "$triple" \
      "$(awk '/^math.sqrt_hardware.available/{print ($2 == 1) ? "instruction used" : "no instruction: portable twice"}' "$B/math-ok-$triple.txt")"
  else
    echo "   FAIL  $triple: the variants do not agree, or the probe did not report them"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

echo " e. planted: one differing bit in the hardware square root alone must make the probe report the variant"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  if math_run "$triple" "$cc" "$qemu" varplant -DGMATH_PLANT_VARIANT $(xarch_fp_cflags "$triple") > /dev/null 2>&1; then
    echo "   FAIL  $triple: the probe passed with the hardware square root one bit off"
    status=1
  elif grep -q '^math.variants.mismatch  sqrt_hardware' "$B/math-varplant-$triple.txt"; then
    echo "   ok    $triple: the variant mismatch is reported, naming sqrt_hardware"
  else
    echo "   FAIL  $triple: the probe failed, but without naming the variant"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

#---------------------------------------------------------------------------
echo
echo "5. no libm symbol in any library's objects, on every target (AD-22)"
GATEL="$ROOT/libs/math/tools/check-libm.sh"
nm_of() {
  case "$1" in
    x86_64-linux-gnu) echo nm ;;
    *) echo "$1-nm" ;;
  esac
}
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  [ -z "$triple" ] && continue
  nmt=$(nm_of "$triple")
  command -v "$nmt" > /dev/null || { echo "   FAIL  $triple: no $nmt to read the objects with"; status=1; continue; }
  # shellcheck disable=SC2046
  fpf=$(xarch_fp_cflags "$triple")
  for opt in -O0 -O2 -Os; do
    objs=""
    ok=1
    for l in $LIBS; do
      g="$B/gen/$l"
      mkdir -p "$g" "$B/libm/$triple$opt/$l"
      [ -d "$g/generated" ] || make -s -C "$ROOT/libs/$l" libver-gen BUILD_DIR="$g" > "$B/gen-$l.log" 2>&1
      while IFS= read -r src; do
        o="$B/libm/$triple$opt/$l/$(echo "${src#"$ROOT/libs/$l/src/"}" | tr / _).o"
        # shellcheck disable=SC2086
        if ! "$cc" -std=c17 $opt -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno $fpf \
          -fPIC -fvisibility=hidden -DGMATH_BUILD -I"$ROOT/libs/$l/include" -I"$g/generated" $(dep_includes "$l") \
          -c "$src" -o "$o" 2> "$o.err"; then
          echo "   FAIL  $triple $opt: cannot build $src (see $o.err)"
          ok=0; status=1
        fi
        objs="$objs $o"
      done < <(find "$ROOT/libs/$l/src" -name '*.c' | sort)
    done
    [ "$ok" -eq 1 ] || continue
    # shellcheck disable=SC2086
    if out=$(NM="$nmt" "$GATEL" $objs 2>&1); then
      printf "   ok    %-22s %-3s %s\n" "$triple" "$opt" "$out"
    else
      echo "$out" | sed 's/^/   | /'
      echo "   FAIL  $triple $opt: a libm symbol in the objects"
      status=1
    fi
  done
  # The plant: a builtin that has to be a call, built for this target, must be refused.
  printf 'double libm_plant(double x) { return __builtin_sin(x); }\n' > "$B/libm-plant-$triple.c"
  # shellcheck disable=SC2086
  "$cc" -std=c17 -O2 $fpf -c "$B/libm-plant-$triple.c" -o "$B/libm-plant-$triple.o" 2> "$B/libm-plant-$triple.err"
  if out=$(NM="$nmt" "$GATEL" "$B/libm-plant-$triple.o" 2>&1); then
    echo "   FAIL  planted: a __builtin_sin built for $triple passed the libm gate"
    status=1
  elif echo "$out" | grep -q 'libm function sin is referenced'; then
    echo "   ok    planted: a __builtin_sin built for $triple is refused, naming sin"
  else
    echo "$out" | sed 's/^/   | /'
    echo "   FAIL  planted: the libm gate refused the $triple fixture without naming sin"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

#---------------------------------------------------------------------------
echo
echo "6. the shape-geometry hash gate (libs/shape-geometry: curves, bounds, paths, the hash spelling)"
GEOM="$ROOT/libs/shape-geometry"
gg="$B/gen/geom-hash"
mkdir -p "$gg"
if ! make -s -C "$GEOM" libver-gen BUILD_DIR="$gg" > "$B/gen-geom-hash.log" 2>&1; then
  echo "   FAIL  shape-geometry: could not write libver_gen.h (see $B/gen-geom-hash.log)"
  status=1
fi
# The stand-in allocator header must declare what cutil's does.
declared() {
  perl -0777 -pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g; s{\bGCU_API\b}{}g' "$1" \
    | perl -0777 -ne 'print "$1\n" while /(typedef\s+struct\s+GCU_Allocator\s*\{.*?\}\s*GCU_Allocator\s*;)/gs; print "$1\n" while /((?:const\s+GCU_Allocator|void)\s*\*?\s*gcu_allocator_(?:default|malloc|calloc|realloc|free)\s*\([^;]*;)/gs' \
    | tr -d ' \t\n' | sed 's/;/;\n/g' | sort
}
CUTIL_ALLOC="$ROOT/libs/cutil/include/ghoti.io/cutil/allocator.h"
if [ -f "$CUTIL_ALLOC" ] && command -v perl > /dev/null; then
  if [ "$(declared "$CUTIL_ALLOC")" = "$(declared "$HERE/shape-stubs/ghoti.io/cutil/allocator.h")" ] \
    && [ -n "$(declared "$CUTIL_ALLOC")" ]; then
    echo "   ok    the allocator stand-in declares what cutil's header declares"
  else
    echo "   FAIL  shape-stubs/ghoti.io/cutil/allocator.h differs from cutil's declarations"
    status=1
  fi
  # The plant: a stand-in with one member renamed must be refused.
  sed 's/realloc_fn/reallocate_fn/' "$HERE/shape-stubs/ghoti.io/cutil/allocator.h" > "$B/stub-plant.h"
  if [ "$(declared "$CUTIL_ALLOC")" = "$(declared "$B/stub-plant.h")" ]; then
    echo "   FAIL  planted: a drifted stand-in header was not noticed"
    status=1
  else
    echo "   ok    planted: a drifted stand-in header is noticed"
  fi
else
  echo "   FAIL  cannot compare the stand-in with cutil's header (no cutil checkout or no perl)"
  status=1
fi
gh_hash=$(awk '$1=="hash"{print $2}' "$GEOM/tests/corpus/geometry.hash")
gh_count=$(awk '$1=="count"{print $2}' "$GEOM/tests/corpus/geometry.hash")
[ -n "$gh_hash" ] && [ -n "$gh_count" ] || { echo "   FAIL  no committed hash in $GEOM/tests/corpus/geometry.hash"; status=1; }
# Story 6: the rotation constructors call math's sine and cosine, so the probe is built with
# math's sources too (section 4's generated header directory is $mg).
GFLAGS="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGSHGEOM_BUILD -DGMATH_BUILD -I$GEOM/include -I$gg/generated -I$MATH/include -I$mg/generated -I$HERE/shape-stubs"

# geom_run TRIPLE CC QEMU TAG FLAGS... -> prints "HASH COUNT"
geom_run() {
  local triple=$1 cc=$2 qemu=$3 tag=$4 out
  shift 4
  # shellcheck disable=SC2086
  "$cc" $GFLAGS "$@" "$GEOM/tools/corpus-hash.c" $(find "$GEOM/src" -name '*.c' | sort) "$MATH"/src/elementary/*.c \
    "$HERE/shape-stubs/cutil-allocator-stub.c" -o "$B/geom-$tag-$triple" 2> "$B/geom-$tag-$triple.err" || return 1
  xarch_run "$triple" "$qemu" "$B/geom-$tag-$triple" > "$B/geom-$tag-$triple.txt" 2>> "$B/geom-$tag-$triple.err" || return 1
  awk '/^shape-geometry.hash/{h=$2} /^shape-geometry.count/{c=$2} END{if (h != "") print h, c}' "$B/geom-$tag-$triple.txt"
}

declare -A GOK GPLANT
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  GOK[$triple]=$(geom_run "$triple" "$cc" "$qemu" ok $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the shape-geometry probe (see $B/geom-ok-$triple.err)"; status=1; GOK[$triple]=""; }
  # shellcheck disable=SC2046
  GPLANT[$triple]=$(geom_run "$triple" "$cc" "$qemu" plant -DSHAPE_GEOM_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted shape-geometry probe"; status=1; GPLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT

# geom_gate PLANTED_TRIPLE -> 0 if every target prints the committed hash and count.
geom_gate() {
  local planted=$1 rc=0 triple cc qemu desc got
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${GPLANT[$triple]:-}; else got=${GOK[$triple]:-}; fi
    if [ -n "$got" ] && [ "$got" = "$gh_hash $gh_count" ]; then
      printf "   ok    %-22s %s\n" "$triple" "$got"
    else
      printf "   FAIL  %-22s %s  (want %s %s)\n" "$triple" "${got:-none}" "$gh_hash" "$gh_count"
      rc=1
    fi
  done <<EOT2
$XARCH_TARGETS
EOT2
  return $rc
}

echo " a. every target prints the committed hash"
geom_gate "" || status=1

echo " b. the controls: the probe sees what the flags prevent"
gbare=$(geom_run i686-linux-gnu i686-linux-gnu-gcc qemu-i386 bare) || gbare=""
if [ -n "$gbare" ] && [ "$gbare" != "$gh_hash $gh_count" ]; then
  echo "   ok    i686 without -msse2 -mfpmath=sse prints $gbare, not the committed hash"
else
  echo "   FAIL  i686 without the SSE flags printed '${gbare:-nothing}': the hash cannot see x87"
  status=1
fi
gfused=$(geom_run aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 fused -ffp-contract=fast) || gfused=""
if [ -n "$gfused" ] && [ "$gfused" != "$gh_hash $gh_count" ]; then
  echo "   ok    aarch64 with -ffp-contract=fast prints $gfused, not the committed hash"
else
  echo "   FAIL  aarch64 with contraction on printed '${gfused:-nothing}': the hash cannot see a fused multiply-add"
  status=1
fi

echo " c. planted: one differing bit on each target in turn must fail the gate, naming it"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if geom_gate "$triple" > "$B/geom-planted-$triple.txt" 2>&1; then
    echo "   FAIL  the shape-geometry gate passed with a bit flipped on $triple: it checks nothing there"
    status=1
  elif grep -q "FAIL  $triple " "$B/geom-planted-$triple.txt" \
    && [ "$(grep -c FAIL "$B/geom-planted-$triple.txt")" -eq 1 ]; then
    echo "   ok    $triple: caught, and only $triple named"
  else
    sed 's/^/   | /' "$B/geom-planted-$triple.txt"
    echo "   FAIL  the shape-geometry gate failed, but not on $triple alone"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

#---------------------------------------------------------------------------
echo
echo "7. the shape-cpu hash gate (libs/shape-cpu: coverage of device paths under both fill rules, CAP-6)"
CPU="$ROOT/libs/shape-cpu"
gc="$B/gen/cpu-hash"
mkdir -p "$gc"
if ! make -s -C "$CPU" libver-gen BUILD_DIR="$gc" > "$B/gen-cpu-hash.log" 2>&1; then
  echo "   FAIL  shape-cpu: could not write libver_gen.h (see $B/gen-cpu-hash.log)"
  status=1
fi
ch_hash=$(awk '$1=="hash"{print $2}' "$CPU/tests/corpus/coverage.hash")
ch_count=$(awk '$1=="count"{print $2}' "$CPU/tests/corpus/coverage.hash")
[ -n "$ch_hash" ] && [ -n "$ch_count" ] || { echo "   FAIL  no committed hash in $CPU/tests/corpus/coverage.hash"; status=1; }
# The probe is built with shape-geometry's sources (the flattener, the device path, the hash)
# and, because those call math, math's.
CPUFLAGS="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGSHCPU_BUILD -DGSHGEOM_BUILD -DGMATH_BUILD -I$CPU/include -I$gc/generated -I$GEOM/include -I$gg/generated -I$MATH/include -I$mg/generated -I$HERE/shape-stubs"

# cpu_run TRIPLE CC QEMU TAG FLAGS... -> prints "HASH COUNT"
cpu_run() {
  local triple=$1 cc=$2 qemu=$3 tag=$4 out
  shift 4
  # shellcheck disable=SC2086
  "$cc" $CPUFLAGS "$@" "$CPU/tools/corpus-hash.c" $(find "$CPU/src" -name '*.c' | sort) $(find "$GEOM/src" -name '*.c' | sort) "$MATH"/src/elementary/*.c \
    "$HERE/shape-stubs/cutil-allocator-stub.c" -o "$B/cpu-$tag-$triple" 2> "$B/cpu-$tag-$triple.err" || return 1
  xarch_run "$triple" "$qemu" "$B/cpu-$tag-$triple" > "$B/cpu-$tag-$triple.txt" 2>> "$B/cpu-$tag-$triple.err" || return 1
  awk '/^shape-cpu.hash/{h=$2} /^shape-cpu.count/{c=$2} END{if (h != "") print h, c}' "$B/cpu-$tag-$triple.txt"
}

declare -A COK CPLANT
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  COK[$triple]=$(cpu_run "$triple" "$cc" "$qemu" ok $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the shape-cpu probe (see $B/cpu-ok-$triple.err)"; status=1; COK[$triple]=""; }
  # shellcheck disable=SC2046
  CPLANT[$triple]=$(cpu_run "$triple" "$cc" "$qemu" plant -DSHAPE_CPU_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted shape-cpu probe"; status=1; CPLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT

# cpu_gate PLANTED_TRIPLE -> 0 if every target prints the committed hash and count.
cpu_gate() {
  local planted=$1 rc=0 triple cc qemu desc got
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${CPLANT[$triple]:-}; else got=${COK[$triple]:-}; fi
    if [ -n "$got" ] && [ "$got" = "$ch_hash $ch_count" ]; then
      printf "   ok    %-22s %s\n" "$triple" "$got"
    else
      printf "   FAIL  %-22s %s  (want %s %s)\n" "$triple" "${got:-none}" "$ch_hash" "$ch_count"
      rc=1
    fi
  done <<EOT2
$XARCH_TARGETS
EOT2
  return $rc
}

echo " a. every target prints the committed hash"
cpu_gate "" || status=1

echo " b. the control: the probe sees a byte-order slip"
# Nothing in the coverage path is floating point, so unlike the shape-geometry probe there is
# no flag (x87, a fused multiply-add) for a control to show. What this probe can see is byte
# order and word size, so the control folds each result in the host's own byte order: the same
# stream as the real one on a little-endian target, another on a big-endian one.
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  cn=$(cpu_run "$triple" "$cc" "$qemu" native -DSHAPE_CPU_CONTROL_NATIVE_ORDER $(xarch_fp_cflags "$triple")) || cn=""
  case "$triple" in
    s390x*|powerpc*|sparc*) want_same=0 ;;
    *) want_same=1 ;;
  esac
  if [ -z "$cn" ]; then
    echo "   FAIL  $triple: the byte-order control did not build or run"
    status=1
  elif [ $want_same -eq 1 ] && [ "$cn" = "$ch_hash $ch_count" ]; then
    echo "   ok    $triple (little-endian): the native-order control prints the committed hash"
  elif [ $want_same -eq 0 ] && [ "$cn" != "$ch_hash $ch_count" ]; then
    echo "   ok    $triple (big-endian): the native-order control prints $cn, not the committed hash"
  else
    echo "   FAIL  $triple: the native-order control printed '$cn' (committed $ch_hash $ch_count): the probe cannot see byte order there"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

echo " c. planted: one differing bit on each target in turn must fail the gate, naming it"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if cpu_gate "$triple" > "$B/cpu-planted-$triple.txt" 2>&1; then
    echo "   FAIL  the shape-cpu gate passed with a bit flipped on $triple: it checks nothing there"
    status=1
  elif grep -q "FAIL  $triple " "$B/cpu-planted-$triple.txt" \
    && [ "$(grep -c FAIL "$B/cpu-planted-$triple.txt")" -eq 1 ]; then
    echo "   ok    $triple: caught, and only $triple named"
  else
    sed 's/^/   | /' "$B/cpu-planted-$triple.txt"
    echo "   FAIL  the shape-cpu gate failed, but not on $triple alone"
    status=1
  fi
done <<EOT
$XARCH_TARGETS
EOT

#---------------------------------------------------------------------------
echo
echo "8. the shape-scene hash gate (libs/shape-scene: resources and lists, the waist: CAP-13, CAP-16)"
SCENE="$ROOT/libs/shape-scene"
gsc="$B/gen/scene-hash"
mkdir -p "$gsc"
if ! make -s -C "$SCENE" libver-gen BUILD_DIR="$gsc" > "$B/gen-scene-hash.log" 2>&1; then
  echo "   FAIL  shape-scene: could not write libver_gen.h (see $B/gen-scene-hash.log)"
  status=1
fi
sh_hash=$(awk '$1=="hash"{print $2}' "$SCENE/tests/corpus/scene.hash")
sh_count=$(awk '$1=="count"{print $2}' "$SCENE/tests/corpus/scene.hash")
[ -n "$sh_hash" ] && [ -n "$sh_count" ] || { echo "   FAIL  no committed hash in $SCENE/tests/corpus/scene.hash"; status=1; }
# The probe is built with shape-scene's sources, shape-geometry's (the paths, the maps, the hash) and, because
# those call math, math's, and cutil's real allocator.c and atomic.c (the scene's mutex is cutil's header-only GCU_MUTEX_T and its
# reference counts are cutil's GCU_Atomic_Size, on every target; no stand-in is used here).
SCFLAGS="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -pthread -DGSHSCENE_BUILD -DGSHGEOM_BUILD -DGMATH_BUILD -I$SCENE/include -I$gsc/generated -I$GEOM/include -I$gg/generated -I$MATH/include -I$mg/generated -I$ROOT/libs/cutil/include -I$B/gen/cutil/include"

# scene_run TRIPLE CC QEMU TAG FLAGS... -> prints "HASH COUNT"
scene_run() {
  local triple=$1 cc=$2 qemu=$3 tag=$4 out
  shift 4
  # shellcheck disable=SC2086
  "$cc" $SCFLAGS "$@" "$SCENE/tools/corpus-hash.c" $(find "$SCENE/src" -name '*.c' | sort) $(find "$GEOM/src" -name '*.c' | sort) "$MATH"/src/elementary/*.c \
    "$ROOT/libs/cutil/src/allocator.c" "$ROOT/libs/cutil/src/atomic.c" -o "$B/scene-$tag-$triple" 2> "$B/scene-$tag-$triple.err" || return 1
  xarch_run "$triple" "$qemu" "$B/scene-$tag-$triple" > "$B/scene-$tag-$triple.txt" 2>> "$B/scene-$tag-$triple.err" || return 1
  awk '/^shape-scene.hash/{h=$2} /^shape-scene.count/{c=$2} END{if (h != "") print h, c}' "$B/scene-$tag-$triple.txt"
}

declare -A SOK SPLANT
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
  qemu=$(echo "$qemu" | tr -d ' ')
  [ -z "$triple" ] && continue
  # shellcheck disable=SC2046
  SOK[$triple]=$(scene_run "$triple" "$cc" "$qemu" ok $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the shape-scene probe (see $B/scene-ok-$triple.err)"; status=1; SOK[$triple]=""; }
  # shellcheck disable=SC2046
  SPLANT[$triple]=$(scene_run "$triple" "$cc" "$qemu" plant -DSHAPE_SCENE_PLANT_BIT $(xarch_fp_cflags "$triple")) \
    || { echo "   FAIL  $triple: cannot build or run the planted shape-scene probe"; status=1; SPLANT[$triple]=""; }
done <<EOT
$XARCH_TARGETS
EOT

# scene_gate PLANTED_TRIPLE -> 0 if every target prints the committed hash and count.
scene_gate() {
  local planted=$1 rc=0 triple cc qemu desc got
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' ')
    [ -z "$triple" ] && continue
    if [ "$triple" = "$planted" ]; then got=${SPLANT[$triple]:-}; else got=${SOK[$triple]:-}; fi
    if [ -n "$got" ] && [ "$got" = "$sh_hash $sh_count" ]; then
      printf "   ok    %-22s %s\n" "$triple" "$got"
    else
      printf "   FAIL  %-22s %s  (want %s %s)\n" "$triple" "${got:-none}" "$sh_hash" "$sh_count"
      rc=1
    fi
  done <<EOT2
$XARCH_TARGETS
EOT2
  return $rc
}

echo " a. every target prints the committed hash"
scene_gate "" || status=1

echo " b. the controls: the probe sees what the flags prevent (its maps are products and sums of inexact doubles)"
sbare=$(scene_run i686-linux-gnu i686-linux-gnu-gcc qemu-i386 bare) || sbare=""
if [ -n "$sbare" ] && [ "$sbare" != "$sh_hash $sh_count" ]; then
  echo "   ok    i686 without -msse2 -mfpmath=sse prints $sbare, not the committed hash"
else
  echo "   FAIL  i686 without the SSE flags printed '${sbare:-nothing}': the hash cannot see x87"
  status=1
fi
sfused=$(scene_run aarch64-linux-gnu aarch64-linux-gnu-gcc qemu-aarch64 fused -ffp-contract=fast) || sfused=""
if [ -n "$sfused" ] && [ "$sfused" != "$sh_hash $sh_count" ]; then
  echo "   ok    aarch64 with -ffp-contract=fast prints $sfused, not the committed hash"
else
  echo "   FAIL  aarch64 with contraction on printed '${sfused:-nothing}': the hash cannot see a fused multiply-add"
  status=1
fi

echo " c. planted: one differing bit on each target in turn must fail the gate, naming it"
while IFS='|' read -r triple cc qemu desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if scene_gate "$triple" > "$B/scene-planted-$triple.txt" 2>&1; then
    echo "   FAIL  the shape-scene gate passed with a bit flipped on $triple: it checks nothing there"
    status=1
  elif grep -q "FAIL  $triple " "$B/scene-planted-$triple.txt" \
    && [ "$(grep -c FAIL "$B/scene-planted-$triple.txt")" -eq 1 ]; then
    echo "   ok    $triple: caught, and only $triple named"
  else
    sed 's/^/   | /' "$B/scene-planted-$triple.txt"
    echo "   FAIL  the shape-scene gate failed, but not on $triple alone"
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
