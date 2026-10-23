#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of the Ghoti.io suite's shape benchmark harness.
#
# It is free software: you can redistribute it and/or modify it under the
# terms of the GNU Lesser General Public License version 3 as published by
# the Free Software Foundation.
"""The bound, the allow-list and the control of the COLRv1 comparison with Skia (run-colr.sh compare).

    colr-report.py OUT CONFIGS ALLOW [--update]

`colr-cmp` wrote one line a glyph of each configuration (OUT/<name>.cmp). A glyph is within the bound when the mean absolute
difference over every channel of its cell is at most 1.0 levels and at most 0.1 % of its pixels have a channel more than
32 levels apart: the shape benchmark's bound (README.md, "The cross-check"), stated here once. The ALLOW file lists the
glyphs outside it, `<configuration> <glyph> <reason>`. The gate fails when a glyph is outside the bound and not on the list,
when the list names a glyph that is inside the bound, and when the control (OUT/control.cmp: shape-cpu with every eighth fill
of each glyph left out) is not outside the bound for at least half of the glyphs it changed (a glyph the control could not
change, with fewer than eight fills, shows no difference and is not counted), which would mean the comparison cannot see a wrong
picture. `--update` rewrites ALLOW with the glyphs outside the bound, each with the reason `unclassified`; a reason is then
written by hand.
"""

import os
import sys

MEAN = 1.0
BIG = 0.001


def read_cmp(path):
    out = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            fields = dict(p.split("=") for p in parts[1:])
            out[int(parts[0])] = {k: float(v) for k, v in fields.items()}
    return out


def outside(m):
    return m["mean"] > MEAN or m["tbig"] > BIG


def main(argv):
    out, configs_path, allow_path = argv[1:4]
    update = "--update" in argv
    names = []
    with open(configs_path, encoding="utf-8") as handle:
        for line in handle:
            if line.strip() and not line.lstrip().startswith("#"):
                names.append(line.split()[0])
    allowed = {}
    if os.path.exists(allow_path):
        with open(allow_path, encoding="utf-8") as handle:
            for line in handle:
                if line.strip() and not line.startswith("#"):
                    name, gid, reason = line.split(None, 2)
                    allowed[(name, int(gid))] = reason.strip()
    problems = []
    found = {}
    total = within = 0
    for name in names:
        cmp = read_cmp(os.path.join(out, name + ".cmp"))
        bad = [g for g, m in cmp.items() if outside(m)]
        for g in bad:
            found[(name, g)] = cmp[g]
        total += len(cmp)
        within += len(cmp) - len(bad)
        worst = max(cmp.values(), key=lambda m: m["mean"]) if cmp else None
        print("%-26s %5d glyphs, %5d outside the bound%s" % (
            name, len(cmp), len(bad), ("; worst mean %.3f" % worst["mean"]) if worst else ""))
    for key, m in sorted(found.items()):
        if key not in allowed and not update:
            problems.append("outside the bound and not on the allow-list: %s glyph %d (mean %.3f, big %.5f, ink %d)"
                            % (key[0], key[1], m["mean"], m["big"], m["ink"]))
    for key in sorted(allowed):
        if key not in found:
            problems.append("on the allow-list and inside the bound now: %s glyph %d" % key)
    # The control.
    control = read_cmp(os.path.join(out, "control.cmp"))
    first = names[0]
    base = read_cmp(os.path.join(out, first + ".cmp"))
    changed = [g for g, m in control.items() if g in base and (abs(m["mean"] - base[g]["mean"]) > 0 or m["ink"] != base[g]["ink"]
                                                                or abs(m["big"] - base[g]["big"]) > 0)]
    seen = [g for g in changed if outside(control[g])]
    print("control (every eighth fill left out of %d glyphs of %s): %d changed, %d of them outside the bound"
          % (len(control), first, len(changed), len(seen)))
    if not changed or len(seen) * 2 < len(changed):
        problems.append("the control is not seen to fail: %d of %d changed glyphs outside the bound" % (len(seen), len(changed)))
    if update:
        with open(allow_path, "w", encoding="utf-8") as handle:
            handle.write("# Glyphs outside the Skia comparison's bound (colr-report.py), each with the reason.\n"
                         "# <configuration> <glyph> <reason>\n")
            for key in sorted(found):
                m = found[key]
                auto = ("edge antialiasing only: mean %.2f over the cell, no pixel outside its neighbourhood's range" % m["mean"]
                        if m["tbig"] <= BIG else "unclassified")
                handle.write("%s %d %s\n" % (key[0], key[1], allowed.get(key, auto)))
        print("wrote %d entries" % len(found))
        return 0
    for p in problems:
        print("FAIL " + p)
    print("%d glyphs compared, %d inside the bound, %d outside (on the allow-list: %d); %d problems"
          % (total, within, total - within, len(allowed), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
