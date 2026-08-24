#!/usr/bin/env python3
"""Render the benchmark tables in BENCHMARKS.md and README.md from measurements.

The numbers a reader sees must be derivable from a committed measurement file,
not transcribed by hand. `bench/results-summary.json` is the measurement; the
markdown tables between the GENERATED markers are its rendering.

    python3 bench/render.py            # rewrite the tables in place
    python3 bench/render.py --check    # fail if the committed tables have drifted

`--check` runs in CI in about a second, so a table that no longer matches the
measurement is caught on the pull request that caused it rather than being
discovered by a reader trying to reproduce it.

Regenerate the measurement itself with bench/run.sh and bench/sweep.sh, then
bench/aggregate.py; see BENCHMARKS.md for the procedure and the hardware.
"""
import argparse
import decimal
import difflib
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUMMARY = ROOT / "bench" / "results-summary.json"

# Concurrency levels highlighted in the README's condensed table. The full
# sweep always renders every level present in the measurement.
README_LEVELS = [1, 25, 100, 400]
README_KNEE = 100


def _round(v, places):
    """Round half away from zero.

    Python's default is round-half-to-even, which turns a measured 5738.5 into
    5738 and leaves a reader comparing the table against rps_runs in the JSON
    wondering which of the two is wrong.
    """
    q = decimal.Decimal(1).scaleb(-places)
    return decimal.Decimal(repr(v)).quantize(q, rounding=decimal.ROUND_HALF_UP)


def _fmt_rps(v):
    return f"{_round(v, 0):,}"


def _fmt_ms(v):
    return f"{_round(v, 2)} ms"


def sweep_rows(sweep):
    return sorted(((int(k), v) for k, v in sweep.items()), key=lambda kv: kv[0])


def render_environment(env, requests_per_measurement):
    short = env["image_digest"].split(":", 1)[1][:12]
    gen = env["load_generator"].removeprefix("ghz ")
    return "\n".join([
        "| | |",
        "|---|---|",
        f'| Host | {env["host"]} |',
        f'| Container runtime | {env["runtime"]} |',
        f'| VM resources | {env["vm_resources"]} |',
        f'| Server image | `{env["image"]}` (`sha256:{short}`) |',
        f'| Server CPUs | pinned to cores {env["server_cpus"]} |',
        f'| Load generator | [ghz](https://ghz.sh) {gen}, pinned to cores'
        f' {env["client_cpus"]} |',
        f'| Cache | {env["cache"]} |',
        f'| Working set | {env["workset"]} |',
        f'| Requests per measurement | {requests_per_measurement:,} |',
    ])


def render_sweep(sweep):
    lines = [
        "| Concurrency | Throughput (req/s) | Range across runs | p50 | p99 | p99.9 |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for c, r in sweep_rows(sweep):
        runs = r["rps_runs"]
        lines.append(
            f'| {c} | {_fmt_rps(r["rps_median"])} | '
            f'{_fmt_rps(min(runs))} – {_fmt_rps(max(runs))} | '
            f'{_fmt_ms(r["p50_median_ms"])} | {_fmt_ms(r["p99_median_ms"])} | '
            f'{_fmt_ms(r["p999_median_ms"])} |'
        )
    return "\n".join(lines)


def render_readme_sweep(sweep):
    lines = [
        "| Concurrency | Throughput | p50 | p99 | p99.9 |",
        "|---:|---:|---:|---:|---:|",
    ]
    for c, r in sweep_rows(sweep):
        if c not in README_LEVELS:
            continue
        cells = [
            str(c),
            f'{_fmt_rps(r["rps_median"])} req/s',
            _fmt_ms(r["p50_median_ms"]),
            _fmt_ms(r["p99_median_ms"]),
            _fmt_ms(r["p999_median_ms"]),
        ]
        if c == README_KNEE:
            cells = [f"**{x}**" for x in cells]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


OP_LABELS = {
    "get": "`Get` (100% hit)",
    "set": "`Set`",
    "incr": "`Incr` (rate-limit bucket)",
    "ping": "`Ping` (transport floor)",
}


def render_ops(ops):
    lines = [
        "| Operation | Throughput (req/s) | p50 | p99 | p99.9 | Errors |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for key, label in OP_LABELS.items():
        o = ops[key]
        errors = o["count"] - o["ok"]
        lines.append(
            f'| {label} | {_fmt_rps(o["rps"])} | {_fmt_ms(o["p50"])} | '
            f'{_fmt_ms(o["p99"])} | {_fmt_ms(o["p999"])} | {errors} |'
        )
    return "\n".join(lines)


def render_get_vs_ping(ops):
    """The one derived claim in the prose: Get as a fraction of the Ping floor.

    It is generated rather than written so it cannot drift away from the table
    directly above it.
    """
    pct = 100.0 * ops["get"]["rps"] / ops["ping"]["rps"]
    return f"{pct:.0f}%"


def blocks(data):
    env = data["environment"]
    ops = data["operation_mix"]
    sweep = data["get_concurrency_sweep"]
    n = data["environment"]["requests_per_measurement"]
    return {
        "environment": render_environment(env, n),
        "sweep": render_sweep(sweep),
        "readme-sweep": render_readme_sweep(sweep),
        "operations": render_ops(ops),
        "get-vs-ping": render_get_vs_ping(ops),
    }


# Two forms. A block marker sits on its own line and wraps a table; an inline
# marker sits inside a sentence and wraps a single derived figure, so a number
# quoted in prose cannot drift away from the table it summarises either.
MARKER = re.compile(
    r"(<!-- BEGIN GENERATED: (?P<name>[a-z-]+) -->(?P<nl>\n?))"
    r"(?P<body>.*?)"
    r"(<!-- END GENERATED: (?P=name) -->)",
    re.DOTALL,
)


def apply_blocks(text, rendered, path):
    seen = []

    def sub(m):
        name = m.group("name")
        seen.append(name)
        if name not in rendered:
            raise SystemExit(f"{path}: unknown generated block '{name}'")
        sep = "\n" if m.group("nl") else ""
        return f"{m.group(1)}{rendered[name]}{sep}{m.group(5)}"

    out = MARKER.sub(sub, text)
    if not seen:
        raise SystemExit(
            f"{path}: no GENERATED markers found. Wrap each table in\n"
            f"  <!-- BEGIN GENERATED: <name> -->\n  ...\n  <!-- END GENERATED: <name> -->"
        )
    return out


TARGETS = ["BENCHMARKS.md", "README.md"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if a committed table has drifted from the measurement")
    args = ap.parse_args()

    if not SUMMARY.exists():
        raise SystemExit(f"missing measurement file: {SUMMARY}")
    data = json.loads(SUMMARY.read_text())
    rendered = blocks(data)

    stale = []
    for name in TARGETS:
        path = ROOT / name
        before = path.read_text()
        after = apply_blocks(before, rendered, name)
        if before == after:
            continue
        if args.check:
            stale.append((name, before, after))
        else:
            path.write_text(after)
            print(f"updated {name}")

    if args.check:
        if stale:
            for name, before, after in stale:
                print(f"::error file={name}::benchmark tables do not match "
                      f"bench/results-summary.json; run: python3 bench/render.py")
                sys.stdout.writelines(difflib.unified_diff(
                    before.splitlines(keepends=True),
                    after.splitlines(keepends=True),
                    fromfile=f"{name} (committed)",
                    tofile=f"{name} (from measurement)",
                ))
            return 1
        print(f"benchmark tables match {SUMMARY.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
