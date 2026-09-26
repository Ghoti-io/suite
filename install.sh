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
# Usage:
#   ./install.sh
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

if [ ! -f "$MANIFEST" ]; then
  echo "install.sh: no libraries.txt beside this script" >&2
  exit 1
fi

make_args=""
for arg in "$@"; do
  case "$arg" in
    --global) GLOBAL=1 ;;
    uninstall) ACTION=uninstall ;;
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
    eval "sudo make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $ACTION $make_args" >"$log" 2>&1
  else
    # shellcheck disable=SC2086
    eval "make -C \"\$LIBS/\$repo\" -j\"\$jobs\" PREFIX=\"\$PREFIX\" $ACTION $make_args" >"$log" 2>&1
  fi
done

echo "Done."
