# Shopping Sessions

This entry reconstructs shopping sessions across linked accounts and writes daily user and campaign Parquet tables. The C++17 pipeline parses memory-mapped CSVs, joins identities with union-find and resolves event versions by earliest ingestion. It keeps nine-byte ASCII identifiers in one machine word. Other identifiers retain their full source bytes. Workers overlap link construction with event parsing. A singly occupied hash bucket certifies a unique event ID. Repeated buckets receive full ID and ingestion-time comparisons. Each worker groups its users’ activity in contiguous buffers, orders dense users by half-hour bins, tracks the seven-day attribution window and writes typed Parquet row groups through Apache Arrow’s C++ libraries. [Organizer's rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

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

All 36 organizer fixtures passed. Both tables matched the public million-event answer exactly, with 117,038 user-days and 27,400 campaign-days. Both tables also matched commit `0434f79` on two separately generated 20-million-event inputs. Seed 71 produced 2,241,647 unique user-days and seed 1049 produced 2,254,399. Each produced 28,014 campaign-days. The organizer’s comparator reported zero missing rows, differing rows or duplicate keys.

The comparison below used an Apple M4 with eight workers. Each cell is the median of five warm end-to-end runs, alternating execution order while other builds were running on the host. The official scorer times Linux/amd64 containers on hidden data.

| Generated input | Commit `0434f79` | This version | Reduction |
| --- | ---: | ---: | ---: |
| 20M events, 5M clicks, seed 71 | 3.874 s | 2.328 s | 39.9% |
| 20M events, 5M clicks, seed 1049 | 3.915 s | 2.536 s | 35.2% |

An independent Python implementation verified a 40,000-event case with a wide date range, more than 255 purchase categories and mixed-width Unicode identifiers. It also verified a dense-event case with exact 30-minute boundaries, day transitions and tied click times. The reader uses AVX2, SSE2 or NEON when supported, with a scalar implementation for other architectures.

Generate the larger workload after `make check-dev`.

```sh
.cache/generator/target/release/generate --seed 71 --events 20000000 --clicks 5000000 --output .cache/bench
time .venv/bin/python run.py .cache/bench .cache/bench-output
```
