#!/bin/bash
# Host side: run a command inside the mingw container with the scratch tree at /w
# and suite/tools at /tools.   m1-win.sh <scratch-dir> <command...>
W=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(cd "$HERE/.." && pwd)
# Already inside the image (Containerfile.wine: the EVO-X2 runs the gate that way, with the scratch
# tree at /w and suite/tools at /tools): there is no container to start, but binfmt_misc needs a
# user namespace of its own, which rootless podman gives and unshare does here.  TMPDIR is
# unset because wine 10.0 frees the string getenv("TMPDIR") returns and aborts with "free():
# invalid pointer" the first time it makes a prefix (nothing sets TMPDIR in the local run).
if [ -n "${M1_IN_IMAGE:-}" ]; then
  exec env -u TMPDIR unshare --user --map-root-user --mount bash -c ". /tools/xwin/m1-env.sh; $*"
fi
exec podman run --rm --cap-add SYS_ADMIN -v /usr:/hostusr:ro,z -v /usr/lib/wine:/usr/lib/wine:ro,z -v /usr/lib/x86_64-linux-gnu/wine:/usr/lib/x86_64-linux-gnu/wine:ro,z -v /usr/share/wine:/usr/share/wine:ro,z -v "$W:/w:rw,z" -v /usr/src/googletest:/gt:ro,z \
  -v "$TOOLS:/tools:ro,z" localhost/ghoti-cross-mingw64:deb13 \
  bash -c ". /tools/xwin/m1-env.sh; $*"
