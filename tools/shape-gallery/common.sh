# SPDX-License-Identifier: LGPL-3.0-only
#
# What run-evo.sh and run-local.sh share, sourced by both (not run on its own).
# The caller sets ROOT (the workspace: libs/, suite/, tools/) and HERE (this
# directory).
#
# The gallery is built from committed trees only.  stage() clones each library
# it needs, and suite, at that checkout's HEAD into a scratch directory under
# runs/, so an uncommitted edit in any checkout - another session's experiment,
# say - never reaches the pictures, and every run's manifest names commits that
# exist.

RUNS=${SHAPE_GALLERY_RUNS:-$HERE/runs}
IMAGE=ghoti-build:gcc16

die() {
  echo "shape-gallery: $*" >&2
  exit 1
}

# The libraries the gallery links, and everything they need, in
# suite/libraries.txt order: the closure of font's and shape-cpu's non-optional
# dependencies (a "?" dependency is optional and left out).
gallery_libs() {
  awk '
    /^[ \t]*(#|$)/ { next }
    { order[++n] = $1; deps[$1] = $4 }
    END {
      need["font"] = 1; need["shape-cpu"] = 1
      for (i = n; i >= 1; i--) {
        l = order[i]
        if (!(l in need) || deps[l] == "-") continue
        k = split(deps[l], d, ",")
        for (j = 1; j <= k; j++) if (d[j] !~ /^\?/) need[d[j]] = 1
      }
      for (i = 1; i <= n; i++) if (order[i] in need) printf "%s ", order[i]
    }' "$1"
}

# stage DIR: DIR/libs/<lib> and DIR/suite, each a clone of the checkout's HEAD
# (the committed tree; the clone's .git is never shipped).  Sets LIBS.  The
# pinned font is copied in when this machine already has it; render.sh checks
# its digest either way and fetches it when it is absent.
stage() {
  s=$1
  mkdir -p "$s/libs"
  git clone -q --shared "$ROOT/suite" "$s/suite" || die "cannot clone suite"
  git -C "$s/suite" checkout -q --detach "$(git -C "$ROOT/suite" rev-parse HEAD)" || die "cannot check out suite at HEAD"
  LIBS=$(gallery_libs "$s/suite/libraries.txt")
  [ -n "$LIBS" ] || die "no libraries read from suite/libraries.txt"
  for l in $LIBS; do
    [ -d "$ROOT/libs/$l/.git" ] || die "no checkout at libs/$l"
    git clone -q --shared "$ROOT/libs/$l" "$s/libs/$l" || die "cannot clone libs/$l"
    # A clone checks out the origin's current branch; make it the origin's HEAD exactly.
    git -C "$s/libs/$l" checkout -q --detach "$(git -C "$ROOT/libs/$l" rev-parse HEAD)" ||
      die "cannot check out libs/$l at HEAD"
  done
  [ -f "$s/suite/tools/shape-gallery/gallery.c" ] ||
    die "suite's HEAD has no tools/shape-gallery/gallery.c; commit the tool first"
  if [ -f "$ROOT/suite/tools/shape-success/data/DejaVuSans.ttf" ]; then
    mkdir -p "$s/suite/tools/shape-success/data"
    cp "$ROOT/suite/tools/shape-success/data/DejaVuSans.ttf" "$s/suite/tools/shape-success/data/"
  fi
}

# The run's directory name: the UTC time it started and the short commits of
# the four libraries whose changes the pictures are about.
run_name() {
  h=
  for l in shape-geometry shape-scene shape-cpu font; do
    h="$h${h:+-}$(git -C "$1/libs/$l" rev-parse --short=7 HEAD)"
  done
  echo "$(date -u +%Y%m%dT%H%M%SZ)_$h"
}

# Warn about what was left out, so a run is never mistaken for the working tree.
warn_dirty() {
  for l in $LIBS; do
    [ -z "$(git -C "$ROOT/libs/$l" status --porcelain --untracked-files=no)" ] ||
      echo "shape-gallery: note: libs/$l has uncommitted changes; the run uses its HEAD only" >&2
  done
  [ -z "$(git -C "$ROOT/suite" status --porcelain --untracked-files=no -- tools/shape-gallery)" ] ||
    echo "shape-gallery: note: suite/tools/shape-gallery has uncommitted changes; the run uses suite's HEAD only" >&2
}

# write_manifest STAGE RUNDIR WHERE: RUNDIR/manifest.txt from the stage's
# commits and what render.sh and the host step left in RUNDIR (build.txt,
# host.txt, decode-check.txt, gallery.exit), which it folds in.
write_manifest() {
  s=$1 r=$2 where=$3
  {
    echo "# shape gallery run; compare.py reads the lib: lines"
    echo "date: $START"
    echo "where: $where"
    [ -f "$r/host.txt" ] && cat "$r/host.txt"
    [ -f "$r/build.txt" ] && cat "$r/build.txt"
    echo "image_base: $(sed -n 's/^FROM \([^ ]*\).*/\1/p' "$s/suite/Containerfile.build" | head -n 1)"
    echo "suite: $(git -C "$s/suite" rev-parse HEAD) (gallery.c, render.sh, libraries.txt)"
    for l in $LIBS; do
      echo "lib: $l $(git -C "$s/libs/$l" rev-parse HEAD) $(git -C "$s/libs/$l" log -1 --format=%cI)"
    done
    echo "gallery_exit: $(cat "$r/gallery.exit" 2> /dev/null || echo 'not run')"
    if [ -f "$r/decode-check.txt" ]; then
      echo "decode_check: $(tail -n 1 "$r/decode-check.txt")"
    else
      echo "decode_check: not run"
    fi
  } > "$r/manifest.txt"
  rm -f "$r/host.txt" "$r/build.txt" "$r/gallery.exit"
}
