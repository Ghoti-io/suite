#!/bin/bash
#
# Large files on 32-bit targets: builds cutil's file, directory and mmap layers
# for 32-bit x86 and 32-bit big-endian PowerPC and runs lfs-check.c under
# qemu-user (notes/cutil/large-files.md).
#
# Run it in the ghoti-xarch image, from the workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 \
#     bash /work/suite/tools/xarch/lfs.sh
#
# Without -D_FILE_OFFSET_BITS=64 a 32-bit glibc has a 32-bit off_t: readdir()
# fails with EOVERFLOW on an ext4 directory, fseeko() truncates an offset of
# 2 GiB or more, and a file that large cannot be described.  cutil's Makefile
# defines the flag (LFS_FLAGS) and file.c, dir.c and mmap.c refuse to compile
# without it, so there are two things to see fail, and this script sees both:
#
#   1. The library built WITHOUT the flag must fail to compile, naming the
#      flag.
#   2. The library built without the flag AND with that refusal switched off
#      must fail the harness at the seek check, and at the directory listing
#      too where this filesystem can show it.  The listing is the check that
#      depends on the host: only a directory whose entries carry 64-bit
#      cookies fails (ext4 does; tmpfs does not).  A probe built without the
#      flag asks the workspace mount whether it can, and the listing is held
#      to the plant only if it can.
#
# Each correct build must pass every check, the x86-64 host as the control.
# The scratch files are sparse and cost no disk.  What this cannot show: qemu-
# user is not real 32-bit hardware, and the sanitizers do not run under it.
#
# Exit status 0 only if every correct build passed and every plant failed.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"

WORK=/work
SRC=$WORK/libs/cutil
B=${B:-/tmp/xarch-lfs}
case "$B" in
  ""|/|/tmp|/tmp/|/work|/work/*|"$HOME"|"$HOME"/)
    echo "lfs.sh: refusing B='$B' (it is deleted and recreated)" >&2; exit 2 ;;
esac
case "$B" in
  /*) ;;
  *) echo "lfs.sh: B must be an absolute path, got '$B'" >&2; exit 2 ;;
esac
rm -rf "$B"
mkdir -p "$B/include/ghoti.io/cutil"

cat > "$B/include/ghoti.io/cutil/libver_gen.h" <<'EOT'
#ifndef GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_LIBVER_GEN_H
#define GHOTIIO_CUTIL_NAME ghotiio_cutil_xarch
#define GHOTIIO_CUTIL_VERSION "0.0.0-xarch"
#define GHOTIIO_CUTIL_VERSION_MAJOR 0
#define GHOTIIO_CUTIL_VERSION_MINOR 0
#define GHOTIIO_CUTIL_VERSION_PATCH 0
#endif
EOT

LIBFLAGS="-pedantic-errors -Wall -Wextra -Werror -Wfatal-errors -std=c17 -O2 -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD -I $SRC/include -I $B/include"
LFS=-D_FILE_OFFSET_BITS=64

status=0
fail() { echo "FAIL: $*"; status=1; }

# build NAME CC LFS_FLAG [blind]  ->  $B/NAME/lfs-check.  LFS_FLAG is "yes"
# for the real build.  A failed compile leaves its text in $B/NAME/build.txt.
build() {
  local name=$1 cc=$2 lfs=$3 extra=${4:-}
  local d=$B/$name flag=""
  local -a blind=()
  mkdir -p "$d"
  [ "$lfs" = yes ] && flag=$LFS
  # The plant that switches off the library's refusal to compile without the
  # flag.  An array, because the replacement text has spaces in it.
  [ "$extra" = blind ] && blind=("-D_Static_assert(a,b)=typedef int lfs_plant_unused")
  : > "$d/build.txt"
  for f in file dir mmap path allocator error; do
    $cc $LIBFLAGS $flag "${blind[@]}" -c "$SRC/src/$f.c" -o "$d/$f.o" \
      >> "$d/build.txt" 2>&1 || return 1
  done
  $cc -std=gnu17 -O2 -g -Wall -Wextra -Werror $flag -I "$SRC/include" \
    -I "$B/include" "$HERE/lfs-check.c" "$d"/*.o -o "$d/lfs-check" \
    >> "$d/build.txt" 2>&1 || return 1
}

# run NAME TRIPLE QEMU  ->  exit status of the harness, output in out.txt
run() {
  local name=$1 triple=$2 qemu=$3
  mkdir -p "$B/scratch"
  xarch_run "$triple" "$qemu" "$B/$name/lfs-check" "$SRC/src" "$B/scratch" \
    > "$B/$name/out.txt" 2>&1
}

echo "== can this filesystem show the readdir failure?"
gcc "$HERE/lfs-probe.c" -o "$B/probe-host" || exit 1
i686-linux-gnu-gcc "$HERE/lfs-probe.c" -o "$B/probe-i686" || exit 1
if xarch_run i686-linux-gnu qemu-i386 "$B/probe-i686" "$SRC/src"; then
  fs_shows_readdir=1
  echo "   yes: a 32-bit readdir() without the flag fails with EOVERFLOW here"
else
  fs_shows_readdir=0
  echo "   no: the directory-listing plant is not held to a failure here"
fi

correct() {
  local name=$1 cc=$2 triple=$3 qemu=$4
  echo
  echo "== $name"
  if ! build "$name" "$cc" yes; then
    sed 's/^/   | /' "$B/$name/build.txt"; fail "$name: cannot build"; return
  fi
  run "$name" "$triple" "$qemu"; local rc=$?
  cat "$B/$name/out.txt"
  [ $rc -eq 0 ] || fail "$name: $rc check(s) failed"
}

xarch_each_target() {
  correct i686 i686-linux-gnu-gcc i686-linux-gnu qemu-i386
  correct powerpc powerpc-linux-gnu-gcc powerpc-linux-gnu qemu-ppc
  # The control: a harness that fails on the host fails for a reason that is
  # not the word size.
  correct x86_64 gcc x86_64-linux-gnu ""
}
xarch_each_target

echo
echo "== plant: i686 without the flag must not compile"
if build plant-noflag i686-linux-gnu-gcc no; then
  fail "plant-noflag: compiled without -D_FILE_OFFSET_BITS=64"
elif grep -q 'build with -D_FILE_OFFSET_BITS=64' "$B/plant-noflag/build.txt"; then
  echo "   refused, naming the flag"
else
  sed 's/^/   | /' "$B/plant-noflag/build.txt"
  fail "plant-noflag: failed, but not on the large-file refusal"
fi

echo
echo "== plant: i686 without the flag, refusal switched off"
if ! build plant-blind i686-linux-gnu-gcc no blind; then
  sed 's/^/   | /' "$B/plant-blind/build.txt"; fail "plant-blind: cannot build"
else
  run plant-blind i686-linux-gnu qemu-i386; rc=$?
  if [ $rc -eq 0 ]; then
    fail "plant-blind: the flag is off and the harness still passed"
  else
    sed 's/^/   | /' "$B/plant-blind/out.txt"
    grep -q '^FAIL seek, write and tell at 3 GiB' "$B/plant-blind/out.txt" \
      || fail "plant-blind: failed, but not at the 3 GiB seek"
    if [ $fs_shows_readdir -eq 1 ]; then
      grep -q '^FAIL list a directory' "$B/plant-blind/out.txt" \
        || fail "plant-blind: the listing did not fail where it should"
    fi
  fi
fi

echo
[ $status -eq 0 ] && echo "lfs.sh: all checks passed, both plants failed" \
  || echo "lfs.sh: FAILED"
exit $status
