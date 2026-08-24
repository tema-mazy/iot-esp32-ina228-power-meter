#!/usr/bin/env python3
"""Standalone SVG charts from discharge logs, for embedding in the docs.

    ./svg_chart.py out.svg log.jsonl --field v --per-cell 5 --ylabel V/cell

Writes one self-contained SVG per call. The file is pure ASCII, adapts to
light and dark via a media query, and needs no external assets - markdown can
reference it directly.

Palette: validated categorical slots 1 and 2 (blue, orange), both modes.
"""

import argparse
import json
import sys

W, H = 640, 260
PAD = {"l": 64, "r": 108, "t": 22, "b": 42}
LIGHT = dict(ink="#0b0b0b", ink2="#52514e", grid="#e6e5e1",
             s1="#2a78d6", s2="#eb6834", bg="#fcfcfb")
DARK = dict(ink="#ffffff", ink2="#c3c2b7", grid="#333331",
            s1="#3987e5", s2="#d95926", bg="#1a1a19")


def load(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "v" in r and "ts" in r:
                    rows.append(r)
    rows.sort(key=lambda r: r["ts"])
    return rows


def ticks(lo, hi, n):
    if hi <= lo:
        hi = lo + 1
    raw = (hi - lo) / n
    mag = 10.0 ** (len(str(int(raw))) - 1) if raw >= 1 else 1.0
    while mag > raw:
        mag /= 10.0
    step = next((mag * m for m in (1, 2, 2.5, 5, 10) if mag * m >= raw), mag * 10)
    out, v = [], (lo // step) * step
    while v <= hi + step * 1e-3:
        if v >= lo - step * 1e-3:
            out.append(v)
        v += step
    return out


def fmt_for(step):
    d = 0
    while step < 1 and d < 4:
        step *= 10
        d += 1
    return "{:." + str(d) + "f}"


def render(series, xlabel, ylabel, caption=""):
    """series = [(label, [(x, y)...], slot, dashed)]"""
    xs = [p[0] for _, s, _, _ in series for p in s]
    ys = [p[1] for _, s, _, _ in series for p in s]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    pad = ((y1 - y0) or 1) * 0.08
    y0 -= pad
    y1 += pad
    pw, ph = W - PAD["l"] - PAD["r"], H - PAD["t"] - PAD["b"]

    def px(x):
        return PAD["l"] + (0 if x1 == x0 else (x - x0) / (x1 - x0) * pw)

    def py(y):
        return PAD["t"] + ph - (y - y0) / (y1 - y0) * ph

    yt, xt = ticks(y0, y1, 5), ticks(x0, x1, 6)
    fy = fmt_for(yt[1] - yt[0]) if len(yt) > 1 else "{:.2f}"
    fx = fmt_for(xt[1] - xt[0]) if len(xt) > 1 else "{:.1f}"

    css = (".bg{fill:%s}.t{fill:%s}.t2{fill:%s}.g{stroke:%s}"
           ".a{stroke:%s}.s1{stroke:%s}.s2{stroke:%s}"
           ".s1f{fill:%s}.s2f{fill:%s}" %
           (LIGHT["bg"], LIGHT["ink"], LIGHT["ink2"], LIGHT["grid"],
            LIGHT["grid"], LIGHT["s1"], LIGHT["s2"], LIGHT["s1"], LIGHT["s2"]))
    css += ("@media(prefers-color-scheme:dark){"
            ".bg{fill:%s}.t{fill:%s}.t2{fill:%s}.g{stroke:%s}"
            ".a{stroke:%s}.s1{stroke:%s}.s2{stroke:%s}"
            ".s1f{fill:%s}.s2f{fill:%s}}" %
            (DARK["bg"], DARK["ink"], DARK["ink2"], DARK["grid"],
             DARK["grid"], DARK["s1"], DARK["s2"], DARK["s1"], DARK["s2"]))
    css += (".g,.a{stroke-width:1}.ln{fill:none;stroke-width:2;"
            "stroke-linejoin:round;stroke-linecap:round}"
            ".t,.t2{font:11px system-ui,sans-serif}"
            ".lb{font:11px system-ui,sans-serif;font-weight:600}"
            ".cap{font:12px system-ui,sans-serif}")

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" role="img">',
         f"<style>{css}</style>",
         f'<rect class="bg" width="{W}" height="{H}"/>']
    for t in yt:
        y = py(t)
        o.append(f'<line class="g" x1="{PAD["l"]}" y1="{y:.1f}" '
                 f'x2="{PAD["l"] + pw}" y2="{y:.1f}"/>')
        o.append(f'<text class="t2" x="{PAD["l"] - 8}" y="{y + 4:.1f}" '
                 f'text-anchor="end">{fy.format(t)}</text>')
    for t in xt:
        if t < x0 or t > x1:
            continue
        o.append(f'<text class="t2" x="{px(t):.1f}" y="{PAD["t"] + ph + 20}" '
                 f'text-anchor="middle">{fx.format(t)}</text>')
    o.append(f'<line class="a" x1="{PAD["l"]}" y1="{PAD["t"] + ph}" '
             f'x2="{PAD["l"] + pw}" y2="{PAD["t"] + ph}"/>')
    o.append(f'<text class="t2" x="{PAD["l"] + pw / 2}" y="{H - 8}" '
             f'text-anchor="middle">{xlabel}</text>')
    o.append(f'<text class="t2" transform="translate(14,{PAD["t"] + ph / 2}) '
             f'rotate(-90)" text-anchor="middle">{ylabel}</text>')

    for label, pts, slot, dashed in series:
        d = " ".join(("M" if i == 0 else "L") + f"{px(x):.1f},{py(y):.1f}"
                     for i, (x, y) in enumerate(pts))
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        o.append(f'<path class="ln s{slot}" d="{d}"{dash}/>')

    # Direct labels, nudged apart so they never overprint.
    lab = sorted(({"t": lb, "s": sl, "x": px(s[-1][0]), "y": py(s[-1][1])}
                  for lb, s, sl, _ in series), key=lambda a: a["y"])
    for i in range(1, len(lab)):
        if lab[i]["y"] - lab[i - 1]["y"] < 13:
            lab[i]["y"] = lab[i - 1]["y"] + 13
    for l in lab:
        o.append(f'<text class="lb s{l["s"]}f" x="{l["x"] + 8:.1f}" '
                 f'y="{l["y"] + 4:.1f}">{l["t"]}</text>')
    if caption:
        o.append(f'<text class="cap t2" x="{PAD["l"]}" y="14">{caption}</text>')
    o.append("</svg>")
    return "".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--field", default="v")
    ap.add_argument("--field2")
    ap.add_argument("--label", default="measured")
    ap.add_argument("--label2", default="")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--per-cell", type=int, default=0)
    ap.add_argument("--ylabel", default="")
    ap.add_argument("--xlabel", default="hours")
    ap.add_argument("--caption", default="")
    args = ap.parse_args()

    rows = load(args.logs)
    if len(rows) < 2:
        sys.exit("need at least 2 samples")
    t0 = rows[0]["ts"]

    def pts(field):
        out = []
        for r in rows:
            v = r.get(field)
            if v is None:
                continue
            v *= args.scale
            if args.per_cell:
                v /= args.per_cell
            out.append(((r["ts"] - t0) / 3600.0, v))
        return out

    series = [(args.label, pts(args.field), 1, False)]
    if args.field2:
        series.append((args.label2 or args.field2, pts(args.field2), 2, False))

    with open(args.out, "w") as f:
        f.write(render(series, args.xlabel, args.ylabel, args.caption))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
