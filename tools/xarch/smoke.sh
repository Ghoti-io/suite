#!/bin/bash
# Prove every toolchain in the image compiles and every qemu runs the result,
# and that each target is the word size and byte order it claims to be.  A
# cross-architecture result is worthless if the "foreign" binary was silently
# the host's.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/targets.sh"
cat > /tmp/e.c <<'EOC'
#include <stdio.h>
#include <stdint.h>
int main(void) {
  union { uint32_t i; unsigned char c[4]; } u;
  u.i = 0x01020304;
  printf("%d-bit  %s-endian\n", (int)(sizeof(void *) * 8),
    u.c[0] == 1 ? "BIG   " : "little");
  return 0;
}
EOC
one() {
  local triple="$1" cc="$2" qemu="$3" desc="$4"
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf "  %-22s MISSING COMPILER\n" "$triple"; return
  fi
  if ! "$cc" -O2 /tmp/e.c -o "/tmp/e-$triple" 2>/tmp/err; then
    printf "  %-22s BUILD FAILED: %s\n" "$triple" "$(head -1 /tmp/err)"; return
  fi
  local got
  if ! got=$(xarch_run "$triple" "$qemu" "/tmp/e-$triple" 2>&1); then
    printf "  %-22s RUN FAILED: %s\n" "$triple" "$(echo "$got" | head -1)"; return
  fi
  printf "  %-22s %-22s | file says: %s\n" "$triple" "$got" \
    "$(file -b "/tmp/e-$triple" | cut -d, -f1-2)"
}
xarch_each one
