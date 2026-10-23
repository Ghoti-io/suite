#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Compare two images of one size (PNG, 8 bits a channel, colour type 2, 4 or 6, not interlaced; or PAM, 8 bits) pixel by
# pixel, and say how many pixels differ, by how many levels at most, and in which channels.
#
#   pixel-diff.py A B [MAXLEVEL]
#
# Exits 0 when every differing pixel differs by at most MAXLEVEL levels in every channel (default 1), 1 when one differs by
# more or the images are not comparable, 2 on a usage error.  Used by dual.sh to show that the targets of two steps that differ
# byte for byte differ only by the one level that an exact tie allows.

import struct
import sys
import zlib


def load_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, hdr = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
    w, h, depth, ctype, _, _, interlace = hdr
    channels = {2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError("unsupported PNG (depth %d, colour type %d, interlace %d)" % (depth, ctype, interlace))
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif f != 0:
            raise ValueError("bad PNG filter")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, channels, bytes(out)


def load_pam(data):
    end = data.index(b"ENDHDR\n") + 7
    fields = dict(l.split(None, 1) for l in data[:end].decode().splitlines()[1:-1])
    w, h, depth = int(fields["WIDTH"]), int(fields["HEIGHT"]), int(fields["DEPTH"])
    if int(fields["MAXVAL"]) != 255:
        raise ValueError("unsupported PAM maxval")
    return w, h, depth, data[end:end + w * h * depth]


def load(path):
    data = open(path, "rb").read()
    return load_png(data) if data[:4] == b"\x89PNG" else load_pam(data)


def main(argv):
    if len(argv) not in (3, 4):
        print("usage: pixel-diff.py A B [MAXLEVEL]", file=sys.stderr)
        return 2
    limit = int(argv[3]) if len(argv) == 4 else 1
    try:
        a, b = load(argv[1]), load(argv[2])
    except (ValueError, OSError, zlib.error) as e:
        print("  pixel-diff: %s" % e)
        return 1
    if a[:3] != b[:3]:
        print("  pixel-diff: the images differ in size or channels")
        return 1
    w, h, c = a[:3]
    pa, pb = a[3], b[3]
    differing, worst, per_channel = 0, 0, [0] * c
    for i in range(0, w * h * c, c):
        if pa[i:i + c] != pb[i:i + c]:
            differing += 1
            for k in range(c):
                d = abs(pa[i + k] - pb[i + k])
                if d:
                    per_channel[k] += 1
                    worst = max(worst, d)
    print("  pixel-diff: %d of %d pixels differ, by at most %d level%s; channels that differ (R G B A order): %s" %
          (differing, w * h, worst, "" if worst == 1 else "s", per_channel))
    return 0 if worst <= limit else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
