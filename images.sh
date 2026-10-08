#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# List the local images this suite built. --apply removes those references.
#
# The name is the last slash-separated component of the repository. It
# matches only when it starts with ghoti-. A tag of <none> does not match.
# Removal is rmi of that reference, never of the image id, and never with
# --force. A fixed string after --apply is matched with grep -F and narrows
# the set; the prefix test still applies, so python or debian removes
# nothing.
#
# Podman is used when it is installed, otherwise Docker. The same engine
# lists and removes.
#
# Usage:
#   ./images.sh
#   ./images.sh --apply
#   ./images.sh --apply ghoti-docs

set -eu

apply=0
narrow=

if [ "$#" -gt 2 ]; then
  printf 'images.sh: unknown argument %s.\n' "$3" >&2
  exit 1
fi

if [ "$#" -ge 1 ]; then
  case "$1" in
    --apply)
      apply=1
      ;;
    *)
      printf 'images.sh: unknown argument %s.\n' "$1" >&2
      exit 1
      ;;
  esac
fi

if [ "$#" -eq 2 ]; then
  case "$2" in
    --*)
      printf 'images.sh: unknown argument %s.\n' "$2" >&2
      exit 1
      ;;
    *)
      narrow=$2
      ;;
  esac
fi

if command -v podman >/dev/null 2>&1; then
  engine=podman
elif command -v docker >/dev/null 2>&1; then
  engine=docker
else
  printf 'images.sh: podman or docker is required.\n' >&2
  exit 1
fi

# {{.Repository}}:{{.Tag}}. The tag is the field after the last colon, so a
# registry port's colon stays in the repository. The last slash-separated
# component of that repository is the name.
list=$("$engine" images --format '{{.Repository}}:{{.Tag}}') || exit 1
ours=$(printf '%s\n' "$list" | while IFS= read -r ref; do
  [ -n "$ref" ] || continue
  tag=${ref##*:}
  repo=${ref%:*}
  name=${repo##*/}
  [ "$tag" != '<none>' ] || continue
  case "$name" in
    ghoti-*) printf '%s\n' "$ref" ;;
  esac
done)

if [ -n "$narrow" ]; then
  ours=$(printf '%s\n' "$ours" | grep -F -e "$narrow" || true)
fi

if [ -z "$ours" ]; then
  exit 0
fi

if [ "$apply" -eq 0 ]; then
  printf '%s\n' "$ours"
  exit 0
fi

# The loop stays in this shell, so a refused rmi exits 1 here. There is
# no second attempt, and no --force.
set -f
old_ifs=$IFS
IFS='
'
for ref in $ours; do
  [ -n "$ref" ] || continue
  tag=${ref##*:}
  repo=${ref%:*}
  name=${repo##*/}
  case "$name" in
    ghoti-*) ;;
    *)
      printf 'images.sh: refusing %s.\n' "$ref" >&2
      exit 1
      ;;
  esac
  if [ "$tag" = '<none>' ]; then
    printf 'images.sh: refusing %s.\n' "$ref" >&2
    exit 1
  fi
  "$engine" rmi "$ref" || exit 1
done
IFS=$old_ifs
set +f
