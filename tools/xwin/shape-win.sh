#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh, which sources
# m1-env.sh first), with suite/tools at /tools and the scratch tree at /w.
# Argument: the native x86_64 hash.  See shape.sh.
set -u
want=${1:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT}
math_hash=${2:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT}
math_count=${3:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT}
geom_hash=${4:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT}
geom_count=${5:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT CPU_HASH CPU_COUNT}
cpu_hash=${6:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT CPU_HASH CPU_COUNT}
cpu_count=${7:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT CPU_HASH CPU_COUNT}
scene_hash=${8:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT CPU_HASH CPU_COUNT SCENE_HASH SCENE_COUNT}
scene_count=${9:?usage: shape-win.sh NATIVE_HASH MATH_HASH MATH_COUNT GEOM_HASH GEOM_COUNT CPU_HASH CPU_COUNT SCENE_HASH SCENE_COUNT}
P=/tools/xarch/shape-hash-probe.c
cd /w || exit 2

x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off "$P" -o shape-probe.exe -lm || { echo "shape-win: the probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc -O2 -std=gnu17 -Wall -Wextra -Werror -ffp-contract=off -DSHAPE_PLANT_BIT "$P" -o shape-plant.exe -lm || { echo "shape-win: the planted probe did not build" >&2; exit 2; }
file shape-probe.exe | cut -c1-100

got=$(./shape-probe.exe 2>/dev/null | tr -d '\r' | awk '/^shape.hash/{print $2}')
planted=$(./shape-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape.hash/{print $2}')
echo "windows (wine) hash: ${got:-none}   planted: ${planted:-none}   native: $want"

rc=0
if [ -n "$got" ] && [ "$got" = "$want" ]; then
  echo "   ok    win64 prints the native hash"
else
  echo "   FAIL  win64 does not print the native hash"; rc=1
fi
if [ -n "$planted" ] && [ "$planted" != "$want" ]; then
  echo "   ok    planted: one differing bit on win64 is not the native hash, so the gate can fail"
else
  echo "   FAIL  planted: the win64 probe with a flipped bit still matches (or did not run)"; rc=1
fi

# The math corpus hash: the committed value, and a planted bit that must differ.
M=/w/math
MF="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGMATH_STATIC -I$M/include -I$M/gen/generated"
x86_64-w64-mingw32-gcc $MF "$M/tools/corpus-hash.c" "$M"/src/elementary/*.c -o math-probe.exe -lm || { echo "shape-win: the math probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc $MF -DGMATH_PLANT_BIT "$M/tools/corpus-hash.c" "$M"/src/elementary/*.c -o math-plant.exe -lm || { echo "shape-win: the planted math probe did not build" >&2; exit 2; }
mgot=$(./math-probe.exe math/tests/corpus 2>/dev/null | tr -d '\r' | awk '/^math.trig.hash/{h=$2} /^math.trig.count/{c=$2} END{if (h != "") print h, c}')
mplant=$(./math-plant.exe math/tests/corpus 2>/dev/null | tr -d '\r' | awk '/^math.trig.hash/{h=$2} /^math.trig.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) math hash: ${mgot:-none}   planted: ${mplant:-none}   committed: $math_hash $math_count"
if [ -n "$mgot" ] && [ "$mgot" = "$math_hash $math_count" ]; then
  echo "   ok    win64 prints the committed math corpus hash"
else
  echo "   FAIL  win64 does not print the committed math corpus hash"; rc=1
fi
if [ -n "$mplant" ] && [ "$mplant" != "$math_hash $math_count" ]; then
  echo "   ok    planted: one differing bit in a math result on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 math probe with a flipped bit still matches (or did not run)"; rc=1
fi
# The portable square root (story 4a, AD-17): the same sources built with -DGMATH_SQRT_PORTABLE must print the
# same hash, and the probe, which runs both square roots over the corpus, must report a planted variant mismatch.
x86_64-w64-mingw32-gcc $MF -DGMATH_SQRT_PORTABLE "$M/tools/corpus-hash.c" "$M"/src/elementary/*.c -o math-port.exe -lm || { echo "shape-win: the portable math probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc $MF -DGMATH_PLANT_VARIANT "$M/tools/corpus-hash.c" "$M"/src/elementary/*.c -o math-varplant.exe -lm || { echo "shape-win: the variant-planted math probe did not build" >&2; exit 2; }
mport=$(./math-port.exe math/tests/corpus 2>/dev/null | tr -d '\r' | awk '/^math.trig.hash/{h=$2} /^math.trig.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) math hash, portable square root: ${mport:-none}"
if [ -n "$mport" ] && [ "$mport" = "$math_hash $math_count" ]; then
  echo "   ok    win64 prints the committed math corpus hash with the portable square root"
else
  echo "   FAIL  win64 does not print the committed math corpus hash with the portable square root"; rc=1
fi
./math-probe.exe math/tests/corpus 2>/dev/null | tr -d '\r' | awk '/^math.sqrt_hardware.available/{print "   hardware square root instruction on win64: " ($2 == 1 ? "used" : "absent")}'
if ./math-varplant.exe math/tests/corpus 2>/dev/null | tr -d '\r' | grep -q '^math.variants.mismatch  sqrt_hardware'; then
  echo "   ok    planted: one bit off in the hardware square root alone is reported as a variant mismatch"
else
  echo "   FAIL  planted: the variant mismatch was not reported on win64"; rc=1
fi
# The shape-geometry hash (CAP-2, CAP-13): the committed value, and a planted bit that must differ.
G=/w/geom
GF="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGSHGEOM_BUILD -DGMATH_STATIC -I$G/include -I$G/gen/generated -I$M/include -I$M/gen/generated -I$G/stubs"
x86_64-w64-mingw32-gcc $GF "$G/tools/corpus-hash.c" $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$G/stubs/cutil-allocator-stub.c" -o geom-probe.exe || { echo "shape-win: the shape-geometry probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc $GF -DSHAPE_GEOM_PLANT_BIT "$G/tools/corpus-hash.c" $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$G/stubs/cutil-allocator-stub.c" -o geom-plant.exe || { echo "shape-win: the planted shape-geometry probe did not build" >&2; exit 2; }
ggot=$(./geom-probe.exe 2>/dev/null | tr -d '\r' | awk '/^shape-geometry.hash/{h=$2} /^shape-geometry.count/{c=$2} END{if (h != "") print h, c}')
gplant=$(./geom-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape-geometry.hash/{h=$2} /^shape-geometry.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) shape-geometry hash: ${ggot:-none}   planted: ${gplant:-none}   committed: $geom_hash $geom_count"
if [ -n "$ggot" ] && [ "$ggot" = "$geom_hash $geom_count" ]; then
  echo "   ok    win64 prints the committed shape-geometry hash"
else
  echo "   FAIL  win64 does not print the committed shape-geometry hash"; rc=1
fi
if [ -n "$gplant" ] && [ "$gplant" != "$geom_hash $geom_count" ]; then
  echo "   ok    planted: one differing bit in a shape-geometry result on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 shape-geometry probe with a flipped bit still matches (or did not run)"; rc=1
fi
# The shape-cpu coverage hash (CAP-6, CAP-13): the committed value, and a planted bit that must differ.
C=/w/cpu
CF="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGSHCPU_BUILD -DGSHGEOM_BUILD -DGMATH_STATIC -I$C/include -I$C/gen/generated -I$G/include -I$G/gen/generated -I$M/include -I$M/gen/generated -I$G/stubs"
x86_64-w64-mingw32-gcc $CF "$C/tools/corpus-hash.c" $(find "$C/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$G/stubs/cutil-allocator-stub.c" -o cpu-probe.exe || { echo "shape-win: the shape-cpu probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc $CF -DSHAPE_CPU_PLANT_BIT "$C/tools/corpus-hash.c" $(find "$C/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$G/stubs/cutil-allocator-stub.c" -o cpu-plant.exe || { echo "shape-win: the planted shape-cpu probe did not build" >&2; exit 2; }
cgot=$(./cpu-probe.exe 2>/dev/null | tr -d '\r' | awk '/^shape-cpu.hash/{h=$2} /^shape-cpu.count/{c=$2} END{if (h != "") print h, c}')
cplant=$(./cpu-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape-cpu.hash/{h=$2} /^shape-cpu.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) shape-cpu hash: ${cgot:-none}   planted: ${cplant:-none}   committed: $cpu_hash $cpu_count"
if [ -n "$cgot" ] && [ "$cgot" = "$cpu_hash $cpu_count" ]; then
  echo "   ok    win64 prints the committed shape-cpu hash"
else
  echo "   FAIL  win64 does not print the committed shape-cpu hash"; rc=1
fi
if [ -n "$cplant" ] && [ "$cplant" != "$cpu_hash $cpu_count" ]; then
  echo "   ok    planted: one differing bit in a shape-cpu result on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 shape-cpu probe with a flipped bit still matches (or did not run)"; rc=1
fi
# The shape-scene resource and list hash (CAP-13, CAP-16): the committed value, and a planted bit that must differ.
N=/w/scene
C=/w/cutil
K=/w/color
I=/w/image
NF="-std=c17 -O2 -pedantic-errors -Wall -Wextra -Werror -ffp-contract=off -DGSHSCENE_BUILD -DGSHGEOM_BUILD -DGMATH_STATIC -DGHOTIIO_CUTIL_STATIC -DGCOL_BUILD -DGIMG_BUILD -I$N/include -I$N/gen/generated -I$G/include -I$G/gen/generated -I$M/include -I$M/gen/generated -I$C/include -I$C/gen/include -I$K/include -I$K/gen/generated -I$I/include -I$I/gen/generated"
x86_64-w64-mingw32-gcc $NF "$N/tools/corpus-hash.c" $(find "$N/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$C/src/allocator.c" "$C/src/atomic.c" "$K/src/gamut.c" "$I/src/raster.c" "$I/core/allocator.c" -o scene-probe.exe || { echo "shape-win: the shape-scene probe did not build" >&2; exit 2; }
x86_64-w64-mingw32-gcc $NF -DSHAPE_SCENE_PLANT_BIT "$N/tools/corpus-hash.c" $(find "$N/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$C/src/allocator.c" "$C/src/atomic.c" "$K/src/gamut.c" "$I/src/raster.c" "$I/core/allocator.c" -o scene-plant.exe || { echo "shape-win: the planted shape-scene probe did not build" >&2; exit 2; }
sgot=$(./scene-probe.exe 2>/dev/null | tr -d '\r' | awk '/^shape-scene.hash/{h=$2} /^shape-scene.count/{c=$2} END{if (h != "") print h, c}')
splant=$(./scene-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape-scene.hash/{h=$2} /^shape-scene.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) shape-scene hash: ${sgot:-none}   planted: ${splant:-none}   committed: $scene_hash $scene_count"
if [ -n "$sgot" ] && [ "$sgot" = "$scene_hash $scene_count" ]; then
  echo "   ok    win64 prints the committed shape-scene hash"
else
  echo "   FAIL  win64 does not print the committed shape-scene hash"; rc=1
fi
if [ -n "$splant" ] && [ "$splant" != "$scene_hash $scene_count" ]; then
  echo "   ok    planted: one differing bit in a shape-scene result on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 shape-scene probe with a flipped bit still matches (or did not run)"; rc=1
fi
[ $rc -eq 0 ] && echo "shape-win: PASS" || echo "shape-win: FAIL"
exit $rc
