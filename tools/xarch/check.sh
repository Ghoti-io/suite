#!/bin/bash
# A gate, not a report.  Exits non-zero if any target disagrees with the rest
# or with Appleby's published verification values.
#
# Two things are asserted, and they are different questions:
#   1. every target reproduces the three published values -- this is
#      correctness against an external reference;
#   2. every target's whole output agrees with x86_64's byte for byte, except
#      the lines listed in EXPECTED_DIFFS below -- this is cross-platform
#      consistency, which the published values alone would not catch.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
. "$HERE/targets.sh"

# gcu_string_hash_64() selects its algorithm on SIZE_MAX -- x64_128 on a
# 64-bit target, x86_128 on a 32-bit one -- so its value differs by word size
# by design, not by byte order.  Any OTHER line differing is a failure.
EXPECTED_DIFFS='hash_64'

bash "$HERE/murmur3.sh" "${1:-$ROOT/libs/cutil/src/string.c}" || exit 1
cd /tmp/xarch-out || exit 1

fail=0
while IFS='|' read -r triple _cc _qemu _desc; do
  triple=$(echo "$triple" | tr -d ' ')
  [ -z "$triple" ] && continue
  if [ ! -s "$triple.txt" ]; then
    printf "   FAIL  %-22s produced no output\n" "$triple"
    fail=1
  fi
done <<EOF
$XARCH_TARGETS
EOF

echo
echo "1. published verification values (B0F57EE3 / B3ECE62A / 6384BA69)"
for f in *.txt; do
  t=$(basename "$f" .txt)
  # awk, not sed: the probe pads its labels, so a single-space strip leaves
  # leading blanks and the comparison fails against a correct run.
  got=$(grep -h '^verify\.' "$f" | awk '{print $2}' | tr '\n' ' ')
  if [ "$got" = "B0F57EE3 B3ECE62A 6384BA69 " ]; then
    printf "   ok    %s\n" "$t"
  else
    printf "   FAIL  %-22s got: %s\n" "$t" "$got"; fail=1
  fi
done

echo
echo "2. agreement with x86_64 (ignoring: $EXPECTED_DIFFS)"
for f in *.txt; do
  t=$(basename "$f" .txt)
  d=$(diff <(grep -v '^size_t=' x86_64-linux-gnu.txt) <(grep -v '^size_t=' "$f") \
      | grep '^>' | sed 's/^> //' | grep -v "^$EXPECTED_DIFFS")
  if [ -z "$d" ]; then
    printf "   ok    %s\n" "$t"
  else
    printf "   FAIL  %-22s unexpected differences:\n" "$t"
    echo "$d" | sed 's/^/           /'; fail=1
  fi
done

echo
[ $fail -eq 0 ] && echo "PASS: all targets agree and match the published values" \
                || echo "FAIL"
exit $fail
