#!/usr/bin/env python3
"""Aggregate repeated sweep runs: report median across repetitions and spread."""
import statistics as st
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from report import summarize  # noqa: E402

levels = [1, 8, 25, 50, 100, 200, 400]
reps = sys.argv[1:] or ["bench/results/rep1", "bench/results/rep2", "bench/results/rep3"]
print(f'{"conc":>5} {"rps (median)":>14} {"spread":>16} {"p50":>7} {"p99":>7} {"p99.9":>7}')
rows = []
for c in levels:
    runs = []
    for r in reps:
        try:
            runs.append(summarize(f"{r}/c{c}.json"))
        except FileNotFoundError:
            pass
    if not runs:
        continue
    rps = [x["rps"] for x in runs]
    row = (c, st.median(rps), min(rps), max(rps),
           st.median(x["p50"] for x in runs),
           st.median(x["p99"] for x in runs),
           st.median(x["p999"] for x in runs))
    rows.append(row)
    print(f'{c:>5} {row[1]:>14,.0f} {min(rps):>7,.0f}-{max(rps):<8,.0f} '
          f'{row[4]:>7.2f} {row[5]:>7.2f} {row[6]:>7.2f}')
