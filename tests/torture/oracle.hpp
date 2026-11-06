#pragma once

#include "kvdb/types.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AckOracle {
public:
    explicit AckOracle(const std::string& journal_path);

    bool record_put(const std::string& key, const std::string& value, std::string& error);
    bool record_del(const std::string& key, std::string& error);
    bool record_batch(const std::vector<kvdb::Operation>& operations, std::string& error);

    std::optional<std::string> get(const std::string& key) const;
    std::unordered_map<std::string, std::string> snapshot() const;

private:
    bool append_record(const std::string& record, std::string& error);

    std::string journal_path_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> expected_state_;
};
