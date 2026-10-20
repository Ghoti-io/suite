#!/bin/sh
# Fetch the real font the shape family's success scene is drawn with (story 16 of
# the shape milestone): DejaVu Sans 2.37, a hinted TrueType font, pinned by the
# digest of its release tarball and of the file inside it.  Story 15 computed the
# tarball's digest from the GitHub release and from SourceForge, two unrelated
# distribution paths, which served the same bytes (suite/tools/shape-bench).
#
#   suite/tools/shape-success/fetch.sh [DIR]     (default: data/ beside this script)
#
# Writes DIR/DejaVuSans.ttf, or fails naming what did not match.  A copy already
# there with the right digest is kept.  DejaVu's licence (the Bitstream Vera
# licence and its public-domain additions) allows redistribution; the font is not
# committed here only so that the suite carries no third-party binary.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-$HERE/data}
URL1=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
URL2=https://downloads.sourceforge.net/project/dejavu/dejavu/2.37/dejavu-fonts-ttf-2.37.tar.bz2
TAR_SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
FONT_SHA256=7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954

mkdir -p "$DIR"
if [ -f "$DIR/DejaVuSans.ttf" ] && printf '%s  %s\n' "$FONT_SHA256" "$DIR/DejaVuSans.ttf" | sha256sum -c - > /dev/null 2>&1; then
  echo "fetch.sh: $DIR/DejaVuSans.ttf is already the pinned font"
  exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
curl -fsSL --max-time 300 -o "$tmp/dejavu.tar.bz2" "$URL1" || curl -fsSL --max-time 300 -o "$tmp/dejavu.tar.bz2" "$URL2" ||
  { echo "fetch.sh: cannot fetch the font from either source" >&2; exit 1; }
printf '%s  %s\n' "$TAR_SHA256" "$tmp/dejavu.tar.bz2" | sha256sum -c - > /dev/null ||
  { echo "fetch.sh: the tarball's digest is not the pinned one" >&2; exit 1; }
tar -xjf "$tmp/dejavu.tar.bz2" -C "$tmp" dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
printf '%s  %s\n' "$FONT_SHA256" "$tmp/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" | sha256sum -c - > /dev/null ||
  { echo "fetch.sh: the font's digest is not the pinned one" >&2; exit 1; }
cp "$tmp/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" "$DIR/DejaVuSans.ttf"
echo "fetch.sh: wrote $DIR/DejaVuSans.ttf"
