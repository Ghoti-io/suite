#!/bin/bash
# Inside the container (via m1-win.sh): build probe-runtime.c against the DLLs
# in the prefix, run it under wine, then run the planted control, which must fail.
set -u
P=$WPREFIX; cd /w
INC=""; for l in runtime-core runtime-jit runtime-debug cutil; do INC="$INC -I$P/include/ghoti.io/$l-0"; done
LIBS="-L$P/lib/ghoti.io -lghoti.io-runtime-debug-0 -lghoti.io-runtime-jit-0 -lghoti.io-runtime-core-0 -lghoti.io-cutil-0 -pthread"
W="-std=c17 -D_GNU_SOURCE -Wall -Wextra -Werror -pedantic-errors -O1 -g"
gcc $W $INC /tools/xwin/probe-runtime.c -o probe-runtime.exe $LIBS || exit 2
gcc $W $INC -DPLANT_SKIP_PROTECT /tools/xwin/probe-runtime.c -o probe-runtime-control.exe $LIBS || exit 2
file probe-runtime.exe | cut -c1-120
echo "=== probe"; ./probe-runtime.exe 2>&1 | grep -vi fontconfig; p=${PIPESTATUS[0]}
echo "=== control (page left read-write; must FAIL)"; ./probe-runtime-control.exe 2>&1 | grep -vi fontconfig | grep -E 'FAIL|checks'; c=${PIPESTATUS[0]}
echo "probe rc=$p control rc=$c"
