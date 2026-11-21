# Shopping Sessions

This entry reconstructs shopping sessions across linked accounts and writes daily user and campaign Parquet tables. The C++17 pipeline parses memory-mapped CSVs, joins identities with union-find and resolves event versions by earliest ingestion. It keeps nine-byte ASCII identifiers in one machine word. Other identifiers retain their full source bytes. Each worker sorts its users’ events and clicks, tracks the seven-day attribution window and writes typed Parquet row groups directly through Apache Arrow’s C++ libraries. [Organizer's rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Run locally

Use a C++17 compiler, Python 3.10 or later, Make, curl, and unzip. The million-event generator also needs Cargo with Rust 1.85 or later. Run the commands in this directory.

```sh
make fixtures
make check-dev
.venv/bin/python run.py /path/to/input /path/to/output
```

The Make targets fetch the official fixtures, generator, and expected outputs into `.cache/`. The runner accepts positional input and output paths or the scorer's `DATA_DIR` and `OUTPUT_DIR`. The runner uses up to eight workers from `NCPU`. It writes directly into the two output directories.

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

All 36 organizer fixtures passed. Both tables matched the public million-event answer exactly, with 117,038 user-days and 27,400 campaign-days. On a separate generator seed with 20 million events and 5 million clicks, all 2,241,647 user-days and 28,014 campaign-days matched the previous implementation, including nulls and duplicate-key checks.

End-to-end times on an Apple M4 were medians of five alternating runs per version, with eight workers and warm input files. The baselines used commit `df65661`. The hosted scorer uses a Linux/amd64 container and hidden data.

| Generated input | Previous version | Current version |
| --- | ---: | ---: |
| 1M events, 250k clicks, seed 1 | 0.187 s | 0.085 s |
| 20M events, 5M clicks, seed 71 | 2.438 s | 1.379 s |

Generate the larger workload after `make check-dev`.

```sh
.cache/generator/target/release/generate --seed 71 --events 20000000 --clicks 5000000 --output .cache/bench
time .venv/bin/python run.py .cache/bench .cache/bench-output
```
