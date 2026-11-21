# Shopping Sessions

This entry reconstructs shopping sessions across linked accounts and writes daily user and campaign Parquet tables. The native C++17 stage memory-maps the inputs, connects IDs with union-find, resolves event versions by earliest ingestion time, and sorts each person's events and clicks. A moving seven-day window tracks each assisting campaign and the click that receives credit. Polars writes the final tables with Parquet dates and nullable purchase categories. [Organizer's rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Run locally

Use a C++17 compiler, Python 3.10 or later, Make, curl, and unzip. The million-event generator also needs Cargo with Rust 1.85 or later. Run the commands in this directory.

```sh
make fixtures
make check-dev
.venv/bin/python run.py /path/to/input /path/to/output
```

The Make targets fetch the official fixtures, generator, and expected outputs into `.cache/`. The runner accepts positional input and output paths or the scorer's `DATA_DIR` and `OUTPUT_DIR`. It uses `SCRATCH_DIR` for temporary files and up to eight workers from `NCPU`.

## Run the competition image

```sh
docker build --platform linux/amd64 -t shopping-sessions .
mkdir -p .cache/docker-output .cache/docker-scratch
chmod a+rwx .cache/docker-output .cache/docker-scratch
docker run --rm --platform linux/amd64 --network none --read-only \
  --cpus 8 --memory 24g --memory-swap 24g --user 1000:1000 \
  --tmpfs /tmp:rw,size=64m,mode=1777 -e HOME=/scratch -e NCPU=8 \
  -v "$PWD/.cache/dev:/data:ro" -v "$PWD/.cache/docker-output:/output" \
  -v "$PWD/.cache/docker-scratch:/scratch" shopping-sessions
.venv/bin/python .cache/starter-duckdb/scripts/compare.py .cache/docker-output .cache/dev-answer
```

Use this directory as the public repository root and submit its Git commit on the [track page](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions).

## Validation and timing

All 36 organizer fixtures passed. The public million-event development answer matched exactly across 117,038 user rows and 27,400 campaign rows. An independent Python implementation also matched both output tables for generator seed 78 with 40,000 events and 10,000 clicks.

On an Apple M4, generator seed 42 with 6 million events and 1.5 million clicks took **0.857 seconds** locally, compared with **1.194 seconds** for the initial sorting-based implementation. These are medians of five warm end-to-end runs per version, alternating execution order. Both versions produced identical results across 685,322 user rows and 28,014 campaign rows. The official scorer times a Linux/amd64 container on hidden data.

The following commands reproduce the six-million-event workload after `make check-dev`.

```sh
.cache/generator/target/release/generate --seed 42 --events 6000000 --clicks 1500000 --output .cache/bench
time .venv/bin/python run.py .cache/bench .cache/bench-output
```
