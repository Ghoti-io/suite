#!/bin/bash
# Runs INSIDE ghoti-cross-mingw64:deb13 (through m1-win.sh) after defiant-run.sh
# has built everything. Each control puts one Windows fix in a library back to
# its old self in a scratch copy and requires the win64 build of that copy to
# fail. A control whose old code still builds means the fix guards nothing.
# Prints "CONTROLS ok" only when every one failed as it must.
set -u
P=$WPREFIX
bad=0
control() { # control <name> <library> <file> <old text> <new text>
  local name=$1 lib=$2 file=$3 old=$4 new=$5 dir=/w/control-$2
  rm -rf "$dir"; cp -a /w/$lib "$dir"; rm -rf "$dir/build"
  python3 - "$dir/$file" "$old" "$new" <<'PY' || { echo "CONTROL $name: the anchor did not match exactly once"; bad=1; return; }
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(3)
open(path, "w").write(text.replace(old, new, 1))
PY
  if ( cd "$dir" && make -j4 all PREFIX=$P > /w/logs/control-$lib.log 2>&1 ); then
    echo "CONTROL $name: the old code still builds, so the fix guards nothing"; bad=1
  else
    echo "CONTROL $name: the old code fails to build, as it must"
  fi
}
control "core unsetenv" defiant-core src/inst/inst.c \
'#ifdef _WIN32
    {' \
'#ifdef NEVER_DEFINED
    {'
control "vendored SQLite warning" vendor-sqlite Makefile \
' -Wno-unused-parameter -include' ' -include'
[ $bad -eq 0 ] && echo "CONTROLS ok" || echo "CONTROLS FAILED"
