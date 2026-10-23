#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""The shape benchmark's results file for the pictures (README.md, "The pictures").

    report-pictures.py --results DIR --shipped SHIPPED [--tag TAG] [--date DATE] [--images OUTDIR]
    report-pictures.py --markdown RECORD.json

Reads what run.sh pictures left in DIR/pictures (raw/<engine>.<rep>.jsonl, prov/, logs/, frames/*.pam, native/*.png) and
the scenes in DIR/scenes, and writes DIR/report/<date>[-TAG]-pictures.json, the record, and the .md generated from it.
Every figure in the Markdown is read from the JSON as it is written; --markdown writes the .md beside a record again.

The pictures are compared with shape-cpu's own picture (the PNG its list was rendered to, `native/`): the share of pixels
whose largest channel differs by more than 1, 4 and 16 levels, the mean absolute difference over the colour channels, the
largest difference, and the coarse tiles where the differences are.  With --images OUTDIR, for each picture a side-by-side
image (shape-cpu | Blend2D | Skia | where they differ from shape-cpu, at half size) and a heatmap of each reference's
difference at full size are written there.  This script needs numpy and Pillow, which nothing else in this directory does.

What the scene says that a reference cannot say is listed beside the figures (`differences`), found by reading the
scene, because a speed for a picture a reference draws differently is not a speed for the same picture.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report  # noqa: E402  (read_prov, scrub, read_err_facts)

ENGINES = ["shape-cpu-native", "shape-cpu", "blend2d", "skia"]
TITLES = {
    "shape-cpu-native": "shape-cpu, the picture's own list",
    "shape-cpu": "shape-cpu, the neutral scene",
    "blend2d": "Blend2D",
    "skia": "Skia",
}
THRESHOLDS = (1, 4, 16)
PICTURES = {"picture-typography": "typography.png", "picture-geometry": "geometry.png"}
HSL_MODES = ("hue", "saturation", "color", "luminosity")


# ---------------------------------------------------------------- the scene, read for what it says

def scene_facts(path):
    """What a scene file holds, counted from its text."""
    f = {"items": 0, "groups": 0, "groups_linear": 0, "composites": {}, "clippaths": 0, "clip_rects": 0, "joins": {},
         "dashed": 0, "sweeps_partial": 0, "sweeps_whole": 0, "radial_start_radius": 0, "radial": 0, "linear": 0,
         "evenodd": 0, "extend": {"pad": 0, "repeat": 0, "reflect": 0}, "glyph_runs": 0, "commands": 0}
    with open(path) as fh:
        for line in fh:
            w = line.split()
            if not w:
                continue
            k = w[0]
            if k in ("fill", "fill-eo", "stroke", "run"):
                f["items"] += 1
                f["evenodd"] += k == "fill-eo"
                if k == "stroke":
                    f["joins"][w[6]] = f["joins"].get(w[6], 0) + 1
                    f["dashed"] += int(w[10]) > 0
            elif k == "group":
                f["groups"] += 1
                f["composites"][w[1]] = f["composites"].get(w[1], 0) + 1
                f["groups_linear"] += len(w) > 3 and w[3] == "linear"
            elif k == "clippath":
                f["clippaths"] += 1
            elif k == "clip":
                f["clip_rects"] += 1
            elif k == "gradient":
                kind, ext = w[1], w[2]
                f["extend"][ext] = f["extend"].get(ext, 0) + 1
                if kind == "sweep":
                    span = float(w[6]) - float(w[5])
                    f["sweeps_whole"] += abs(span - 360.0) < 1e-6 and float(w[5]) == 0.0
                    f["sweeps_partial"] += not (abs(span - 360.0) < 1e-6 and float(w[5]) == 0.0)
                elif kind == "radial":
                    f["radial"] += 1
                    f["radial_start_radius"] += float(w[5]) != 0.0
                else:
                    f["linear"] += 1
            elif k in "MLQCZ" and len(k) == 1:
                f["commands"] += 1
    return f


def differences(facts, emit_notes):
    """The ways the picture the references draw differs in its definition from the one shape-cpu draws."""
    d = []
    if facts["groups_linear"]:
        d.append("%d of %d groups composite in linear light in shape-cpu's picture (the colour-glyph lowering's choice); "
                 "Blend2D and Skia composite every group in the target's encoded values, so where translucent layers "
                 "overlap in those groups the pixels differ by construction" % (facts["groups_linear"], facts["groups"]))
    hsl = {m: facts["composites"].get(m, 0) for m in HSL_MODES if facts["composites"].get(m, 0)}
    if hsl:
        d.append("Blend2D has no hue, saturation, color or luminosity operator: %s drawn source-over there; Skia draws them"
                 % ", ".join("%d %s group%s" % (n, m, "" if n == 1 else "s") for m, n in hsl.items()))
    if facts["clippaths"]:
        d.append("%d clip%s to a glyph outline: Skia clips to the path; Blend2D clips to rectangles only, so each is a layer "
                 "multiplied by the coverage of the path (a different antialiasing of the clip's edge)"
                 % (facts["clippaths"], "" if facts["clippaths"] == 1 else "s"))
    if facts["joins"].get("miterclip") or facts["joins"].get("arcs"):
        d.append("SVG 2 joins: %d miter-clip and %d arcs. Blend2D has miter-clip (BL_STROKE_JOIN_MITER_CLIP) and no arcs "
                 "(drawn miter-bevel); Skia has neither (both drawn as its miter, a bevel past the limit)"
                 % (facts["joins"].get("miterclip", 0), facts["joins"].get("arcs", 0)))
    if facts["sweeps_partial"]:
        d.append("%d sweep gradient%s over part of a turn: Skia states them; Blend2D's conic gradient spans whole turns, so "
                 "its color line is sampled into stops over one turn (the scene's seam, folds and jumps included)"
                 % (facts["sweeps_partial"], "" if facts["sweeps_partial"] == 1 else "s"))
    if facts["radial_start_radius"]:
        d.append("%d radial gradient%s with a start circle of nonzero radius (a two-circle cone): shape-cpu and Skia "
                 "state it; Blend2D through its start radius" % (facts["radial_start_radius"],
                                                                "" if facts["radial_start_radius"] == 1 else "s"))
    if facts["dashed"]:
        d.append("%d dashed stroke%s: this Blend2D draws a dash array solid, through the context and through "
                 "BLPath::add_stroked_path alike (the dash array in BLStrokeOptions), so its picture has solid strokes where "
                 "shape-cpu and Skia (SkDashPathEffect) have dashes, and its time is for solid strokes" % (
                     facts["dashed"], "" if facts["dashed"] == 1 else "s"))
    d.append("Gradients are interpolated in the target's encoded values straight; a gradient shape-cpu interpolates in "
             "linear light is sampled into 16 encoded stops in the scene (the nearest the references have)")
    d.append("Text is the outlines of the font library's own outline provider drawn as paths by Blend2D, Skia and, in the "
             "scene arm, shape-cpu: shaping, outline loading and glyph caches are not timed. Skia's and Blend2D's "
             "antialiasing and curve flattening are their own")
    d.append("A solid color is rounded to 8 bits and a gradient's stops to 8 bits in the scene; a group's opacity and a "
             "gradient's alpha multiplier are carried exactly (opacity) or to 8 bits (alpha)")
    for n in emit_notes:
        d.append("recording the picture's list: " + n)
    return d


# ---------------------------------------------------------------- pixels

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_pam(path):
    import numpy as np
    with open(path, "rb") as fh:
        data = fh.read()
    end = data.index(b"ENDHDR\n") + 7
    fields = dict(l.split(None, 1) for l in data[:end].decode().splitlines()[1:-1])
    w, h = int(fields["WIDTH"]), int(fields["HEIGHT"])
    return np.frombuffer(data[end:end + w * h * 4], dtype=np.uint8).reshape(h, w, 4)


def load_png(path):
    import numpy as np
    from PIL import Image
    return np.array(Image.open(path).convert("RGBA"))


def diff_stats(ref, img, tile=150):
    import numpy as np
    d = np.abs(ref[..., :3].astype(np.int16) - img[..., :3].astype(np.int16)).max(axis=2)
    out = {"pixels": int(d.size), "mean_abs": float(np.abs(ref[..., :3].astype(np.int16) - img[..., :3].astype(np.int16)).mean()),
           "max_abs": int(d.max()), "alpha_differs": int((ref[..., 3] != img[..., 3]).sum())}
    for t in THRESHOLDS:
        out["gt%d_percent" % t] = float((d > t).mean() * 100.0)
    # Where the large differences are: the tiles of the picture with the most pixels past 16 levels.
    h, w = d.shape
    tiles = []
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            n = int((d[y:y + tile, x:x + tile] > 16).sum())
            if n:
                tiles.append((n, x, y))
    tiles.sort(reverse=True)
    out["tile"] = tile
    out["top_tiles_gt16"] = [{"x": x, "y": y, "pixels": n, "share_of_tile_percent": 100.0 * n / (min(tile, w - x) * min(tile, h - y))}
                              for n, x, y in tiles[:8]]
    return out, d


def heat(d):
    """A difference map (max channel, levels) as a picture: black none, blue 1 to 3, cyan 4 to 15, yellow 16 to 63, red 64 on."""
    import numpy as np
    h = np.zeros(d.shape + (3,), dtype=np.uint8)
    h[d >= 1] = (20, 40, 160)
    h[d >= 4] = (0, 200, 220)
    h[d >= 16] = (255, 220, 0)
    h[d >= 64] = (255, 40, 30)
    return h


def make_images(outdir, name, ref, frames, diffs):
    import numpy as np
    from PIL import Image, ImageDraw
    short = name.replace("picture-", "")
    os.makedirs(outdir, exist_ok=True)
    panels = [("shape-cpu", ref), ("Blend2D", frames["blend2d"]), ("Skia", frames["skia"])]
    worst = np.maximum(diffs["blend2d"], diffs["skia"])
    panels.append(("where Blend2D or Skia differ from shape-cpu: >1 blue, >4 cyan, >16 yellow, >63 red", heat(worst)))
    w, h = ref.shape[1] // 2, ref.shape[0] // 2
    strip = Image.new("RGB", (w * 4, h + 28), (16, 16, 16))
    dr = ImageDraw.Draw(strip)
    for i, (label, img) in enumerate(panels):
        a = img if img.ndim == 3 and img.shape[2] == 3 else img[..., :3]
        strip.paste(Image.fromarray(np.ascontiguousarray(a)).resize((w, h), Image.LANCZOS), (i * w, 28))
        dr.text((i * w + 8, 8), label, fill=(235, 235, 235))
    strip.save(os.path.join(outdir, "%s-side-by-side.png" % short))
    for e in ("blend2d", "skia"):
        Image.fromarray(heat(diffs[e])).save(os.path.join(outdir, "%s-diff-%s.png" % (short, e)))
    return ["%s-side-by-side.png" % short, "%s-diff-blend2d.png" % short, "%s-diff-skia.png" % short]


# ---------------------------------------------------------------- timings

def read_raw(rawdir):
    """{(engine, scene): [per repetition measure]}, {(engine, scene): [cold]}"""
    meas, cold = {}, {}
    for fn in sorted(os.listdir(rawdir)):
        m = re.match(r"(.+)\.(\d+)\.jsonl$", fn)
        if not m:
            continue
        with open(os.path.join(rawdir, fn)) as fh:
            for line in fh:
                j = json.loads(line)
                if j["engine"] == "shape-cpu-cold":
                    cold.setdefault(("shape-cpu", j["scene"]), []).append(j["cold"])
                elif "cold" in j and "frame" not in j:
                    cold.setdefault((j["engine"], j["scene"]), []).append(j["cold"])
                else:
                    meas.setdefault((j["engine"], j["scene"]), []).append(j)
    return meas, cold


def med(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def summarize(reps):
    """The repetitions of one engine on one scene: each one's statistics, the median of their medians and the spread."""
    out = {"repetitions": len(reps), "warmup_frames": [r["warmup_frames"] for r in reps], "frames": reps[0]["frames"]}
    for part in ("build", "submit", "frame"):
        if reps[0].get(part) is None:
            out[part] = None
            continue
        meds = [r[part]["median"] for r in reps]
        pick = reps[sorted(range(len(reps)), key=lambda i: meds[i])[len(reps) // 2]][part]
        out[part] = {"median": med(meds), "p25": pick["p25"], "p75": pick["p75"], "min": min(r[part]["min"] for r in reps),
                     "max": max(r[part]["max"] for r in reps), "rep_medians": meds,
                     "spread_of_medians": (max(meds) - min(meds)) / med(meds)}
    out["per_repetition"] = [{p: r[p] for p in ("build", "submit", "frame") if r.get(p)} for r in reps]
    return out


def summarize_cold(cs):
    out = {}
    for part in ("build", "submit", "frame"):
        v = [c[part] for c in cs]
        out[part] = {"median": med(v), "min": min(v), "max": max(v), "samples": v}
    return out


# ---------------------------------------------------------------- the record

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--markdown", metavar="RECORD")
    ap.add_argument("--results")
    ap.add_argument("--shipped")
    ap.add_argument("--tag")
    ap.add_argument("--date")
    ap.add_argument("--images")
    args = ap.parse_args()
    if args.markdown:
        with open(args.markdown) as fh:
            rec = json.load(fh)
        write_markdown(rec, os.path.splitext(args.markdown)[0] + ".md", os.path.basename(args.markdown))
        return 0
    if not (args.results and args.shipped):
        ap.error("--results and --shipped are required")
    root = os.path.join(args.results)
    pic = os.path.join(root, "pictures")
    meas, cold = read_raw(os.path.join(pic, "raw"))
    date = args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")

    engines = {}
    for eng in ENGINES:
        files = sorted(f for f in os.listdir(os.path.join(pic, "prov")) if re.fullmatch(re.escape(eng) + r"\.\d+\.txt", f))
        if not files:
            raise SystemExit("report-pictures.py: no provenance for %s" % eng)
        p = report.scrub(report.read_prov(os.path.join(pic, "prov", files[0])))
        p["repetitions"] = []
        for f in files:
            q = report.scrub(report.read_prov(os.path.join(pic, "prov", f)))
            p["repetitions"].append({"before": q.get("before"), "after": q.get("after")})
        p["driver_log"] = report.read_err_facts(os.path.join(pic, "logs", files[0].replace(".txt", ".err")))
        for line in p["driver_log"]:
            word, _, rest = line.partition(" ")
            if word in ("base", "blend2d", "asmjit", "skia", "snapshot", "freetype") and rest:
                p.setdefault(word, rest)
        engines[eng] = p
    build = report.read_prov(os.path.join(root, "prov", "shape-cpu.build.txt"))
    engines["shape-cpu"]["build"] = build
    nb = os.path.join(pic, "native-build.txt")
    if os.path.exists(nb):
        engines["shape-cpu-native"]["build"] = report.read_prov(nb)
    with open(args.shipped) as fh:
        shipped = [l.strip() for l in fh if l.strip()]
    emit_log = os.path.join(pic, "emit.log")
    emit_text = open(emit_log).read() if os.path.exists(emit_log) else ""

    scenes = {}
    for name, png in PICTURES.items():
        if (("shape-cpu", name) not in meas):
            continue
        facts = scene_facts(os.path.join(root, "scenes", name + ".scene"))
        notes = []
        in_block = False
        for line in emit_text.splitlines():
            if line.startswith("emit %s:" % name):
                in_block = True
                continue
            if line.startswith("emit "):
                in_block = False
            if in_block and line.strip().startswith("NOTE "):
                notes.append(line.strip()[5:])
        ref = load_png(os.path.join(pic, "native", png))
        frames = {"shape-cpu": load_pam(os.path.join(pic, "frames", "shape-cpu.%s.pam" % name)),
                  "blend2d": load_pam(os.path.join(pic, "frames", "blend2d.%s.pam" % name)),
                  "skia": load_pam(os.path.join(pic, "frames", "skia.%s.pam" % name))}
        pixels, diffs = {}, {}
        for e in ("shape-cpu", "blend2d", "skia"):
            pixels[e], diffs[e] = diff_stats(ref, frames[e])
        pixels["blend2d-vs-skia"], _ = diff_stats(frames["blend2d"], frames["skia"])
        res = {}
        for e in ENGINES:
            if (e, name) in meas:
                res[e] = summarize(meas[(e, name)])
            c = cold.get((e, name))
            if c:
                res.setdefault(e, {})["cold"] = summarize_cold(c)
        images = make_images(args.images, name, ref, frames, diffs) if args.images else []
        scenes[name] = {"extent": [ref.shape[1], ref.shape[0]], "scene_sha256": sha256(os.path.join(root, "scenes", name + ".scene")),
                        "native_png_sha256": sha256(os.path.join(pic, "native", png)), "scene": facts, "differences": differences(facts, notes),
                        "results": res, "pixels": pixels, "images": images}
    rec = {
        "format": "shape-bench-results 1",
        "kind": "pictures",
        "date": date,
        "tag": args.tag,
        "generated_by": "suite/tools/shape-bench/report-pictures.py",
        "protocol": {
            "threads": 1,
            "warmup": "at least 5 frames and 0.25 s",
            "timed_frames": 31,
            "statistic": "median of the timed frames of a repetition; the whole run (every engine) repeated, and the figure "
                         "is the median of the repetitions' medians, with their spread (max - min) / median",
            "timed": "from the engine's first call until its output is complete; the clear to the background is not timed",
            "build": "shape-cpu: recording the list from resources interned once; Blend2D and Skia draw immediately and "
                     "have no build step",
            "cold": "the first frame on an engine with nothing cached (shape-cpu's expansion cache of the strokes empty), "
                    "one frame a repetition, before the warm-up; the scene arm's is the first frame of its process, the "
                    "picture's-own-list arm's follows that process's render of the picture (code and memory warm, the "
                    "sink new)",
            "target": "caller-owned 8-bit RGBA premultiplied (Blend2D: BGRA in memory, its PRGB32), source-over, no dithering",
            "reference_picture": "shape-cpu's own picture, the PNG of the list as recorded (not the scene arm's frame)",
            "pixel_thresholds_levels": list(THRESHOLDS),
            "pixel_measure": "largest difference over R, G and B of the pixel; share of the pixels past each threshold",
            "limits": "one run on one machine; there is no speed threshold (AD-28)",
        },
        "shipped": shipped,
        "engines": engines,
        "scenes": scenes,
    }
    os.makedirs(os.path.join(root, "report"), exist_ok=True)
    stem = "%s%s-pictures" % (date, "-" + args.tag if args.tag else "")
    jpath = os.path.join(root, "report", stem + ".json")
    with open(jpath, "w") as fh:
        json.dump(rec, fh, indent=1)
        fh.write("\n")
    write_markdown(rec, os.path.join(root, "report", stem + ".md"), stem + ".json")
    print(jpath)
    return 0


def ms(x):
    return "%.1f" % (x * 1e3)


def write_markdown(rec, path, jname):
    L = []
    sc = rec["engines"]["shape-cpu"]
    L.append("# Shape benchmark results, the pictures, %s%s" % (rec["date"], " (%s)" % rec["tag"] if rec.get("tag") else ""))
    L.append("")
    L.append("Generated by `report-pictures.py` from `%s`, which holds every figure here and the full provenance. "
             "Do not edit by hand." % jname)
    L.append("")
    L.append("CPU model: %s." % (sc.get("cpu_model") or "not recorded"))
    L.append("")
    L.append("%s, %s CPUs online, governor %s, boost %s; every engine pinned to CPU %s, one thread."
             % (sc.get("arch"), sc.get("cpus_online"), sc.get("governor"), sc.get("boost"), sc.get("pinned_cpu")))
    L.append("")
    L.append("Two large pictures, drawn by every engine from the same description. Each figure is the median of the "
             "repetitions' medians (each repetition is %d timed frames after warm-up), with the quartiles of the middle "
             "repetition. One run on one machine: no speed threshold. A picture a reference draws differently is "
             "listed under its table, and its speed is for that picture." % rec["protocol"]["timed_frames"])
    L.append("")
    for name, s in rec["scenes"].items():
        w, h = s["extent"]
        L.append("## %s (%dx%d)" % (name, w, h))
        L.append("")
        L.append("Scene file sha256 %s; shape-cpu's picture (PNG) sha256 %s." % (s["scene_sha256"], s["native_png_sha256"]))
        L.append("")
        L.append("The scene: %d drawn items, %d path commands, %d groups (%d in linear light), %d path clips, %d rectangle clips, "
                 "%d dashed strokes, joins %s." % (s["scene"]["items"], s["scene"]["commands"], s["scene"]["groups"],
                                                   s["scene"]["groups_linear"], s["scene"]["clippaths"], s["scene"]["clip_rects"],
                                                   s["scene"]["dashed"], json.dumps(s["scene"]["joins"], sort_keys=True)))
        L.append("")
        L.append("| engine | build ms | submit ms | total ms (median) | p25 to p75 ms | spread of medians | cold build ms | cold submit ms | cold total ms |")
        L.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for e in ENGINES:
            r = s["results"].get(e)
            if not r or "frame" not in r:
                continue
            c = r.get("cold")
            L.append("| %s | %s | %s | %s | %s to %s | %.1f %% | %s | %s | %s |" % (
                TITLES[e], ms(r["build"]["median"]) if r.get("build") else "none", ms(r["submit"]["median"]),
                ms(r["frame"]["median"]), ms(r["frame"]["p25"]), ms(r["frame"]["p75"]), r["frame"]["spread_of_medians"] * 100,
                ms(c["build"]["median"]) if c else "n/a", ms(c["submit"]["median"]) if c else "n/a",
                ms(c["frame"]["median"]) if c else "n/a"))
        L.append("")
        L.append("Cold is the first frame, with the expansion cache of the strokes empty; Blend2D and Skia keep no such "
                 "cache across frames, so their cold figure is their first frame and only shows warm-up of code and memory. "
                 "The repetitions' medians of the total, in ms: " + "; ".join(
                     "%s %s" % (TITLES[e], ", ".join(ms(m) for m in s["results"][e]["frame"]["rep_medians"]))
                     for e in ENGINES if e in s["results"] and "frame" in s["results"][e]) + ".")
        L.append("")
        L.append("Against shape-cpu's own picture (share of the %d pixels whose largest colour-channel difference is past "
                 "1, 4 and 16 levels; mean absolute difference over the colour channels; the largest difference):" %
                 s["pixels"]["blend2d"]["pixels"])
        L.append("")
        L.append("| picture | > 1 level | > 4 levels | > 16 levels | mean abs | max |")
        L.append("| --- | ---: | ---: | ---: | ---: | ---: |")
        for key, label in (("shape-cpu", "shape-cpu, the neutral scene"), ("blend2d", "Blend2D"), ("skia", "Skia"),
                           ("blend2d-vs-skia", "Blend2D against Skia")):
            p = s["pixels"][key]
            L.append("| %s | %.3f %% | %.3f %% | %.3f %% | %.3f | %d |" % (label, p["gt1_percent"], p["gt4_percent"],
                                                                         p["gt16_percent"], p["mean_abs"], p["max_abs"]))
        L.append("")
        for key, label in (("blend2d", "Blend2D"), ("skia", "Skia")):
            t = s["pixels"][key]["top_tiles_gt16"]
            L.append("Tiles (%d pixels square) of %s with the most pixels past 16 levels: %s." % (
                s["pixels"][key]["tile"], label, "; ".join("(%d, %d) %d px" % (x["x"], x["y"], x["pixels"]) for x in t[:5]) or "none"))
            L.append("")
        L.append("What is not equal between the engines in this picture:")
        L.append("")
        for d in s["differences"]:
            L.append("- " + d)
        L.append("")
        for im in s["images"]:
            L.append("Image: `%s` (kept beside the record by the run's owner, not in this repository)." % im)
        L.append("")
    with open(path, "w") as fh:
        fh.write("\n".join(L))
        fh.write("\n")


if __name__ == "__main__":
    sys.exit(main())
