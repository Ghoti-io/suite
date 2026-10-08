# The target matrix, sourced by the other scripts in this directory.
# Fields: triple | cc | qemu | description
XARCH_TARGETS='
x86_64-linux-gnu    |gcc                     |         |64-bit little-endian (the host)
i686-linux-gnu      |i686-linux-gnu-gcc      |qemu-i386|32-bit little-endian
s390x-linux-gnu     |s390x-linux-gnu-gcc     |qemu-s390x|64-bit BIG-endian
powerpc64-linux-gnu |powerpc64-linux-gnu-gcc |qemu-ppc64|64-bit BIG-endian
powerpc-linux-gnu   |powerpc-linux-gnu-gcc   |qemu-ppc |32-bit BIG-endian
sparc64-linux-gnu   |sparc64-linux-gnu-gcc   |qemu-sparc64|64-bit BIG-endian, strict alignment
aarch64-linux-gnu   |aarch64-linux-gnu-gcc   |qemu-aarch64|64-bit little-endian
'

# Run $2.. as a command for target $1, under qemu when the target is foreign.
xarch_run() {
  local triple="$1" qemu="$2" bin="$3"; shift 3
  if [ -z "$qemu" ]; then "$bin" "$@"; else "$qemu" -L "/usr/$triple" "$bin" "$@"; fi
}

# Callback gets: triple cc qemu description. It runs in this shell, so a
# status it sets is still set when this returns. A callback that returns
# non-zero fails this function after the remaining targets have run.
xarch_each() {
  local rc=0 triple cc qemu desc
  while IFS='|' read -r triple cc qemu desc; do
    triple=$(echo "$triple" | tr -d ' '); cc=$(echo "$cc" | tr -d ' ')
    qemu=$(echo "$qemu" | tr -d ' ')
    [ -z "$triple" ] && continue
    "$1" "$triple" "$cc" "$qemu" "$desc" || rc=1
  done <<EOF
$XARCH_TARGETS
EOF
  return "$rc"
}
