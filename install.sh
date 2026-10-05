#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Build and install every library in libraries.txt, in dependency order.
#
# The default prefix is the sibling .local/ directory. Nothing there needs
# root, and pkg-config is pointed at it for the rest of the run. --global
# installs to the system paths instead, which is `sudo make install` with
# no PREFIX and does run ldconfig.
#
# uninstall removes the same install, walking the manifest from the leaf
# back to the root. It takes the same --global flag.
#
# Any other argument is passed to make, so a debug build is:
#
#     ./install.sh BUILD=debug
#
# --test runs `make test` in each library once all of them are installed, so
# one command builds, installs and tests the whole set; a library's tests may
# need a library after it (regex's need text), which is why the tests wait for
# the whole install. Any tools/*/fetch.sh a library has runs first, since its
# tests may need the data it fetches. --test=a,b
# tests only the named libraries (all are still built and installed). A
# failure stops the run and names the log, .bootstrap-<library>.log in the
# parent directory.
#
# Usage:
#   ./install.sh
#   ./install.sh --test
#   ./install.sh --test=runtime-core,lang-tang
#   ./install.sh --global
#   ./install.sh uninstall
#   ./install.sh uninstall --global

set -e

SUITE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SUITE/.." && pwd)
PREFIX="$ROOT/.local"
LIBS="$ROOT/libs"
MANIFEST="$SUITE/libraries.txt"
GLOBAL=0
ACTION=install
TEST=""

if [ ! -f "$MANIFEST" ]; then
  echo "install.sh: no libraries.txt beside this script" >&2
  exit 1
fi

make_args=""
for arg in "$@"; do
  case "$arg" in
    --global) GLOBAL=1 ;;
    uninstall) ACTION=uninstall ;;
    --test) TEST=all ;;
    --test=*) TEST=",${arg#--test=}," ;;
    *)
      quoted=$(printf '%s' "$arg" | sed "s/'/'\\\\''/g; s/^/'/; s/$/'/")
      make_args="$make_args $quoted"
      ;;
  esac
done

ORDER=$(awk '!/^[[:space:]]*#/ && NF >= 4 { print $1 }' "$MANIFEST")

seen=" "
for repo in $ORDER; do
  deps=$(awk -v r="$repo" '!/^[[:space:]]*#/ && $1 == r { print $4 }' "$MANIFEST")
  [ "$deps" = "-" ] && { seen="$seen$repo "; continue; }
  for dep in $(echo "$deps" | tr ',' ' '); do
    case "$dep" in \?*) continue ;; esac
    case "$seen" in
      *" $dep "*) ;;
      *)
        echo "install.sh: libraries.txt lists '$repo' before its dependency '$dep'" >&2
        exit 1
        ;;
    esac
  done
  seen="$seen$repo "
done

if [ "$ACTION" = uninstall ]; then
  walk=""
  for repo in $ORDER; do
    walk="$repo $walk"
  done
else
  walk=$ORDER
fi

jobs=$(nproc 2>/dev/null || echo 4)

if [ "$GLOBAL" -eq 1 ]; then
  echo "Prefix: system (sudo make $ACTION)"
else
  echo "Prefix: $PREFIX"
  if [ "$ACTION" = install ]; then
    export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig"
    mkdir -p "$PKG_CONFIG_PATH"
  fi
fi

for repo in $walk; do
  if [ ! -f "$LIBS/$repo/Makefile" ]; then
    echo "  $repo: not present, skipping"
    continue
  fi
  echo "  $repo"
  log="$ROOT/.bootstrap-$repo.log"
  if [ "$GLOBAL" -eq 1 ]; then
    # shellcheck disable=SC2086
    eval "sudo make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $ACTION $make_args" >"$log" 2>&1 \
      || { echo "install.sh: $repo failed; see $log" >&2; exit 1; }
  else
    # shellcheck disable=SC2086
    eval "make -C \"\$LIBS/\$repo\" -j\"\$jobs\" PREFIX=\"\$PREFIX\" $ACTION $make_args" >"$log" 2>&1 \
      || { echo "install.sh: $repo failed; see $log" >&2; exit 1; }
  fi
done

# The tests run after everything is installed, not one by one as each library
# lands: regex lists text as an optional dependency and text depends on regex,
# so regex's own tests (the JSON Schema adapter gate) need text installed, which
# a test run right after regex's install cannot give them.
if [ "$ACTION" = install ] && [ -n "$TEST" ]; then
  for repo in $ORDER; do
    [ -f "$LIBS/$repo/Makefile" ] || continue
    case "$TEST" in
      all) ;;
      *",$repo,"*) ;;
      *) continue ;;
    esac
    log="$ROOT/.bootstrap-$repo.log"
    # A library's tests can measure against somebody else's data (the Unicode
    # Character Database, the JSON Schema test suite), which is pinned and
    # fetched, never committed. A gate that cannot find it fails rather than
    # skips, so a fresh clone fetches it first; the scripts keep what they
    # have already fetched.
    for fetch in "$LIBS/$repo"/tools/*/fetch.sh; do
      [ -f "$fetch" ] || continue
      echo "  $repo: ${fetch#"$LIBS/$repo/"}"
      sh "$fetch" >>"$log" 2>&1 \
        || { echo "install.sh: $repo: $fetch failed; see $log" >&2; exit 1; }
    done
    echo "  $repo: make test"
    if [ "$GLOBAL" -eq 1 ]; then
      prefix_arg=""
    else
      prefix_arg="PREFIX=\"\$PREFIX\""
    fi
    # shellcheck disable=SC2086
    eval "make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $prefix_arg test $make_args" >>"$log" 2>&1 \
      || { echo "install.sh: $repo: make test failed; see $log" >&2; exit 1; }
  done
fi

echo "Done."
