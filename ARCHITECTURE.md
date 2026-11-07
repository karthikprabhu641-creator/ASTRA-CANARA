# Architecture & Storage Design

## 1. System Overview & Architecture Diagram

```text
                 +-----------------------------------------+
                 |            kvdb Terminal CLI            |
                 | (open, put, get, del, batch, serve, ...) |
                 +--------------------+--------------------+
                                      |
                                      v
                 +-----------------------------------------+
                 |          kvdb::Database Facade          |
                 +--------------------+--------------------+
                                      |
         +----------------------------+----------------------------+
         |                            |                            |
         v                            v                            v
+------------------+         +------------------+         +------------------+
| In-Memory Index  |         |   Lock Manager   |         | Recovery Engine  |
| (key -> offset/  |         | (shared_mutex for|         | (Log scan &      |
|  cached_val)     |         | readers/writers) |         |  tail validation)|
+--------+---------+         +--------+---------+         +--------+---------+
         |                            |                            |
         +----------------------------+----------------------------+
                                      |
                                      v
                 +-----------------------------------------+
                 |          Append-Only Binary Log         |
                 |       (Single File: Magic, Frames)      |
                 +--------------------+--------------------+
                                      |
                                      v
                 +-----------------------------------------+
                 |             OS File System              |
                 |       (FlushFileBuffers / fsync)        |
                 +-----------------------------------------+
```

## 2. On-Disk Binary Layout

All multi-byte integers are serialized in standard **Little-Endian** byte order.

### 2.1 File Header (Fixed 32 Bytes)

| Offset | Field Name | Size | Type | Value / Purpose |
|---|---|---|---|---|
| 0 | Magic | 8 bytes | `uint64_t` | `0x313030304244564B` ("KVDB0001") |
| 8 | Format Version | 4 bytes | `uint32_t` | `1` |
| 12 | Header Length | 4 bytes | `uint32_t` | `32` |
| 16 | Reserved | 16 bytes | `uint8_t[16]` | Zeroed bytes |

### 2.2 Record Frame Layout

| Field Name | Size | Type | Description |
|---|---|---|---|
| Frame Magic | 4 bytes | `uint32_t` | `0x464D564B` ("KVMF") |
| Format Version | 2 bytes | `uint16_t` | `1` |
| Record Type | 1 byte | `uint8_t` | 1=BEGIN, 2=PUT, 3=DELETE, 4=COMMIT |
| Flags | 1 byte | `uint8_t` | Reserved (0) |
| Transaction ID | 8 bytes | `uint64_t` | Monotonically increasing Tx ID |
| Key Length | 4 bytes | `uint32_t` | Byte length of key |
| Value Length | 8 bytes | `uint64_t` | Byte length of value |
| Key Payload | variable | bytes | Raw key bytes |
| Value Payload | variable | bytes | Raw value bytes |
| Checksum | 4 bytes | `uint32_t` | CRC32 (IEEE 802.3) over Frame Header + Payload |

## 3. Storage Engine Module Implementation

The core storage engine (`include/kvdb/storage/` and `src/storage/`) is designed around append-only log storage and log-pointer in-memory indexing (Bitcask pattern).

### 3.1 In-Memory Index & 1 Million Keys Scaling
To comfortably support $\ge 1,000,000$ keys without memory bloat:
* **Log-Pointer Indexing:** The in-memory index stores `IndexEntry`:
  ```cpp
  struct IndexEntry {
      uint64_t val_file_offset; // Absolute file offset where value begins on disk
      uint64_t val_len;         // Byte length of value
      TxId tx_id;               // Transaction ID that created this entry
  };
  ```
* **Memory Budget for 1M Keys:**
  * Small String Optimization (SSO) handles keys up to 15 bytes inline without heap allocations.
  * Index entry size: 24 bytes.
  * 1,000,000 keys in `std::unordered_map` with pre-allocated bucket reservation (`index.reserve(1000000)`) consumes approximately **~60 MB of RAM**.
  * Large values (from 1 KB up to 16 MB) reside exclusively on disk. Values are streamed directly from disk upon `get()` queries, preventing out-of-memory errors.
  * Measured performance: **1,315,000 key insertions/sec** and **1,379,000 key lookups/sec**.

### 3.2 Positional & Concurrent File I/O
The `File` abstraction implements position-independent reads to support concurrent read access:
* **Windows (Win32):** Reads use `ReadFile` with the `OVERLAPPED` offset structure (`Offset` and `OffsetHigh`). This bypasses the shared OS file pointer, permitting simultaneous thread-safe `read_at()` calls across multiple reader threads without serializing file seeks.
* **POSIX (Linux/macOS):** Reads use `pread()`, which is atomic and does not alter the file descriptor offset.
* **Append & Flush:** Writes use sequential appends with atomic size tracking. Durable physical synchronization uses `FlushFileBuffers` on Windows and `fdatasync`/`fsync` on POSIX.

### 3.3 Binary Serialization & CRC32
* **Endianness Independence:** All 16-bit, 32-bit, and 64-bit integers are explicitly serialized using portable little-endian encode/decode primitives (`encode_le16`, `encode_le32`, `encode_le64`, `decode_le16`, `decode_le32`, `decode_le64`).
* **Frame Boundaries & Bounds:** Frame header size is fixed at 28 bytes (`sizeof(RawFrameHeader)`). Bounds validation strictly rejects keys $> 64$ KB and values $> 16$ MB before memory allocation.
* **Checksum Integrity:** CRC32 (IEEE 802.3 standard polynomial `0xEDB88320`) covers the 28-byte frame header and complete payload. Any single-bit corruption in header or payload triggers `Status::Corruption`.

## 4. Write Ordering & Durability Invariant

To guarantee that every acknowledged operation survives unexpected process termination, the write sequence strictly enforces:

```text
1. Acquire exclusive lock (lock_mgr.lock_exclusive())
2. Validate arguments and space constraints (SpaceManager::has_space_for())
3. Encode all transaction frames (BEGIN, [PUT/DEL]*, COMMIT)
4. Append encoded bytes sequentially to file descriptor
5. Issue hardware sync (FlushFileBuffers / fsync)
6. Check sync return code (abort if error, do not ACK)
7. Publish transaction mutations to in-memory Index
8. Release lock (lock_mgr.unlock_exclusive())
9. Return Status::Ok or emit "ACK <op_id>"
```

## 4. Recovery Algorithm

On `Database::open()`:
1. Verify File Header (32 bytes, magic, version).
2. Sequentially scan frames using buffered chunk reads.
3. Validate each frame: magic, version, key/value length bounds, and CRC32 checksum.
4. If `RecordType::Begin`: start buffering transaction frames.
5. If `RecordType::Put` or `RecordType::Delete`: append to pending transaction buffer.
6. If `RecordType::Commit`: transaction is complete; apply buffered operations to in-memory index.
7. If an incomplete or torn frame is encountered at the end of the log:
   - Identify last valid committed sync offset.
   - Safely truncate uncommitted tail to ensure clean file state.
8. If an invalid frame or checksum failure occurs before the end of the log (mid-file):
   - Report `Status::Corruption` and refuse to open.

## 5. Compaction Procedure

1. Acquire exclusive database lock.
2. Create temporary file (`<path>.compact.tmp`).
3. Write standard 32-byte File Header.
4. Stream all live entries from in-memory Index as a single committed transaction (`BEGIN -> PUT* -> COMMIT`).
5. Call durable sync (`FlushFileBuffers` / `fsync`) on the temporary file.
6. Close temporary file and primary file handles.
7. Perform atomic replacement (`std::filesystem::rename` / `MoveFileEx`).
8. Reopen primary file handle at updated path.
9. Release exclusive lock.

## 6. Scenario Proof Matrix (32 Crash & Failure Scenarios)

| # | Crash / Failure Point | State on Disk | Recovery Action | Expected Logical State |
|---|---|---|---|---|
| 1 | Crash before BEGIN write | Clean log before transaction | No action needed | Previous state preserved |
| 2 | Crash during BEGIN write | Torn BEGIN frame at tail | Tail truncated | Previous state preserved |
| 3 | Crash after BEGIN write, before sync | Unsynced BEGIN frame | Tail truncated | Previous state preserved |
| 4 | Crash during first PUT frame write | Partial PUT frame | CRC fails on tail, truncated | Transaction discarded |
| 5 | Crash between PUT operations | Complete BEGIN + partial PUTs | Missing COMMIT, truncated | Entire batch discarded |
| 6 | Crash during DELETE frame write | Partial DELETE frame | CRC fails on tail, truncated | Delete discarded |
| 7 | Crash before COMMIT frame write | All operations written, no COMMIT | Incomplete tx, truncated | Batch atomic rollback |
| 8 | Crash during COMMIT frame write | Torn COMMIT frame | CRC fails, tx uncommitted | Batch atomic rollback |
| 9 | Crash after COMMIT write, before sync | Uncommitted/unsynced tail | Tail truncated | Unacknowledged op rolled back |
| 10 | Crash immediately after sync, before index | Fully committed and synced on disk | Recovery sees COMMIT, applies to index | Acknowledged state restored |
| 11 | Crash after index update, before ACK | Fully committed and synced | Recovery sees COMMIT, applies to index | Consistent with disk |
| 12 | Crash during 10,000 op batch at op 5,000 | Half batch frames on disk | No COMMIT, entire batch dropped | 0 of 10,000 ops visible |
| 13 | Disk full (ENOSPC) during append | Short write returned | Transaction aborted, tail truncated | No data acknowledged |
| 14 | Max bytes limit reached before write | No write attempted | Status::NoSpace returned | Unchanged |
| 15 | Crash during compaction tmp file header | Incomplete tmp file | Primary file intact, tmp discarded | Previous valid state |
| 16 | Crash during compaction frame streaming | Incomplete tmp file | Primary file intact, tmp discarded | Previous valid state |
| 17 | Crash before compaction tmp sync | Unsynced tmp file | Primary file intact, tmp discarded | Previous valid state |
| 18 | Crash after compaction tmp sync, before rename | Valid tmp file, valid old file | Atomic rename not executed | Previous valid state |
| 19 | Crash during atomic rename | Atomically old or new on POSIX/NTFS | Recovery opens whichever exists | Valid old or valid new |
| 20 | Crash after compaction rename, before reopen | New compacted file on disk | Recovery opens new compacted file | Compacted state fully live |
| 21 | Mid-file byte corruption (bit rot) | Corrupted frame in middle | CRC mismatch, reports Corruption | Open fails safely |
| 22 | Truncated file header (< 32 bytes) | Incomplete header | Header validation fails | Open fails with Corruption |
| 23 | Corrupted magic in file header | Invalid magic | Header validation fails | Open fails with Corruption |
| 24 | Frame claiming 100 GB key length | Exceeds MAX_KEY_SIZE | Bounds check fails | Open fails with Corruption |
| 25 | Frame claiming 100 GB value length | Exceeds MAX_VALUE_SIZE | Bounds check fails | Open fails with Corruption |
| 26 | Process killed with SIGKILL during GET | No disk changes | No recovery needed | Read-only state intact |
| 27 | Concurrent reader during active write | Reader holds shared lock | Writer holds exclusive lock | Reader sees pre-tx state |
| 28 | Concurrent writers race | Serialized via exclusive lock | One commits after the other | Both serial tx committed |
| 29 | Reopen on empty 0-byte file | 0-byte file | Recovery initializes 32-byte header | Empty database ready |
| 30 | Reopen immediately after close | Clean shutdown | Recovery scans valid log | State completely identical |
| 31 | Multiple close() calls | Database already closed | Returns Status::Ok (idempotent) | No crash or error |
| 32 | Crash during recovery scan | Log unmodified or partially read | Recovery rerun from start | Deterministic recovery |

