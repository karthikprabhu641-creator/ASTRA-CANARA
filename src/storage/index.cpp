#include "kvdb/storage/index.hpp"

namespace kvdb::storage {

void Index::put(const std::string& key, const IndexEntry& entry) {
    table_[key] = entry;
}

bool Index::remove(const std::string& key) {
    return table_.erase(key) > 0;
}

std::optional<IndexEntry> Index::get(const std::string& key) const {
    auto it = table_.find(key);
    if (it != table_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool Index::contains(const std::string& key) const {
    return table_.find(key) != table_.end();
}

std::size_t Index::size() const {
    return table_.size();
}

void Index::clear() {
    table_.clear();
}

void Index::reserve(std::size_t capacity) {
    table_.reserve(capacity);
}

void Index::for_each(const std::function<void(const std::string&, const IndexEntry&)>& fn) const {
    for (const auto& [k, v] : table_) {
        fn(k, v);
    }
}

std::vector<std::pair<std::string, IndexEntry>> Index::get_all_entries() const {
    std::vector<std::pair<std::string, IndexEntry>> entries;
    entries.reserve(table_.size());
    for (const auto& [k, v] : table_) {
        entries.emplace_back(k, v);
    }
    return entries;
}

} // namespace kvdb::storage
