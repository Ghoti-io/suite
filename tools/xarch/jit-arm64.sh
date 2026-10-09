#!/bin/bash
#
# Builds the runtime stack for AArch64 and runs its suites under qemu-aarch64: the baseline
# JIT's (story 17 of the runtime-stack milestone, CAP-14) and, since the calls spec's story 7,
# runtime-core's, runtime-heap's and runtime-debug's, the relocation arm of runtime-heap, the
# calls, tail calls and natives of runtime-jit on arm64, and the planted defects and mutations
# that show those tests fail on a defect.
#
# Run it in the image Containerfile.arm64-jit builds, from the workspace root:
#
#   podman build -t ghoti-xarch-arm64-jit:deb13-gxx14-gtest \
#     -f suite/tools/xarch/Containerfile.arm64-jit suite/tools/xarch
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarch-jit:/scratch:z \
#     ghoti-xarch-arm64-jit:deb13-gxx14-gtest bash /work/suite/tools/xarch/jit-arm64.sh
#
# What it does, in order:
#
#   1. Checks the machinery: an AArch64 binary is AArch64 by its ELF header, it
#      does NOT run on this host without qemu, and under qemu it runs.
#   2. For each of two builds, builds cutil, unicode, chron, regex, text,
#      runtime-core, runtime-heap, runtime-debug, runtime-jit and lang-tang into a
#      scratch directory outside every repository and outside .local/,
#      installing into a prefix of its own there. The builds are AArch64 (cross
#      compiled, run under `qemu-aarch64 -L /usr/aarch64-linux-gnu`) and x86-64
#      (native: the control, which must pass too).
#   3. Runs every test binary of runtime-jit (its calls, tail calls and natives
#      among them, which emit and run arm64 code), of runtime-core, runtime-heap
#      and runtime-debug, and lang-tang's JIT arm (testJit, and the bounded
#      suites, which include the execution-corpus sweep, with
#      GLTANG_TEST_JIT_THRESHOLD=1: every function tiers up at its first poll,
#      which is the interpreter-against-JIT frame differential). A non-zero
#      exit fails the script.
#   4. Builds runtime-heap with RELOCATE=yes in a prefix of its own, runs every
#      test binary with GRHEAP_RELOCATE=1 (what its `make test-relocate` does)
#      and runs `check-relocation-present` and `check-relocation-gates`, whose
#      fixtures and planted libraries are run through the same qemu command
#      (GRHEAP_GATE_RUNNER).
#   5. Plants, one at a time and each in a tree of its own, the defects the
#      library lists in tools/arm64-plants.txt (4, the SHR/SAR swap, and 20 to 29,
#      the calls, tail calls and natives), and requires the test named for each,
#      run alone under qemu, to FAIL by an assertion (a crash or a hang is not
#      one) while the unplanted control passes. Then runs the planted-mutation
#      harness for arm64 (tools/check-planted-calls.py --target=arm64): its
#      self-test (an edit that changes nothing is MISSED, one that does not compile
#      BUILD FAILED, one that hangs TIMEOUT) and every mutation of src/arm64, each
#      of which must be CAUGHT by a test and not by the pin alone.
#
# Rules the script holds itself to:
#
#   - It never trusts that a binary is foreign. Before a binary runs, its ELF
#     machine is read (readelf and file) and must be the one that build is for.
#   - A skipped test is listed by name with its reason, in the output, and the
#     number of skips is asserted. Nothing is skipped by a pattern.
#   - It reports what ran. It does not say "passed" for a run that did not
#     execute AArch64 code, and it says what it cannot show: the instruction
#     cache, which qemu-user does not model, and real arm64 hardware.
#
# What it needs: this image. The scratch directory is /scratch; mount a host
# directory there to keep the build between runs (the script rebuilds only what
# changed in the library sources, which are copied in each time).

set -u
set -o pipefail

WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}"
JOBS="${JOBS:-$(nproc)}"
QEMU_SYSROOT=/usr/aarch64-linux-gnu
STARTED=$(date +%s)
# The longest one test binary may run (seconds), under qemu or native: a hang is a failure with a name, not a
# script that never ends. The slowest binary under qemu takes minutes; this is generous and finite.
BINARY_TIMEOUT="${JIT_ARM64_BINARY_TIMEOUT:-1800}"

# The libraries, in dependency order, and the ones whose tests run.
STACK="cutil unicode chron regex text runtime-core runtime-heap runtime-debug runtime-jit lang-tang"
# The libraries whose test programs are all run (lang-tang's are chosen below).
SUITE_LIBS="runtime-core runtime-heap runtime-debug runtime-jit"

# lang-tang's JIT arm: testJit on its own, then these with
# GLTANG_TEST_JIT_THRESHOLD=1 (the Makefile's TORTURE_BOUNDED, the list its
# `make test` runs in the JIT arm; it includes the execution corpus).
LANG_TANG_THRESHOLD_TESTS="testExecute_simple testExecute_complex testEngine testCompile testLibrary testRandom testErrors testTemplate testGen testObserver testNative_gate testExec_corpus testJit_calls testNative_calls"

fail() { printf '\njit-arm64: FAIL: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
heading() { printf '\n=== %s ===\n' "$*"; }

# ---- The skips, by name, with the reason ------------------------------------
#
# A test that cannot meaningfully run under user-mode emulation is excluded by
# its full name (a gtest filter naming exactly it) or says so itself
# (GTEST_SKIP). Each build lists what it expects to see skipped, and the count
# is asserted.

# Excluded by the script under qemu (full names; never a pattern):
SKIPPED_BY_FILTER_AARCH64=(
  "runtime-jit|testAsm|Asm.TheSystemDisassemblerReadsTheSameInstructions|runs the host's x86-64 objdump through popen(), and qemu-user cannot exec a native x86-64 program from an AArch64 guest"
  "runtime-heap|testVerify|VerifyTest.AbortModeNamesTheTypeAndTheSlotAndAborts|a gtest death test in the threadsafe style, which re-executes the test binary with execv(), and qemu-user cannot exec a foreign binary through the host kernel (Exec format error); the x86-64 control build runs it"
)
# The same exclusion, seen again by the relocation arm (its binary is run a second time).
SKIPPED_BY_FILTER_AARCH64_RELOC=(
  "runtime-heap (relocating)|testVerify|VerifyTest.AbortModeNamesTheTypeAndTheSlotAndAborts|the same death test, in the RELOCATE=yes build"
)
# Skips the tests themselves report (GTEST_SKIP), per build:
SKIPPED_BY_TEST_AARCH64=(
  "runtime-jit|testWin64|Win64Shape.TheProbeLoopDisassemblesAsTheSequenceItIsMeantToBe|reads the Win64 prologue back through an x86-64 objdump run by popen(), which a guest under qemu-user has none of; the x86-64 control build runs it"
  "runtime-jit|testWin64|Win64Registers.TheDisassemblerAgreesThatNoCalleeSavedRegisterAppears|the same objdump, over the bytes of 200 generated functions; the x86-64 control build runs it"
  "runtime-jit|testWin64_calls|Win64Calls.TheDisassemblerAgreesThatNoCalleeSavedRegisterAppearsInAnyGeneratedCallableFunction|the same objdump, over the bytes of every generated callable function; the x86-64 control build runs it"
  "runtime-jit|testAsm_arm64|AsmArm64.TheRecordedDisassemblyAgreesWithObjdumpWhenTheToolIsPresent|needs the cross objdump through popen(), which qemu-user cannot run; the x86-64 control build (GRJIT_AARCH64_OBJDUMP set) re-checks every recorded encoding with it"
)
SKIPPED_BY_TEST_X86_64=(
  "runtime-jit|testArm64_emit|Arm64Memory.ACompileOnArm64SyncsTheCacheOncePerCode|the native backend of this build is x86-64; the AArch64 build runs it"
)

# ---- Helpers ------------------------------------------------------------------

# The ELF machine of a file, from the header (readelf) and from file(1), and the
# machine the build is for. Both readers must agree with the build.
elf_machine_ok() { # <aarch64|x86_64> <path>
  local want="$1" path="$2" re fe
  re=$(/usr/bin/readelf -h "$path" 2>/dev/null | sed -n 's/^ *Machine: *//p')
  fe=$(file -b "$path")
  case "$want" in
    aarch64)
      [[ "$re" == *AArch64* && "$fe" == *"ARM aarch64"* ]] ;;
    x86_64)
      [[ "$re" == *X86-64* && "$fe" == *"x86-64"* ]] ;;
  esac
}

# Runs a command for a build: under qemu for AArch64, directly for x86-64.
run_for() { # <arch> <command...>
  local arch="$1"; shift
  if [ "$arch" = aarch64 ]; then
    qemu-aarch64 -L "$QEMU_SYSROOT" "$@"
  else
    "$@"
  fi
}

# Runs a command for a build with some environment assignments, given before a
# `--`: `env` for x86-64, qemu's `-E` for AArch64 (a native `env` cannot be the
# guest program, and -E sets the guest's environment only, not qemu's own).
run_env() { # <arch> VAR=value... -- <command...>
  local arch="$1"; shift
  local -a ev=()
  while [ "$1" != "--" ]; do
    ev+=("$1")
    shift
  done
  shift
  # Bounded: exit status 124 is a command that did not finish in $BINARY_TIMEOUT seconds (and is killed
  # ten seconds after it was asked to stop).
  if [ "$arch" = aarch64 ]; then
    local -a q=(qemu-aarch64 -L "$QEMU_SYSROOT")
    local e
    for e in "${ev[@]}"; do
      q+=(-E "$e")
    done
    timeout -k 10 "$BINARY_TIMEOUT" "${q[@]}" "$@"
  else
    timeout -k 10 "$BINARY_TIMEOUT" env "${ev[@]}" "$@"
  fi
}

# The test names a gtest run reports as skipped, one per line: "Suite.Test", and for a parameterized test
# "Prefix/Suite.Test/3" (so the name may hold slashes). Only the summary lines, which carry no timing.
skipped_names() {
  grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' || true
}

# The environment of a build.
setup_env() { # <arch>
  local arch="$1"
  P="$S/$arch/prefix"
  SRC="$S/$arch/src"
  mkdir -p "$P" "$SRC" "$S/$arch/shim"
  # A fresh PATH every time: the shim directory replaces the binutils with the
  # cross ones for the AArch64 build (the Makefiles call `ar`, `nm` and
  # `objdump` by their plain names).
  PATH="$ORIG_PATH"
  if [ "$arch" = aarch64 ]; then
    for t in ar ranlib nm objdump readelf strip objcopy; do
      ln -sf "$(command -v aarch64-linux-gnu-$t)" "$S/$arch/shim/$t"
    done
    PATH="$S/$arch/shim:$PATH"
    CROSS_VARS=(CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++)
    # The target's googletest first, so pkg-config does not find the host's.
    export PKG_CONFIG_PATH="$P/share/pkgconfig:/opt/aarch64/lib/pkgconfig"
  else
    CROSS_VARS=()
    export PKG_CONFIG_PATH="$P/share/pkgconfig"
  fi
  export PATH
}

sync_source() { # <lib>
  local lib="$1"
  mkdir -p "$SRC/$lib"
  rsync -a --exclude build --exclude docs --exclude '.git' "$WORK/libs/$lib/" "$SRC/$lib/"
}

mk() { # <lib> <make args...>
  local lib="$1"; shift
  make -C "$SRC/$lib" "${CROSS_VARS[@]}" PREFIX="$P" "$@"
}

# The names of a library's test executables, as its Makefile derives them.
test_names() { # <lib>
  local lib="$1"
  echo 'print-%: ; @echo $($*)' |
    make -s --no-print-directory -C "$SRC/$lib" -f Makefile -f - "${CROSS_VARS[@]}" \
      PREFIX="$P" WITH_DEBUG=no print-TEST_NAMES 2>/dev/null
}

# cutil makes float.h by running a program it has just built, which an AArch64
# build cannot do on this host. Build the program, run it under qemu, and write
# float.h the way the Makefile's recipe does, so make finds it up to date.
cutil_float_header() { # <arch>
  local arch="$1" apps="$SRC/cutil/build/linux/release/apps"
  local inc="$SRC/cutil/build/linux/release/include/ghoti.io/cutil"
  mk cutil build/linux/release/apps/float_identifier >/dev/null || fail "cutil: float_identifier did not build ($arch)"
  elf_machine_ok "$arch" "$apps/float_identifier" ||
    fail "cutil: float_identifier is not the $arch binary it should be"
  local f32 f64
  f32=$(run_for "$arch" "$apps/float_identifier" 32) || fail "float_identifier 32 failed"
  f64=$(run_for "$arch" "$apps/float_identifier" 64) || fail "float_identifier 64 failed"
  [ -n "$f32" ] && [ -n "$f64" ] || fail "float_identifier produced no type name"
  mkdir -p "$inc"
  sed "s/FLOAT32/$f32/; s/FLOAT64/$f64/" "$SRC/cutil/src/float.h.template" > "$inc/float.h"
  say "  cutil float.h: $f32 / $f64 (from the $arch binary)"
}

build_stack() { # <arch>
  local arch="$1" lib
  heading "Building the stack for $arch into $P"
  for lib in $STACK; do
    sync_source "$lib"
  done
  for lib in $STACK; do
    say "-- $lib"
    case "$lib" in
      cutil) cutil_float_header "$arch" ;;
    esac
    local extra=()
    [ "$lib" = lang-tang ] && extra=(WITH_DEBUG=no)
    mk "$lib" -j"$JOBS" all "${extra[@]}" >"$S/$arch/build-$lib.log" 2>&1 ||
      { tail -30 "$S/$arch/build-$lib.log" >&2; fail "$lib did not build for $arch (log: $S/$arch/build-$lib.log)"; }
    mk "$lib" install "${extra[@]}" >>"$S/$arch/build-$lib.log" 2>&1 ||
      { tail -30 "$S/$arch/build-$lib.log" >&2; fail "$lib did not install for $arch"; }
  done
  # Every shared object the prefix holds is for this architecture.
  local so n=0
  for so in "$P"/lib/ghoti.io/*.so.*.*.*; do
    elf_machine_ok "$arch" "$so" || fail "$so is not an $arch object"
    n=$((n + 1))
  done
  [ "$n" -gt 0 ] || fail "the prefix holds no libraries; the check above measured nothing"
  say "  $n installed libraries, each an $arch object by ELF header and file(1)"
}

# ---- Running test binaries -------------------------------------------------------

RAN=0
PASSED_TESTS=0
SKIPPED_TESTS=0
declare -a SKIP_SEEN=()
declare -a RAN_LIST=()

# Runs one test binary; folds its counts in. $4.. are extra environment words.
# APP_SUBDIR, RUN_PREFIX and PHASE_LABEL name what a run is of: the ordinary build (release, the
# arch's prefix), or the relocation arm's (release-reloc, its own prefix, "relocating" in the
# skip list's names).
APP_SUBDIR=release
RUN_PREFIX=""
PHASE_LABEL=""

run_binary() { # <arch> <lib> <name> [VAR=value ...]
  local arch="$1" lib="$2" name="$3"; shift 3
  local dir="$SRC/$lib/build/linux/$APP_SUBDIR/apps"
  local pfx="${RUN_PREFIX:-$P}"
  local shown="$lib${PHASE_LABEL:+ ($PHASE_LABEL)}"
  local bin="$dir/$name"
  [ -x "$bin" ] || fail "$lib/$name was not built"
  elf_machine_ok "$arch" "$bin" || fail "$bin is not an $arch binary: refusing to run it as one"
  local filter=""
  if [ "$arch" = aarch64 ]; then
    local entry f_lib f_bin f_test f_why
    for entry in "${SKIPPED_BY_FILTER_AARCH64[@]}"; do
      IFS='|' read -r f_lib f_bin f_test f_why <<<"$entry"
      if [ "$f_lib" = "$lib" ] && [ "$f_bin" = "$name" ]; then
        # The name must still exist: an exclusion of a test that was renamed or
        # removed would count as a skip that never happened.
        local listed
        listed=$(cd "$SRC/$lib" && run_env "$arch" LD_LIBRARY_PATH="$pfx/lib/ghoti.io:$dir" -- "$bin" --gtest_list_tests 2>&1 |
          awk '/^[^ ]/ {suite=$1} /^  / {print suite $1}')
        grep -qxF "$f_test" <<<"$listed" ||
          fail "the filtered test $f_test is not in $lib/$name's --gtest_list_tests output: the skip list is stale"
        filter="--gtest_filter=-$f_test"
        SKIP_SEEN+=("$shown|$name|$f_test|by the script (filter): $f_why")
        SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
      fi
    done
  fi
  local out rc
  local envs=(LD_LIBRARY_PATH="$pfx/lib/ghoti.io:$dir" "$@")
  [ "$arch" = x86_64 ] && envs+=(GRJIT_AARCH64_OBJDUMP=/usr/bin/aarch64-linux-gnu-objdump)
  out=$(cd "$SRC/$lib" && run_env "$arch" "${envs[@]}" -- "$bin" $filter 2>&1)
  rc=$?
  if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
    printf '%s\n' "$out" | tail -20 >&2
    fail "$shown/$name did not finish in ${BINARY_TIMEOUT}s ($arch): a hang is a failure"
  fi
  if [ $rc -ne 0 ]; then
    printf '%s\n' "$out" | tail -40 >&2
    fail "$shown/$name exited $rc ($arch, ${*:-no extra environment})"
  fi
  local passed
  passed=$(printf '%s\n' "$out" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1)
  passed=${passed:-0}
  # runtime-core/testChild holds Windows-only tests (the child-process helper), so
  # on Linux it is a binary with no tests by design; every other empty one is a failure.
  if [ "$lib/$name" = "runtime-core/testChild" ] && printf '%s\n' "$out" | grep -q 'Running 0 tests'; then
    printf '  %-36s Windows-only, no tests on Linux\n' "$shown/$name"
    return 0
  fi
  [ "$passed" -gt 0 ] || fail "$lib/$name exited 0 having passed no tests: a binary that ran nothing is not a pass"
  PASSED_TESTS=$((PASSED_TESTS + passed))
  RAN=$((RAN + 1))
  RAN_LIST+=("$shown/$name${*:+ [$*]}: $passed passed")
  # Skips a test reports itself: "[  SKIPPED ] Suite.Name" lines.
  local line
  while IFS= read -r line; do
    SKIP_SEEN+=("$shown|$name|${line#\[  SKIPPED \] }|by the test (GTEST_SKIP)")
    SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
  done < <(printf '%s\n' "$out" | skipped_names)
  printf '  %-34s %4s passed%s\n' "$lib/$name" "$passed" "${*:+  [$*]}"
}

build_tests() { # <lib> <names...>
  local lib="$1"; shift
  local names=("$@") n
  local targets=()
  for n in "${names[@]}"; do
    targets+=("build/linux/release/apps/$n")
  done
  local extra=()
  [ "$lib" = lang-tang ] && extra=(WITH_DEBUG=no)
  mk "$lib" -j"$JOBS" "${targets[@]}" "${extra[@]}" >"$S/$ARCH_NOW/build-tests-$lib.log" 2>&1 ||
    { tail -30 "$S/$ARCH_NOW/build-tests-$lib.log" >&2; fail "the $lib tests did not build for $ARCH_NOW"; }
}

check_expected_skips() { # <arch>
  local arch="$1"
  local -a expected=()
  local e
  if [ "$arch" = aarch64 ]; then
    expected=("${SKIPPED_BY_FILTER_AARCH64[@]}" "${SKIPPED_BY_FILTER_AARCH64_RELOC[@]}" "${SKIPPED_BY_TEST_AARCH64[@]}")
  else
    expected=("${SKIPPED_BY_TEST_X86_64[@]}")
  fi
  say ""
  say "Skipped on $arch (by name, with the reason): ${#SKIP_SEEN[@]}"
  for e in "${SKIP_SEEN[@]}"; do
    IFS='|' read -r s_lib s_bin s_test s_why <<<"$e"
    say "  - $s_lib/$s_bin: $s_test"
    say "      $s_why"
  done
  if [ "${#SKIP_SEEN[@]}" -ne "${#expected[@]}" ]; then
    fail "expected exactly ${#expected[@]} skipped test(s) on $arch and saw ${#SKIP_SEEN[@]}; a skip appeared or vanished"
  fi
  # Every expected skip is the one seen, by name.
  for e in "${expected[@]}"; do
    IFS='|' read -r x_lib x_bin x_test x_why <<<"$e"
    local found=0 seen
    for seen in "${SKIP_SEEN[@]}"; do
      IFS='|' read -r s_lib s_bin s_test s_why <<<"$seen"
      [ "$s_lib" = "$x_lib" ] && [ "$s_bin" = "$x_bin" ] && [ "$s_test" = "$x_test" ] && found=1
    done
    [ "$found" = 1 ] || fail "the expected skip $x_lib/$x_bin $x_test was not seen on $arch"
  done
}

# runtime-heap with RELOCATE=yes, in a prefix of its own: every test binary with GRHEAP_RELOCATE=1
# (what `make test-relocate` runs), and `check-relocation-present` and `check-relocation-gates`,
# whose fixtures and planted libraries are run through the same qemu command for AArch64
# (GRHEAP_GATE_RUNNER). The prefix is a copy of the build's, so the relocation build never
# overwrites the ordinary one.
heap_relocation() { # <arch>
  local arch="$1" n names targets=()
  heading "runtime-heap with RELOCATE=yes: the unit suite relocating, and the relocation gates ($arch)"
  local RP="$S/$arch/prefix-reloc"
  rm -rf "$RP"
  mkdir -p "$RP"
  cp -a "$P/." "$RP/"
  sed -i "s#$P#$RP#g" "$RP"/share/pkgconfig/*.pc
  local saved_pkg="$PKG_CONFIG_PATH"
  if [ "$arch" = aarch64 ]; then
    export PKG_CONFIG_PATH="$RP/share/pkgconfig:/opt/aarch64/lib/pkgconfig"
  else
    export PKG_CONFIG_PATH="$RP/share/pkgconfig"
  fi
  local MK=(make -C "$SRC/runtime-heap" "${CROSS_VARS[@]}" PREFIX="$RP" RELOCATE=yes)
  "${MK[@]}" -j"$JOBS" all >"$S/$arch/build-reloc.log" 2>&1 ||
    { tail -30 "$S/$arch/build-reloc.log" >&2; fail "runtime-heap (RELOCATE=yes) did not build for $arch"; }
  "${MK[@]}" install >>"$S/$arch/build-reloc.log" 2>&1 ||
    { tail -30 "$S/$arch/build-reloc.log" >&2; fail "runtime-heap (RELOCATE=yes) did not install for $arch"; }
  names=$(echo 'print-%: ; @echo $($*)' |
    make -s --no-print-directory -C "$SRC/runtime-heap" -f Makefile -f - "${CROSS_VARS[@]}" \
      PREFIX="$RP" RELOCATE=yes WITH_DEBUG=no print-TEST_NAMES 2>/dev/null)
  [ -n "$names" ] || fail "could not read runtime-heap's test names for the relocation build"
  case " $names " in *" testRelocate "*) ;; *) fail "the RELOCATE=yes build does not list testRelocate: it is not the relocation build" ;; esac
  for n in $names; do
    targets+=("build/linux/release-reloc/apps/$n")
  done
  "${MK[@]}" -j"$JOBS" "${targets[@]}" >"$S/$arch/build-reloc-tests.log" 2>&1 ||
    { tail -30 "$S/$arch/build-reloc-tests.log" >&2; fail "the relocation build's tests did not build for $arch"; }
  APP_SUBDIR=release-reloc
  RUN_PREFIX="$RP"
  PHASE_LABEL=relocating
  for n in $names; do
    run_binary "$arch" runtime-heap "$n" GRHEAP_RELOCATE=1
  done
  APP_SUBDIR=release
  RUN_PREFIX=""
  PHASE_LABEL=""
  # The gates: each planted defect fails and its control passes, on this target's code.
  local runner="" gout
  [ "$arch" = aarch64 ] && runner="qemu-aarch64 -L $QEMU_SYSROOT"
  gout=$(GRHEAP_GATE_RUNNER="$runner" "${MK[@]}" check-relocation-present check-relocation-gates 2>&1) ||
    { printf '%s\n' "$gout" | tail -40 >&2; fail "the relocation gates failed for $arch"; }
  grep -q 'check-relocation-gates: all [0-9]* checks behaved' <<<"$gout" ||
    fail "check-relocation-gates did not report that every check behaved ($arch)"
  printf '%s\n' "$gout" | grep -E 'check-relocation-(present|gates):' | sed 's/^/  /'
  export PKG_CONFIG_PATH="$saved_pkg"
}

# runtime-jit's benchmark harness with --smoke: every case once with a tiny workload, the calls and
# natives cases included (they run on arm64 now). Every loop case checks its own sum inside the program (a wrong
# one is a failed run), and the script reads the check word each case prints and compares it with the one the
# case must give for the iteration count it says it ran (below), so the figure's sum is checked from outside
# too. No figure is a timing, because qemu-user's clock says nothing about hardware.
#
# The check word of a loop case: the plain, poll and call loops return the sum 0 + 1 + ... + (n - 1), xor the
# running count of helper calls (n after the call loop's, none before it); the compiled-call, tail-call, helper,
# native and native-status loops return n.
bench_expected_check() { # <case> <n> -> the check word, 16 hex digits; empty for a case with no loop sum
  local name="$1" n="$2" tri
  tri=$(( n * (n - 1) / 2 ))
  case "$name" in
    loop-plain|loop-poll) printf '%016x' "$tri" ;;
    loop-call) printf '%016x' $(( tri ^ n )) ;;
    loop-compiled-call|loop-tail-call|loop-helper-gc|loop-native|loop-native-status) printf '%016x' "$n" ;;
    *) printf '' ;;
  esac
}

# Reads a benchmark's output: every loop case ran, and its check word is the one it must give. Fails (through
# `fail`) with the case named.
bench_check_sums() { # <arch> <output>
  local arch="$1" out="$2" c line n got want checked=0
  for c in loop-plain loop-poll loop-call loop-compiled-call loop-tail-call loop-helper-gc loop-native loop-native-status; do
    line=$(grep -E "^$c " <<<"$out" | head -1)
    [ -n "$line" ] || fail "the benchmark case $c did not run on $arch"
    n=$(sed -n 's/.*(\([0-9][0-9]*\) ops x [0-9]*, check [0-9a-f]*)$/\1/p' <<<"$line")
    got=$(sed -n 's/.*check \([0-9a-f]*\))$/\1/p' <<<"$line")
    [ -n "$n" ] && [ -n "$got" ] || fail "the benchmark case $c printed no iteration count and check word: $line"
    want=$(bench_expected_check "$c" "$n")
    [ "$got" = "$want" ] || fail "the benchmark case $c ($arch) gave check word $got for $n iterations, not $want: its sum is wrong"
    checked=$((checked + 1))
  done
  say "  $checked loop cases' check words are the sums they must give"
}

bench_smoke() { # <arch>
  local arch="$1"
  heading "runtime-jit: the benchmark harness, --smoke ($arch)"
  local dir="$SRC/runtime-jit/build/linux/release/apps"
  mk runtime-jit -j"$JOBS" build/linux/release/apps/bench/bench >"$S/$arch/build-bench.log" 2>&1 ||
    { tail -30 "$S/$arch/build-bench.log" >&2; fail "runtime-jit's benchmark did not build for $arch"; }
  local bin="$dir/bench/bench"
  elf_machine_ok "$arch" "$bin" || fail "$bin is not an $arch binary"
  local out rc
  out=$(cd "$SRC/runtime-jit" && run_env "$arch" LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" -- "$bin" --smoke 2>&1)
  rc=$?
  [ $rc -eq 0 ] || { printf '%s\n' "$out" | tail -30 >&2; fail "runtime-jit's benchmark --smoke exited $rc ($arch)"; }
  grep -q 'smoke run' <<<"$out" || fail "the benchmark did not say it ran in smoke mode"
  bench_check_sums "$arch" "$out"
  say "  $(printf '%s\n' "$out" | grep -c ' best ') cases ran once each, the calls, tail-call and native cases among them (no figure here is a timing)"
}

run_suites() { # <arch>
  local arch="$1"
  ARCH_NOW="$arch"
  RAN=0
  PASSED_TESTS=0
  SKIPPED_TESTS=0
  SKIP_SEEN=()
  RAN_LIST=()

  local lib names n
  for lib in $SUITE_LIBS; do
    heading "$lib: every test binary ($arch)"
    names=$(test_names "$lib")
    [ -n "$names" ] || fail "could not read $lib's test names"
    # shellcheck disable=SC2086
    build_tests "$lib" $names
    for n in $names; do
      run_binary "$arch" "$lib" "$n"
    done
  done

  heap_relocation "$arch"
  bench_smoke "$arch"

  heading "lang-tang: the JIT arm ($arch)"
  # shellcheck disable=SC2086
  build_tests lang-tang testJit testJit_calls testNative_calls $LANG_TANG_THRESHOLD_TESTS
  run_binary "$arch" lang-tang testJit
  run_binary "$arch" lang-tang testJit_calls
  run_binary "$arch" lang-tang testNative_calls
  for n in $LANG_TANG_THRESHOLD_TESTS; do
    run_binary "$arch" lang-tang "$n" GLTANG_TEST_JIT_THRESHOLD=1
  done
  check_expected_skips "$arch"
  say ""
  say "$arch: $RAN test binaries, $PASSED_TESTS tests passed, ${#SKIP_SEEN[@]} skipped"
  if [ "$arch" = aarch64 ]; then
    AARCH64_BINARIES=$RAN
    AARCH64_TESTS=$PASSED_TESTS
    AARCH64_SKIPS=${#SKIP_SEEN[@]}
  else
    X86_BINARIES=$RAN
    X86_TESTS=$PASSED_TESTS
    X86_SKIPS=${#SKIP_SEEN[@]}
  fi
}

# ---- The planted defect ------------------------------------------------------------

# The defects the library lists in tools/arm64-plants.txt (4, and 20 to 29): each is planted in a tree
# of its own (BUILD_DIR), the test named for it is run alone, and it must FAIL BY AN ASSERTION: the
# output says FAILED and names a failure, and the program was not killed by a signal (a crash says a
# defect was reached, not that a test noticed it). Its control, the same test in the ordinary tree,
# runs first and must pass, so a test that fails for another reason is not a catch.
plant_arm64_defects() {
  heading "The planted arm64 defects, each caught under qemu-aarch64 by an assertion"
  setup_env aarch64
  ARCH_NOW=aarch64
  local lib=runtime-jit
  # ARM64_PLANTS_FILE replaces the list, to show the gate itself fails on a defect that is not caught,
  # a filter that matches no test and a catch that is a crash.
  local list="${ARM64_PLANTS_FILE:-$WORK/libs/$lib/tools/arm64-plants.txt}"
  [ -f "$list" ] || fail "$list is missing: no planted arm64 defect is named"
  local dir="$SRC/$lib/build/linux/release/apps"
  local id n test filter extra desc count=0 out rc tree
  while IFS='|' read -r id n test filter extra desc <&3; do
    case "$id" in ''|'#'*) continue ;; esac
    count=$((count + 1))
    # The control: the same binary, the same test, without the defect.
    [ -x "$dir/$test" ] || { build_tests "$lib" "$test"; }
    elf_machine_ok aarch64 "$dir/$test" || fail "the control binary $test is not AArch64"
    # (grep -q and grep -m exit at the first match: fed by a pipe from printf under pipefail that is a
    # SIGPIPE race, so they read the output as a here-string.)
    out=$(cd "$SRC/$lib" && run_env aarch64 LD_LIBRARY_PATH="$P/lib/ghoti.io:$dir" -- \
      "$dir/$test" --gtest_brief=1 --gtest_filter="$filter" 2>&1)
    rc=$?
    [ $rc -eq 0 ] || { printf '%s\n' "$out" | tail -20 >&2; fail "defect $id: the control $test does not pass under qemu"; }
    grep -q 'PASSED  \] [1-9]' <<<"$out" || fail "defect $id: the control $test ran no test (filter '$filter'): a pass of nothing is not a control"
    # The planted tree.
    tree="build/linux/release-plant-$id"
    mk "$lib" -j"$JOBS" "$tree/apps/$test" BUILD_DIR="$tree" \
      EXTRA_CFLAGS="-DGRJIT_TEST_PLANT_BUG=$n $extra" >"$S/aarch64/build-plant-$id.log" 2>&1 ||
      { tail -30 "$S/aarch64/build-plant-$id.log" >&2; fail "defect $id: the planted tree did not build (a build failure is not a catch)"; }
    elf_machine_ok aarch64 "$SRC/$lib/$tree/apps/$test" || fail "the planted binary $test is not AArch64"
    out=$(cd "$SRC/$lib" && run_env aarch64 LD_LIBRARY_PATH="$P/lib/ghoti.io:$SRC/$lib/$tree/apps" -- \
      "$SRC/$lib/$tree/apps/$test" --gtest_brief=1 --gtest_filter="$filter" 2>&1)
    rc=$?
    [ $rc -ne 124 ] || fail "defect $id ($desc): $test did not finish in ${BINARY_TIMEOUT}s: a hang is not a catch"
    [ $rc -ne 0 ] || fail "defect $id ($desc) was NOT caught by $test under qemu"
    [ $rc -lt 128 ] || { printf '%s\n' "$out" | tail -10 >&2; fail "defect $id ($desc): $test was killed by a signal (exit $rc): a crash is not a catch"; }
    grep -q 'FAILED' <<<"$out" || fail "defect $id: $test exited $rc without reporting a failure"
    grep -q 'Failure' <<<"$out" || fail "defect $id: $test failed without naming an assertion that failed"
    say "  ok   defect $id ($desc) is caught by $test:"
    grep -m3 -A3 'Failure' <<<"$out" | sed 's/^/         /'
    say "       its control passes"
  done 3< "$list"
  [ "$count" -ge 11 ] || [ -n "${ARM64_PLANTS_FILE:-}" ] ||
    fail "only $count planted defects were run: $list should name 4, 20 to 29 and the second form of 27"
  PLANTED_COUNT=$count
}

# The planted-mutation harness for arm64: every mutation of src/arm64 built with the cross compiler
# and run under qemu, each of which must be CAUGHT by a test (a catch by the pin alone is PIN-ONLY,
# which is not one), after the harness has shown it can fail (its self-test).
mutation_harness_arm64() {
  heading "The planted-mutation harness for arm64 (tools/check-planted-calls.py --target=arm64)"
  setup_env aarch64
  local h="$WORK/libs/runtime-jit/tools/check-planted-calls.py"
  [ -f "$h" ] || fail "$h is missing"
  local out rc
  out=$(python3 "$h" --target=arm64 --prefix="$P" --self-test 2>&1)
  rc=$?
  printf '%s\n' "$out" | sed 's/^/  /'
  [ $rc -eq 0 ] || fail "the harness's self-test (an edit that changes nothing MISSED, one that does not compile BUILD FAILED, one that hangs TIMEOUT) did not hold"
  out=$(python3 "$h" --target=arm64 --prefix="$P" 2>&1)
  rc=$?
  printf '%s\n' "$out" | sed 's/^/  /'
  [ $rc -eq 0 ] || fail "a mutation of the arm64 emitter was not CAUGHT (see above: MISSED, PIN-ONLY, TIMEOUT or BUILD FAILED)"
  MUTATION_SUMMARY=$(printf '%s\n' "$out" | grep -E '^[0-9]+ mutations, [0-9]+ not caught' | tail -1)
  [ -n "$MUTATION_SUMMARY" ] || fail "the harness printed no summary"
  say "  $MUTATION_SUMMARY"
}

# ---- The machinery check ---------------------------------------------------------------

machinery_checks() {
  heading "The machinery"
  mkdir -p "$S/probe"
  cat > "$S/probe/p.c" <<'EOC'
#include <stdio.h>
int main(void) {
#if defined(__aarch64__)
  puts("aarch64");
#elif defined(__x86_64__)
  puts("x86_64");
#else
  puts("other");
#endif
  return 0;
}
EOC
  aarch64-linux-gnu-gcc -O2 "$S/probe/p.c" -o "$S/probe/p-aarch64" || fail "the cross compiler does not compile"
  gcc -O2 "$S/probe/p.c" -o "$S/probe/p-x86_64" || fail "the host compiler does not compile"
  elf_machine_ok aarch64 "$S/probe/p-aarch64" || fail "the cross compiler's output is not AArch64 by ELF header"
  elf_machine_ok x86_64 "$S/probe/p-x86_64" || fail "the host compiler's output is not x86-64 by ELF header"
  # Without qemu the AArch64 binary must not run here: if it does, "foreign" is a lie.
  if "$S/probe/p-aarch64" >/dev/null 2>&1; then
    fail "an AArch64 binary ran on this host without qemu: the control cannot tell the architectures apart"
  fi
  say "  an AArch64 binary does not execute here without qemu (as it must not)"
  local got
  got=$(qemu-aarch64 -L "$QEMU_SYSROOT" "$S/probe/p-aarch64") || fail "qemu-aarch64 does not run an AArch64 binary"
  [ "$got" = aarch64 ] || fail "the binary run under qemu says it is '$got'"
  say "  under qemu-aarch64 it runs, and reports itself as: $got"
  got=$("$S/probe/p-x86_64")
  [ "$got" = x86_64 ] || fail "the native binary says it is '$got'"
  say "  the native control reports itself as: $got"
}

# ---- Main --------------------------------------------------------------------------------

# Sourced for its functions (the trials of the gates in suite/tools/xarch/jit-arm64-selftest.sh), not run.
if [ -n "${JIT_ARM64_SOURCE_ONLY:-}" ]; then
  return 0 2>/dev/null || exit 0
fi

ORIG_PATH="$PATH"
mkdir -p "$S"
say "jit-arm64: scratch $S, $JOBS jobs, workspace $WORK"
say "  the libraries are copied from $WORK/libs; nothing is built in them"
machinery_checks

setup_env aarch64
build_stack aarch64
run_suites aarch64
plant_arm64_defects
mutation_harness_arm64

setup_env x86_64
build_stack x86_64
run_suites x86_64

[ -z "${ARM64_PLANTS_FILE:-}" ] ||
  fail "ARM64_PLANTS_FILE was set, so the planted defects run were not the library's list: this was a trial of the gate, not a run of the suite"
ELAPSED=$(( $(date +%s) - STARTED ))
heading "Result"
say "AArch64 build, under qemu-aarch64: $AARCH64_BINARIES test binaries, $AARCH64_TESTS tests passed, $AARCH64_SKIPS skipped (listed above, by name)."
say "x86-64 control build, native:     $X86_BINARIES test binaries, $X86_TESTS tests passed, $X86_SKIPS skipped."
say "The planted arm64 defects ($PLANTED_COUNT: 4, 20 to 29 and the second form of 27) each failed their test under qemu by an assertion while its control passed."
say "The arm64 mutation harness: $MUTATION_SUMMARY."
say "Time: ${ELAPSED}s."
say ""
say "What this does NOT show: instruction-cache coherence (qemu-user translates lazily and"
say "does not model it; the library's cache call is covered by a unit test that checks it is"
say "reached, not by this run) and anything about real arm64 hardware (memory ordering,"
say "unaligned-access cost, the real cache, a real kernel's W^X)."
say "jit-arm64: OK"
