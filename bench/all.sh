#!/usr/bin/env bash
# The one committed command that regenerates every published benchmark number.
#
#   ./bench/all.sh [image]
#
# Runs the operation mix once and the concurrency sweep three times (the
# "median of 3" every table claims), writes bench/results-summary.json from the
# raw ghz output, and re-renders the tables in BENCHMARKS.md and README.md.
#
# Nothing here is hand-transcribed: bench/summarize.py derives the JSON from
# the runs, bench/render.py derives the markdown from the JSON, and CI's
# `python3 bench/render.py --check` fails if the markdown and the JSON drift.
#
# Takes a couple of hours. Run it on an otherwise idle machine -- the numbers
# are only worth publishing if the load generator had the CPUs it was pinned to.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
IMAGE="${1:-ghcr.io/celikgo/cachelib-grpc-server:latest}"
REPS="${REPS:-3}"
OUT="$HERE/results"

echo "==> Pulling $IMAGE so the digest recorded is the digest measured"
docker pull --quiet "$IMAGE"

echo "==> Operation mix"
"$HERE/run.sh" "$IMAGE" "$OUT"

REPDIRS=()
for i in $(seq 1 "$REPS"); do
  echo "==> Concurrency sweep, repetition $i of $REPS"
  "$HERE/sweep.sh" "$IMAGE" "$OUT/rep$i"
  REPDIRS+=("$OUT/rep$i")
done

echo "==> Summarising"
python3 "$HERE/summarize.py" "$OUT" "${REPDIRS[@]}" > "$HERE/results-summary.json"

echo "==> Rendering"
python3 "$HERE/render.py"

echo
echo "Done. Review the diff before committing:"
echo "  git -C $ROOT diff bench/results-summary.json BENCHMARKS.md README.md"
