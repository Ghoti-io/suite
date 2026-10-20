#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The shape benchmark's job on the EVO-X2, in the two phases tools/evo gives a
# job (README.md, "Running it"):
#
#   run-evo.sh container   inside ghoti-build:gcc16: install the libraries into
#                          the job's empty prefix in suite/libraries.txt order,
#                          build bench-shape and pamdiff, fetch the pinned
#                          font, write the scenes, and run shape-cpu's
#                          `make test` (before anything is timed).
#   run-evo.sh host        on the box itself, after the container: run each
#                          engine in its own image, one after another, pinned
#                          to one CPU, recording the provenance and the box's
#                          load around each; then the cross-check and the
#                          results file (report.py).
#
# Every engine is timed in the host phase, through the same container engine
# wrapper and on the same CPU, so that what was running beside each one is
# recorded the same way for all of them.

set -eu

B=ws/suite/tools/shape-bench
LIBS="cutil security compress unicode color image math shape-geometry shape-scene shape-cpu font"
BUILD_IMAGE=ghoti-build:gcc16
BLEND2D_IMAGE=localhost/ghoti-shape-bench-blend2d:0.21.3-2
SKIA_IMAGE=localhost/ghoti-shape-bench-skia:m156-2

# DejaVu Sans 2.37. The tarball's digest is this repository's own: computed
# from the GitHub release and from SourceForge, two unrelated distribution
# paths, which served the same bytes on 2026-10-20. The font's digest is of the
# file inside it.
FONT_URL1=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
FONT_URL2=https://downloads.sourceforge.net/project/dejavu/dejavu/2.37/dejavu-fonts-ttf-2.37.tar.bz2
FONT_TAR_SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
FONT_SHA256=7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954

die() {
  echo "run-evo.sh: $*" >&2
  exit 1
}

container() {
  : "${JOB:?}" "${PREFIX:?}" "${RESULTS:?}"
  cd "$JOB"
  mkdir -p "$RESULTS/logs" "$RESULTS/scenes" "$RESULTS/frames" "$RESULTS/raw" "$RESULTS/prov"
  for l in $LIBS; do
    echo "== install $l"
    make -C "ws/libs/$l" -j16 install PREFIX="$PREFIX" > "$RESULTS/logs/install-$l.log" 2>&1 ||
      { tail -n 40 "$RESULTS/logs/install-$l.log"; die "installing $l failed"; }
  done
  echo "== build the harness"
  make -C "$B" PREFIX="$PREFIX" BUILD_DIR="$JOB/bench-build" > "$RESULTS/logs/bench-build.log" 2>&1 ||
    { cat "$RESULTS/logs/bench-build.log"; die "building the harness failed"; }

  echo "== fetch the font"
  t="$TMPDIR/dejavu.tar.bz2"
  curl -fsSL --max-time 300 -o "$t" "$FONT_URL1" || curl -fsSL --max-time 300 -o "$t" "$FONT_URL2" ||
    die "cannot fetch the font"
  printf '%s  %s\n' "$FONT_TAR_SHA256" "$t" | sha256sum -c - > /dev/null || die "the font tarball's digest moved"
  tar -xjf "$t" -C "$TMPDIR" dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
  cp "$TMPDIR/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" "$RESULTS/scenes/DejaVuSans.ttf"
  printf '%s  %s\n' "$FONT_SHA256" "$RESULTS/scenes/DejaVuSans.ttf" | sha256sum -c - > /dev/null ||
    die "the font's digest moved"

  echo "== write the scenes"
  "$JOB/bench-build/bench-shape" emit "$RESULTS/scenes" "$RESULTS/scenes/DejaVuSans.ttf" 2> "$RESULTS/logs/emit.log" ||
    { cat "$RESULTS/logs/emit.log"; die "writing the scenes failed"; }
  cat "$RESULTS/logs/emit.log"

  echo "== record the build"
  {
    echo "compiler: $(gcc --version | head -n 1)"
    echo "harness_flags: $(make -s -C "$B" PREFIX="$PREFIX" flags)"
    for l in shape-cpu font; do
      f=$(find "ws/libs/$l/build" -path '*release*' -name .flags 2> /dev/null | head -n 1)
      [ -n "$f" ] || f=$(find "ws/libs/$l/build" -name .flags 2> /dev/null | grep -v asan | head -n 1)
      echo "$l""_flags: $(cat "$f" 2> /dev/null || echo unknown)"
    done
    echo "font_file: DejaVuSans.ttf 2.37 sha256 $FONT_SHA256"
  } > "$RESULTS/prov/shape-cpu.build.txt"

  echo "== shape-cpu make test"
  if make -C ws/libs/shape-cpu -j16 test PREFIX="$PREFIX" > "$RESULTS/logs/shape-cpu-test.log" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  echo "$rc" > "$RESULTS/logs/shape-cpu-test.exit"
  tail -n 15 "$RESULTS/logs/shape-cpu-test.log"
  [ "$rc" -eq 0 ] || die "shape-cpu make test failed"
}

# The box's state, as text the report keeps beside the numbers.
state() {
  echo "time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "loadavg: $(cat /proc/loadavg)"
  echo "containers: $(docker ps --format '{{.Image}}' | tr '\n' ' ')"
  echo "busiest:"
  # top's one-shot sample, not ps's lifetime average: what is using a CPU now.
  { top -b -n 2 -d 1 -o %CPU 2> /dev/null | awk '/^top -/ {n++} n == 2' | sed -n '8,13p'; } | sed 's/^/  /'
}

run_engine() {
  name=$1 image=$2 cmd=$3
  info="$RESULTS/prov/$name.txt"
  docker image inspect "$image" > /dev/null 2>&1 || die "no image $image on the box"
  {
    echo "engine: $name"
    echo "image: $image"
    echo "image_id: $(docker image inspect --format '{{.Id}}' "$image")"
    echo "host: $(hostname)"
    echo "cpu_model: $(grep -m 1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "cpus_online: $(nproc --all)"
    echo "kernel: $(uname -r)"
    echo "governor: $(cat "/sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor" 2> /dev/null || echo unknown)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2> /dev/null || echo unknown)"
    echo "pinned_cpu: $CPU"
    echo "nice: the container runs at the box's default priority, its only competition what 'busiest' shows"
    echo "before:"
    state | sed 's/^/  /'
  } > "$info"
  echo "== $name"
  "$ENGINE" run --rm --cpuset-cpus="$CPU" --user "$(id -u):$(id -g)" -v "$JOB:/work" -w /work "$image" \
    sh -c "$cmd" > "$RESULTS/raw/$name.jsonl" 2> "$RESULTS/logs/$name.err" ||
    { cat "$RESULTS/logs/$name.err"; die "$name failed"; }
  {
    echo "after:"
    state | sed 's/^/  /'
  } >> "$info"
  cat "$RESULTS/logs/$name.err"
}

host() {
  : "${JOB:?}" "${RESULTS:?}"
  cd "$JOB"
  ENGINE=${GHOTI_CONTAINER_ENGINE:-docker}
  CPU=${SHAPE_BENCH_CPU:-8}
  # SHAPE_BENCH_ENGINES=shape-cpu re-measures shape-cpu alone; the report is then made with report.py --reuse over
  # an earlier run's results (README.md, "Re-measuring shape-cpu alone").
  ENGINES=${SHAPE_BENCH_ENGINES:-shape-cpu blend2d skia}
  want() {
    case " $ENGINES " in *" $1 "*) return 0 ;; esac
    return 1
  }
  base=$(sed -n 's/^FROM \([^ ]*\).*/\1/p' ws/suite/Containerfile.build | head -n 1)

  if want shape-cpu; then
    run_engine shape-cpu "$BUILD_IMAGE" \
      "echo \"compiler: \$(gcc --version | head -n 1)\" >&2; bench-build/bench-shape run results/scenes results/frames"
    echo "base: $base" >> "$RESULTS/prov/shape-cpu.txt"
  fi

  want blend2d && run_engine blend2d "$BLEND2D_IMAGE" "set -e
    lib=\$(find /opt/blend2d -name 'libblend2d*.a' | head -n 1)
    flags='-O2 -DBL_STATIC -I/opt/blend2d/include'
    echo \"driver: clang++-19 -std=c++20 \$flags (scene.c: clang-19 -std=c17 -O2)\" >&2
    cat /opt/blend2d/share/PROVENANCE >&2
    clang-19 -std=c17 -O2 -c $B/scene.c -o /tmp/scene.o
    clang++-19 -std=c++20 \$flags -o /tmp/bench-blend2d $B/refs/bench-blend2d.cpp /tmp/scene.o \$lib -lpthread -lrt -ldl
    /tmp/bench-blend2d results/scenes results/frames"

  want skia && run_engine skia "$SKIA_IMAGE" "set -e
    defs=\$(grep -E '^ *[A-Z_][A-Z0-9_]*(=.*)?\$' /opt/skia/share/DEFINES | grep -v SKIA_IMPLEMENTATION | sed 's/^ */-D/' | tr '\n' ' ')
    flags=\"-O2 -DNDEBUG \$defs -I/opt/skia\"
    echo \"driver: clang++-19 -std=c++20 \$flags (scene.c: clang-19 -std=c17 -O2)\" >&2
    cat /opt/skia/share/PROVENANCE /opt/skia/share/GN_ARGS >&2
    clang-19 -std=c17 -O2 -c $B/scene.c -o /tmp/scene.o
    clang++-19 -std=c++20 \$flags -o /tmp/bench-skia $B/refs/bench-skia.cpp /tmp/scene.o \
      -Wl,--start-group /opt/skia/out/Release/*.a -Wl,--end-group -lfreetype -lpthread -ldl
    /tmp/bench-skia results/scenes results/frames"

  if [ "$ENGINES" != "shape-cpu blend2d skia" ]; then
    echo "== report: not made here (engines: $ENGINES); run report.py --reuse over an earlier run's results"
    return 0
  fi
  echo "== report"
  "$ENGINE" run --rm --user "$(id -u):$(id -g)" -v "$JOB:/work" -w /work "$BUILD_IMAGE" \
    python3 "$B/report.py" --results results --pamdiff bench-build/pamdiff --shipped SHIPPED
}

case "${1:-}" in
container) container ;;
host) host ;;
*) die "usage: run-evo.sh container | host" ;;
esac
