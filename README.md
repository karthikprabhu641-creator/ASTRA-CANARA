# Embedded Key-Value Database

A single-process, file-backed key-value database implemented from scratch in **C++17** for the 24-hour engineering hackathon.

The project is intentionally focused on storage-engine fundamentals: durable writes, crash recovery, atomic batches, concurrent access, compaction, and independent failure testing.

> **Status:** Initial project documentation. The team must review and approve `CONTRACT.md` before implementation details are treated as final.

## Project goals

- Store key-value data in one database file.
- Support `PUT`, `GET`, `DELETE`, and atomic `BATCH`.
- Recover acknowledged operations after an abrupt process termination.
- Support at least 1,000,000 keys and batches of up to 10,000 operations.
- Support concurrent readers and writers within one process.
- Compact the file so obsolete records do not grow indefinitely.
- Enforce a configurable maximum file size and report capacity errors safely.
- Provide a terminal CLI and an independent torture-testing tool.

## Non-goals and restrictions

- No GUI, web application, or network API is required.
- Do not use an existing database/storage engine or database implementation library (for example SQLite, Redis, RocksDB, or LevelDB).
- Do not copy an existing database engine's source code.
- Standard C++ libraries and ordinary OS file-system calls are allowed. Confirm any additional dependency against the hackathon rules before using it.

## Proposed architecture

```text
                Terminal CLI
                     |
                     v
              Database API
                     |
          +----------+----------+
          |                     |
          v                     v
   In-memory hash index    Synchronization
   (key -> value/offset)   (locks)
          |
          v
   Append-only binary log
          |
          v
     One database file

On open:
  scan log -> validate frames -> identify committed transactions
  -> rebuild index -> serve requests

During maintenance:
  compact live state -> temporary file -> durable replacement
```

The design is an append-only log with an in-memory hash index. The exact record layout, transaction protocol, and failure semantics are specified in `CONTRACT.md` and must be agreed by the team before implementation.

## Repository layout

The team may adjust filenames, but keep module boundaries clear.

```text
.
├── include/
│   └── kvdb/
├── src/
│   ├── storage/
│   ├── recovery/
│   ├── concurrency/
│   └── cli/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── torture/
├── docs/
├── CMakeLists.txt
├── README.md
├── CONTRACT.md
├── ARCHITECTURE.md
├── BREAKING.md
└── PROMPTS.md
```

## Build

The project uses CMake and C++17.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable name and exact build targets should be kept consistent with the final implementation. The examples below assume the executable is named `kvdb`.

## Phase 5 torture workload

The additive Phase 5 runner is built as `torture_tool_phase5`, leaving the existing Phase 4 `torture_tool` target unchanged. It supports reproducible seeded workloads and concurrent workers:

```bash
./build/torture_tool_phase5 --seed 1337 --iterations 500 --threads 4
./build/torture_tool_phase5 --mode batch --db batch-test.db --oracle batch-test.ack.log
```

The `all` mode uses the 50/30/15/5 PUT/GET/DEL/BATCH distribution. Batch mode generates batches of 1–500 operations. `--iterations` is the total number of top-level operations shared across workers.

## Phase 6 crash/recovery torture

The separate `torture_tool_phase6` target starts `kvdb serve` as a child process, sends acknowledged operations, abruptly kills the child after each workload cycle, then runs `kvdb check` and `kvdb dump` to verify recovered state against the persistent ACK oracle. Each cycle also submits a batch and kills the server before acknowledging it; if no ACK was observed, the batch must be absent after recovery.

```bash
./build/torture_tool_phase6 --seed 1337 --iterations 500 --mode all
./build/torture_tool_phase6 --seed 42 --iterations 50 --mode crash --reset
./build/torture_tool_phase6 --kvdb ./build/kvdb --db ./tmp/crash.db --oracle ./tmp/crash.ack.log
```

By default the Phase 6 runner uses separate `torture_phase6.db` and `torture_phase6.ack.log` files and looks for the `kvdb` executable beside itself. Use `--reset` only when intentionally discarding those specified test files. Modes are `all` (mixed workload plus crash batches), `batch` (batch workload plus crash batches), and `crash` (only interrupted-batch crash cycles).

## Phase 7 oracle correctness

The ACK oracle supports atomic batch journal records and continues to replay the legacy PUT/DEL record format. The Phase 5 runner serializes each database operation with its corresponding oracle update, preserving commit order in the expected state while worker threads submit workloads. Run `oracle_tests` to validate batch replay, concurrent journal writes, malformed-record rejection, and torn-tail recovery.

## CLI interface

The following command shape is the proposed judge-facing interface:

```bash
./kvdb open <path> [--max-bytes N]
./kvdb put <path> <key> <value>
./kvdb get <path> <key>
./kvdb del <path> <key>
./kvdb batch <path> <file>
./kvdb dump <path>
./kvdb check <path>
./kvdb serve <path>
```

The team must confirm the exact argument grammar and output format. In particular:

- `get` must distinguish a missing key from an empty value.
- Mutating commands must report success only after the durability condition in `CONTRACT.md` is met.
- `batch` input format must be documented and validated.
- `dump` and `check` must not silently repair or mutate data unless that behavior is explicitly documented.
- `serve` must emit `ACK <op-id>` only after the corresponding operation is durable.

## Development workflow

1. Read and agree on `CONTRACT.md` before implementing modules.
2. Work on your assigned feature branch; do not push directly to `main`.
3. Keep changes within your module unless coordinated with the team.
4. Add or update tests with every meaningful change.
5. Open a pull request targeting `main`.
6. Include a summary, test commands/results, and known limitations in the PR.
7. The team lead reviews and runs integration tests before merging.

Suggested branches:

- `feature/storage-engine`
- `feature/crash-recovery`
- `feature/concurrency-compaction`
- `feature/cli-torture`

## Testing checklist

- [ ] PUT then GET returns the exact value.
- [ ] DELETE makes the key absent.
- [ ] Empty keys/values and large values behave as documented.
- [ ] Reopening the database rebuilds the same logical state.
- [ ] Acknowledged operations survive forced process termination.
- [ ] An interrupted/unacknowledged operation is either fully present or fully absent.
- [ ] A batch is recovered entirely or not at all.
- [ ] Concurrent operations do not corrupt the index or log.
- [ ] Compaction preserves the logical key-value state.
- [ ] A crash during compaction leaves a valid old or new database.
- [ ] Maximum-size and disk-full failures do not acknowledge unwritten data.
- [ ] The torture tool compares recovered state against an independently persisted ACK oracle.

## Hackathon deliverables

- Database engine and terminal CLI.
- Torture-testing tool with reproducible seeds and crash/restart testing.
- `ARCHITECTURE.md`: architecture, on-disk layout, write ordering, recovery, locks, and scenario proof table.
- `BREAKING.md`: crash points, reasoning, weaknesses, failed attempts, and future improvements.
- `PROMPTS.md`: chronological, unedited AI prompts with tool and approximate time, as required by the event rules.

## Important engineering note

A successful `write()` or C++ stream `flush()` alone does **not** prove that data has reached durable storage. The implementation must use the platform's durability primitive (for example, `fsync()` on supported POSIX systems), handle errors, and acknowledge success only after the agreed commit protocol is durable. See `CONTRACT.md`.

## Team decision log

Before implementation, record decisions here or in `CONTRACT.md`:

- [ ] Target operating system and supported file-system assumptions.
- [ ] Exact binary frame layout and checksum algorithm.
- [ ] Transaction and commit-marker protocol.
- [ ] Locking model and process-exclusivity policy.
- [ ] Batch input format and CLI output format.
- [ ] Maximum key/value sizes and file-size behavior.
- [ ] Compaction replacement and directory-sync procedure.
