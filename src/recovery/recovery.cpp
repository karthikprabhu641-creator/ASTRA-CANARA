#include "kvdb/recovery/recovery.hpp"
#include "kvdb/storage/format.hpp"
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>
#if defined(KVDB_ENABLE_CRASH_HOOKS)
#include "crash_hook.hpp"
#endif

namespace kvdb::recovery {

namespace {

constexpr std::size_t CHUNK_SIZE = 1024 * 1024; // 1 MB scan buffer

struct StagedOp {
    RecordType type;
    std::string key;
    std::uint64_t val_offset;
    std::uint64_t val_len;
    TxId tx_id;
};

// Helper: Scan remaining file to check if any valid COMMIT frame exists past broken_offset
bool has_valid_commits_ahead(storage::File& file, std::uint64_t from_offset, std::uint64_t file_len) {
    if (from_offset >= file_len) return false;

    std::vector<std::uint8_t> buf(CHUNK_SIZE);
    std::uint64_t cur = from_offset;

    while (cur < file_len) {
        std::size_t to_read = static_cast<std::size_t>(std::min<std::uint64_t>(CHUNK_SIZE, file_len - cur));
        if (!is_ok(file.read_at(cur, buf.data(), to_read))) {
            return false;
        }

        // Search for FRAME_MAGIC
        for (std::size_t i = 0; i + storage::FRAME_HEADER_SIZE <= to_read; ++i) {
            std::uint32_t magic = storage::decode_le32(buf.data() + i);
            if (magic == FRAME_MAGIC) {
                storage::FrameHeader hdr;
                if (is_ok(storage::deserialize_frame_header(buf.data() + i, to_read - i, hdr))) {
                    if (hdr.type == RecordType::Commit) {
                        // Check if full commit frame has valid CRC32
                        std::size_t total_frame = hdr.total_frame_size();
                        if (i + total_frame <= to_read) {
                            std::uint32_t exp_crc = storage::decode_le32(buf.data() + i + total_frame - storage::CRC32_SIZE);
                            std::uint32_t comp_crc = storage::calculate_crc32(buf.data() + i, total_frame - storage::CRC32_SIZE);
                            if (exp_crc == comp_crc) {
                                return true; // Found a valid commit ahead -> mid-file corruption!
                            }
                        }
                    }
                }
            }
        }
        cur += to_read;
    }
    return false;
}

} // namespace

RecoveryStats RecoveryEngine::recover(storage::File& file, storage::Index& index) {
    auto start_time = std::chrono::high_resolution_clock::now();
    RecoveryStats stats;
    index.clear();

    std::uint64_t file_len = file.size();

    // 1. Empty database file: initialize with File Header
    if (file_len == 0) {
        auto hdr_bytes = storage::serialize_file_header();
        Status s = file.append(hdr_bytes.data(), hdr_bytes.size());
        if (!is_ok(s)) {
            stats.status = s;
            return stats;
        }
        s = file.sync();
        if (!is_ok(s)) {
            stats.status = s;
            return stats;
        }
        stats.valid_bytes = hdr_bytes.size();
        stats.status = Status::Ok;
        stats.clean_shutdown = true;
        return stats;
    }

    // 2. Header check
    if (file_len < storage::FILE_HEADER_SIZE) {
        stats.status = Status::Corruption;
        return stats;
    }

    std::vector<std::uint8_t> hdr_buf(storage::FILE_HEADER_SIZE);
    Status s = file.read_at(0, hdr_buf.data(), hdr_buf.size());
    if (!is_ok(s)) {
        stats.status = s;
        return stats;
    }

    s = storage::deserialize_file_header(hdr_buf.data(), hdr_buf.size());
    if (!is_ok(s)) {
        stats.status = Status::Corruption;
        return stats;
    }

    // 3. Chunked Frame Scan
    std::uint64_t last_committed_offset = storage::FILE_HEADER_SIZE;
    std::uint64_t file_read_pos = storage::FILE_HEADER_SIZE;
    std::vector<std::uint8_t> buffer;
    std::size_t buffer_cursor = 0;

    std::vector<StagedOp> current_tx_ops;
    TxId active_tx_id = 0;
    bool in_tx = false;
    bool encountered_tail_issue = false;

#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("recovery_scan");
#endif
    while (file_read_pos < file_len || buffer_cursor < buffer.size()) {
        // Refill buffer if fewer than minimum frame bytes remain
        std::size_t remaining_in_buffer = buffer.size() - buffer_cursor;
        if (remaining_in_buffer < storage::FRAME_HEADER_SIZE && file_read_pos < file_len) {
            std::vector<std::uint8_t> next_buf;
            next_buf.reserve(remaining_in_buffer + CHUNK_SIZE);
            if (remaining_in_buffer > 0) {
                next_buf.insert(next_buf.end(), buffer.begin() + static_cast<std::ptrdiff_t>(buffer_cursor), buffer.end());
            }

            std::size_t to_read = static_cast<std::size_t>(std::min<std::uint64_t>(CHUNK_SIZE, file_len - file_read_pos));
            std::size_t old_size = next_buf.size();
            next_buf.resize(old_size + to_read);
            s = file.read_at(file_read_pos, next_buf.data() + old_size, to_read);
            if (!is_ok(s)) {
                stats.status = s;
                return stats;
            }
            file_read_pos += to_read;
            buffer = std::move(next_buf);
            buffer_cursor = 0;
            remaining_in_buffer = buffer.size();
        }

        // Check if we can parse a frame header
        if (remaining_in_buffer < storage::FRAME_HEADER_SIZE) {
            encountered_tail_issue = true;
            break; // Truncated frame at tail
        }

        storage::FrameHeader fhdr;
        s = storage::deserialize_frame_header(buffer.data() + buffer_cursor, remaining_in_buffer, fhdr);
        if (!is_ok(s)) {
            encountered_tail_issue = true;
            break; // Corrupted frame header
        }

        std::size_t total_frame_size = fhdr.total_frame_size();

        // Ensure full frame payload is buffered
        if (remaining_in_buffer < total_frame_size) {
            if (file_read_pos < file_len) {
                std::size_t needed = total_frame_size - remaining_in_buffer;
                std::size_t to_read = static_cast<std::size_t>(std::min<std::uint64_t>(std::max(CHUNK_SIZE, needed), file_len - file_read_pos));

                std::vector<std::uint8_t> expanded_buf;
                expanded_buf.reserve(remaining_in_buffer + to_read);
                expanded_buf.insert(expanded_buf.end(), buffer.begin() + static_cast<std::ptrdiff_t>(buffer_cursor), buffer.end());

                std::size_t old_size = expanded_buf.size();
                expanded_buf.resize(old_size + to_read);
                s = file.read_at(file_read_pos, expanded_buf.data() + old_size, to_read);
                if (!is_ok(s)) {
                    stats.status = s;
                    return stats;
                }
                file_read_pos += to_read;
                buffer = std::move(expanded_buf);
                buffer_cursor = 0;
                remaining_in_buffer = buffer.size();

                if (remaining_in_buffer < total_frame_size) {
                    encountered_tail_issue = true;
                    break;
                }
            } else {
                encountered_tail_issue = true;
                break; // Incomplete payload at EOF
            }
        }

        // Validate CRC32
        const std::uint8_t* frame_ptr = buffer.data() + buffer_cursor;
        std::size_t data_len_for_crc = storage::FRAME_HEADER_SIZE + fhdr.payload_size();
        std::uint32_t expected_crc = storage::decode_le32(frame_ptr + data_len_for_crc);
        std::uint32_t computed_crc = storage::calculate_crc32(frame_ptr, data_len_for_crc);

        if (expected_crc != computed_crc) {
            encountered_tail_issue = true;
            break; // CRC mismatch
        }

        // Calculate absolute file offset of this frame
        std::uint64_t current_frame_file_offset = file_read_pos - remaining_in_buffer;

        // Process Frame
        if (fhdr.type == RecordType::Begin) {
            if (in_tx) {
                // Nested BEGIN without COMMIT -> invalid transaction stream
                encountered_tail_issue = true;
                break;
            }
            in_tx = true;
            active_tx_id = fhdr.tx_id;
            current_tx_ops.clear();
        } else if (fhdr.type == RecordType::Put) {
            if (!in_tx || fhdr.tx_id != active_tx_id) {
                encountered_tail_issue = true;
                break;
            }
            const char* key_ptr = reinterpret_cast<const char*>(frame_ptr + storage::FRAME_HEADER_SIZE);
            std::string key(key_ptr, fhdr.key_len);
            std::uint64_t val_offset = current_frame_file_offset + storage::FRAME_HEADER_SIZE + fhdr.key_len;
            current_tx_ops.push_back({RecordType::Put, std::move(key), val_offset, fhdr.val_len, fhdr.tx_id});
        } else if (fhdr.type == RecordType::Delete) {
            if (!in_tx || fhdr.tx_id != active_tx_id) {
                encountered_tail_issue = true;
                break;
            }
            const char* key_ptr = reinterpret_cast<const char*>(frame_ptr + storage::FRAME_HEADER_SIZE);
            std::string key(key_ptr, fhdr.key_len);
            current_tx_ops.push_back({RecordType::Delete, std::move(key), 0, 0, fhdr.tx_id});
        } else if (fhdr.type == RecordType::Commit) {
            if (!in_tx || fhdr.tx_id != active_tx_id) {
                encountered_tail_issue = true;
                break;
            }
            // Commit verified: publish all staged operations to in-memory index
            for (const auto& op : current_tx_ops) {
                if (op.type == RecordType::Put) {
                    index.put(op.key, storage::IndexEntry{op.val_offset, op.val_len, op.tx_id});
                } else if (op.type == RecordType::Delete) {
                    index.remove(op.key);
                }
            }
            current_tx_ops.clear();
            in_tx = false;
            last_committed_offset = current_frame_file_offset + total_frame_size;
            stats.committed_transactions++;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
            torture::report_test_phase("recovery_commit_replayed");
#endif
        }

        buffer_cursor += total_frame_size;
    }

    // 4. Check for mid-file corruption vs. trailing uncommitted tail
    if (encountered_tail_issue || in_tx || last_committed_offset < file_len) {
        // If there are valid commits further ahead, this is mid-file corruption!
        if (has_valid_commits_ahead(file, last_committed_offset, file_len)) {
            stats.status = Status::Corruption;
            return stats;
        }

        // Safe tail truncation: truncate uncommitted/torn frames at physical tail
        stats.truncated_bytes = file_len - last_committed_offset;
        stats.uncommitted_transactions = in_tx ? 1 : 0;
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        torture::report_test_phase("recovery_before_tail_truncate");
#endif
        s = file.truncate(last_committed_offset);
        if (!is_ok(s)) {
            stats.status = s;
            return stats;
        }
#if defined(KVDB_ENABLE_CRASH_HOOKS)
        torture::report_test_phase("recovery_after_tail_truncate");
#endif
        stats.clean_shutdown = false;
    } else {
        stats.clean_shutdown = true;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats.recovery_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    stats.valid_bytes = last_committed_offset;
    stats.recovered_keys = index.size();
    stats.status = Status::Ok;
    return stats;
}

Status RecoveryEngine::verify_integrity(storage::File& file) {
    storage::Index dummy_index;
    auto stats = recover(file, dummy_index);
    return stats.status;
}

} // namespace kvdb::recovery
