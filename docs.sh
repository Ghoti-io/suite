#!/bin/sh
#
# Build the public manual.
#
# documents/ is the text written for someone using the libraries. This
# script measures the tree (line counts, the tests a built library lists,
# version pins declared in source, and each library's version from its
# Makefile), writes that page, folds each library README into its manual
# page, and runs Doxygen. Output is
# docs/html/index.html. docs/ is gitignored.
#
# Library READMEs are folded into pages by this script. They are not
# Doxygen inputs on their own.
#
# The left-hand tree has one Libraries node. Opening a library shows its
# README. manual/*.md lists that library's other pages, and this script
# writes manual/pages/ with the README folded in. The libraries are
# alphabetical under Libraries.
#
# Each library can still build its own manual with `make docs`.
#
# Usage:
#   ./docs.sh
#   ./docs.sh --container

set -u

# Paths come from this file, so the shell can be in any directory. The
# libraries and the generated manual are siblings of suite/, in the parent.
# --container rebuilds nothing it can reuse: the image is the toolchain,
# and the tree is mounted in.
SUITE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SUITE/.." && pwd)

if [ "${1:-}" = "--container" ]; then
  shift
  if command -v podman >/dev/null 2>&1; then
    run=podman
  elif command -v docker >/dev/null 2>&1; then
    run=docker
  else
    printf "docs.sh: podman or docker is required for --container.\n" >&2
    exit 1
  fi
  image=ghoti-io-docs
  # Build every time. An unchanged Containerfile is cached, and a change
  # has to take effect: an image left from an older file would rebuild
  # the manual with the wrong Doxygen.
  "$run" build -t "$image" -f "$SUITE/Containerfile" "$SUITE"
  exec "$run" run --rm -v "$ROOT":/work -w /work "$image" "$@"
fi

if ! command -v doxygen >/dev/null 2>&1; then
  printf "docs.sh: doxygen is not installed.\n" >&2
  exit 1
fi

if ! command -v cloc >/dev/null 2>&1; then
  printf "docs.sh: cloc is not installed.\n" >&2
  exit 1
fi

export SUITE ROOT
cd "$ROOT"

cleanup() {
  rm -rf "$SUITE/suite.md" "$SUITE/manual/pages"
}
trap cleanup EXIT

mkdir -p "$SUITE/manual/pages"

python3 - << 'PY'
import json, os, pathlib, re, subprocess

root = pathlib.Path(os.environ["ROOT"])
suite = pathlib.Path(os.environ["SUITE"])
order = []
for line in (suite / "libraries.txt").read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if line:
        order.append(line.split()[0])

names = {
    "cutil": "CUtil", "unicode": "Unicode", "chron": "Chron",
    "compress": "Compress", "text": "Text", "image": "Image",
    "font": "Font", "model": "Model", "regex": "Regex",
    "ctang": "CTang", "cjelly": "CJelly", "security": "Security",
    "color": "Color", "archive": "Archive",
}

exclude = {}
for line in (suite / "documents" / "measure.conf").read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    lib, path = line.split()
    exclude.setdefault(lib, []).append(path)

def cloc_code(paths):
    existing = [p for p in paths if pathlib.Path(p).exists()]
    if not existing:
        return 0
    proc = subprocess.run(
        ["cloc", "--quiet", "--json", *existing],
        check=True, capture_output=True, text=True)
    data = json.loads(proc.stdout)
    return int(data.get("SUM", {}).get("code", 0))

def cloc_recipe(lib):
    text = (root / "libs" / lib / "Makefile").read_text()
    match = re.search(r"(?m)^cloc:.*\n\tcloc (.+)$", text)
    if not match:
        raise SystemExit(f"docs.sh: {lib} has no cloc recipe")
    return match.group(1).split()

rows = []
for lib in order:
    base = root / "libs" / lib
    recipe = cloc_recipe(lib)
    excluded = exclude.get(lib, [])
    def inside_recipe(path):
        return any(path == item or path.startswith(item.rstrip("/") + "/")
                   for item in recipe if not item.startswith("-"))
    # The code column is `make cloc` minus the excluded paths that sit
    # inside that recipe. The generated column is every excluded path,
    # including ones `make cloc` never counted.
    whole = cloc_code([str(base / p) for p in recipe])
    generated = cloc_code([str(base / p) for p in excluded])
    overlap = cloc_code([str(base / p) for p in excluded if inside_recipe(p)])
    code = whole - overlap
    if code < 0:
        raise SystemExit(f"docs.sh: {lib} generated lines exceed the cloc recipe")
    rows.append((lib, code, generated))
n_libraries = len(rows)

def list_tests(lib):
    base = root / "libs" / lib
    apps_dirs = [p for p in base.glob("build/*/*/apps") if p.is_dir()]
    if not apps_dirs:
        return None
    apps = max(apps_dirs, key=lambda p: p.stat().st_mtime)
    lib_path = [str(apps)]
    lib_path += [str(p) for p in root.glob("libs/*/build/*/*/apps") if p.is_dir()]
    local = root / ".local" / "lib"
    if local.is_dir():
        lib_path.append(str(local))
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = ":".join(lib_path)
    total = 0
    found = False
    for binary in sorted(apps.iterdir()):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            continue
        if binary.suffix == ".so" or "fuzz" in binary.name:
            continue
        if not binary.name.startswith("test"):
            continue
        try:
            proc = subprocess.run(
                [str(binary), "--gtest_list_tests"],
                check=False, capture_output=True, text=True, timeout=15, env=env)
        except subprocess.TimeoutExpired:
            continue
        if proc.returncode != 0:
            continue
        cases = [ln for ln in proc.stdout.splitlines() if ln.startswith("  ") and ln.strip()]
        suites = [ln for ln in proc.stdout.splitlines() if ln.endswith(".") and not ln.startswith(" ")]
        if not suites and not cases:
            continue
        found = True
        total += len(cases)
    return total if found else None

test_counts = {lib: list_tests(lib) for lib, _, _ in rows}

def parse_images(path):
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 3:
            continue
        rows.append({
            "name": fields[0],
            "ref": fields[1],
            "version": fields[2],
            "answers": fields[3] if len(fields) > 3 else "",
        })
    if not rows:
        raise SystemExit(f"docs.sh: no pins in {path}")
    return rows

def image_and_digest(ref):
    ref = ref.removeprefix("docker.io/library/")
    if "@" in ref:
        image, digest = ref.split("@", 1)
        return image, digest
    return ref, ""

def version_catalog(path):
    """name -> (release or None, pin). A release is taken from the comment
    block above the row when that comment names one."""
    catalog = {}
    comment = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            comment.append(line)
            continue
        name, ref = line.split(None, 1)
        blob = "\n".join(comment)
        comment = []
        release = None
        if name == "libjpeg-turbo":
            match = re.search(r"libjpeg-turbo\s+(\d+\.\d+\.\d+)", blob)
            release = match.group(1) if match else None
        elif name == "jpeg-10":
            match = re.search(r"IJG v(\d+)", blob)
            release = f"IJG v{match.group(1)}" if match else None
        catalog[name] = (release, ref.strip())
    if not catalog:
        raise SystemExit(f"docs.sh: no pins in {path}")
    return catalog

def resolve_version(row, catalog):
    version = row["version"]
    image, digest = image_and_digest(row["ref"])
    if version == "@VERSIONS" or version.startswith("@VERSIONS:"):
        key = row["name"] if version == "@VERSIONS" else version.split(":", 1)[1]
        release, pin = catalog[key]
        return release if release else "no release named", f"`{pin}`"
    if digest:
        return version, f"`{image}` `{digest}`"
    return version, f"`{image}`"

def comma(n):
    return f"{n:,}"

measured = ["@page suite_measured What was measured", ""]
measured.append("Read from the tree when this manual was built.")
measured.append("")
measured.append("## Lines")
measured.append("")
measured.append("| Library | Code | Generated data |")
measured.append("| --- | ---: | ---: |")
for lib, code, generated in rows:
    measured.append(f"| {names[lib]} | {comma(code)} | {comma(generated)} |")
measured.append("")
measured.append("Code lines are cloc's code count over the same paths as that")
measured.append("library's `make cloc`. Generated data is the paths named in")
measured.append("`documents/measure.conf`: generated tables, the embedded zone")
measured.append("database, and upstream conformance files. Where one of those")
measured.append("paths also sits inside `make cloc`, its lines are in the second")
measured.append("column and have been taken out of the first.")
measured.append("")
measured.append("## Tests")
measured.append("")
measured.append("| Library | Cases the build lists |")
measured.append("| --- | ---: |")
for lib, _, _ in rows:
    count = test_counts[lib]
    cell = comma(count) if count is not None else "—"
    measured.append(f"| {names[lib]} | {cell} |")
measured.append("")
measured.append("Each number is `--gtest_list_tests` on the test binaries in")
measured.append("that library's own build directory. A dash means the library")
measured.append("had not been built, so there was nothing to list. The count")
measured.append("is the cases those binaries would run.")
measured.append("")
measured.append("## Pins")
measured.append("")
measured.append("Grouped by the library whose checks use them. The version is")
measured.append("what the reference reports. The pin is the digest or commit")
measured.append("those bytes are, when the reference is pinned that way.")
measured.append("A row that says \"no release named\" is a commit the pin file")
measured.append("records without a release number.")
measured.append("")

def pin_table(rows):
    measured.append("| Reference | Version | Pin | What it checks |")
    measured.append("| --- | --- | --- | --- |")
    for name, version, pin, answers in rows:
        answers = answers.replace("|", "/")
        measured.append(f"| `{name}` | {version} | {pin} | {answers} |")
    measured.append("")

ucd = (root / "libs/unicode/include/ghoti.io/unicode/enums.h").read_text()
ucd_match = re.search(r'#define GUNI_UCD_VERSION_STRING "([^"]+)"', ucd)
if not ucd_match:
    raise SystemExit("docs.sh: Unicode version pin not found")
tz = (root / "libs/chron/src/zone/tzdata_embedded.c").read_text(errors="replace")
tz_match = re.search(r"\(tzdata ([^)]+)\)", tz)
if not tz_match:
    raise SystemExit("docs.sh: tzdata pin not found")
image_versions = version_catalog(root / "libs/image/tools/oracle/VERSIONS")

pin_groups = [
    ("Unicode", [
        ("Character Database", ucd_match.group(1),
         "`GUNI_UCD_VERSION_STRING`",
         "the tables this library is built from"),
    ], root / "libs/unicode/tools/oracle/containers/IMAGES"),
    ("Chron", [
        ("zone database", f"tzdata {tz_match.group(1)}",
         "the header of `tzdata_embedded.c`",
         "the time-zone data compiled into the library"),
    ], root / "libs/chron/tools/oracle/containers/IMAGES"),
    ("Compress", [], root / "libs/compress/tools/oracle/containers/IMAGES"),
    ("Text", [], root / "libs/text/tools/oracle/containers/IMAGES"),
    ("Image", [], root / "libs/image/tools/oracle/containers/IMAGES"),
    ("Font", [], root / "libs/font/tools/oracle/containers/IMAGES"),
    ("Security", [], root / "libs/security/tools/oracle/containers/IMAGES"),
    ("Archive", [], root / "libs/archive/tools/oracle/containers/IMAGES"),
    ("Regex", [], root / "libs/regex/tools/oracle/containers/IMAGES"),
]
def pin_heading(title):
    slug = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
    # A markdown heading would be id="chron" and would take the link that
    # belongs to the Chron page.
    return f'<h3 id="pins-{slug}">{title}</h3>'

for title, lead, path in pin_groups:
    measured.append(pin_heading(title))
    measured.append("")
    rows = list(lead)
    catalog = image_versions if title == "Image" else {}
    for row in parse_images(path):
        version, pin = resolve_version(row, catalog)
        rows.append((row["name"], version, pin, row["answers"]))
    pin_table(rows)

measured.append(pin_heading("Regex corpora"))
measured.append("")
measured.append("The files a Regex check is scored against, from")
measured.append("`libs/regex/tools/corpus/VERSIONS`.")
measured.append("")
corpus_rows = []
for line in (root / "libs/regex/tools/corpus/VERSIONS").read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    name, ref = line.split(None, 1)
    if re.fullmatch(r"[0-9a-f]{7,}", ref) or ref.startswith("sha256:"):
        corpus_rows.append((name, "no release named", f"`{ref}`", ""))
    else:
        corpus_rows.append((name, ref, "—", ""))
if not corpus_rows:
    raise SystemExit("docs.sh: regex corpus pins not found")
pin_table(corpus_rows)

# Which dialects have a front end. The default branch is the ones that
# are named and not built. The labels are the enum, which is what the
# table is read from.
front = (root / "libs/regex/src/syntax/frontend.c").read_text()
switch = re.search(r"switch \(syntax\) \{(.*?)\n    default:", front, re.S)
if not switch:
    raise SystemExit("docs.sh: regex front-end table not found")
built = re.findall(r"case (GRX_SYNTAX_[A-Z0-9_]+):", switch.group(1))
header = (root / "libs/regex/include/ghoti.io/regex/syntax.h").read_text()
enum = re.search(r"typedef enum \{(.*?)\n\} GRX_Syntax;", header, re.S)
if not enum:
    raise SystemExit("docs.sh: GRX_Syntax not found")
named = re.findall(r"GRX_SYNTAX_([A-Z0-9_]+) = 0,|GRX_SYNTAX_([A-Z0-9_]+),", enum.group(1))
named = [a or b for a, b in named if (a or b) != "COUNT"]
pretty = {
    "POSIX_BRE": "POSIX BRE", "POSIX_ERE": "POSIX ERE",
    "GNU_BRE": "GNU BRE", "GNU_ERE": "GNU ERE",
    "PERL": "Perl", "PCRE": "PCRE2", "ECMASCRIPT": "ECMAScript",
    "PYTHON": "Python", "JAVA": "Java", "DOTNET": ".NET",
    "RUBY": "Ruby", "RE2": "RE2", "RUST": "Rust", "TCL": "Tcl",
    "VIM": "Vim", "EMACS": "Emacs", "IREGEXP": "I-Regexp",
}
built_names = [pretty[n.removeprefix("GRX_SYNTAX_")] for n in built]
missing = [pretty[n] for n in named if f"GRX_SYNTAX_{n}" not in built]
measured.append("## Dialects")
measured.append("")
measured.append("Read from the front-end table in `libs/regex/src/syntax/frontend.c`.")
measured.append("A dialect with no entry there is named and reports")
measured.append("`GRX_ERR_UNSUPPORTED`.")
measured.append("")
measured.append("| | Dialects |")
measured.append("| --- | --- |")
measured.append("| Implemented | " + ", ".join(built_names) + " |")
measured.append("| Not implemented | " + ", ".join(missing) + " |")
measured.append("")
(suite / "manual" / "pages" / "measured.md").write_text("\n".join(measured))
print("docs.sh: measured", n_libraries, "libraries")
PY

# Fold each library README in just under the @page line, above the list of
# child pages. The README's own title is dropped. Its headings are written
# as HTML so they stay on the page and out of the tree: a Markdown heading
# on a page that also has @subpage is drawn under the first child.
# libraries.txt is the list. A library with no manual/<name>.md fails in
# the fold below.
for lib in $(awk 'NF && $1 !~ /^#/ { print $1 }' "$SUITE/libraries.txt"); do
  python3 - "$lib" << 'PY'
import os, pathlib, re, sys
lib = sys.argv[1]

def slug(title):
    text = re.sub(r"[^\w\s-]", "", title.lower())
    return re.sub(r"\s+", "-", text).strip("-")
root = pathlib.Path(os.environ["ROOT"])
suite = pathlib.Path(os.environ["SUITE"])
make = (root / "libs" / lib / "Makefile").read_text()
major = re.search(r"^MAJOR_VERSION\s*:=\s*(\S+)", make, re.M)
minor = re.search(r"^MINOR_VERSION\s*:=\s*(\S+)", make, re.M)
if not major or not minor:
    raise SystemExit(f"docs.sh: {lib} Makefile has no version")
# The same spelling the .pc file and the generated version header use:
# MAJOR_VERSION, then MINOR_VERSION, which already holds minor and patch.
version = f"{major.group(1)}.{minor.group(1)}"
readme = (root / "libs" / lib / "README.md").read_text()
lines = readme.splitlines(keepends=True)
if lines and lines[0].startswith("# "):
    lines = lines[1:]
noted = False
with_version = []
for line in lines:
    if not noted and line.startswith("## License"):
        with_version.append(f"Version {version}\n")
        with_version.append("\n")
        noted = True
    with_version.append(line)
if not noted:
    with_version.append(f"\nVersion {version}\n")
lines = with_version
out = []
fence = False
for line in lines:
    if line.startswith("```"):
        fence = not fence
    elif not fence:
        for n, tag in ((4, "h4"), (3, "h3"), (2, "h2")):
            prefix = "#" * n + " "
            if line.startswith(prefix):
                title = line[len(prefix):].strip()
                line = f'<{tag} id="{slug(title)}">{title}</{tag}>\n'
                break
    out.append(line)
body = "".join(out).strip() + "\n"
manual = (suite / "manual" / f"{lib}.md").read_text().splitlines(keepends=True)
placed = False
merged = []
for line in manual:
    if not placed and line.startswith("@page "):
        merged.append(line)
        merged.append("\n")
        merged.append(body)
        merged.append("\n")
        placed = True
        continue
    merged.append(line)
(suite / "manual" / "pages" / f"{lib}.md").write_text("".join(merged))
PY
done

# The public main page. Headings become HTML for the same reason as the
# library pages. The subpage list has no heading of its own: one here would
# be pulled into the first library node.
python3 - << 'PY'
import os, pathlib, re
root = pathlib.Path(os.environ["ROOT"])
suite = pathlib.Path(os.environ["SUITE"])

def slug(title):
    text = re.sub(r"[^\w\s-]", "", title.lower())
    return re.sub(r"\s+", "-", text).strip("-")

# Getting Started is the suite README. GitHub shows that file; this page is
# the same text, so the two cannot drift. The title is the page name, and
# the headings are HTML so the menu stays one entry.
readme = (suite / "README.md").read_text().splitlines(keepends=True)
if readme and readme[0].startswith("# "):
    readme = readme[1:]
started = []
readme_fence = False
for line in readme:
    if line.startswith("```"):
        readme_fence = not readme_fence
    elif not readme_fence:
        for n, tag in ((4, "h4"), (3, "h3"), (2, "h2")):
            prefix = "#" * n + " "
            if line.startswith(prefix):
                title = line[len(prefix):].strip()
                line = f'<{tag} id="{slug(title)}">{title}</{tag}>\n'
                break
    started.append(line)
(suite / "manual" / "pages" / "getting-started.md").write_text(
    "@page getting_started Getting Started\n\n" + "".join(started).strip() + "\n")

lines = (suite / "documents" / "index.md").read_text().splitlines(keepends=True)
# The first heading is the title drawn on the page. @mainpage stays
# "Ghoti.io", the same words as PROJECT_NAME: a different @mainpage title
# becomes its own tree node and the libraries nest under it.
page_plain = "Ghoti.io"
page_heading = page_plain
if lines and lines[0].startswith("# "):
    raw = lines[0][2:].strip()
    page_plain = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", raw)
    page_heading = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        r'<a href="\2">\1</a>',
        raw,
    )
    lines = lines[1:]
out = []
fence = False
for line in lines:
    if line.startswith("```"):
        fence = not fence
    elif not fence:
        for n, tag in ((4, "h4"), (3, "h3"), (2, "h2")):
            prefix = "#" * n + " "
            if line.startswith(prefix):
                title = line[len(prefix):].strip()
                line = f'<{tag} id="{slug(title)}">{title}</{tag}>\n'
                break
    out.append(line)
body = "".join(out).strip() + "\n"
# The table under "## Libraries" in documents/index.md is the one list.
# The menu is that table, alphabetical. @subpage builds the menu and would
# also print the list, so those commands sit in a hidden block.
source = (suite / "documents" / "index.md").read_text()
section = re.search(r"(?m)^## Libraries\n\n(.*?)(?=\n## |\Z)", source, re.S)
if not section:
    raise SystemExit("docs.sh: documents/index.md has no Libraries table")
table = "\n".join(
    line for line in section.group(1).splitlines()
    if line.startswith("|")
).strip()
entries = re.findall(r"^\| \[([^\]]+)\]\(@ref ([^)]+)\) \|", table, re.M)
if len(entries) < 2:
    raise SystemExit("docs.sh: could not read the library table")
entries.sort(key=lambda item: item[0].casefold())
hidden = ["<div style=\"display:none\">", ""]
for label, page_id in entries:
    hidden.append(f"- @subpage {page_id} \"{label}\"")
hidden.append("")
hidden.append("</div>")
hidden.append("")
(suite / "manual" / "pages" / "libraries.md").write_text(
    "@page libraries Libraries\n\n" + table + "\n\n" + "\n".join(hidden))
pages = """
<div style="display:none">

- @subpage getting_started "Getting Started"
- @subpage libraries "Libraries"
- @subpage suite_measured "What was measured"

</div>
"""
(suite / "suite.md").write_text("@mainpage Ghoti.io\n\n" + body + pages)
(suite / "manual" / "pages" / "title.txt").write_text(
    page_plain + "\n" + page_heading + "\n")
PY

rm -rf docs/html
doxygen "$SUITE/Doxyfile"

# Doxygen titles the page from @mainpage, which has to stay "Ghoti.io" so the
# library list stays at the top of the tree. The heading in documents/index.md
# is written over that title afterwards. A link is kept in the heading and
# left out of the browser tab, which cannot show one.
python3 - << 'PY'
import os, pathlib
root = pathlib.Path(os.environ["ROOT"])
suite = pathlib.Path(os.environ["SUITE"])
title_path = suite / "manual" / "pages" / "title.txt"
index = root / "docs" / "html" / "index.html"
if title_path.exists() and index.exists():
    plain, heading = title_path.read_text().splitlines()
    html = index.read_text()
    old = '<div class="title">Ghoti.io </div>'
    new = f'<div class="title">{heading} </div>'
    if old not in html:
        raise SystemExit("docs.sh: main page title was not where it was expected")
    html = html.replace(old, new, 1)
    html = html.replace("<title>Ghoti.io: Ghoti.io</title>", f"<title>{plain}</title>", 1)
    index.write_text(html)

import re

def unescape_tt(html):
    """A markdown code span in a heading is emitted as the text &lt;tt&gt;.
    Turn that back into an element so the word stays monospaced. Leave the
    same text alone inside a code or pre block, and keep the browser tab
    plain, because a tab cannot show the face."""
    def title_plain(match):
        text = match.group(1)
        text = text.replace("&lt;tt&gt;", "").replace("&lt;/tt&gt;", "")
        return "<title>" + text + "</title>"
    html = re.sub(r"<title>(.*?)</title>", title_plain, html, count=1, flags=re.S)
    token = re.compile(r"<(/?)(code|pre)\b[^>]*>|(&lt;tt&gt;|&lt;/tt&gt;)", re.I)
    depth = 0
    out = []
    pos = 0
    for match in token.finditer(html):
        out.append(html[pos:match.start()])
        pos = match.end()
        if match.group(3):
            if depth == 0:
                out.append("<tt>" if match.group(3) == "&lt;tt&gt;" else "</tt>")
            else:
                out.append(match.group(3))
        else:
            if match.group(1):
                depth = max(0, depth - 1)
            elif not match.group(0).rstrip().endswith("/>"):
                depth += 1
            out.append(match.group(0))
    out.append(html[pos:])
    return "".join(out)

html_dir = root / "docs" / "html"
for page in html_dir.glob("*.html"):
    original = page.read_text(errors="replace")
    if "&lt;tt&gt;" in original or "&lt;/tt&gt;" in original:
        page.write_text(unescape_tt(original))

nav_js = html_dir / "navtree.js"
if nav_js.exists():
    script = nav_js.read_text()
    old = """  node.label = document.createTextNode(text);
  node.expanded = false;
  a.appendChild(node.label);"""
    new = """  node.label = document.createElement("span");
  // Titles carry <tt> for a code span. Everything else is text, including
  // a literal "<".
  var safe = text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  safe = safe.replace(/&lt;tt&gt;/g, "<tt>").replace(/&lt;\\/tt&gt;/g, "</tt>");
  node.label.innerHTML = safe;
  node.expanded = false;
  a.appendChild(node.label);"""
    if old not in script:
        raise SystemExit("docs.sh: nav tree label was not where it was expected")
    nav_js.write_text(script.replace(old, new, 1))

# The left-hand menu names each page from manual/*.md, and it lists pages
# rather than every heading inside them. A heading stays on its own page.
labels = {}
for manual in (suite / "manual").glob("*.md"):
    for match in re.finditer(r'@subpage\s+(\S+)\s+"([^"]+)"', manual.read_text()):
        labels[match.group(1)] = match.group(2)

def parse_js(src, i):
    while src[i] in " \n\r\t":
        i += 1
    if src.startswith("null", i):
        return None, i + 4
    if src[i] == '"':
        i += 1
        buf = []
        while src[i] != '"':
            if src[i] == "\\":
                buf.append(src[i + 1])
                i += 2
            else:
                buf.append(src[i])
                i += 1
        return "".join(buf), i + 1
    if src[i] != "[":
        raise SystemExit(f"docs.sh: nav tree could not be read at {src[i:i+20]!r}")
    i += 1
    items = []
    while True:
        while src[i] in " \n\r\t,":
            i += 1
        if src[i] == "]":
            return items, i + 1
        item, i = parse_js(src, i)
        items.append(item)

def tidy(node):
    if isinstance(node, list) and node and isinstance(node[0], list):
        return [tidy(item) for item in node]
    if not isinstance(node, list) or len(node) < 2 or not isinstance(node[1], str):
        return node
    title, url = node[0], node[1]
    extra = node[2] if len(node) > 2 else None
    page = url.split("#", 1)[0]
    if page.endswith(".html") and "#" not in url:
        label = labels.get(page[:-5])
        if label:
            title = label
    if isinstance(extra, list):
        kept = []
        for child in extra:
            if (isinstance(child, list) and len(child) > 1
                    and isinstance(child[1], str) and "#" in child[1]):
                continue
            kept.append(tidy(child))
        return [title, url, kept or None]
    if len(node) > 2:
        return [title, url, extra]
    return [title, url]

def emit_js(node, indent=0):
    pad = "  " * indent
    if node is None:
        return "null"
    if isinstance(node, str):
        escaped = node.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    lines = [emit_js(item, indent + 1) for item in node]
    inner = ",\n".join(pad + "  " + line for line in lines)
    return "[\n" + inner + "\n" + pad + "]"

nav_data = html_dir / "navtreedata.js"
if nav_data.exists() and labels:
    src = nav_data.read_text()
    marker = "var NAVTREE =\n"
    at = src.find(marker)
    if at < 0:
        raise SystemExit("docs.sh: nav tree was not where it was expected")
    start = src.find("[", at)
    tree, end = parse_js(src, start)
    while end < len(src) and src[end] in " \n\r\t":
        end += 1
    if end < len(src) and src[end] == ";":
        end += 1
    nav_data.write_text(src[:at] + "var NAVTREE =\n" + emit_js(tidy(tree)) + ";\n" + src[end:])
PY
