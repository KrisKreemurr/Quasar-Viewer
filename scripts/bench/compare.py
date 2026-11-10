#!/usr/bin/env python3
"""Compare two render benchmark reports (doc/bench.md) and flag regressions.

  compare.py BASE.json NEW.json [--pct 5] [--ms 0.2] [--psnr-db 1.0] [--image-psnr 35] [--json]

A metric regresses when NEW is worse than BASE by more than --pct percent AND by more than
--ms milliseconds (both, so noise on tiny passes and small relative jitter on big ones do not
trip it). Checked per segment and overall: frame time p50 / p95 / p99, GPU frame avg / p95,
and every named GPU pass (avg). Also: VRAM peak (+5% and +64 MB), cold pipeline compile
wall time (+10%, only when both runs were cold), noise at each still key (PSNR down by more
than --psnr-db), and -- when both reports wrote still-key mean images -- the PSNR between the
two images (below --image-psnr dB = the picture changed; reported, not a perf regression).

Exit status: 0 = no regression, 1 = regression(s), 2 = unusable input.
Comparing runs of different captures, GPUs or window sizes is allowed but warned about.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib


def load_report(path):
    try:
        with open(path, encoding="utf-8") as f:
            rep = json.load(f)
    except (OSError, ValueError) as e:
        raise SystemExit("cannot read %s: %s" % (path, e))
    if rep.get("format") != "slipstream-bench-report":
        raise SystemExit("%s is not a slipstream-bench-report" % path)
    if rep.get("version", 0) > 1:
        raise SystemExit("%s: report version %s is newer than this script" % (path, rep.get("version")))
    return rep


def read_png_rgb(path):
    """Minimal PNG reader: 8-bit gray / RGB / RGBA, not interlaced. Returns (w, h, bytes RGB)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, [], None
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0 or ctype not in (0, 2, 6):
                raise ValueError("unsupported PNG layout")
            channels = {0: 1, 2: 3, 6: 4}[ctype]
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if w is None:
        raise ValueError("no IHDR")
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if channels == 3:
        return w, h, bytes(out)
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        if channels == 1:
            rgb[3 * i:3 * i + 3] = bytes((out[i],)) * 3
        else:
            rgb[3 * i:3 * i + 3] = out[4 * i:4 * i + 3]
    return w, h, bytes(rgb)


def image_psnr(path_a, path_b):
    wa, ha, a = read_png_rgb(path_a)
    wb, hb, b = read_png_rgb(path_b)
    if (wa, ha) != (wb, hb):
        return None
    se = 0
    for x, y in zip(a, b):
        d = x - y
        se += d * d
    mse = se / float(len(a))
    return 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)


class Comparison:
    def __init__(self, args):
        self.args = args
        self.regressions = []
        self.improvements = []
        self.notes = []
        self.rows = []

    def metric(self, where, name, base, new, lower_is_better=True):
        if base is None or new is None:
            return
        delta = new - base
        pct = (delta / base * 100.0) if base else (0.0 if not delta else float("inf"))
        worse = delta > 0 if lower_is_better else delta < 0
        big = abs(delta) > self.args.ms and abs(pct) > self.args.pct
        status = "REGRESSION" if (worse and big) else ("better" if (big and not worse) else "")
        row = {"where": where, "metric": name, "base": base, "new": new, "delta": delta, "pct": pct, "status": status}
        self.rows.append(row)
        if status == "REGRESSION":
            self.regressions.append(row)
        elif status == "better":
            self.improvements.append(row)


def stat(o, key, field):
    v = (o or {}).get(key) or {}
    return v.get(field) if v.get("n", 0) else None


def compare_timing(cmp, where, base, new):
    for field in ("p50", "p95", "p99"):
        cmp.metric(where, "frame_ms." + field, stat(base, "frame_ms", field), stat(new, "frame_ms", field))
    for field in ("avg", "p95"):
        cmp.metric(where, "gpu_ms." + field, stat(base, "gpu_ms", field), stat(new, "gpu_ms", field))
    bp, np_ = base.get("passes") or {}, new.get("passes") or {}
    for name in sorted(set(bp) | set(np_)):
        if name not in bp or name not in np_:
            cmp.notes.append("%s: pass %s only in %s" % (where, name, "new" if name in np_ else "base"))
            continue
        cmp.metric(where, "pass." + name, stat(bp, name, "avg"), stat(np_, name, "avg"))
    for rep, label in ((base, "base"), (new, "new")):
        if rep.get("unfocused_frames"):
            cmp.notes.append("%s: %s had %d unfocused frames (background FPS cap?)" % (where, label, rep["unfocused_frames"]))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("new")
    ap.add_argument("--pct", type=float, default=5.0, help="relative threshold, percent (default 5)")
    ap.add_argument("--ms", type=float, default=0.2, help="absolute threshold, ms (default 0.2)")
    ap.add_argument("--psnr-db", type=float, default=1.0, help="still-key PSNR drop that regresses (default 1 dB)")
    ap.add_argument("--image-psnr", type=float, default=35.0, help="still images below this PSNR are 'changed' (default 35 dB)")
    ap.add_argument("--json", action="store_true", help="print the comparison as JSON")
    args = ap.parse_args(argv)

    base, new = load_report(args.base), load_report(args.new)
    cmp = Comparison(args)
    for rep, label in ((base, "base"), (new, "new")):
        if not rep.get("ok", False):
            cmp.notes.append("%s run reported an error: %s" % (label, rep.get("error")))
    for key in ("capture_id", "gpu", "driver", "window", "mode", "path", "loops"):
        if base.get(key) != new.get(key):
            cmp.notes.append("different %s: %r vs %r" % (key, base.get(key), new.get(key)))
    if base.get("settings") != new.get("settings"):
        cmp.notes.append("different render settings: %r vs %r" % (base.get("settings"), new.get("settings")))

    compare_timing(cmp, "overall", base.get("overall") or {}, new.get("overall") or {})
    bseg = {s["name"]: s for s in base.get("segments", [])}
    nseg = {s["name"]: s for s in new.get("segments", [])}
    for name in [s["name"] for s in base.get("segments", [])] + [n for n in nseg if n not in bseg]:
        if name in bseg and name in nseg:
            compare_timing(cmp, "segment " + name, bseg[name], nseg[name])
        else:
            cmp.notes.append("segment %s only in %s" % (name, "new" if name in nseg else "base"))

    bv, nv = base.get("vram") or {}, new.get("vram") or {}
    if bv.get("valid") and nv.get("valid"):
        b, n = bv.get("usage_mb_peak", 0), nv.get("usage_mb_peak", 0)
        row = {"where": "vram", "metric": "usage_mb_peak", "base": b, "new": n, "delta": n - b,
               "pct": ((n - b) / b * 100.0) if b else 0.0, "status": ""}
        if n - b > 64 and b and (n - b) / b > 0.05:
            row["status"] = "REGRESSION"
            cmp.regressions.append(row)
        cmp.rows.append(row)

    bpipe, npipe = base.get("pipelines") or {}, new.get("pipelines") or {}
    if bpipe.get("cold") and npipe.get("cold"):
        b, n = bpipe.get("wall_ms", 0.0), npipe.get("wall_ms", 0.0)
        row = {"where": "pipelines (cold)", "metric": "wall_ms", "base": b, "new": n, "delta": n - b,
               "pct": ((n - b) / b * 100.0) if b else 0.0, "status": ""}
        if b and (n - b) / b > 0.10:
            row["status"] = "REGRESSION"
            cmp.regressions.append(row)
        cmp.rows.append(row)
        if bpipe.get("compiled") != npipe.get("compiled"):
            cmp.notes.append("cold compiles: %s -> %s pipelines" % (bpipe.get("compiled"), npipe.get("compiled")))
    elif bpipe.get("cold") != npipe.get("cold"):
        cmp.notes.append("one run was cold, the other not: pipeline totals not compared")

    bnoise = {s.get("key"): s for s in base.get("noise", [])}
    for s in new.get("noise", []):
        b = bnoise.get(s.get("key"))
        if not b or "psnr_db" not in b or "psnr_db" not in s:
            continue
        where = "still key %s" % s.get("key")
        drop = b["psnr_db"] - s["psnr_db"]
        row = {"where": where, "metric": "psnr_db", "base": b["psnr_db"], "new": s["psnr_db"], "delta": -drop,
               "pct": 0.0, "status": ""}
        if drop > args.psnr_db:
            row["status"] = "REGRESSION"
            cmp.regressions.append(row)
        cmp.rows.append(row)
        ib, inew = b.get("mean_image"), s.get("mean_image")
        if ib and inew and os.path.isfile(ib) and os.path.isfile(inew):
            try:
                p = image_psnr(ib, inew)
            except (OSError, ValueError, zlib.error) as e:
                cmp.notes.append("%s: cannot compare images (%s)" % (where, e))
                p = None
            if p is not None:
                cmp.rows.append({"where": where, "metric": "image_psnr_db", "base": None, "new": p, "delta": None,
                                 "pct": None, "status": "changed" if p < args.image_psnr else ""})
                if p < args.image_psnr:
                    cmp.notes.append("%s: the still image changed (PSNR %.1f dB between runs)" % (where, p))

    if args.json:
        print(json.dumps({"regressions": cmp.regressions, "improvements": cmp.improvements, "notes": cmp.notes,
                          "rows": cmp.rows}, indent=2))
    else:
        for note in cmp.notes:
            print("note: " + note)
        for r in cmp.rows:
            if r["base"] is None:
                print("%-28s %-34s %10s -> %9.2f %s" % (r["where"], r["metric"], "", r["new"], r["status"]))
                continue
            print("%-28s %-34s %10.3f -> %9.3f  %+8.3f (%+6.1f%%) %s" % (
                r["where"], r["metric"], r["base"], r["new"], r["delta"], r["pct"], r["status"]))
        print("%d regression(s), %d improvement(s)" % (len(cmp.regressions), len(cmp.improvements)))
    return 1 if cmp.regressions else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
