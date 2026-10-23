#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# The shape benchmark's run, in two phases (README.md, "Running it"). Both take
# WORK, one directory that holds everything a run reads and writes:
#
#   WORK/ws/libs/<lib>, WORK/ws/suite   the sources, laid out as suite/clone.sh
#                                       lays them out
#   WORK/prefix                         the install prefix ($PREFIX when set;
#                                       it must be inside WORK)
#   WORK/bench-build                    bench-shape, pamdiff and corroborate
#   WORK/results                        scenes, frames, raw figures, provenance,
#                                       logs, and report/ (the results files)
#
#   run.sh build WORK [JOBS]   inside ghoti-build:gcc16 (suite/Containerfile.build):
#                              install the libraries into the prefix in
#                              suite/libraries.txt order, build bench-shape and
#                              pamdiff, fetch the pinned font, write the scenes,
#                              and run shape-cpu's `make test` (before anything
#                              is timed).
#   run.sh time WORK           on the machine itself, with a container engine:
#                              run each engine in its own image, one after
#                              another, pinned to one CPU, recording the
#                              provenance and the machine's load around each;
#                              then the cross-check and the results files
#                              (report.py).
#
# Every engine is timed in the second phase, through the same container engine
# and on the same CPU, so that what was running beside each one is recorded the
# same way for all of them. The images mount WORK at the path it had when it
# was built (bench-build/WORKDIR), so the rpath bench-shape was linked with
# resolves inside them.
#
# Settings, from the environment:
#   SHAPE_BENCH_LIBS       the libraries to install, in suite/libraries.txt order
#   SHAPE_BENCH_ENGINES    the engines to time (default: shape-cpu blend2d skia);
#                          anything less leaves the report to report.py --reuse
#   SHAPE_BENCH_CPU        the CPU every engine is pinned to (default 8)
#   SHAPE_BENCH_CLI        the container command for inspecting images (docker)
#   GHOTI_CONTAINER_ENGINE the command that runs the images (default: SHAPE_BENCH_CLI)
#   SHAPE_BENCH_BUILD_IMAGE the image shape-cpu runs in (ghoti-build:gcc16)

set -eu

LIBS=${SHAPE_BENCH_LIBS:-cutil security compress unicode color image math shape-geometry shape-scene shape-cpu font}
BUILD_IMAGE=${SHAPE_BENCH_BUILD_IMAGE:-ghoti-build:gcc16}
BLEND2D_IMAGE=localhost/ghoti-shape-bench-blend2d:0.21.3-2
SKIA_IMAGE=localhost/ghoti-shape-bench-skia:m156-2
B=ws/suite/tools/shape-bench

# DejaVu Sans 2.37. The tarball's digest is this repository's own: computed
# from the GitHub release and from SourceForge, two unrelated distribution
# paths, which served the same bytes on 2026-10-20. The font's digest is of the
# file inside it.
FONT_URL1=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
FONT_URL2=https://downloads.sourceforge.net/project/dejavu/dejavu/2.37/dejavu-fonts-ttf-2.37.tar.bz2
FONT_TAR_SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
FONT_SHA256=7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954

die() {
  echo "shape-bench run.sh: $*" >&2
  exit 1
}

build() {
  jobs=${1:-4}
  PREFIX=${PREFIX:-$WORK/prefix}
  case "$PREFIX" in "$WORK"/*) ;; *) die "PREFIX ($PREFIX) is not inside WORK ($WORK)" ;; esac
  R=$WORK/results
  T=${TMPDIR:-/tmp}
  export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig:$PREFIX/lib/pkgconfig"
  cd "$WORK"
  mkdir -p "$PREFIX" "$R/logs" "$R/scenes" "$R/frames" "$R/raw" "$R/prov"
  for l in $LIBS; do
    echo "== install $l"
    # PREFIX on every make: the rpath is added only when it is set.
    make -C "ws/libs/$l" -j"$jobs" install PREFIX="$PREFIX" > "$R/logs/install-$l.log" 2>&1 ||
      { tail -n 40 "$R/logs/install-$l.log"; die "installing $l failed"; }
  done
  echo "== build the harness"
  make -C "$B" PREFIX="$PREFIX" BUILD_DIR="$WORK/bench-build" > "$R/logs/bench-build.log" 2>&1 ||
    { cat "$R/logs/bench-build.log"; die "building the harness failed"; }
  echo "$WORK" > "$WORK/bench-build/WORKDIR"

  echo "== fetch the font"
  t="$T/dejavu.tar.bz2"
  curl -fsSL --max-time 300 -o "$t" "$FONT_URL1" || curl -fsSL --max-time 300 -o "$t" "$FONT_URL2" ||
    die "cannot fetch the font"
  printf '%s  %s\n' "$FONT_TAR_SHA256" "$t" | sha256sum -c - > /dev/null || die "the font tarball's digest moved"
  tar -xjf "$t" -C "$T" dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf
  cp "$T/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" "$R/scenes/DejaVuSans.ttf"
  printf '%s  %s\n' "$FONT_SHA256" "$R/scenes/DejaVuSans.ttf" | sha256sum -c - > /dev/null ||
    die "the font's digest moved"

  echo "== write the scenes"
  "$WORK/bench-build/bench-shape" emit "$R/scenes" "$R/scenes/DejaVuSans.ttf" 2> "$R/logs/emit.log" ||
    { cat "$R/logs/emit.log"; die "writing the scenes failed"; }
  cat "$R/logs/emit.log"

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
  } > "$R/prov/shape-cpu.build.txt"

  echo "== shape-cpu make test"
  if make -C ws/libs/shape-cpu -j"$jobs" test PREFIX="$PREFIX" > "$R/logs/shape-cpu-test.log" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  echo "$rc" > "$R/logs/shape-cpu-test.exit"
  tail -n 15 "$R/logs/shape-cpu-test.log"
  [ "$rc" -eq 0 ] || die "shape-cpu make test failed"
}

# The machine's state, as text the report keeps beside the numbers: the load
# average, how many other containers ran, and the CPU shares of the busiest
# processes from top's one-shot sample (what is using a CPU now, not ps's
# lifetime average). Never their names, IDs or owners, nor the machine's name.
state() {
  echo "time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "loadavg: $(cat /proc/loadavg)"
  echo "other containers running: $("$CLI" ps -q 2> /dev/null | wc -l)"
  echo "busiest processes' %CPU: $(top -b -n 2 -d 1 -o %CPU 2> /dev/null | awk '/^top -/ {n++} n == 2' |
    sed -n '8,13p' | awk '{ printf "%s%s", (NR > 1 ? " " : ""), $9 }')"
}

run_engine() {
  name=$1 image=$2 cmd=$3
  info="$R/prov/$name.txt"
  "$CLI" image inspect "$image" > /dev/null 2>&1 || die "no image $image here (README.md says how to build it)"
  {
    echo "engine: $name"
    echo "image: $image"
    echo "image_id: $("$CLI" image inspect --format '{{.Id}}' "$image")"
    echo "cpu_model: $(grep -m 1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "arch: $(uname -m)"
    echo "cpus_online: $(nproc --all)"
    echo "governor: $(cat "/sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor" 2> /dev/null || echo unknown)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2> /dev/null || echo unknown)"
    echo "pinned_cpu: $CPU"
    echo "nice: the container runs at the default priority, its only competition what the busiest processes show"
    echo "before:"
    state | sed 's/^/  /'
  } > "$info"
  echo "== $name"
  "$ENGINE" run --rm --cpuset-cpus="$CPU" --user "$(id -u):$(id -g)" -v "$WORK:$MNT" -w "$MNT" "$image" \
    sh -c "$cmd" > "$R/raw/$name.jsonl" 2> "$R/logs/$name.err" ||
    { cat "$R/logs/$name.err"; die "$name failed"; }
  {
    echo "after:"
    state | sed 's/^/  /'
  } >> "$info"
  cat "$R/logs/$name.err"
}

timing() {
  R=$WORK/results
  cd "$WORK"
  [ -f bench-build/WORKDIR ] || die "no $WORK/bench-build/WORKDIR; run the build phase first"
  MNT=$(cat bench-build/WORKDIR)
  CLI=${SHAPE_BENCH_CLI:-docker}
  ENGINE=${GHOTI_CONTAINER_ENGINE:-$CLI}
  CPU=${SHAPE_BENCH_CPU:-8}
  ENGINES=${SHAPE_BENCH_ENGINES:-shape-cpu blend2d skia}
  want() {
    case " $ENGINES " in *" $1 "*) return 0 ;; esac
    return 1
  }
  base=$(sed -n 's/^FROM \([^ ]*\).*/\1/p' ws/suite/Containerfile.build | head -n 1)

  if want shape-cpu; then
    run_engine shape-cpu "$BUILD_IMAGE" \
      "echo \"compiler: \$(gcc --version | head -n 1)\" >&2; bench-build/bench-shape run results/scenes results/frames"
    echo "base: $base" >> "$R/prov/shape-cpu.txt"
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

  # The libraries' commits, for the record: SHIPPED when whoever laid out WORK
  # wrote one (each line "<lib> <commit> clean|dirty"), else read from git.
  if [ ! -f SHIPPED ]; then
    for l in $LIBS; do
      c=$(git -C "ws/libs/$l" rev-parse --short HEAD 2> /dev/null || echo unknown)
      d=clean
      [ -z "$(git -C "ws/libs/$l" status --porcelain --untracked-files=no 2> /dev/null)" ] || d=dirty
      echo "$l $c $d"
    done > SHIPPED
  fi

  if [ "$ENGINES" != "shape-cpu blend2d skia" ]; then
    echo "== report: not made here (engines: $ENGINES); run report.py --reuse over an earlier run's results"
    return 0
  fi
  echo "== report"
  "$ENGINE" run --rm --user "$(id -u):$(id -g)" -v "$WORK:$MNT" -w "$MNT" "$BUILD_IMAGE" \
    python3 "$B/report.py" --results results --pamdiff bench-build/pamdiff --corroborate bench-build/corroborate --shipped SHIPPED
}

[ $# -ge 2 ] || die "usage: run.sh build WORK [JOBS] | run.sh time WORK"
WORK=$(cd "$2" && pwd)
case "$1" in
build) build "${3:-}" ;;
time) timing ;;
*) die "usage: run.sh build WORK [JOBS] | run.sh time WORK" ;;
esac
