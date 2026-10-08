#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13.  Cross-compiles cutil and security
# for win64, links the probe, and links a mutant whose BCryptGenRandom call
# must fail -- the control, without which a green probe means nothing.
#
# /src is the workspace read-only, /out a writable scratch directory.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CC=x86_64-w64-mingw32-gcc
AR=x86_64-w64-mingw32-ar
W="-pedantic-errors -Wall -Wextra -Wfloat-conversion -fstrict-aliasing"
W="$W -Wstrict-aliasing=1 -std=c17 -O2"
# Both libraries are linked as archives, so GSEC_API and GCU_API must be
# plain extern rather than dllimport.
D="-DGSEC_STATIC -DGHOTIIO_CUTIL_STATIC"

# cutil's generated libver_gen.h exists only in an installed prefix.
CUTIL_GEN=$(echo /src/.local/include/ghoti.io/cutil-[0-9]* | cut -d' ' -f1)
if [ ! -f "$CUTIL_GEN/ghoti.io/cutil/libver_gen.h" ]; then
  echo "no cutil libver_gen.h under $CUTIL_GEN -- run ./bootstrap.sh first" >&2
  exit 2
fi
SEC_GEN=/src/libs/security/build/linux/release/generated
if [ ! -f "$SEC_GEN/ghoti.io/security/libver_gen.h" ]; then
  echo "no security libver_gen.h -- build the library on Linux first" >&2
  exit 2
fi
CI="-I /src/libs/cutil/include -I $CUTIL_GEN"
SI="-I /src/libs/security/include -I $SEC_GEN"

mkdir -p /out/obj /out/log
rm -f /out/obj/*.o /out/*.a /out/*.exe

# The .template.c sources are #included by other translation units, not
# compiled on their own.
# $1 = repo dir, $2 = object prefix, $3 = strict (1 = a warning is a failure,
# because security is the subject here), $4.. = includes.
#
# cutil is scaffolding: it is compiled only so the three symbols security
# references can be linked, and holding it to security's warning set would
# fail on cutil's own code under flags cutil does not use.  Its warnings are
# printed and not counted.
compile_tree() {
  local dir="$1" pfx="$2" strict="$3"; shift 3
  local ok=0 bad=0 warned=0 f o l flags="$W"
  [ "$strict" -eq 1 ] || flags="-std=c17 -O2 -Wall"
  cd "$dir" || return 1
  for f in $(find src -name '*.c' ! -name '*.template.c' | sort); do
    o=/out/obj/${pfx}_$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    l=/out/log/${pfx}_$(echo "$f" | tr '/' '_').log
    if $CC $flags "$@" $D -c "$f" -o "$o" > "$l" 2>&1; then
      ok=$((ok + 1))
      if [ -s "$l" ]; then warned=$((warned + 1)); echo "WARN $f"; cat "$l"; fi
    else
      bad=$((bad + 1)); echo "FAIL $f"; cat "$l"
    fi
  done
  echo "$pfx: compiled=$ok failed=$bad warned=$warned"
  [ "$bad" -eq 0 ] || return 1
  [ "$strict" -eq 0 ] || [ "$warned" -eq 0 ]
}

rc=0
compile_tree /src/libs/cutil cutil 0 $CI || rc=1
compile_tree /src/libs/security sec 1 $SI $CI || rc=1
if [ "$rc" -ne 0 ]; then
  echo "cross-compile is not clean; not linking" >&2
  exit 1
fi

$AR rcs /out/libsec.a /out/obj/sec_*.o
$AR rcs /out/libcutil.a /out/obj/cutil_*.o

# Archives, not a pile of objects: cutil has translation units carrying their
# own main() and others needing -luserenv, and only the members security
# actually references should be pulled in.
link() {                 # $1 = archive, $2 = exe
  $CC $W $SI $CI $D -o "$2" "$HERE/probe-security.c" \
      "$1" /out/libcutil.a -lbcrypt
}
link /out/libsec.a /out/probe.exe || exit 1
echo "probe.exe linked"

# The control: BCryptGenRandom with a NULL algorithm handle and no
# USE_SYSTEM_PREFERRED_RNG flag must be refused, so every assertion that
# depends on the call has to fail while the earlier refusals still pass.
sed 's/BCRYPT_USE_SYSTEM_PREFERRED_RNG/0u/' \
    /src/libs/security/src/random/random.c > /out/random_mut.c
grep -q '0u);' /out/random_mut.c || { echo "control did not apply" >&2; exit 1; }
$CC $W $SI $CI $D -c /out/random_mut.c -o /out/mut_random.o || exit 1
cp /out/libsec.a /out/libsec_mut.a
$AR d /out/libsec_mut.a sec_src_random_random.o
$AR r /out/libsec_mut.a /out/mut_random.o
link /out/libsec_mut.a /out/probe_mut.exe || exit 1
echo "probe_mut.exe linked"

x86_64-w64-mingw32-objdump -p /out/probe.exe | grep -q 'BCryptGenRandom' \
  && echo "probe.exe imports BCryptGenRandom" \
  || { echo "probe.exe does not import BCryptGenRandom" >&2; exit 1; }
