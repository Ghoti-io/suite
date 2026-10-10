#!/bin/bash
#
# Runs runtime-jit's CPU-feature tests under qemu CPU models: the one thing a host that has every
# feature can never show is detection on a CPU that lacks one (spec-runtime-float, story 4).
#
# runtime-jit uses SSE4.1 (`roundsd`), POPCNT, LZCNT and BMI1 (`tzcnt`) where it detects them, and on a CPU
# without LZCNT or BMI1 the instructions do not fault: they run as `bsr` and `bsf`, with other answers. So a
# detection bug gives wrong results, not a crash, and only a CPU that lacks the feature can show it. The
# decoder over the raw CPUID words is tested by a table on any host; what this adds is that the assembly
# reads the right registers, and that code compiled for the detected set is right.
#
# Run it in the image Containerfile.arm64-jit builds (it has qemu-x86_64 from ghoti-xarch:deb13, and g++,
# pkg-config and googletest for x86-64 as the control build), from the workspace root:
#
#   podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarch-x86-cpu:/scratch:z \
#     ghoti-xarch-arm64-jit:deb13-gxx14-gtest bash /work/suite/tools/xarch/jit-x86-cpu.sh
#
# What it does, in order:
#
#   1. Checks the machinery. x86-cpu-probe.c (plain C: CPUID, and the four instructions encoded by hand,
#      sharing nothing with runtime-jit) is built and run under `qemu-x86_64 -cpu <model>` for each model. What
#      it reads (the four CPUID bits) and what the instructions do (`lzcnt` and `tzcnt` as themselves or as
#      `bsr` and `bsf`; `popcnt` and `roundsd` or SIGILL) must equal the MODELS table below. The table is data
#      about qemu, not about runtime-jit: qemu changes, and when it does this stage fails and says which
#      cell moved, before any claim about runtime-jit is made. The models must also differ from the native
#      run where the host has the feature, or `-cpu` did nothing.
#   2. Builds cutil, runtime-core and runtime-jit natively as x86-64 (the control) and runs `testCpu_features`
#      natively, once, with nothing set.
#   3. For each model runs `qemu-x86_64 -cpu <model> testCpu_features` with GRJIT_TEST_CPU_MODEL,
#      GRJIT_TEST_EXPECT_FEATURES (the stamp of the model's set), GRJIT_TEST_LZCNT_AS and GRJIT_TEST_TZCNT_AS (what
#      stage 1 measured the instructions to do) and GRJIT_TEST_CPU_CROSSED. The detected set must equal the
#      model's, code compiled for it must be right where the model's instructions behave as its bits say, and the
#      forced-wrong-arm control must show the instruction the model has: `clz(0x10)` is 59 where `lzcnt` ran
#      and 4 where the CPU ran it as `bsr`. A model where the answer is not the one measured fails.
#   4. Plants, one tree each, the defects runtime-jit lists in tools/x86-cpu-plants.txt and requires the model
#      test to FAIL by an assertion (a crash or a hang is not one) while the unplanted control passes.
#   5. Counts, per model, the tests that ran and the ones skipped, by name, and asserts both. A model that
#      skips more than its missing features explain, or fewer, fails the run.
#
# Two models (`Haswell,-bmi1,-bmi2` and `Haswell,-abm,-bmi2`) behave inconsistently in qemu 10.0.13: removing BMI1 makes `lzcnt` run as `bsr` while its CPUID bit
# stays set, and removing ABM (the LZCNT bit) makes `tzcnt` run as `bsf` while the BMI1 bit stays set. Detection
# is still checked against the bits there, and the forced-arm control against the measured behaviour; running
# compiled code is asserted only on the models where every set bit has its instruction (stage 1 derives this from
# the probe and requires it to equal the table).
#
# What it does NOT show: a real CPU (qemu-user's models are qemu's own idea of them; the host's native run is the
# one real CPU here) and any operating system's feature gating (XSAVE state is not involved in these four).

set -u
set -o pipefail

WORK="${WORK:-/work}"
S="${SCRATCH:-/scratch}"
JOBS="${JOBS:-$(nproc)}"
STARTED=$(date +%s)
BINARY_TIMEOUT="${JIT_X86_CPU_TIMEOUT:-900}"
ORIG_PATH="$PATH"

fail() { printf '\njit-x86-cpu: FAIL: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }
heading() { printf '\n=== %s ===\n' "$*"; }

# ---- What qemu 10.0.13 does, per model: data about qemu (measured 2026-10-10) -----------------------------
#
# model | stamp of the set its CPUID bits say | sse4_1 popcnt lzcnt bmi1 (CPUID) | lzcnt_as | tzcnt_as | popcnt | roundsd
#
# lzcnt_as is what `lzcnt rcx, rax` of 0x10 does (59: lzcnt; 4: bsr), tzcnt_as what `tzcnt` of 0 does (64: tzcnt;
# rcx left alone: bsf). The two crossed models are Haswell,-bmi1,-bmi2 (lzcnt runs as bsr with its bit set) and
# Haswell,-abm,-bmi2 (tzcnt runs as bsf with its bit set).
#
# Both are written with `-bmi2` and not as plain `Haswell,-bmi1` and `Haswell,-abm`, because under the plain ones the
# image's glibc does not work: the CPU says AVX2 and BMI2 together with a BMI1 that is missing (`-bmi1`) or a `tzcnt` that
# runs as `bsf` (`-abm`), and glibc's AVX2 string functions (chosen when AVX2 and BMI2 are present) use BMI1 instructions.
# Under `Haswell,-bmi1` the dynamic loader dies with SIGILL before main; under `Haswell,-abm` it is worse and silent:
# the program starts and gtest registers ZERO tests ("[  PASSED  ] 0 tests."), because a string function answered wrongly.
# That is why stage 3 lists the tests under the model first and requires all of them. Taking BMI2 away makes glibc choose
# its SSE variants; the probe measured the same instruction behaviour with BMI2 removed.
MODELS=(
  "max|x86_64-v1+sse4.1+popcnt+lzcnt+bmi1|1 1 1 1|lzcnt|tzcnt|ok|ok"
  "qemu64|x86_64-v1|0 0 0 0|bsr|bsf|SIGILL|SIGILL"
  "Nehalem|x86_64-v1+sse4.1+popcnt|1 1 0 0|bsr|bsf|ok|ok"
  "Haswell,-sse4.1|x86_64-v1+popcnt+lzcnt+bmi1|0 1 1 1|lzcnt|tzcnt|ok|SIGILL"
  "Haswell,-popcnt|x86_64-v1+sse4.1+lzcnt+bmi1|1 0 1 1|lzcnt|tzcnt|SIGILL|ok"
  "Haswell,-bmi1,-bmi2|x86_64-v1+sse4.1+popcnt+lzcnt|1 1 1 0|bsr|tzcnt|ok|ok"
  "Haswell,-abm,-bmi2|x86_64-v1+sse4.1+popcnt+bmi1|1 1 0 1|lzcnt|bsf|ok|ok"
)

# JIT_X86_CPU_MODELS="qemu64 Haswell,-bmi1,-bmi2" runs a subset, as a trial of the pieces: the result says TRIAL and never
# prints the OK line the gate prints.
TRIAL=""
if [ -n "${JIT_X86_CPU_MODELS:-}" ]; then
  TRIAL=1
  kept=()
  for entry in "${MODELS[@]}"; do
    for m in $JIT_X86_CPU_MODELS; do
      [ "${entry%%|*}" = "$m" ] && kept+=("$entry")
    done
  done
  [ "${#kept[@]}" -gt 0 ] || { printf 'jit-x86-cpu: FAIL: JIT_X86_CPU_MODELS names no model of the table\n' >&2; exit 1; }
  MODELS=("${kept[@]}")
fi

# The tests that skip by name, and why (the counts are asserted per model in stage 5).
T_CPUINFO="CpuDetect.TheDetectedSetIsTheCpuinfoFlagsOfTheHostWhenThatFileIsTheHostsOwn"
T_REUSE="CpuReuse.CodeRunsWhereItsRecordedSetIsASubsetOfTheRunningOneAndNowhereElse"
T_FORCED_HOST="CpuModel.AForcedArmOfAFeatureTheHostHasRunsItsInstruction"
T_CODE="CpuModel.CodeCompiledForTheDetectedSetIsRightWhereTheModelsInstructionsBehaveAsItsBitsSay"
# The compiled-code tests that a crossed model skips (GRJIT_TEST_CPU_CROSSED=1), besides T_CODE.
T_RUN=(
  "CpuRun.TheWorkedAnswersOfTheIntegerOperationsAreTheSameUnderEverySubsetOfTheHostsFeatures"
  "CpuRun.TheWorkedAnswersOfTheRoundingsAreTheSameUnderEverySubsetOfTheHostsFeatures"
  "CpuRun.TheV1ArmGivesTheWorkedAnswersOnAHostThatHasTheFeaturesBecauseItsMaskedOff"
  "CpuRun.EveryFeatureDependentOperationGivesTheEvaluatorsAnswersUnderEverySubsetAndEveryMaskAgrees"
)
T_SKIPS="CpuSkips.OnAHostWithEveryFeatureNothingWasSkippedForAMissingOne"
T_DETECT="CpuModel.TheDetectedSetIsTheOneTheRunExpectsAndTheNativeRunExpectsTheHostsOwn"

declare -A PROBE

# Reads a probe line into PROBE[key]=value.
parse_probe() { # <line>
  local w
  PROBE=()
  for w in $1; do
    PROBE["${w%%=*}"]="${w#*=}"
  done
}

# ---- Stage 1: the machinery --------------------------------------------------------------------------------
machinery_checks() {
  heading "1. The machinery: the probe under each model, against the table"
  gcc -O1 -Wall -Wextra -Werror -o "$S/probe" "$WORK/suite/tools/xarch/x86-cpu-probe.c" ||
    fail "the CPU probe did not compile"
  [[ "$(file -b "$S/probe")" == *"x86-64"* ]] || fail "the probe is not an x86-64 binary"
  local native
  native=$("$S/probe") || fail "the probe does not run natively"
  say "native: $native"
  local native_bits
  parse_probe "$native"
  native_bits="${PROBE[sse4_1_bit]} ${PROBE[popcnt_bit]} ${PROBE[lzcnt_bit]} ${PROBE[bmi1_bit]}"
  local entry model stamp bits ln tn pc rs qemu64_differs=0
  CROSSED=()
  for entry in "${MODELS[@]}"; do
    IFS='|' read -r model stamp bits ln tn pc rs <<<"$entry"
    local err="$S/probe-$model.err" line
    line=$(qemu-x86_64 -cpu "$model" "$S/probe" 2>"$err") || fail "the probe does not run under -cpu $model: $(cat "$err")"
    parse_probe "$line"
    local got="${PROBE[sse4_1_bit]} ${PROBE[popcnt_bit]} ${PROBE[lzcnt_bit]} ${PROBE[bmi1_bit]}"
    local ok=1 why=""
    [ "$got" = "$bits" ] || { ok=0; why+=" CPUID bits [sse4.1 popcnt lzcnt bmi1] are '$got', the table says '$bits';"; }
    [ "${PROBE[lzcnt_as]}" = "$ln" ] || { ok=0; why+=" lzcnt ran as ${PROBE[lzcnt_as]}, the table says $ln;"; }
    [ "${PROBE[tzcnt_as]}" = "$tn" ] || { ok=0; why+=" tzcnt ran as ${PROBE[tzcnt_as]}, the table says $tn;"; }
    [ "${PROBE[popcnt]}" = "$pc" ] || { ok=0; why+=" popcnt is ${PROBE[popcnt]}, the table says $pc;"; }
    [ "${PROBE[roundsd]}" = "$rs" ] || { ok=0; why+=" roundsd is ${PROBE[roundsd]}, the table says $rs;"; }
    if [ "$ok" != 1 ]; then
      say "  $model: $line"
      fail "qemu's behaviour changed under -cpu $model (the table is data about qemu, so re-measure it):$why"
    fi
    # Crossed: a bit that is set whose instruction does not behave as the bit says.
    local crossed=0 reasons=""
    [ "${PROBE[lzcnt_bit]}" = 1 ] && [ "${PROBE[lzcnt_as]}" != lzcnt ] && { crossed=1; reasons+="LZCNT bit set but lzcnt runs as ${PROBE[lzcnt_as]}; "; }
    [ "${PROBE[bmi1_bit]}" = 1 ] && [ "${PROBE[tzcnt_as]}" != tzcnt ] && { crossed=1; reasons+="BMI1 bit set but tzcnt runs as ${PROBE[tzcnt_as]}; "; }
    [ "${PROBE[popcnt_bit]}" = 1 ] && [ "${PROBE[popcnt]}" != ok ] && { crossed=1; reasons+="POPCNT bit set but popcnt is ${PROBE[popcnt]}; "; }
    [ "${PROBE[sse4_1_bit]}" = 1 ] && [ "${PROBE[roundsd]}" != ok ] && { crossed=1; reasons+="SSE4.1 bit set but roundsd is ${PROBE[roundsd]}; "; }
    CROSSED+=("$model|$crossed|$reasons")
    say "  $model: bits [sse4.1 popcnt lzcnt bmi1] = $got; lzcnt ran as $ln, tzcnt as $tn, popcnt $pc, roundsd $rs$([ "$crossed" = 1 ] && echo "; CROSSED: $reasons")"
    if [ -s "$err" ]; then
      say "      qemu said on stderr (not filtered):"
      sed 's/^/        /' "$err"
    fi
    # `-cpu` must have changed something where the host has the feature: otherwise it did nothing.
    [ "$model" = qemu64 ] && [ "$got" != "$native_bits" ] && qemu64_differs=1
  done
  [ "$qemu64_differs" = 1 ] || fail "qemu64 does not differ from the native run (the host has none of these features, or -cpu did nothing): the control that the models change anything has nothing to compare"
  # The crossed models are the two the notes name, and no others.
  local c
  for c in "${CROSSED[@]}"; do
    IFS='|' read -r model crossed reasons <<<"$c"
    case "$model" in
      Haswell,-bmi1,-bmi2|Haswell,-abm,-bmi2) [ "$crossed" = 1 ] || fail "$model was expected to be crossed and is not" ;;
      *) [ "$crossed" = 0 ] || fail "$model is crossed ($reasons) and is not on the list of crossed models" ;;
    esac
  done
  say "  the probe agrees with the table for all ${#MODELS[@]} models; qemu64 differs from the native run"
}

# ---- Stage 2: build the control --------------------------------------------------------------------------------
P="$S/x86_64/prefix"
SRC="$S/x86_64/src"
sync_source() { # <lib> [dest]
  local lib="$1" dest="${2:-$SRC/$1}"
  mkdir -p "$dest"
  rsync -a --delete --exclude build --exclude docs --exclude '.git' "$WORK/libs/$lib/" "$dest/"
}
mk() { # <dir> <make args...>
  local dir="$1"; shift
  make -C "$dir" PREFIX="$P" "$@"
}

build_native() {
  heading "2. Building cutil, runtime-core and runtime-jit natively (the control)"
  mkdir -p "$P" "$SRC"
  PATH="$ORIG_PATH"
  export PKG_CONFIG_PATH="$P/share/pkgconfig"
  local lib
  for lib in cutil runtime-core runtime-jit; do
    sync_source "$lib"
  done
  for lib in cutil runtime-core; do
    say "-- $lib"
    mk "$SRC/$lib" -j"$JOBS" install >"$S/x86_64/build-$lib.log" 2>&1 ||
      { tail -30 "$S/x86_64/build-$lib.log" >&2; fail "$lib did not build"; }
  done
  say "-- runtime-jit (the library and testCpu_features)"
  mk "$SRC/runtime-jit" -j"$JOBS" all build/linux/release/apps/testCpu_features >"$S/x86_64/build-runtime-jit.log" 2>&1 ||
    { tail -30 "$S/x86_64/build-runtime-jit.log" >&2; fail "runtime-jit did not build"; }
  TESTBIN="$SRC/runtime-jit/build/linux/release/apps/testCpu_features"
  [[ "$(file -b "$TESTBIN")" == *"x86-64"* ]] || fail "$TESTBIN is not an x86-64 binary"
  TEST_COUNT=$(LD_LIBRARY_PATH="$P/lib/ghoti.io" "$TESTBIN" --gtest_list_tests | grep -c '^  ')
  [ "$TEST_COUNT" -gt 20 ] || fail "testCpu_features lists only $TEST_COUNT tests"
  say "  testCpu_features: $TEST_COUNT tests"
  LD_LIBRARY_PATH="$P/lib/ghoti.io" "$TESTBIN" --gtest_list_tests | awk '/^[^ ]/ {suite=$1} /^  / {print suite $1}' >"$S/x86_64/test-names.txt"
  for t in "$T_CPUINFO" "$T_REUSE" "$T_FORCED_HOST" "$T_CODE" "$T_SKIPS" "$T_DETECT" "${T_RUN[@]}"; do
    grep -qxF "$t" "$S/x86_64/test-names.txt" || fail "the test $t named in this script is not in testCpu_features: the skip table is stale"
  done
}

# qemu's -E splits its argument at commas (getsubopt), so a model name with a comma in it cannot be a value:
# the tests are told the model with the commas written as spaces ("Haswell -popcnt").
label_of() { printf '%s' "${1//,/ }"; }

# Runs the test binary: <model or ""> <binary> <filter or ""> [VAR=value...] -> $OUT, $RC
run_test() {
  local model="$1" bin="$2" filter="$3"; shift 3
  local -a envs=(LD_LIBRARY_PATH="$P/lib/ghoti.io" "$@")
  local -a f=()
  [ -n "$filter" ] && f=("--gtest_filter=$filter")
  if [ -n "$model" ]; then
    local -a q=(qemu-x86_64 -cpu "$model")
    local e
    for e in "${envs[@]}"; do
      q+=(-E "$e")
    done
    OUT=$(timeout -k 10 "$BINARY_TIMEOUT" "${q[@]}" "$bin" "${f[@]}" 2>&1)
  else
    OUT=$(timeout -k 10 "$BINARY_TIMEOUT" env "${envs[@]}" "$bin" "${f[@]}" 2>&1)
  fi
  RC=$?
}

skipped_names() { grep -E '^\[  SKIPPED \] [A-Za-z0-9_/]+\.[A-Za-z0-9_/]+$' | sed 's/^\[  SKIPPED \] //' || true; }

native_run() {
  heading "2b. testCpu_features natively (nothing set)"
  run_test "" "$TESTBIN" ""
  [ $RC -eq 0 ] || { printf '%s\n' "$OUT" | tail -40 >&2; fail "testCpu_features failed natively (exit $RC)"; }
  say "$(printf '%s\n' "$OUT" | grep -E '^detected features|^\[  PASSED|SKIPPED')"
  local passed
  passed=$(printf '%s\n' "$OUT" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1)
  say "  native: ${passed:-0} passed, $(printf '%s\n' "$OUT" | skipped_names | wc -l) skipped"
  NATIVE_SKIPS=$(printf '%s\n' "$OUT" | skipped_names | wc -l)
}

# ---- Stage 3 and 5: the models ----------------------------------------------------------------------------------
MODEL_RESULTS=()
model_runs() {
  heading "3. testCpu_features under each model; 5. the counts, by name"
  local entry model stamp bits ln tn pc rs crossed
  for entry in "${MODELS[@]}"; do
    IFS='|' read -r model stamp bits ln tn pc rs <<<"$entry"
    crossed=0
    local c
    for c in "${CROSSED[@]}"; do
      [ "${c%%|*}" = "$model" ] && { crossed=${c#*|}; crossed=${crossed%%|*}; }
    done
    # The program must start and register all of its tests under the model: a libc that misbehaves under a model
    # runs a binary that lists none, and "0 tests passed" is not a result about the library.
    local listed
    listed=$(qemu-x86_64 -cpu "$model" -E LD_LIBRARY_PATH="$P/lib/ghoti.io" "$TESTBIN" --gtest_list_tests 2>/dev/null | grep -c '^  ')
    [ "$listed" = "$TEST_COUNT" ] ||
      fail "under -cpu $model testCpu_features lists $listed tests, not $TEST_COUNT: the program does not run correctly under this model (glibc?)"
    run_test "$model" "$TESTBIN" "" GRJIT_TEST_CPU_MODEL="$(label_of "$model")" GRJIT_TEST_EXPECT_FEATURES="$stamp" \
      GRJIT_TEST_LZCNT_AS="$ln" GRJIT_TEST_TZCNT_AS="$tn" GRJIT_TEST_CPU_CROSSED="$crossed"
    if [ $RC -ne 0 ]; then
      printf '%s\n' "$OUT" | tail -40 >&2
      fail "testCpu_features under -cpu $model exited $RC"
    fi
    say "-- $model"
    printf '%s\n' "$OUT" | grep -E '^model |^forced |^model run' | sed 's/^/     /'
    # The set the run detected, against the table.
    printf '%s\n' "$OUT" | grep -q "^model $(label_of "$model"): detected " || fail "the model run of $model did not report its detected set"
    # What should have been skipped, by name.
    local -a want=("$T_CPUINFO")
    local sse41 popcnt lzcnt bmi1
    read -r sse41 popcnt lzcnt bmi1 <<<"$bits"
    [ "$lzcnt" = 1 ] || want+=("$T_REUSE")
    { [ "$sse41" = 1 ] && [ "$popcnt" = 1 ]; } || want+=("$T_FORCED_HOST")
    [ "$crossed" = 0 ] || want+=("$T_CODE" "${T_RUN[@]}")
    [ "$bits" = "1 1 1 1" ] || want+=("$T_SKIPS")
    local got
    got=$(printf '%s\n' "$OUT" | skipped_names | sort)
    local expected
    expected=$(printf '%s\n' "${want[@]}" | sort)
    if [ "$got" != "$expected" ]; then
      say "  skipped (seen):"; printf '%s\n' "$got" | sed 's/^/    /'
      say "  skipped (expected from the model's bits):"; printf '%s\n' "$expected" | sed 's/^/    /'
      fail "under -cpu $model the skipped tests are not the ones its missing features explain"
    fi
    local passed
    passed=$(printf '%s\n' "$OUT" | sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' | tail -1)
    [ "${passed:-0}" -eq $((TEST_COUNT - ${#want[@]})) ] ||
      fail "under -cpu $model ${passed:-0} tests passed, expected $((TEST_COUNT - ${#want[@]})) of $TEST_COUNT"
    say "     $passed passed, ${#want[@]} skipped by name:"
    local s
    for s in "${want[@]}"; do say "       - $s"; done
    MODEL_RESULTS+=("$model: $passed passed, ${#want[@]} skipped$([ "$crossed" = 1 ] && echo ' (crossed: compiled-code assertions skipped, detection and the forced-arm control run)')")
  done
}

# ---- Stage 4: the planted defects ------------------------------------------------------------------------------
PLANTED=0
plant_defects() {
  heading "4. Planted defects: each must fail the model test by an assertion, with its control passing"
  local list="$WORK/libs/runtime-jit/tools/x86-cpu-plants.txt"
  [ -f "$list" ] || fail "$list is missing"
  local line id bug test filter defs model what
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    IFS='|' read -r id bug test filter defs model what <<<"$line"
    local entry m_stamp bits ln tn pc rs m_crossed=0
    for entry in "${MODELS[@]}"; do
      [ "${entry%%|*}" = "$model" ] && IFS='|' read -r _ m_stamp bits ln tn pc rs <<<"$entry"
    done
    [ -n "${m_stamp:-}" ] || fail "plant $id names the model $model, which is not in the table"
    local dir="$S/x86_64/plant-$id"
    sync_source runtime-jit "$dir"
    mk "$dir" -j"$JOBS" all EXTRA_CFLAGS="-DGRJIT_TEST_PLANT_BUG=$bug $defs" "build/linux/release/apps/$test" \
      >"$S/x86_64/plant-$id.log" 2>&1 || { tail -30 "$S/x86_64/plant-$id.log" >&2; fail "the tree with defect $id did not build"; }
    local bin="$dir/build/linux/release/apps/$test"
    # The control: the unplanted binary, same model, same test.
    run_test "$model" "$TESTBIN" "$filter" GRJIT_TEST_CPU_MODEL="$(label_of "$model")" GRJIT_TEST_EXPECT_FEATURES="$m_stamp" \
      GRJIT_TEST_LZCNT_AS="$ln" GRJIT_TEST_TZCNT_AS="$tn"
    [ $RC -eq 0 ] || { printf '%s\n' "$OUT" | tail -20 >&2; fail "the control for defect $id does not pass under -cpu $model"; }
    run_test "$model" "$bin" "$filter" GRJIT_TEST_CPU_MODEL="$(label_of "$model")" GRJIT_TEST_EXPECT_FEATURES="$m_stamp" \
      GRJIT_TEST_LZCNT_AS="$ln" GRJIT_TEST_TZCNT_AS="$tn"
    if [ $RC -eq 0 ]; then
      fail "defect $id ($what) was NOT caught under -cpu $model"
    fi
    printf '%s\n' "$OUT" | grep -q 'FAILED' || { printf '%s\n' "$OUT" | tail -20 >&2; fail "defect $id: the run exited $RC without reporting a failed assertion (a crash or a hang is not a catch)"; }
    say "  ok   defect $id is caught under -cpu $model, by: $(printf '%s\n' "$OUT" | grep -m1 -A3 'Failure' | grep -vE 'Failure|^--$' | tr '\n' ' ' | cut -c1-200)"
    PLANTED=$((PLANTED + 1))
  done <"$list"
  [ "$PLANTED" -gt 0 ] || fail "no planted defect was run; this stage measured nothing"
}

mkdir -p "$S" "$S/x86_64"
say "jit-x86-cpu: scratch $S, $JOBS jobs, workspace $WORK"
say "  qemu: $(qemu-x86_64 --version | head -1)"
machinery_checks
build_native
native_run
model_runs
plant_defects

ELAPSED=$(( $(date +%s) - STARTED ))
heading "Result"
say "The probe agreed with the table for ${#MODELS[@]} models, and qemu64 differed from the native run."
say "testCpu_features, native control: $TEST_COUNT tests, $NATIVE_SKIPS skipped."
for r in "${MODEL_RESULTS[@]}"; do say "  under -cpu $r"; done
say "Planted defects: $PLANTED, each failed the model test by an assertion while its control passed."
say "Time: ${ELAPSED}s."
say ""
say "What this does NOT show: a real CPU of any of these models (qemu-user's are qemu's own), or anything about an"
say "operating system's gating of features (none of the four needs XSAVE state)."
if [ -n "$TRIAL" ]; then
  say "jit-x86-cpu: TRIAL (JIT_X86_CPU_MODELS=$JIT_X86_CPU_MODELS): the pieces ran; this is not a run of the gate"
  exit 0
fi
say "jit-x86-cpu: OK"
