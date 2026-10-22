#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""The shape benchmark's cross-check and results file.

    report.py --results DIR --pamdiff PAMDIFF --shipped SHIPPED [--reuse EARLIER] [--tag TAG]
    report.py --markdown RECORD.json

Reads what run.sh left in DIR (raw/<engine>.jsonl, frames/*.pam, prov/*.txt)
and writes DIR/report/<date>.json, the record, and DIR/report/<date>.md,
generated from it. Nothing in the Markdown is typed: every figure is read from
the JSON as it is written. --markdown writes the .md beside a record again,
from the record alone.

--reuse EARLIER takes the reference engines (Blend2D and Skia) from an earlier
run's results directory instead of DIR: their measurements, provenance and
frames are that run's, and they are cross-checked here against this run's
shape-cpu frames. It is for re-measuring shape-cpu alone (run.sh time with
SHAPE_BENCH_ENGINES=shape-cpu); the record names the run they came from. --tag
names the files <date>-<tag>.json and .md, for a second run on one date.

The cross-check's bound is stated here and nowhere else (the story's spec gives
the same numbers): against shape-cpu's last frame, the mean absolute difference
over every channel of every pixel at most 1.0 (8-bit levels), and pixels with
any channel more than 32 levels apart at most 0.1 %. The control - shape-cpu's
frame with every sixteenth drawable dropped - must exceed the bound for every
scene, or the check could not have seen a wrong answer. An engine outside the
bound on a scene is marked invalid there and not ranked.

Exit 1 when a control does not exceed the bound or an input is missing;
results outside the bound are a finding, recorded, not a failure of the run.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

MEAN_ABS_MAX = 1.0
BAD_FRACTION_MAX = 0.001
BAD_THRESHOLD = 32
CONTROL_STRIDE = 16
ENGINES = ["shape-cpu", "font", "blend2d", "skia"]
SCENES = ["fill-rect", "fill-circle", "paths", "text", "ui", "gradients", "strokes"]


def within(d):
    return d["mean_abs"] <= MEAN_ABS_MAX and d["bad_fraction"] <= BAD_FRACTION_MAX


def pamdiff(tool, a, b):
    out = subprocess.run([tool, a, b], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("report.py: pamdiff failed on %s %s: %s" % (a, b, out.stderr.strip()))
    d = json.loads(out.stdout)
    if d["bad_threshold"] != BAD_THRESHOLD:
        raise SystemExit("report.py: pamdiff's threshold %d is not the bound's %d" % (d["bad_threshold"], BAD_THRESHOLD))
    return d


def read_prov(path):
    """Top-level 'key: value' lines as a dict; indented blocks kept as text under their key."""
    prov, block = {}, None
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith(" ") and block:
                prov[block] += line.strip() + "\n"
                continue
            key, _, value = line.partition(":")
            value = value.strip()
            if value:
                prov[key] = value
                block = None
            else:
                prov[key] = ""
                block = key
    return prov


# What a record may not carry, because it names the machine it ran on rather than what it is: its host name, its
# kernel string, and the processes beside the run (kept as how many other containers ran and the busiest processes'
# CPU shares, without names, IDs or owners). What the figures depend on stays: the CPU's model name, its
# architecture and the number of CPUs online, the compilers and their flags, the governor, boost and the load average.
SCRUBBED_KEYS = ("host", "kernel")


def scrub_state(block):
    out = []
    for line in (block or "").splitlines():
        line = line.strip()
        if line.startswith(("time:", "loadavg:", "other containers running:", "busiest processes' %CPU:")):
            out.append(line)
        elif line.startswith("containers:"):
            out.append("other containers running: %d" % len(line.split(":", 1)[1].split()))
        elif line == "busiest:":
            out.append("busiest processes' %CPU:")
        elif line:
            f = line.split()
            # top -b: PID USER PR NI VIRT RES SHR S %CPU %MEM TIME+ COMMAND
            if len(f) >= 12:
                out[-1] += " " + f[8]
    return "\n".join(out) + "\n"


def scrub(prov):
    # The architecture as `uname -m` gave it; a run from before run.sh recorded it is placed by its CPU's vendor.
    if not prov.get("arch") and prov.get("cpu_model"):
        prov["arch"] = "x86_64" if re.search(r"\b(AMD|Intel)\b", prov["cpu_model"]) else "unknown"
    for k in SCRUBBED_KEYS:
        prov.pop(k, None)
    for k in ("before", "after"):
        if k in prov:
            prov[k] = scrub_state(prov[k])
    return prov


def read_err_facts(path):
    """The driver's own stderr: its compiler line and, for a reference, the image's PROVENANCE and gn args."""
    facts = []
    if os.path.exists(path):
        with open(path) as f:
            facts = [l.rstrip("\n") for l in f if l.strip()]
    return facts


def throughput(scene, frame_median):
    unit, work = scene["work_unit"], scene["work"]
    per_s = work / frame_median
    if unit == "pixels":
        return per_s / 1e6, "Mpixel/s"
    return per_s, unit + "/s"


def fmt_ms(s):
    return "%.3f" % (s * 1e3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--markdown", metavar="RECORD", help="write RECORD's .md again from it, and nothing else")
    ap.add_argument("--results")
    ap.add_argument("--pamdiff")
    ap.add_argument("--shipped")
    ap.add_argument("--reuse", help="an earlier run's results directory, for the reference engines")
    ap.add_argument("--tag", help="names the files <date>-<tag>.*")
    args = ap.parse_args()
    if args.markdown:
        with open(args.markdown) as f:
            rec = json.load(f)
        write_markdown(rec, os.path.splitext(args.markdown)[0] + ".md", os.path.basename(args.markdown))
        return 0
    if not (args.results and args.pamdiff and args.shipped):
        ap.error("--results, --pamdiff and --shipped are required")
    root = args.results
    frames = os.path.join(root, "frames")
    reused = ["blend2d", "skia"] if args.reuse else []

    def home(engine):
        """The results directory an engine's files are read from."""
        return args.reuse if engine in reused else root

    measures = {}
    for engine in ENGINES + ["shape-cpu-empty"]:
        path = os.path.join(home(engine), "raw", engine + ".jsonl")
        if engine in ("font", "shape-cpu-empty"):
            path = os.path.join(root, "raw", "shape-cpu.jsonl")
        if not os.path.exists(path):
            raise SystemExit("report.py: no %s" % path)
        with open(path) as f:
            for line in f:
                m = json.loads(line)
                if m["engine"] == engine:
                    measures[(engine, m["scene"])] = m

    engines = {}
    for engine in ["shape-cpu", "blend2d", "skia"]:
        p = read_prov(os.path.join(home(engine), "prov", engine + ".txt"))
        p["driver_log"] = read_err_facts(os.path.join(home(engine), "logs", engine + ".err"))
        for line in p["driver_log"]:
            word, _, rest = line.partition(" ")
            # The image's PROVENANCE lines: its base digest, the reference's commit, the package snapshot.
            if word in ("base", "blend2d", "asmjit", "skia", "snapshot", "freetype") and rest:
                p.setdefault(word, rest)
        engines[engine] = scrub(p)
    build = read_prov(os.path.join(root, "prov", "shape-cpu.build.txt"))
    engines["shape-cpu"]["build"] = build
    engines["font"] = {"runs_in": "shape-cpu (the same process and image)", "build": build}
    with open(args.shipped) as f:
        shipped = [l.strip() for l in f if l.strip()]

    scenes, blind, invalid = {}, [], []
    for name in SCENES:
        ref = os.path.join(frames, "shape-cpu.%s.pam" % name)
        base = measures.get(("shape-cpu", name))
        if not base or not os.path.exists(ref):
            raise SystemExit("report.py: shape-cpu has no result for %s" % name)
        control = pamdiff(args.pamdiff, ref, os.path.join(frames, "shape-cpu.%s.control.pam" % name))
        control["exceeds_bound"] = not within(control)
        if not control["exceeds_bound"]:
            blind.append(name)
        entry = {k: base[k] for k in ("metric", "extent", "work_unit", "work")}
        entry["control"] = control
        empty = measures.get(("shape-cpu-empty", name))
        entry["shape_cpu_empty_list"] = empty["frame"] if empty else None
        entry["results"] = {}
        for engine in ENGINES:
            m = measures.get((engine, name))
            if not m:
                continue
            r = {k: m[k] for k in ("warmup_frames", "frames", "build", "submit", "frame", "note", "output")}
            tp, unit = throughput(base, m["frame"]["median"])
            r["throughput"] = tp
            r["throughput_unit"] = unit
            r["spread"] = (m["frame"]["p75"] - m["frame"]["p25"]) / m["frame"]["median"]
            if engine == "shape-cpu":
                r["cross_check"] = None
                r["valid"] = True
            else:
                d = pamdiff(args.pamdiff, ref, os.path.join(home(engine), "frames", m["output"]))
                d["within_bound"] = within(d)
                r["cross_check"] = d
                r["valid"] = d["within_bound"]
                if not r["valid"]:
                    invalid.append("%s/%s" % (engine, name))
            r["vs_shape_cpu"] = base["frame"]["median"] / m["frame"]["median"]
            entry["results"][engine] = r
        scenes[name] = entry

    date = datetime.date.today().isoformat()
    stem = "%s%s" % (date, "-" + args.tag if args.tag else "")
    reuse = None
    if reused:
        earlier = sorted(f for f in os.listdir(os.path.join(args.reuse, "report")) if f.endswith(".json"))
        if len(earlier) != 1:
            raise SystemExit("report.py: %s/report holds %d records, not one" % (args.reuse, len(earlier)))
        reuse = {"engines": reused, "from": earlier[0],
                 "note": "measured in that run and not re-run; cross-checked here against this run's shape-cpu frames"}
    record = {
        "format": "shape-bench-results 1",
        "date": date,
        "tag": args.tag,
        "generated_by": "suite/tools/shape-bench/report.py",
        "protocol": {
            "threads": 1,
            "warmup": "at least 5 frames and 0.25 s",
            "timed_frames": 31,
            "statistic": "median of the timed frames; quartiles, minimum and maximum; spread = (p75 - p25) / median",
            "timed": "from the engine's first call until its output is complete; the clear to the background is not timed",
            "throughput": "work / median frame time (build plus submit)",
            "target": "caller-owned 8-bit RGBA premultiplied (Blend2D: BGRA in memory, its PRGB32), source-over, non-zero, anti-aliased",
        },
        "bound": {
            "against": "shape-cpu's last frame",
            "mean_abs_max": MEAN_ABS_MAX,
            "bad_threshold_levels": BAD_THRESHOLD,
            "bad_fraction_max": BAD_FRACTION_MAX,
            "control": "shape-cpu with every %dth drawable, counted from the topmost, dropped must exceed the bound"
                       % CONTROL_STRIDE,
        },
        "shipped": shipped,
        "reused": reuse,
        "engines": engines,
        "scenes": scenes,
        "verdict": {"blind_checks": blind, "invalid": invalid},
    }
    out = os.path.join(root, "report")
    os.makedirs(out, exist_ok=True)
    jpath = os.path.join(out, stem + ".json")
    with open(jpath, "w") as f:
        json.dump(record, f, indent=1, sort_keys=False)
        f.write("\n")
    with open(jpath) as f:
        write_markdown(json.load(f), os.path.join(out, stem + ".md"), os.path.basename(jpath))
    print("report.py: wrote %s; blind checks: %s; invalid: %s" % (jpath, blind or "none", invalid or "none"))
    return 1 if blind else 0


def write_markdown(rec, path, jname):
    """Every figure below is read from the record just written."""
    L = []
    sc = rec["engines"]["shape-cpu"]
    L.append("# Shape benchmark results, %s%s" % (rec["date"], " (%s)" % rec["tag"] if rec.get("tag") else ""))
    L.append("")
    L.append("Generated by `report.py` from `%s`, which holds every figure here and the full provenance. "
             "Do not edit by hand." % jname)
    L.append("")
    # The model name has a line of its own: the leak gate lets only this line, and the record's cpu_model field,
    # name a CPU.
    L.append("CPU model: %s." % (sc.get("cpu_model") or "not recorded"))
    L.append("")
    L.append("%s, %s CPUs online, governor %s, boost %s; every engine pinned to CPU %s, one thread."
             % (sc.get("arch"), sc.get("cpus_online"), sc.get("governor"), sc.get("boost"), sc.get("pinned_cpu")))
    L.append("")
    if rec.get("reused"):
        L.append("%s: not re-run here; their measurements, provenance and frames are those of `%s` (%s)." % (
            " and ".join(rec["reused"]["engines"]), rec["reused"]["from"], rec["reused"]["note"]))
        L.append("")
    L.append("Each row: the median frame over %d timed frames after warm-up, the quartiles, the throughput derived "
             "from the median, the speed relative to shape-cpu (above 1 is faster), and the cross-check against "
             "shape-cpu's frame (mean absolute difference in 8-bit levels; pixels more than %d levels apart). "
             "Bound: mean at most %s, such pixels at most %s %%." % (
                 rec["protocol"]["timed_frames"], rec["bound"]["bad_threshold_levels"], rec["bound"]["mean_abs_max"],
                 rec["bound"]["bad_fraction_max"] * 100))
    L.append("")
    for name, s in rec["scenes"].items():
        c = s["control"]
        L.append("## %s (%s; %s %s a frame, %dx%d)" % (name, s["metric"], "%.6g" % s["work"], s["work_unit"],
                                                       s["extent"][0], s["extent"][1]))
        L.append("")
        L.append("| engine | frame ms (median) | p25 to p75 ms | build ms | submit ms | throughput | vs shape-cpu "
                 "| mean abs diff | pixels > %d | valid |" % rec["bound"]["bad_threshold_levels"])
        L.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
        for engine, r in s["results"].items():
            cc = r["cross_check"]
            L.append("| %s | %s | %s to %s | %s | %s | %.4g %s | %.3gx | %s | %s | %s |" % (
                engine, fmt_ms(r["frame"]["median"]), fmt_ms(r["frame"]["p25"]), fmt_ms(r["frame"]["p75"]),
                fmt_ms(r["build"]["median"]) if r["build"] else "none", fmt_ms(r["submit"]["median"]),
                r["throughput"], r["throughput_unit"], r["vs_shape_cpu"],
                "reference" if cc is None else "%.4f" % cc["mean_abs"],
                "reference" if cc is None else "%.4f %%" % (cc["bad_fraction"] * 100),
                "yes" if r["valid"] else "**no**"))
        L.append("")
        L.append("Control (every %dth drawable, from the topmost, dropped): mean %.4f, pixels > %d %.4f %%: %s." % (
            16, c["mean_abs"], c["bad_threshold"], c["bad_fraction"] * 100,
            "exceeds the bound, so the check can see a wrong frame" if c["exceeds_bound"]
            else "**within the bound: this scene's check is blind**"))
        if s.get("shape_cpu_empty_list"):
            L.append("")
            L.append("shape-cpu rendering an empty list on this extent and target (its fixed cost a frame): %s ms "
                     "median, %s to %s." % (fmt_ms(s["shape_cpu_empty_list"]["median"]),
                                            fmt_ms(s["shape_cpu_empty_list"]["p25"]),
                                            fmt_ms(s["shape_cpu_empty_list"]["p75"])))
        notes = ["%s: %s" % (e, r["note"]) for e, r in s["results"].items() if r["note"]]
        if notes:
            L.append("")
            for n in notes:
                L.append("- %s" % n)
        L.append("")
    L.append("## Provenance")
    L.append("")
    L.append("| engine | image | image ID | load before | load after |")
    L.append("| --- | --- | --- | --- | --- |")
    for engine in ["shape-cpu", "blend2d", "skia"]:
        e = rec["engines"][engine]

        def load(block):
            for line in (e.get(block) or "").splitlines():
                if line.startswith("loadavg:"):
                    return line.split(":", 1)[1].strip()
            return "unknown"
        L.append("| %s | `%s` | `%s` | %s | %s |" % (engine, e.get("image"), e.get("image_id"), load("before"),
                                                   load("after")))
    L.append("")
    L.append("- shape-cpu and font: %s; base `%s`; harness `%s`." % (
        sc["build"].get("compiler"), sc.get("base"), sc["build"].get("harness_flags")))
    L.append("- shape-cpu's library flags (its flag stamp): `%s`." % sc["build"].get("shape-cpu_flags", "unknown")[:400])
    L.append("- font file: %s." % sc["build"].get("font_file"))
    for engine in ["blend2d", "skia"]:
        L.append("- %s, as its image and driver recorded them:" % engine)
        for line in rec["engines"][engine]["driver_log"]:
            if line.startswith(("bench-", "{")):
                continue
            L.append("  - `%s`" % line[:300])
    L.append("- Libraries shipped (commit, and whether the tree was dirty): %s." % "; ".join(rec["shipped"]))
    L.append("")
    v = rec["verdict"]
    L.append("Blind checks: %s. Results outside the bound: %s." % (", ".join(v["blind_checks"]) or "none",
                                                                  ", ".join(v["invalid"]) or "none"))
    L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))


if __name__ == "__main__":
    sys.exit(main())
