#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <vector>
#include <functional>
#include "kvdb/types.hpp"

namespace kvdb::storage {

// Lightweight index record pointing to physical location on disk
struct IndexEntry {
    std::uint64_t val_file_offset{0}; // Absolute file offset of the value payload
    std::uint64_t val_len{0};         // Byte length of value
    TxId tx_id{0};                    // Transaction ID that wrote this entry
};

class Index {
public:
    Index() = default;

    // Insert or update index entry
    void put(const std::string& key, const IndexEntry& entry);

    // Remove key from index (tombstone)
    bool remove(const std::string& key);

    // Look up key in index
    std::optional<IndexEntry> get(const std::string& key) const;

    // Check if key exists in index
    bool contains(const std::string& key) const;

    // Current count of live keys
    std::size_t size() const;

    // Clear all entries
    void clear();

    // Pre-allocate hash buckets to avoid rehashing for large key counts (e.g. 1,000,000 keys)
    void reserve(std::size_t capacity);

    // Streaming iterator to avoid allocating large vectors for compaction and dump
    void for_each(const std::function<void(const std::string&, const IndexEntry&)>& fn) const;

    // Retrieve all live entries (convenience)
    std::vector<std::pair<std::string, IndexEntry>> get_all_entries() const;

private:
    std::unordered_map<std::string, IndexEntry> table_;
};

} // namespace kvdb::storage
