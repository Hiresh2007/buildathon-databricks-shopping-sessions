# Shopping Sessions

This entry reconstructs shopping sessions across linked accounts and writes daily user and campaign Parquet tables. The C++17 pipeline parses memory-mapped CSVs, joins identities with union-find and resolves event versions by earliest ingestion. It keeps eight-byte ASCII identifiers and nine-byte ASCII identifiers with a nonzero leading byte in one machine word. Other identifiers retain their full source bytes. Workers claim link and event chunks from the same queue. Concurrent union-find always attaches the larger identity to the smaller one. Each successful merge records the child for the final alias index. A singly occupied hash bucket certifies a unique event ID. Repeated buckets receive full ID and ingestion-time comparisons. Each worker groups its users’ activity in contiguous buffers, orders dense users by half-hour bins, tracks the seven-day attribution window and writes plain Parquet pages with buffered POSIX writes. The executable encodes typed columns, optional definition levels and Compact Protocol metadata directly. [Organizer's rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Run locally

Use a C++17 compiler, Python 3.10 or later, Make, curl, and unzip. The million-event generator also needs Cargo with Rust 1.85 or later. Run the commands in this directory.

```sh
make fixtures
make check-dev
./sessions /path/to/input /path/to/output
```

The Make targets fetch the official fixtures, generator, and expected outputs into `.cache/`. The native runner reads the two timestamp fields of the manifest directly and accepts positional input and output paths or the scorer's `DATA_DIR` and `OUTPUT_DIR`. The Python launcher handles escaped JSON strings, additional manifest fields and additional ISO 8601 window representations. The runner uses up to eight workers from `NCPU`. It writes directly into the two output directories.

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

The block scanner checks quote placement using the separators on both sides of each 64-byte block. It selects AVX2 or SSE2 once per chunk on x86. The identity index stores aliases whose owner differs from their original ID. A compact membership bitmap skips lookups when no alias can have that hash. Within each user shard, the grouping table uses hash bits above those that selected the shard.

These end-to-end medians compare six measured pairs after one warm-up. The host was also running other builds. The official comparator matched every row of both tables against commit `0146a00`, including 2,241,391 user-days and 28,014 campaign-days on the new 20M seed.

| Input | Commit `0146a00` | Current pipeline | CPU time before and after |
| --- | ---: | ---: | ---: |
| 10M events, 2.5M clicks | 0.922 s | 0.749 s | 4.918 s to 3.925 s |
| 20M events, 5M clicks, seed 4027 | 2.143 s | 1.678 s | 10.355 s to 8.236 s |

A separate parser check compared 168,000 generated lines against an independent implementation, including malformed quotes at vector boundaries and rows without a final newline.

An independent Python implementation verified a 40,000-event case with a wide date range, more than 255 purchase categories and mixed-width Unicode identifiers. It also verified a dense-event case with exact 30-minute boundaries, day transitions and tied click times. The reader uses AVX2, SSE2 or NEON when supported, with a scalar implementation for other architectures.

The parallel identity graph matched commit `2d4625f` on an independent 20-million-event, 5-million-click dataset (seed 513917). Both tables matched across 2,239,293 user-days and 28,014 campaign-days. A separate 50,000-identity chain mixed ASCII and UTF-8 names, reversed duplicate edges, and 24 input files. Its expected single session and attributed campaign matched with one, two and eight workers.

The eight- and nine-byte identifier encoding round-tripped 103,072 ASCII and binary strings and matched one million bytewise ordering and equality comparisons. Both output tables matched the organizer’s million-event answers and the previous implementation on an independent 10-million-event input (seed 76129).

The earlier batch scanner validated complete rows and indexed their field boundaries. For windows and purchase categories that fit the packed representation, sorting an eight-byte event key orders it by timestamp and preserves the purchase data. Wider inputs retain their full times and categories.

On the Apple M4, alternating ten warm measured runs per build on seed 76129 (10M events, 2.5M clicks) gave medians of 0.8822 s for `3e79282` and 0.8252 s for this pipeline. CPU time fell from 3.3992 s to 3.2198 s. All rows matched. A sanitizer build matched an independent CSV decoder on 210,000 generated lines across the link, click and event schemas, including malformed rows, vector boundaries and unterminated final lines.

The plain writer on this branch matched all 36 organizer fixtures on Linux/amd64. Both tested CPU feature sets produced the complete public million-event answers. On a separate 10-million-event dataset, the organizer comparator found 1,140,271 user-days and 28,014 campaign-days with zero differences. In six warm pairs on an Apple M4 with eight workers, median wall time changed from 0.6363 to 0.5940 seconds against commit `39b5ea5`. CPU time changed from 3.0675 to 3.0075 seconds. A nominal 60-million-event stress test used six hard-linked copies of the 10-million-event inputs. It used 7,641 MiB peak RSS and produced the same tables.

The layout cache handles records up to 128 bytes. It verifies the complete delimiter and quote masks before reusing field offsets. Common event and click widths have specialized field readers. Longer records, other widths and machines without SIMD use the general decoder. Fixed-width UTC ingestion timestamps compare chronologically as bytes during deduplication. The event validator only decodes the ingestion time of day when the date alone leaves the one-day lateness test undecided.

On the Apple M4 using eight workers, ten alternating warm pairs on seed 76129 (10M events and 2.5M clicks) measured 0.5247 s for commit `b30566c` and 0.4931 s for this scanner. Median CPU time changed from 2.9924 s to 2.8047 s. Both output tables matched, covering 1,140,271 user-days and 28,014 campaign-days. On the public million-event input, twenty alternating pairs measured 88.76 ms and 89.21 ms wall time, and 305.13 ms and 290.31 ms CPU time. A sanitizer test compared 1.2 million timestamp inputs with the prior decoder.

The reader divides each input file into 2 MiB ranges before reading its contents. Workers skip the header and partial first record, finish any record crossing their range, and ask the kernel to begin reading up to two chunks per worker ahead. The x86 scanner reads separator masks directly at each record start. NEON retains block scanning. The Linux/amd64 image selects an x86-64-v3 executable when supported and a baseline executable on other x86-64 CPUs.

On a separate eight-worker Apple M4 comparison against commit `30a635d`, eight cold-file pairs on the 10M dataset measured 1.3926 s and 1.1279 s median wall time, with 5.5572 s and 4.4178 s CPU time. Each run used a fresh APFS file clone and `mincore` confirmed zero of 58,561 input pages were resident before execution. Ten warm pairs measured 0.4253 s and 0.4212 s wall time, with 2.4581 s and 2.3782 s CPU time. All 1,140,271 user-days and 28,014 campaign-days matched. Six hard-linked copies of these inputs produced the same complete answers at nominal 60M-event scale and reached 8,497 MiB peak RSS. Separately, both x86 scanning paths matched an independent decoder's row counts and field digests across a combined 210,000 generated records.

Generate the larger workload after `make check-dev`.

```sh
.cache/generator/target/release/generate --seed 71 --events 20000000 --clicks 5000000 --output .cache/bench
time ./sessions .cache/bench .cache/bench-output
```
