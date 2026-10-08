#!/bin/bash
# Host side: run a command inside the mingw container with the scratch tree at /w
# and suite/tools at /tools.   m1-win.sh <scratch-dir> <command...>
W=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(cd "$HERE/.." && pwd)
exec podman run --rm --cap-add SYS_ADMIN -v /usr:/hostusr:ro,z -v /usr/lib/wine:/usr/lib/wine:ro,z -v /usr/lib/x86_64-linux-gnu/wine:/usr/lib/x86_64-linux-gnu/wine:ro,z -v /usr/share/wine:/usr/share/wine:ro,z -v "$W:/w:rw,z" -v /usr/src/googletest:/gt:ro,z \
  -v "$TOOLS:/tools:ro,z" localhost/ghoti-cross-mingw64:deb13 \
  bash -c ". /tools/xwin/m1-env.sh; $*"
