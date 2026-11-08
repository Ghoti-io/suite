#!/bin/bash
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# The defects that would let the Defiant demo's changes go unseen, planted one
# at a time in a copy of defiant-core, each required to fail the demo at the
# step of its own change, by name.
#
# The demo (libs/defiant-web/tools/demo.sh) changes the site's title, a menu
# entry, a template and a widget through the admin forms while requests keep
# arriving, and each change has to show on the very next request. Each defect
# below makes one part of the core read once and keep what it read:
#
#   templates-compiled-once    a template's body is taken the first time it is
#                              rendered and kept            FAIL 9, the edit
#   title-read-once            the site name is read once   FAIL 7, the title
#   menu-built-once            the admin menu is drawn once FAIL 8, the menu
#   widget-order-read-once     the front page's panes are read once
#                                                           FAIL 10, the widget
#
# The control comes first: a copy of the core with nothing planted passes the
# demo. Each plant is an exact text edit of the copy (an anchor that does not
# match exactly once, or an edit that changes nothing, stops the run), the copy's
# library is built, and the demo runs with it ahead of the installed one.
#
# This needs the sources of defiant-core and defiant-web side by side, as the
# manifest lays them out (libs/ beside suite/), and an installed prefix holding
# the libraries they depend on. A source that is missing fails the run; this is
# a gate and does not skip.
#
# Usage: tools/check-demo-plants.sh PREFIX      (PREFIX: where the libraries are installed)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PREFIX=${1:-}
CORE=$ROOT/libs/defiant-core
WEB=$ROOT/libs/defiant-web
RENDER=src/render/render.c
TAG=check-demo-plants

die() { echo "$TAG: $*" >&2; exit 1; }

[ -n "$PREFIX" ] || die "usage: check-demo-plants.sh PREFIX"
[ -f "$CORE/$RENDER" ] || die "no defiant-core source at $CORE, and this gate needs it"
[ -f "$WEB/tools/demo.sh" ] || die "no defiant-web source at $WEB, and this gate needs it"
command -v python3 > /dev/null || die "python3 is needed"
command -v cmp > /dev/null || die "cmp is needed"
export PKG_CONFIG_PATH="$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
JOBS=${GHOTI_CHECK_JOBS:-2}

SCRATCH=$(mktemp -d) || exit 1
trap 'rm -rf "$SCRATCH"' EXIT

# The program and the demo's helper, built as the library's own `make demo` does.
make -C "$WEB" PREFIX="$PREFIX" -j"$JOBS" build/linux/release/apps/defiant \
  build/linux/release/apps/defiant-demo > "$SCRATCH/web.log" 2>&1 || {
  tail -20 "$SCRATCH/web.log" >&2
  die "defiant-web did not build"
}
APP=$WEB/build/linux/release/apps
LDP="$PREFIX/lib/ghoti.io"

# core_copy <dir>: the core source, with its build output if it has any, so
# that only what a plant edits is compiled again.
core_copy() {
  mkdir -p "$1"
  for d in src include tools pkgconfig documentation build; do
    [ -e "$CORE/$d" ] && cp -a "$CORE/$d" "$1/"
  done
  cp -a "$CORE/Makefile" "$1/"
  [ -e "$CORE/Doxyfile" ] && cp -a "$CORE/Doxyfile" "$1/"
  return 0
}

# core_library <dir> <log>: build the copy's library; print its directory.
core_library() {
  make -C "$1" --no-print-directory -j"$JOBS" all PREFIX="$PREFIX" > "$2" 2>&1 || return 1
  so=$(find "$1/build" -name 'libghoti.io-defiant-core-*.so*' 2> /dev/null | head -1)
  [ -n "$so" ] || return 1
  dirname "$so"
}

run_demo() { # <library dir>
  LD_LIBRARY_PATH="$1:$APP:$LDP" APP_DIR="$APP" "$WEB/tools/demo.sh" 2>&1
}

failed=0

# the control
dir=$SCRATCH/control
core_copy "$dir"
lib=$(core_library "$dir" "$dir.log") || { tail -20 "$dir.log" >&2; die "the unplanted copy of defiant-core did not build"; }
out=$(run_demo "$lib"); rc=$?
if [ $rc -ne 0 ]; then
  printf '%s\n' "$out" | tail -15 >&2
  die "the CONTROL (an unplanted copy of defiant-core) fails the demo, so no plant would mean anything"
fi
echo "$TAG: control passes (the demo against an unplanted copy of defiant-core)"

# demo_plant <name> <step> <needle> <old> <new> [<old> <new>...]
demo_plant() {
  name=$1 step=$2 needle=$3
  shift 3
  dir=$SCRATCH/$name
  core_copy "$dir"
  if ! python3 - "$dir/$RENDER" "$@" <<'PY'
import sys
path, pairs = sys.argv[1], sys.argv[2:]
text = open(path).read()
for i in range(0, len(pairs), 2):
    old, new = pairs[i], pairs[i + 1]
    n = text.count(old)
    if n != 1:
        sys.stderr.write("anchor matched %d times: %r\n" % (n, old))
        sys.exit(3)
    if old == new:
        sys.stderr.write("an edit that changes nothing\n")
        sys.exit(3)
    text = text.replace(old, new, 1)
open(path, "w").write(text)
PY
  then
    echo "$TAG: $name: THE EDIT DID NOT APPLY, nothing was tested" >&2
    failed=1
    return
  fi
  if cmp -s "$CORE/$RENDER" "$dir/$RENDER"; then
    echo "$TAG: $name: the edited file equals the original" >&2
    failed=1
    return
  fi
  touch "$dir/$RENDER"
  lib=$(core_library "$dir" "$dir.log") || {
    echo "$TAG: $name: the planted copy did not build, which is not the demo catching it:" >&2
    grep 'error' "$dir.log" | head -10 >&2
    failed=1
    return
  }
  out=$(run_demo "$lib"); rc=$?
  if [ $rc -ne 0 ] && printf '%s\n' "$out" | grep -q "^FAIL $step .*$needle"; then
    echo "$TAG: $name: the demo failed at step $step, by name:"
    printf '%s\n' "$out" | grep '^FAIL' | sed "s/^/$TAG:   /"
  else
    echo "$TAG: $name: the demo did not fail at step $step (exit $rc):" >&2
    printf '%s\n' "$out" | tail -12 >&2
    failed=1
  fi
}

demo_plant templates-compiled-once 9 'edit' \
  '    rc = tang_run(catalog, request, contract, body, body_len, fuel, binds, nbinds,
        native, native_user, native_name, html, html_len);' \
  '    static char frozen_name[16][64];
    static char * frozen_body[16];
    static size_t frozen_len[16];
    const char * use = body;
    size_t use_len = body_len;
    size_t slot;
    for (slot = 0; slot < 16 && frozen_body[slot] != NULL; slot++) {
      if (strlen(frozen_name[slot]) == tmpl_len &&
          memcmp(frozen_name[slot], tmpl, tmpl_len) == 0) {
        break;
      }
    }
    if (slot < 16 && frozen_body[slot] == NULL && tmpl_len < 64) {
      frozen_body[slot] = (char *)malloc(body_len + 1);
      if (frozen_body[slot] != NULL) {
        memcpy(frozen_body[slot], body, body_len);
        memcpy(frozen_name[slot], tmpl, tmpl_len);
        frozen_name[slot][tmpl_len] = 0;
        frozen_len[slot] = body_len;
      }
    }
    if (slot < 16 && frozen_body[slot] != NULL) {
      use = frozen_body[slot];
      use_len = frozen_len[slot];
    }
    rc = tang_run(catalog, request, contract, use, use_len, fuel, binds, nbinds,
        native, native_user, native_name, html, html_len);'

demo_plant title-read-once 7 'title' \
  '  bind[0].name = "name";
  bind[0].text = name == NULL ? "" : name;
  bind[0].len = name_len;' \
  '  static char frozen_name[256];
  static size_t frozen_len;
  static int frozen_have;
  if (!frozen_have && name != NULL && name_len < sizeof frozen_name) {
    memcpy(frozen_name, name, name_len);
    frozen_len = name_len;
    frozen_have = 1;
  }
  bind[0].name = "name";
  bind[0].text = frozen_have ? frozen_name : (name == NULL ? "" : name);
  bind[0].len = frozen_have ? frozen_len : name_len;'

demo_plant menu-built-once 8 'menu' \
  '  TangBind bind;
  GDEFCORE_Result rc;
  *html = NULL;
  *html_len = 0;
  if (which == 0) {' \
  '  TangBind bind;
  GDEFCORE_Result rc;
  static char * frozen_html;
  static size_t frozen_html_len;
  *html = NULL;
  *html_len = 0;
  if (which == 0) {' \
  '  rc = describe_menu(items, &value);
  if (rc == GDEFCORE_OK) {
    rc = gdefcore_template_contract_open(catalog, NULL, tkind, tkind_len,' \
  '  if (which == 1 && frozen_html != NULL) {
    rc = gdefcore_text_dup(catalog, frozen_html, frozen_html_len, html);
    if (rc == GDEFCORE_OK) {
      *html_len = frozen_html_len;
    }
    goto done;
  }
  rc = describe_menu(items, &value);
  if (rc == GDEFCORE_OK) {
    rc = gdefcore_template_contract_open(catalog, NULL, tkind, tkind_len,' \
  '    *html = bytes;
    *html_len = n;
  }
done:
  gdefcore_template_contract_close(contract);
  gltang_value_description_release(value);' \
  '    *html = bytes;
    *html_len = n;
    if (which == 1 && frozen_html == NULL) {
      frozen_html = (char *)malloc(n + 1);
      if (frozen_html != NULL) {
        memcpy(frozen_html, bytes, n);
        frozen_html_len = n;
      }
    }
  }
done:
  gdefcore_template_contract_close(contract);
  gltang_value_description_release(value);'

demo_plant widget-order-read-once 10 'gone' \
  '  for (i = 0; i < count; i++) {
    const char * region;
    const char * widget;
    const char * target;
    size_t region_len = 0;
    size_t widget_len = 0;
    size_t target_len = 0;
    char * html = NULL;
    size_t html_len = 0;
    if (!gdefcore_render_split_pane(panes[i], lens[i], &region, &region_len,' \
  '  static char * frozen_pane[RENDER_MAX_PANES];
  static size_t frozen_pane_len[RENDER_MAX_PANES];
  static size_t frozen_count;
  static int frozen_have;
  /* only the page the flow changes: it holds two panes at first (the site
   * name and the recent posts); the pages of the earlier steps hold one */
  int is_front = name_len == 5 && memcmp(name, "front", 5) == 0 &&
                 (frozen_have || count >= 2);
  if (is_front && !frozen_have) {
    for (i = 0; i < count; i++) {
      frozen_pane[i] = (char *)malloc(lens[i] + 1);
      if (frozen_pane[i] == NULL) {
        break;
      }
      memcpy(frozen_pane[i], panes[i], lens[i]);
      frozen_pane_len[i] = lens[i];
    }
    frozen_count = count;
    frozen_have = 1;
  }
  int use_frozen = is_front && frozen_have;
  for (i = 0; i < (use_frozen ? frozen_count : count); i++) {
    const char * region;
    const char * widget;
    const char * target;
    size_t region_len = 0;
    size_t widget_len = 0;
    size_t target_len = 0;
    char * html = NULL;
    size_t html_len = 0;
    if (!gdefcore_render_split_pane(use_frozen ? frozen_pane[i] : panes[i],
            use_frozen ? frozen_pane_len[i] : lens[i], &region, &region_len,'


if [ "$failed" != 0 ]; then
  exit 1
fi
echo "$TAG: every planted defect turned the demo red at its own step"
