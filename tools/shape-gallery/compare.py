#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Show what changed between two shape gallery runs.

    python3 -I compare.py [OLD NEW]
    python3 -I compare.py check FILE.png...

With no arguments, compares the two most recent runs in runs/ beside this
script (directory names start with the UTC time, so they sort by age). For each
PNG in either run it reports identical, changed (the count of changed pixels and
the largest difference in any channel), a size change, or a file only one run
has; for each changed image it writes DIFF/<name>, the newer image dimmed to a
pale grey with every changed pixel in magenta and a cyan frame around the
changed region. It also compares gallery.log
(paths in "wrote" lines aside) and lists the libraries whose commit differs
between the two manifests. DIFF is NEW/diff-vs-<OLD's name>/, which also gets
summary.txt (always) and, when the logs differ, gallery.log.diff.

It is for seeing changes, not a gate: pixels change as features land. It
exits 0 whatever it finds, 2 on a usage error or an unreadable run.

`check` is the decode check render.sh runs on a new run: every chunk's CRC, the
image data inflated and unfiltered to exactly the size the header implies, IEND
last. It exits 1 when any file fails.

The PNG reader and writer here are a minimal standard-library one for this
tool's own use (8-bit, non-interlaced, as image's writer produces); the gallery
itself is drawn and encoded only by the suite's libraries.
"""

import difflib
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get("SHAPE_GALLERY_RUNS", os.path.join(HERE, "runs"))
SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


class PngError(Exception):
    pass


def read_png(path):
    """(width, height, rows of RGBA bytes) for an 8-bit, non-interlaced PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != SIGNATURE:
        raise PngError("bad signature")
    pos, idat, ihdr, plte, trns, names = 8, [], None, None, None, []
    while pos < len(data):
        if pos + 12 > len(data):
            raise PngError("truncated chunk at %d" % pos)
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if len(body) != n or pos + 12 + n > len(data):
            raise PngError("truncated %r chunk" % typ)
        (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        if zlib.crc32(typ + body) != crc:
            raise PngError("CRC mismatch in %r" % typ)
        names.append(typ.decode("latin-1"))
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat.append(body)
        pos += 12 + n
    if not names or names[0] != "IHDR" or names[-1] != "IEND":
        raise PngError("chunk order %s" % ",".join(names))
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in CHANNELS or interlace != 0:
        raise PngError("depth %d color type %d interlace %d: this reader takes 8-bit, non-interlaced only"
                       % (depth, ctype, interlace))
    bpp = CHANNELS[ctype]
    stride = w * bpp
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as e:
        raise PngError("image data does not inflate: %s" % e)
    if len(raw) != h * (1 + stride):
        raise PngError("inflated %d bytes, want %d" % (len(raw), h * (1 + stride)))
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (1 + stride)]
        line = bytearray(raw[y * (1 + stride) + 1:(y + 1) * (1 + stride)])
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        elif ft != 0:
            raise PngError("row %d has filter type %d" % (y, ft))
        rows.append(to_rgba(bytes(line), ctype, plte, trns))
        prev = line
    return w, h, rows


def to_rgba(line, ctype, plte, trns):
    if ctype == 6:
        return line
    out = bytearray()
    if ctype == 2:
        for i in range(0, len(line), 3):
            out += line[i:i + 3] + b"\xff"
    elif ctype == 0:
        for v in line:
            out += bytes((v, v, v, 255))
    elif ctype == 4:
        for i in range(0, len(line), 2):
            out += bytes((line[i], line[i], line[i], line[i + 1]))
    else:
        if plte is None:
            raise PngError("palette image without PLTE")
        for v in line:
            a = trns[v] if trns is not None and v < len(trns) else 255
            out += plte[3 * v:3 * v + 3] + bytes((a,))
    return bytes(out)


def write_png(path, w, h, rows):
    """Write 8-bit RGB rows (filter type 0)."""
    def chunk(typ, body):
        return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ + body))
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def check(paths):
    bad = 0
    for p in paths:
        try:
            w, h, _ = read_png(p)
            print("%s: ok %dx%d, %d bytes" % (os.path.basename(p), w, h, os.path.getsize(p)))
        except (OSError, PngError) as e:
            bad += 1
            print("%s: FAILED %s" % (os.path.basename(p), e))
    print("%s: %d of %d PNGs decode" % ("ok" if not bad else "FAILED", len(paths) - bad, len(paths)))
    return 1 if bad else 0


def dimmed(row):
    """The row as pale grey RGB: luminance squeezed into 176..255 over white."""
    out = bytearray(len(row) // 4 * 3)
    for i in range(len(row) // 4):
        r, g, b, a = row[4 * i:4 * i + 4]
        lum = (r * 54 + g * 183 + b * 19) >> 8
        lum = (lum * a + 255 * (255 - a)) // 255
        v = 176 + lum * 79 // 255
        out[3 * i:3 * i + 3] = bytes((v, v, v))
    return out


def compare_image(name, old, new, outdir):
    try:
        wa, ha, ra = read_png(old)
        wb, hb, rb = read_png(new)
    except (OSError, PngError) as e:
        return "%s: cannot be read: %s" % (name, e)
    if (wa, ha) != (wb, hb):
        return "%s: size changed, %dx%d -> %dx%d (not compared pixel by pixel)" % (name, wa, ha, wb, hb)
    changed, maxd, diff_rows = 0, 0, []
    bbox = [wa, ha, -1, -1]
    for y in range(ha):
        a, b = ra[y], rb[y]
        if a == b:
            diff_rows.append(None)
            continue
        marks = []
        for x in range(wa):
            pa, pb = a[4 * x:4 * x + 4], b[4 * x:4 * x + 4]
            if pa != pb:
                changed += 1
                maxd = max(maxd, max(abs(pa[k] - pb[k]) for k in range(4)))
                marks.append(x)
                bbox = [min(bbox[0], x), min(bbox[1], y), max(bbox[2], x), max(bbox[3], y)]
        diff_rows.append(marks)
    if not changed:
        return "%s: identical (%dx%d)" % (name, wa, ha)
    os.makedirs(outdir, exist_ok=True)
    # A cyan frame 3 pixels outside the changed region, so a few scattered pixels can still be found.
    fx0, fy0, fx1, fy1 = max(bbox[0] - 3, 0), max(bbox[1] - 3, 0), min(bbox[2] + 3, wa - 1), min(bbox[3] + 3, ha - 1)
    rows = []
    for y in range(ha):
        row = dimmed(rb[y])
        if y in (fy0, fy1):
            row[3 * fx0:3 * fx1 + 3] = b"\x00\xc0\xe0" * (fx1 - fx0 + 1)
        elif fy0 < y < fy1:
            row[3 * fx0:3 * fx0 + 3] = row[3 * fx1:3 * fx1 + 3] = b"\x00\xc0\xe0"
        for x in diff_rows[y] or ():
            row[3 * x:3 * x + 3] = b"\xff\x00\xff"
        rows.append(bytes(row))
    write_png(os.path.join(outdir, name), wa, ha, rows)
    return ("%s: CHANGED, %d of %d pixels (%.3f%%), max channel difference %d, within x %d..%d y %d..%d; "
            "diff image %s" % (name, changed, wa * ha, 100.0 * changed / (wa * ha), maxd, bbox[0], bbox[2],
                               bbox[1], bbox[3], os.path.join(os.path.basename(outdir), name)))


def manifest(run):
    libs, info = {}, {}
    try:
        with open(os.path.join(run, "manifest.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or ":" not in line:
                    continue
                k, v = line.rstrip("\n").split(":", 1)
                if k == "lib":
                    parts = v.split()
                    libs[parts[0]] = parts[1] if len(parts) > 1 else "?"
                else:
                    info[k] = v.strip()
    except OSError:
        pass
    return libs, info


WROTE = re.compile(r"^wrote .*/([^/]+\.png) ")


def log_lines(run):
    try:
        with open(os.path.join(run, "gallery.log"), encoding="utf-8", errors="replace") as f:
            return [WROTE.sub(r"wrote \1 ", line) for line in f]
    except OSError:
        return None


def runs():
    try:
        names = sorted(n for n in os.listdir(RUNS)
                       if not n.startswith(".") and os.path.isdir(os.path.join(RUNS, n)))
    except OSError:
        names = []
    return [os.path.join(RUNS, n) for n in names]


def main(argv):
    if argv and argv[0] == "check":
        return check(argv[1:])
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if len(argv) == 2:
        old, new = argv
    elif not argv:
        r = runs()
        if len(r) < 2:
            sys.stderr.write("compare.py: %s holds %d runs; need two\n" % (RUNS, len(r)))
            return 2
        old, new = r[-2], r[-1]
    else:
        sys.stderr.write("usage: compare.py [OLD NEW] | compare.py check FILE...\n")
        return 2
    for d in (old, new):
        if not os.path.isdir(d):
            sys.stderr.write("compare.py: no run directory %s\n" % d)
            return 2
    old, new = os.path.abspath(old), os.path.abspath(new)
    outdir = os.path.join(new, "diff-vs-" + os.path.basename(old))
    out = ["old: %s" % os.path.basename(old), "new: %s" % os.path.basename(new), ""]

    la, ia = manifest(old)
    lb, ib = manifest(new)
    moved = ["  %s %s -> %s" % (l, la.get(l, "-")[:10], lb.get(l, "-")[:10])
             for l in sorted(set(la) | set(lb)) if la.get(l) != lb.get(l)]
    out.append("libraries whose commit differs:" if moved else "libraries: the same commits")
    out += moved
    for k in ("compiler", "image_id", "suite"):
        if ia.get(k) != ib.get(k):
            out.append("%s: %s -> %s" % (k, ia.get(k, "-"), ib.get(k, "-")))
    out.append("")

    pa = {n for n in os.listdir(old) if n.endswith(".png")}
    pb = {n for n in os.listdir(new) if n.endswith(".png")}
    n_changed = 0
    for name in sorted(pa | pb):
        if name not in pb:
            line = "%s: only in old" % name
        elif name not in pa:
            line = "%s: only in new" % name
        else:
            line = compare_image(name, os.path.join(old, name), os.path.join(new, name), outdir)
        n_changed += not line.split(": ", 1)[1].startswith("identical")
        out.append(line)

    a, b = log_lines(old), log_lines(new)
    if a is None or b is None:
        out.append("gallery.log: missing in %s" % ("old" if a is None else "new"))
    elif a == b:
        out.append("gallery.log: identical (paths in 'wrote' lines aside)")
    else:
        d = list(difflib.unified_diff(a, b, "old/gallery.log", "new/gallery.log"))
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "gallery.log.diff"), "w", encoding="utf-8") as f:
            f.writelines(d)
        nd = sum(1 for x in d if x[:1] in "+-" and x[:3] not in ("+++", "---"))
        out.append("gallery.log: %d lines differ; %s" % (nd, os.path.join(os.path.basename(outdir), "gallery.log.diff")))
    out.append("")
    out.append("%d of %d images differ" % (n_changed, len(pa | pb)) if n_changed else
               "all %d images identical" % len(pa | pb))
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
