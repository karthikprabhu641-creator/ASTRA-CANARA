#include "kvdb/concurrency/compactor.hpp"
#include "kvdb/storage/format.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <vector>
#if defined(KVDB_ENABLE_CRASH_HOOKS)
#include "crash_hook.hpp"
#endif

#if defined(_WIN32)
#include <process.h>
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace kvdb::concurrency {

namespace {

Status io_status() {
#if defined(_WIN32)
    if (errno == ENOSPC) return Status::NoSpace;
    if (GetLastError() == ERROR_DISK_FULL) return Status::NoSpace;
#else
    if (errno == ENOSPC) return Status::NoSpace;
#ifdef EDQUOT
    if (errno == EDQUOT) return Status::NoSpace;
#endif
#endif
    return Status::IoError;
}

bool add_size(std::uint64_t amount, std::uint64_t& total) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += amount;
    return true;
}

Status create_unique_temp_file(const std::string& path) {
#if defined(_WIN32)
    const std::filesystem::path temp_path(path);
    HANDLE handle = CreateFileW(temp_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return Status::AlreadyOpen;
        }
        return error == ERROR_DISK_FULL ? Status::NoSpace : Status::IoError;
    }
    if (CloseHandle(handle) != 0) return Status::Ok;
    const Status error = io_status();
    DeleteFileW(temp_path.c_str());
    return error;
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) return errno == EEXIST ? Status::AlreadyOpen : io_status();
    if (::close(fd) == 0) return Status::Ok;
    const Status error = io_status();
    ::unlink(path.c_str());
    return error;
#endif
}

Status replace_file_atomically(const std::string& temp_path, const std::string& db_path,
                               bool& replaced) {
    replaced = false;
#if defined(_WIN32)
    const std::filesystem::path temp_file(temp_path);
    const std::filesystem::path db_file(db_path);
    if (!MoveFileExW(temp_file.c_str(), db_file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return GetLastError() == ERROR_DISK_FULL ? Status::NoSpace : Status::IoError;
    }
    replaced = true;
#else
    if (::rename(temp_path.c_str(), db_path.c_str()) != 0) return io_status();
    replaced = true;

    const std::filesystem::path parent =
        std::filesystem::path(db_path).parent_path().empty()
            ? std::filesystem::path(".")
            : std::filesystem::path(db_path).parent_path();
    const int directory_fd = ::open(parent.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                    | O_DIRECTORY
#endif
    );
    if (directory_fd < 0) return io_status();
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_before_directory_sync");
#endif
    const int sync_result = fsync(directory_fd);
    const Status sync_status = sync_result == 0 ? Status::Ok : io_status();
    ::close(directory_fd);
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    if (is_ok(sync_status)) torture::report_test_phase("compaction_after_directory_sync");
#endif
    if (!is_ok(sync_status)) return sync_status;
#endif
    return Status::Ok;
}

} // namespace

bool SpaceManager::has_space_for(std::uint64_t current_file_size, std::uint64_t additional_bytes, std::uint64_t max_bytes) {
    if (max_bytes == 0) return true; // unlimited
    return current_file_size <= max_bytes &&
           additional_bytes <= max_bytes - current_file_size;
}

Status Compactor::compact(
    const std::string& db_path,
    storage::File& file,
    storage::Index& index,
    LockManager& lock_mgr,
    std::uint64_t max_bytes
) {
    auto lock = lock_mgr.acquire_exclusive();
    if (!file.is_open()) return Status::NotOpen;
    storage::File tmp_file;
    auto entries = index.get_all_entries();

    const auto header = storage::serialize_file_header();
    std::uint64_t compacted_size = static_cast<std::uint64_t>(header.size());
    const TxId tx_id = 1;
    const auto begin_bytes = storage::serialize_frame(
        storage::Frame{RecordType::Begin, tx_id, "", ""});
    const auto commit_bytes = storage::serialize_frame(
        storage::Frame{RecordType::Commit, tx_id, "", ""});

    if (!entries.empty()) {
        if (!add_size(static_cast<std::uint64_t>(begin_bytes.size()), compacted_size) ||
            !add_size(static_cast<std::uint64_t>(commit_bytes.size()), compacted_size)) {
            return Status::NoSpace;
        }
        for (const auto& entry : entries) {
            const auto frame_size = static_cast<std::uint64_t>(
                storage::calculate_frame_size(entry.first.size(), static_cast<std::size_t>(entry.second.val_len)));
            if (!add_size(frame_size, compacted_size)) return Status::NoSpace;
        }
    }
    if (max_bytes != 0 && compacted_size > max_bytes) return Status::NoSpace;

    static std::atomic<std::uint64_t> temp_counter{0};
    std::string tmp_path;
    Status s = Status::IoError;
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
#if defined(_WIN32)
        const auto process_id = static_cast<unsigned long>(_getpid());
#else
        const auto process_id = static_cast<unsigned long>(getpid());
#endif
        const auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        tmp_path = db_path + ".compact." + std::to_string(process_id) + "." +
                   std::to_string(nonce) + "." +
                   std::to_string(temp_counter.fetch_add(1)) + ".tmp";
        s = create_unique_temp_file(tmp_path);
        if (s == Status::Ok) break;
        if (s != Status::AlreadyOpen) return s;
    }
    if (s == Status::AlreadyOpen) return Status::IoError;
    if (!is_ok(s)) return s;

    s = tmp_file.open(tmp_path, false);
    if (!is_ok(s)) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return ec ? Status::IoError : s;
    }

    const auto discard_temp = [&]() {
        const Status close_status = tmp_file.close();
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return ec ? Status::IoError : close_status;
    };

#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_generation_started");
#endif
    s = tmp_file.append(header.data(), header.size());
    if (!is_ok(s)) {
        const Status error = s == Status::IoError ? io_status() : s;
        const Status cleanup_status = discard_temp();
        return is_ok(cleanup_status) ? error : cleanup_status;
    }

    std::vector<std::pair<std::string, storage::IndexEntry>> new_index_entries;

    if (!entries.empty()) {
        s = tmp_file.append(begin_bytes.data(), begin_bytes.size());
        if (!is_ok(s)) {
            const Status error = s == Status::IoError ? io_status() : s;
            const Status cleanup_status = discard_temp();
            return is_ok(cleanup_status) ? error : cleanup_status;
        }

        std::uint64_t cur_tmp_offset = tmp_file.size();

        for (const auto& [k, v] : entries) {
            std::string val;
            if (v.val_len > 0) {
                val.resize(static_cast<std::size_t>(v.val_len));
                s = file.read_at(v.val_file_offset,
                    reinterpret_cast<std::uint8_t*>(&val[0]),
                    static_cast<std::size_t>(v.val_len));
                if (!is_ok(s)) {
                    const Status cleanup_status = discard_temp();
                    return is_ok(cleanup_status) ? s : cleanup_status;
                }
            }
            storage::Frame put_frame{RecordType::Put, tx_id, k, val};
            auto p_bytes = storage::serialize_frame(put_frame);
            s = tmp_file.append(p_bytes.data(), p_bytes.size());
            if (!is_ok(s)) {
                const Status error = s == Status::IoError ? io_status() : s;
                const Status cleanup_status = discard_temp();
                return is_ok(cleanup_status) ? error : cleanup_status;
            }

            std::uint64_t new_val_offset = cur_tmp_offset + storage::FRAME_HEADER_SIZE + k.size();
            new_index_entries.emplace_back(k, storage::IndexEntry{new_val_offset, v.val_len, tx_id});
            cur_tmp_offset += p_bytes.size();
        }

        s = tmp_file.append(commit_bytes.data(), commit_bytes.size());
        if (!is_ok(s)) {
            const Status error = s == Status::IoError ? io_status() : s;
            const Status cleanup_status = discard_temp();
            return is_ok(cleanup_status) ? error : cleanup_status;
        }
    }

#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_generation_completed");
    torture::report_test_phase("compaction_before_file_sync");
#endif
    s = tmp_file.sync();
    if (!is_ok(s)) {
        const Status error = s == Status::IoError ? io_status() : s;
        const Status cleanup_status = discard_temp();
        return is_ok(cleanup_status) ? error : cleanup_status;
    }
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_after_file_sync");
    torture::report_test_phase("compaction_before_temp_close");
#endif
    s = tmp_file.close();
    if (!is_ok(s)) {
        const Status error = s == Status::IoError ? io_status() : s;
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return ec ? Status::IoError : error;
    }
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_after_temp_close");
    torture::report_test_phase("compaction_before_database_close");
#endif

    s = file.close();
    if (!is_ok(s)) {
        const Status error = s == Status::IoError ? io_status() : s;
        const Status reopen_status = file.open(db_path, false);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return is_ok(reopen_status) ? error : reopen_status;
    }
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_after_database_close");
#endif

#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_before_replace");
#endif
    bool replaced = false;
    s = replace_file_atomically(tmp_path, db_path, replaced);
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    if (replaced) torture::report_test_phase("compaction_after_replace");
#endif
    const Status reopen_status = file.open(db_path, false);
    if (!is_ok(s)) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        if (!is_ok(reopen_status)) return reopen_status;
        if (replaced && !entries.empty()) {
            index.clear();
            for (const auto& [k, v] : new_index_entries) {
                index.put(k, v);
            }
        }
        return ec ? Status::IoError : s;
    }
    if (!is_ok(reopen_status)) {
        return reopen_status;
    }

    if (!entries.empty()) {
        index.clear();
        for (const auto& [k, v] : new_index_entries) {
            index.put(k, v);
        }
    }
#if defined(KVDB_ENABLE_CRASH_HOOKS)
    torture::report_test_phase("compaction_old_file_replaced");
#endif
    return Status::Ok;
}

} // namespace kvdb::concurrency
