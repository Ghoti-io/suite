#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Build and install every library in libraries.txt, in dependency order.
#
# The default prefix is the sibling .local/ directory. Nothing there needs
# root, and pkg-config is pointed at it for the rest of the run. A bare
# run builds ghoti-build:gcc16 and re-execs inside it, with this tree mounted
# at /work. --no-container compiles with the host compiler and does not build
# the image. --global compiles in the image and installs to the host
# /usr/local; the host runs ldconfig after the container exits.
#
# uninstall removes the same install, walking the manifest from the leaf
# back to the root. It runs on the host and does not build the image. It
# takes the same --global flag.
#
# Any other argument is passed to make, so a debug build is:
#
#     ./install.sh BUILD=debug
#
# --test runs `make test` in each library once all of them are installed, so
# one command builds, installs and tests the whole set; a library's tests may
# need a library after it (regex's need text), which is why the tests wait for
# the whole install. Any tools/*/fetch.sh a library has runs first, since its
# tests may need the data it fetches. --test=a,b
# tests only the named libraries (all are still built and installed). A
# failure stops the run and names the log, .bootstrap-<library>.log in the
# parent directory.
#
# Usage:
#   ./install.sh
#   ./install.sh --test
#   ./install.sh --test=runtime-core,lang-tang
#   ./install.sh --no-container
#   ./install.sh --global
#   ./install.sh uninstall
#   ./install.sh uninstall --global

set -e

SUITE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SUITE/.." && pwd)
PREFIX="$ROOT/.local"
LIBS="$ROOT/libs"
MANIFEST="$SUITE/libraries.txt"
GLOBAL=0
NO_CONTAINER=0
ACTION=install
TEST=""

if [ ! -f "$MANIFEST" ]; then
  echo "install.sh: no libraries.txt beside this script" >&2
  exit 1
fi

make_args=""
for arg in "$@"; do
  case "$arg" in
    --global) GLOBAL=1 ;;
    --no-container) NO_CONTAINER=1 ;;
    uninstall) ACTION=uninstall ;;
    --test) TEST=all ;;
    --test=*) TEST=",${arg#--test=}," ;;
    PREFIX=*)
      PREFIX=${arg#PREFIX=}
      ;;
    *)
      quoted=$(printf '%s' "$arg" | sed "s/'/'\\\\''/g; s/^/'/; s/$/'/")
      make_args="$make_args $quoted"
      ;;
  esac
done

# --global is the system prefix. A PREFIX= beside it used to be forwarded
# to make and is now parsed out, so it would be ignored. Say so.
if [ "$GLOBAL" -eq 1 ]; then
  for arg in "$@"; do
    case "$arg" in
      PREFIX=*)
        echo "install.sh: PREFIX= cannot be combined with --global" >&2
        exit 1
        ;;
    esac
  done
fi

# The marker is set by the outer run. The inner script is the install loop
# below and must not build the image again. uninstall only deletes files.
# --no-container is the host compiler.
if [ -z "${GHOTI_BUILD_CONTAINER:-}" ] && [ "$ACTION" != uninstall ] && [ "$NO_CONTAINER" -eq 0 ]; then
  if command -v podman >/dev/null 2>&1; then
    run=podman
  elif command -v docker >/dev/null 2>&1; then
    run=docker
  else
    echo "install.sh: podman or docker is required" >&2
    exit 1
  fi

  image=ghoti-build:gcc16
  # A prefix outside the parent is not on the /work mount, so the container
  # would write it into a layer --rm throws away. Rewrite a prefix under
  # the parent to the path it has inside the container.
  if [ "$GLOBAL" -eq 0 ]; then
    # realpath -m resolves ".." and a relative prefix. "$ROOT"/* also
    # matches "$ROOT/../tmp", which is not on the /work mount, so the
    # container would write it into a layer --rm throws away.
    root_real=$(realpath -m "$ROOT")
    pref_real=$(realpath -m "$PREFIX")
    case "$pref_real" in
      "$root_real"/*) ;;
      *)
        echo "install.sh: PREFIX must be under $ROOT so the container can write it" >&2
        exit 1
        ;;
    esac
    rel=${pref_real#"$root_real"/}
    newpref="/work/$rel"
    replaced=0
    for arg in "$@"; do
      case "$arg" in
        PREFIX=*)
          arg="PREFIX=$newpref"
          replaced=1
          ;;
      esac
      set -- "$@" "$arg"
      shift
    done
    # The default prefix is already /work/.local once the script is the
    # copy inside the container. Any other prefix has to be passed in.
    if [ "$replaced" -eq 0 ] && [ "$pref_real" != "$root_real/.local" ]; then
      set -- "$@" "PREFIX=$newpref"
    fi
  fi

  if [ "$GLOBAL" -eq 1 ]; then
    # The compiler in the official image lives in /usr/local. Mounting the
    # whole host /usr/local over it would hide gcc. The libraries land in
    # these three directories and in the host ld.so.conf.d; the host
    # ldconfig after exit is what records them. Rootless podman cannot
    # write the host system prefix, so that engine is invoked as root.
    priv=
    if [ "$run" = podman ] && [ "$("$run" info --format '{{.Host.Security.Rootless}}')" = true ]; then
      priv=sudo
    fi
    # Build with the invoking user's engine. Root's store is separate, and
    # a `sudo ./install.sh` would otherwise rebuild there with no cache.
    # The run below is the privileged one, so copy the image across when
    # those stores differ.
    as_user() {
      if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
        uid=$(id -u "$SUDO_USER")
        if [ -d "/run/user/$uid" ]; then
          sudo -u "$SUDO_USER" -H env XDG_RUNTIME_DIR="/run/user/$uid" "$run" "$@"
        else
          sudo -u "$SUDO_USER" -H -- "$run" "$@"
        fi
      else
        "$run" "$@"
      fi
    }
    as_priv() {
      if [ -n "$priv" ]; then
        # shellcheck disable=SC2086
        $priv "$run" "$@"
      else
        "$run" "$@"
      fi
    }
    sudo mkdir -p /usr/local/lib/ghoti.io /usr/local/include/ghoti.io /usr/local/share/pkgconfig
    as_user build -t "$image" -f "$SUITE/Containerfile.build" "$SUITE"
    if [ -n "$priv" ] || { [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; }; then
      src=$(as_user image inspect --format '{{.Id}}' "$image")
      dest=$(as_priv image inspect --format '{{.Id}}' "$image" 2>/dev/null || true)
      if [ "$src" != "$dest" ]; then
        tar=$(mktemp)
        # mktemp runs as whoever invoked the script. `sudo ./install.sh`
        # creates a root-owned 0600 file, and the save below runs as the
        # invoking user, who cannot open it.
        if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
          chown "$SUDO_USER" "$tar"
        fi
        as_user save -o "$tar" "$image" || { rm -f "$tar"; exit 1; }
        as_priv load -i "$tar" || { rm -f "$tar"; exit 1; }
        rm -f "$tar"
      fi
    fi
    # The container is root so it can write the host prefix. Give the
    # tree back to the invoking user afterwards, including when the
    # install fails, so a later unprivileged build can overwrite it.
    set +e
    as_priv run --rm \
      -v "$ROOT":/work \
      -v /usr/local/lib/ghoti.io:/usr/local/lib/ghoti.io \
      -v /usr/local/include/ghoti.io:/usr/local/include/ghoti.io \
      -v /usr/local/share/pkgconfig:/usr/local/share/pkgconfig \
      -v /etc/ld.so.conf.d:/etc/ld.so.conf.d \
      -w /work/suite \
      -e GHOTI_BUILD_CONTAINER=1 \
      -e GHOTI_HOST_ROOT="$ROOT" \
      "$image" ./install.sh "$@"
    run_rc=$?
    set -e
    own_uid=$(id -u)
    own_gid=$(id -g)
    if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
      own_uid=$(id -u "$SUDO_USER")
      own_gid=$(id -g "$SUDO_USER")
    fi
    sudo chown -R "$own_uid:$own_gid" "$LIBS"
    for logf in "$ROOT"/.bootstrap-*.log; do
      [ -e "$logf" ] || continue
      sudo chown "$own_uid:$own_gid" "$logf"
    done
    if [ "$run_rc" -ne 0 ]; then
      exit "$run_rc"
    fi
    sudo ldconfig
    exit 0
  fi

  own_uid=$(id -u)
  own_gid=$(id -g)
  if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
    own_uid=$(id -u "$SUDO_USER")
    own_gid=$(id -g "$SUDO_USER")
  fi

  "$run" build -t "$image" -f "$SUITE/Containerfile.build" "$SUITE"
  if [ "$run" = podman ]; then
    # Rootless podman maps the host user to container root. --user alone
    # is a different uid and cannot write the mount. keep-id makes the
    # process the invoking user. A sudo'd script is root's podman, which
    # has no such mapping: --user is the human who invoked sudo.
    if [ "$(id -u)" -ne 0 ]; then
      exec "$run" run --rm --userns=keep-id --user "$own_uid:$own_gid" \
        -v "$ROOT":/work -w /work/suite \
        -e HOME=/tmp \
        -e GHOTI_BUILD_CONTAINER=1 \
        -e GHOTI_HOST_ROOT="$ROOT" \
        "$image" ./install.sh "$@"
    fi
  fi
  exec "$run" run --rm --user "$own_uid:$own_gid" \
    -v "$ROOT":/work -w /work/suite \
    -e HOME=/tmp \
    -e GHOTI_BUILD_CONTAINER=1 \
    -e GHOTI_HOST_ROOT="$ROOT" \
    "$image" ./install.sh "$@"
fi

ORDER=$(awk '!/^[[:space:]]*#/ && NF >= 4 { print $1 }' "$MANIFEST")

seen=" "
for repo in $ORDER; do
  deps=$(awk -v r="$repo" '!/^[[:space:]]*#/ && $1 == r { print $4 }' "$MANIFEST")
  [ "$deps" = "-" ] && { seen="$seen$repo "; continue; }
  for dep in $(echo "$deps" | tr ',' ' '); do
    case "$dep" in \?*) continue ;; esac
    case "$seen" in
      *" $dep "*) ;;
      *)
        echo "install.sh: libraries.txt lists '$repo' before its dependency '$dep'" >&2
        exit 1
        ;;
    esac
  done
  seen="$seen$repo "
done

if [ "$ACTION" = uninstall ]; then
  walk=""
  for repo in $ORDER; do
    walk="$repo $walk"
  done
else
  walk=$ORDER
fi

jobs=$(nproc 2>/dev/null || echo 4)

if [ "$GLOBAL" -eq 1 ]; then
  if [ -n "${GHOTI_BUILD_CONTAINER:-}" ]; then
    echo "Prefix: system"
  else
    echo "Prefix: system (sudo make $ACTION)"
  fi
else
  echo "Prefix: $PREFIX"
  if [ "$ACTION" = install ]; then
    export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig"
    mkdir -p "$PKG_CONFIG_PATH"
  fi
fi

if [ -n "${GHOTI_BUILD_CONTAINER:-}" ] && [ "$ACTION" = install ]; then
  gcc --version
fi

for repo in $walk; do
  if [ ! -f "$LIBS/$repo/Makefile" ]; then
    echo "  $repo: not present, skipping"
    continue
  fi
  echo "  $repo"
  log="$ROOT/.bootstrap-$repo.log"
  shown=$log
  if [ -n "${GHOTI_HOST_ROOT:-}" ]; then
    shown="${GHOTI_HOST_ROOT}/.bootstrap-$repo.log"
  fi
  if [ "$GLOBAL" -eq 1 ]; then
    # Inside the image the process is already root, and sudo is not
    # installed there. The host ldconfig runs after the container exits,
    # so this make's ldconfig is not the one that records the host.
    if [ -n "${GHOTI_BUILD_CONTAINER:-}" ]; then
      run_make="make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $ACTION $make_args"
    else
      run_make="sudo make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $ACTION $make_args"
    fi
    # shellcheck disable=SC2086
    eval "$run_make" >"$log" 2>&1 \
      || { echo "install.sh: $repo failed; see $shown" >&2; exit 1; }
  else
    # shellcheck disable=SC2086
    eval "make -C \"\$LIBS/\$repo\" -j\"\$jobs\" PREFIX=\"\$PREFIX\" $ACTION $make_args" >"$log" 2>&1 \
      || { echo "install.sh: $repo failed; see $shown" >&2; exit 1; }
  fi
done

# The tests run after everything is installed, not one by one as each library
# lands: regex lists text as an optional dependency and text depends on regex,
# so regex's own tests (the JSON Schema adapter gate) need text installed, which
# a test run right after regex's install cannot give them.
if [ "$ACTION" = install ] && [ -n "$TEST" ]; then
  for repo in $ORDER; do
    [ -f "$LIBS/$repo/Makefile" ] || continue
    case "$TEST" in
      all) ;;
      *",$repo,"*) ;;
      *) continue ;;
    esac
    log="$ROOT/.bootstrap-$repo.log"
    shown=$log
    if [ -n "${GHOTI_HOST_ROOT:-}" ]; then
      shown="${GHOTI_HOST_ROOT}/.bootstrap-$repo.log"
    fi
    # A library's tests can measure against somebody else's data (the Unicode
    # Character Database, the JSON Schema test suite), which is pinned and
    # fetched, never committed. A gate that cannot find it fails rather than
    # skips, so a fresh clone fetches it first; the scripts keep what they
    # have already fetched.
    for fetch in "$LIBS/$repo"/tools/*/fetch.sh; do
      [ -f "$fetch" ] || continue
      echo "  $repo: ${fetch#"$LIBS/$repo/"}"
      sh "$fetch" >>"$log" 2>&1 \
        || { echo "install.sh: $repo: $fetch failed; see $shown" >&2; exit 1; }
    done
    echo "  $repo: make test"
    if [ "$GLOBAL" -eq 1 ]; then
      prefix_arg=""
    else
      prefix_arg="PREFIX=\"\$PREFIX\""
    fi
    # shellcheck disable=SC2086
    eval "make -C \"\$LIBS/\$repo\" -j\"\$jobs\" $prefix_arg test $make_args" >>"$log" 2>&1 \
      || { echo "install.sh: $repo: make test failed; see $shown" >&2; exit 1; }
  done
fi

echo "Done."
