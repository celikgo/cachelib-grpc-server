#!/usr/bin/env python3
"""Generate the repository social-preview card (1280x640) from measured data.

Reads bench/results-summary.json so the chart can never drift from the numbers
in BENCHMARKS.md. Throughput and latency are different measures on different
scales, so they get two panels sharing an x-axis -- never a dual y-axis.
"""
import json
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
data = json.loads((ROOT / "bench" / "results-summary.json").read_text())
sweep = data["get_concurrency_sweep"]
conc = sorted(int(k) for k in sweep)
rps = [sweep[str(c)]["rps_median"] for c in conc]
p99 = [sweep[str(c)]["p99_median_ms"] for c in conc]

W, H = 1280, 640
SURFACE, INK, INK2, INK3 = "#1a1a19", "#ffffff", "#c3c2b7", "#807e74"
BLUE, ORANGE, GRID = "#3987e5", "#d95926", "#2e2e2c"

PW, PH = 470, 228          # panel plot area
PY = 302                   # panel top
LX, RX = 90, 720           # panel left origins


def xpos(c, left):
    lo, hi = math.log10(conc[0] or 1), math.log10(conc[-1])
    return left + (math.log10(c) - lo) / (hi - lo) * PW


def ypos(v, vmax):
    return PY + PH - (v / vmax) * PH


def panel(left, title, color, values, vmax, fmt, ticks):
    o = [f'<circle cx="{left+4}" cy="{PY-42}" r="4.5" fill="{color}"/>',
         f'<text x="{left+18}" y="{PY-37}" fill="{INK}" font-size="17" '
         f'font-family="Helvetica,Arial,sans-serif" font-weight="600">{title}</text>']
    for t in ticks:
        y = ypos(t, vmax)
        o.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left+PW}" y2="{y:.1f}" '
                 f'stroke="{GRID}" stroke-width="1"/>')
        o.append(f'<text x="{left-12}" y="{y+4:.1f}" fill="{INK3}" font-size="13" '
                 f'text-anchor="end" font-family="Helvetica,Arial,sans-serif">{fmt(t)}</text>')
    pts = " ".join(f"{xpos(c,left):.1f},{ypos(v,vmax):.1f}" for c, v in zip(conc, values))
    o.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="2.5" '
             f'stroke-linejoin="round" stroke-linecap="round"/>')
    for c, v in zip(conc, values):
        o.append(f'<circle cx="{xpos(c,left):.1f}" cy="{ypos(v,vmax):.1f}" r="4.5" '
                 f'fill="{color}" stroke="{SURFACE}" stroke-width="2"/>')
    for c in conc:
        o.append(f'<text x="{xpos(c,left):.1f}" y="{PY+PH+26}" fill="{INK3}" font-size="13" '
                 f'text-anchor="middle" font-family="Helvetica,Arial,sans-serif">{c}</text>')
    o.append(f'<line x1="{left}" y1="{PY+PH}" x2="{left+PW}" y2="{PY+PH}" '
             f'stroke="{INK3}" stroke-width="1"/>')
    o.append(f'<text x="{left+PW/2}" y="{PY+PH+52}" fill="{INK3}" font-size="13" '
             f'text-anchor="middle" font-family="Helvetica,Arial,sans-serif">concurrency</text>')
    return o


s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
     f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
     f'<text x="{LX-4}" y="88" fill="{INK}" font-size="46" font-weight="700" '
     f'font-family="Helvetica,Arial,sans-serif">cachelib-grpc-server</text>',
     f'<text x="{LX-4}" y="124" fill="{INK2}" font-size="20" '
     f'font-family="Helvetica,Arial,sans-serif">'
     f'A standalone gRPC server for Meta’s CacheLib · 19 RPCs · hybrid DRAM+SSD</text>',
     f'<line x1="{LX-4}" y1="152" x2="{W-LX+4}" y2="152" stroke="{GRID}" stroke-width="1"/>']

hero = [("33,360", "req/s", "GET at concurrency 100"),
        ("6.81 ms", "p99", "at that throughput"),
        ("0.11 ms", "p50", "unloaded round trip")]
for i, (big, unit, sub) in enumerate(hero):
    x = LX - 4 + i * 300
    s.append(f'<text x="{x}" y="212" fill="{INK}" font-size="38" font-weight="700" '
             f'font-family="Helvetica,Arial,sans-serif">{big} '
             f'<tspan fill="{INK3}" font-size="19" font-weight="400">{unit}</tspan></text>')
    s.append(f'<text x="{x}" y="236" fill="{INK3}" font-size="14" '
             f'font-family="Helvetica,Arial,sans-serif">{sub}</text>')

s += panel(LX, "GET throughput", BLUE, rps, 40000,
           lambda v: f"{v//1000:.0f}k" if v else "0", [0, 10000, 20000, 30000, 40000])
s += panel(RX, "GET p99 latency", ORANGE, p99, 30,
           lambda v: f"{v:.0f} ms" if v else "0", [0, 10, 20, 30])

s.append(f'<text x="{LX-4}" y="{H-18}" fill="{INK3}" font-size="13" '
         f'font-family="Helvetica,Arial,sans-serif">'
         f'Apple M2 Max, 12 vCPU, Docker Desktop · 4 GiB DRAM · 200k × 1 KiB keys, '
         f'100% hits · median of 3 runs · harness in bench/</text>')
s.append("</svg>")

out = ROOT / "bench" / "social-preview.svg"
out.write_text("\n".join(s))
print(f"wrote {out}")
