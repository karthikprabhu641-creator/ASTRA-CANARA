#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <memory>
#include "kvdb/status.hpp"
#include "kvdb/types.hpp"
#include "kvdb/storage/file.hpp"
#include "kvdb/storage/index.hpp"
#include "kvdb/storage/format.hpp"

namespace kvdb::storage {

class StorageEngine {
public:
    StorageEngine();
    ~StorageEngine();

    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;
    StorageEngine(StorageEngine&&) noexcept;
    StorageEngine& operator=(StorageEngine&&) noexcept;

    // Lifecycle
    Status open(const std::string& path, std::uint64_t max_bytes = 0);
    Status close();
    bool is_open() const;

    // Core Key-Value Operations
    Status put(TxId tx_id, const std::string& key, const std::string& value);
    Result get(const std::string& key) const;
    Status remove(TxId tx_id, const std::string& key);

    // Direct read of value by index entry
    Status read_value(const IndexEntry& entry, std::string& out_value) const;

    // Durability sync
    Status sync();

    // Module accessors for recovery, compaction, and concurrency
    File& file() { return file_; }
    const File& file() const { return file_; }
    Index& index() { return index_; }
    const Index& index() const { return index_; }

    std::uint64_t size() const { return file_.size(); }
    const std::string& path() const { return file_.path(); }

private:
    File file_;
    Index index_;
    std::uint64_t max_bytes_{0};
    bool is_open_{false};
};

} // namespace kvdb::storage

