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
render_hash=${10:?usage: shape-win.sh ... SCENE_HASH SCENE_COUNT RENDER_HASH RENDER_COUNT}
render_count=${11:?usage: shape-win.sh ... SCENE_HASH SCENE_COUNT RENDER_HASH RENDER_COUNT}
font_hash=${12:?usage: shape-win.sh ... RENDER_HASH RENDER_COUNT FONT_HASH FONT_COUNT}
font_count=${13:?usage: shape-win.sh ... RENDER_HASH RENDER_COUNT FONT_HASH FONT_COUNT}
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
# The shape-cpu hashes (CAP-6, CAP-13, story 13b): the coverage part and the sink's renders of the fixture set, the
# committed values, and a planted bit that must differ.  Built as the scene's probe is, with shape-cpu's sources too.
U=/w/cpu
UF="$NF -DGSHCPU_BUILD -I$U/include -I$U/gen/generated -I/w/scene-testing"
# shellcheck disable=SC2086
x86_64-w64-mingw32-gcc $UF "$U/tools/corpus-hash.c" $(find "$U/src" -name '*.c' | sort) $(find "$N/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$C/src/allocator.c" "$C/src/atomic.c" "$K/src/gamut.c" "$I/src/raster.c" "$I/core/allocator.c" -o cpu-probe.exe || { echo "shape-win: the shape-cpu probe did not build" >&2; exit 2; }
# shellcheck disable=SC2086
x86_64-w64-mingw32-gcc $UF -DSHAPE_CPU_PLANT_BIT "$U/tools/corpus-hash.c" $(find "$U/src" -name '*.c' | sort) $(find "$N/src" -name '*.c' | sort) $(find "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$C/src/allocator.c" "$C/src/atomic.c" "$K/src/gamut.c" "$I/src/raster.c" "$I/core/allocator.c" -o cpu-plant.exe || { echo "shape-win: the planted shape-cpu probe did not build" >&2; exit 2; }
./cpu-probe.exe 2>/dev/null | tr -d '\r' > cpu-probe.txt
cwant="$cpu_hash $cpu_count $render_hash $render_count"
cgot=$(awk '/^shape-cpu.hash/{h=$2} /^shape-cpu.count/{c=$2} /^shape-cpu.render-hash/{rh=$2} /^shape-cpu.render-count/{rc=$2} END{if (h != "" && rh != "") print h, c, rh, rc}' cpu-probe.txt)
cplant=$(./cpu-plant.exe 2>/dev/null | tr -d '\r' | awk '/^shape-cpu.hash/{h=$2} /^shape-cpu.count/{c=$2} /^shape-cpu.render-hash/{rh=$2} /^shape-cpu.render-count/{rc=$2} END{if (h != "" && rh != "") print h, c, rh, rc}')
echo "windows (wine) shape-cpu hashes: ${cgot:-none}   planted: ${cplant:-none}   committed: $cwant"
if [ -n "$cgot" ] && [ "$cgot" = "$cwant" ]; then
  echo "   ok    win64 prints the committed shape-cpu coverage hash and renders"
else
  echo "   FAIL  win64 does not print the committed shape-cpu hashes"; rc=1
  awk '$1=="shape-cpu.render"{print $2, $3, $4}' cpu-probe.txt > cpu-renders.txt
  awk '$1=="render"{print $2, $3, $4}' "$U/tests/render.hash" | diff - cpu-renders.txt | sed -n 's/^> /   |   differs: /p'
fi
if [ -n "$cplant" ] && [ "$cplant" != "$cwant" ]; then
  echo "   ok    planted: one differing bit in a shape-cpu result on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 shape-cpu probe with a flipped bit still matches (or did not run)"; rc=1
fi
# The font provider's probe (story 14b; AD-5, AD-1): every source of font with shape-cpu's, shape-scene's,
# shape-geometry's and math's and the parts of cutil, color and image above.  font's other dependencies are never
# reached: unreached sections are dropped (--gc-sections) and a symbol still named gets a stub that aborts.  font's
# sources are built with -w (their warnings are its make test's business; this gate's is the bits).
F=/w/font
FF="-std=c17 -O2 -w -ffp-contract=off -ffunction-sections -fdata-sections -DGFNT_BUILD -DGSHCPU_BUILD -DGSHSCENE_BUILD -DGSHGEOM_BUILD -DGMATH_STATIC -DGHOTIIO_CUTIL_STATIC -DGCOL_BUILD -DGIMG_BUILD -DGCOMP_STATIC -DGSEC_STATIC -DGUNI_STATIC -I$F/include -I$F/gen/generated -I/w/compress/include -I/w/compress/gen/generated -I/w/security/include -I/w/security/gen/generated -I/w/unicode/include -I/w/unicode/gen/generated -I$U/include -I$U/gen/generated -I$N/include -I$N/gen/generated -I$G/include -I$G/gen/generated -I$M/include -I$M/gen/generated -I$C/include -I$C/gen/include -I$K/include -I$K/gen/generated -I$I/include -I$I/gen/generated"
rm -rf font-obj
mkdir -p font-obj
for src in "$F/tools/scene-hash.c" $(find "$F/src" "$U/src" "$N/src" "$G/src" -name '*.c' | sort) "$M"/src/elementary/*.c "$C/src/allocator.c" "$C/src/atomic.c" "$C/src/file.c" "$K/src/gamut.c" "$I/src/raster.c" "$I/core/allocator.c"; do
  # shellcheck disable=SC2086
  x86_64-w64-mingw32-gcc $FF -c "$src" -o "font-obj/$(echo "$src" | tr / _).o" || { echo "shape-win: the font probe did not build ($src)" >&2; exit 2; }
done
printf '#include <stdio.h>\n#include <stdlib.h>\n' > font-stubs.c
if ! x86_64-w64-mingw32-gcc -Wl,--gc-sections font-obj/*.o -o font-probe.exe 2> font-link.txt; then
  sed -n "s/.*undefined reference to \`\([A-Za-z0-9_]*\)'.*/\1/p" font-link.txt | sort -u > font-undefined.txt
  [ -s font-undefined.txt ] || { cat font-link.txt >&2; echo "shape-win: the font probe did not link" >&2; exit 2; }
  # A stub that is reached says which, then aborts.
  awk '{printf "void %s(void);\nvoid %s(void) { fputs(\"stub reached: %s\\n\", stderr); abort(); }\n", $1, $1, $1}' font-undefined.txt >> font-stubs.c
  x86_64-w64-mingw32-gcc -w -c font-stubs.c -o font-obj/stubs.o && x86_64-w64-mingw32-gcc -Wl,--gc-sections font-obj/*.o -o font-probe.exe \
    || { echo "shape-win: the font probe did not link with its stubs" >&2; exit 2; }
  echo "   |     $(wc -l < font-undefined.txt) symbols the font probe never reaches are stubbed (abort)"
fi
fwant="$font_hash $font_count"
./font-probe.exe font/fonts 2> font-probe.err | tr -d '\r' > font-probe.txt
frc=${PIPESTATUS[0]}
fgot=$(awk '/^font-scene.hash/{h=$2} /^font-scene.count/{c=$2} END{if (h != "") print h, c}' font-probe.txt)
fplant=$(FONT_SCENE_PLANT=1 ./font-probe.exe font/fonts 2>/dev/null | tr -d '\r' | awk '/^font-scene.hash/{h=$2} /^font-scene.count/{c=$2} END{if (h != "") print h, c}')
echo "windows (wine) font provider hash: ${fgot:-none} (exit $frc)   planted: ${fplant:-none}   committed: $fwant"
if [ "$frc" -eq 0 ] && [ -n "$fgot" ] && [ "$fgot" = "$fwant" ]; then
  echo "   ok    win64 prints the committed font provider hash, with AD-5 and AD-1 holding"
else
  echo "   FAIL  win64 does not print the committed font provider hash (or an equality failed)"; rc=1
  grep '^font-scene.FAIL' font-probe.txt | sed 's/^/   |   /'
  tr -d '\r' < font-probe.err | grep 'stub reached' | sort -u | sed 's/^/   |   /'
fi
if [ -n "$fplant" ] && [ "$fplant" != "$fwant" ]; then
  echo "   ok    planted: one differing bit in the font probe on win64 is not the committed hash"
else
  echo "   FAIL  planted: the win64 font probe with a flipped bit still matches (or did not run)"; rc=1
fi
# The success scene (story 16): font's example program with the objects above (all but the probe's main), on the
# pinned font; its whole output must be expected.txt, and a copy with one bit of its fold flipped must not be.
x86_64-w64-mingw32-gcc $FF -c "$F/examples/font-scene.c" -o success-main.o || { echo "shape-win: the success scene did not build" >&2; exit 2; }
probe_obj="font-obj/$(echo "$F/tools/scene-hash.c" | tr / _).o"
sobjs=$(ls font-obj/*.o | grep -v -e "^$probe_obj\$" -e '^font-obj/stubs.o$')
printf '#include <stdio.h>\n#include <stdlib.h>\n' > success-stubs.c
# shellcheck disable=SC2086
if ! x86_64-w64-mingw32-gcc -Wl,--gc-sections $sobjs success-main.o -o font-scene.exe 2> success-link.txt; then
  sed -n "s/.*undefined reference to \`\([A-Za-z0-9_]*\)'.*/\1/p" success-link.txt | sort -u > success-undefined.txt
  [ -s success-undefined.txt ] || { cat success-link.txt >&2; echo "shape-win: the success scene did not link" >&2; exit 2; }
  awk '{printf "void %s(void);\nvoid %s(void) { fputs(\"stub reached: %s\\n\", stderr); abort(); }\n", $1, $1, $1}' success-undefined.txt >> success-stubs.c
  # shellcheck disable=SC2086
  x86_64-w64-mingw32-gcc -w -c success-stubs.c -o success-stubs.o && x86_64-w64-mingw32-gcc -Wl,--gc-sections $sobjs success-main.o success-stubs.o -o font-scene.exe \
    || { echo "shape-win: the success scene did not link with its stubs" >&2; exit 2; }
  echo "   |     $(wc -l < success-undefined.txt) symbols the success scene never reaches are stubbed (abort)"
fi
./font-scene.exe success/DejaVuSans.ttf AB 2> success.err | tr -d '\r' > success.txt
src=${PIPESTATUS[0]}
awk '$1=="font-scene.hash"{d=substr($2,16,1); n=index("0123456789abcdef",d)-1; n=(n%2==0)?n+1:n-1;
  $2=substr($2,1,15) substr("0123456789abcdef",n+1,1)} {print}' success.txt > success-planted.txt
echo "windows (wine) success scene: $(awk '$1=="font-scene.hash"{print $2}' success.txt) (exit $src)   committed: $(awk '$1=="font-scene.hash"{print $2}' success/expected.txt)"
if [ "$src" -eq 0 ] && cmp -s success.txt success/expected.txt; then
  echo "   ok    win64 prints the success scene's expected.txt"
else
  echo "   FAIL  win64 does not print the success scene's expected.txt (or a check of its own failed)"; rc=1
  diff success/expected.txt success.txt | sed 's/^/   |   /'
  tr -d '\r' < success.err | sed 's/^/   |   /'
fi
if cmp -s success-planted.txt success/expected.txt; then
  echo "   FAIL  planted: the success scene with a flipped bit still matches"; rc=1
else
  echo "   ok    planted: one differing bit in the success scene's output is not expected.txt"
fi
[ $rc -eq 0 ] && echo "shape-win: PASS" || echo "shape-win: FAIL"
exit $rc
