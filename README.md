# Shopping Sessions

This entry reconstructs shopping sessions across linked accounts and writes daily user and campaign Parquet tables. The C++17 pipeline parses memory-mapped CSVs, joins identities with union-find and resolves event versions by earliest ingestion. It keeps nine-byte ASCII identifiers in one machine word. Other identifiers retain their full source bytes. Workers claim link and event chunks from the same queue. Concurrent union-find always attaches the larger identity to the smaller one. Each successful merge records the child for the final alias index. A singly occupied hash bucket certifies a unique event ID. Repeated buckets receive full ID and ingestion-time comparisons. Each worker groups its users’ activity in contiguous buffers, orders dense users by half-hour bins, tracks the seven-day attribution window and writes typed Parquet row groups through Apache Arrow’s C++ libraries. [Organizer's rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Run locally

Use a C++17 compiler, Python 3.10 or later, Make, curl, and unzip. The million-event generator also needs Cargo with Rust 1.85 or later. Run the commands in this directory.

```sh
make fixtures
make check-dev
./sessions /path/to/input /path/to/output
```

The Make targets fetch the official fixtures, generator, and expected outputs into `.cache/`. The native runner reads the manifest with Apache Arrow’s JSON parser and accepts positional input and output paths or the scorer's `DATA_DIR` and `OUTPUT_DIR`. The existing Python launcher handles additional ISO 8601 window representations. The runner uses up to eight workers from `NCPU`. It writes directly into the two output directories.

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

| Generated input | Commit `0434f79` | Commit `ef19f7f` | Reduction |
| --- | ---: | ---: | ---: |
| 20M events, 5M clicks, seed 71 | 3.874 s | 2.328 s | 39.9% |
| 20M events, 5M clicks, seed 1049 | 3.915 s | 2.536 s | 35.2% |

Commit `0146a00` adopted the native executable as the entrypoint and saved 43 ms at 1M events and 23 ms at 10M events in paired warm runs on the M4. The organizer’s comparator found identical outputs at both sizes. On the 10M input it compared 1,134,699 user-days and 28,014 campaign-days.

The current CSV scanner checks quote placement using the separators on both sides of each 64-byte block. It selects AVX2 or SSE2 once per chunk on x86. The identity index stores aliases whose owner differs from their original ID. A compact membership bitmap skips lookups when no alias can have that hash. Within each user shard, the grouping table uses hash bits above those that selected the shard.

These end-to-end medians compare six measured pairs after one warm-up. The host was also running other builds. The official comparator matched every row of both tables against commit `0146a00`, including 2,241,391 user-days and 28,014 campaign-days on the new 20M seed.

| Input | Commit `0146a00` | Current pipeline | CPU time before and after |
| --- | ---: | ---: | ---: |
| 10M events, 2.5M clicks | 0.922 s | 0.749 s | 4.918 s to 3.925 s |
| 20M events, 5M clicks, seed 4027 | 2.143 s | 1.678 s | 10.355 s to 8.236 s |

A separate parser check compared 168,000 generated lines against an independent implementation, including malformed quotes at vector boundaries and rows without a final newline.

An independent Python implementation verified a 40,000-event case with a wide date range, more than 255 purchase categories and mixed-width Unicode identifiers. It also verified a dense-event case with exact 30-minute boundaries, day transitions and tied click times. The reader uses AVX2, SSE2 or NEON when supported, with a scalar implementation for other architectures.

The parallel identity graph matched commit `2d4625f` on an independent 20-million-event, 5-million-click dataset (seed 513917). Both tables matched across 2,239,293 user-days and 28,014 campaign-days. A separate 50,000-identity chain mixed ASCII and UTF-8 names, reversed duplicate edges, and 24 input files. Its expected single session and attributed campaign matched with one, two and eight workers.

Generate the larger workload after `make check-dev`.

```sh
.cache/generator/target/release/generate --seed 71 --events 20000000 --clicks 5000000 --output .cache/bench
time ./sessions .cache/bench .cache/bench-output
```
