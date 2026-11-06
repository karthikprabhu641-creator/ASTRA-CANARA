#include "oracle.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::string encode_hex(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

bool decode_hex(const std::string& encoded, std::string& decoded) {
    if (encoded.size() % 2 != 0) return false;
    decoded.clear();
    decoded.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const int high = hex_value(encoded[i]);
        const int low = hex_value(encoded[i + 1]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool sync_file(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#if defined(_WIN32)
    const int fd = _fileno(file);
    if (fd < 0) return false;
    const intptr_t handle = _get_osfhandle(fd);
    return handle != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(handle)) != 0;
#else
    const int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0;
#endif
}

bool parse_legacy_record(const std::string& line, char& operation, std::string& key,
                         std::string& value) {
    const std::size_t first_tab = line.find('\t');
    if (first_tab == std::string::npos || first_tab != 1) return false;

    operation = line[0];
    if (operation == 'P') {
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos ||
            line.find('\t', second_tab + 1) != std::string::npos ||
            !decode_hex(line.substr(first_tab + 1, second_tab - first_tab - 1), key) ||
            !decode_hex(line.substr(second_tab + 1), value)) {
            return false;
        }
        return !key.empty();
    }

    if (operation == 'D') {
        if (line.find('\t', first_tab + 1) != std::string::npos ||
            !decode_hex(line.substr(first_tab + 1), key)) {
            return false;
        }
        value.clear();
        return !key.empty();
    }

    return false;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator == std::string::npos
            ? std::string::npos : separator - start));
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return fields;
}

bool parse_batch_record(const std::string& line, std::vector<kvdb::Operation>& operations) {
    const auto fields = split_tabs(line);
    if (fields.size() < 2 || fields[0] != "B") return false;

    std::size_t count = 0;
    const auto parsed = std::from_chars(fields[1].data(),
        fields[1].data() + fields[1].size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != fields[1].data() + fields[1].size() ||
        count == 0 || count > kvdb::MAX_BATCH_OPERATIONS) {
        return false;
    }

    operations.clear();
    operations.reserve(count);
    std::size_t field = 2;
    for (std::size_t i = 0; i < count; ++i) {
        if (field >= fields.size()) return false;
        const std::string& type = fields[field++];
        std::string key;
        std::string value;
        if (type == "P") {
            if (field + 1 >= fields.size() ||
                !decode_hex(fields[field++], key) ||
                !decode_hex(fields[field++], value) || key.empty()) {
                return false;
            }
            operations.push_back({kvdb::Operation::Type::Put, std::move(key), std::move(value)});
        } else if (type == "D") {
            if (field >= fields.size() || !decode_hex(fields[field++], key) || key.empty()) {
                return false;
            }
            operations.push_back({kvdb::Operation::Type::Delete, std::move(key), {}});
        } else {
            return false;
        }
    }
    return field == fields.size();
}

} // namespace

AckOracle::AckOracle(const std::string& journal_path) : journal_path_(journal_path) {
    std::ifstream input(journal_path_, std::ios::binary);
    if (!input) {
        if (std::filesystem::exists(journal_path_)) {
            throw std::runtime_error("cannot read ACK journal: " + journal_path_);
        }
        return;
    }

    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        throw std::runtime_error("failed while reading ACK journal: " + journal_path_);
    }

    const std::size_t complete_size = contents.find_last_of('\n') == std::string::npos
        ? 0
        : contents.find_last_of('\n') + 1;
    std::size_t record_start = 0;
    while (record_start < complete_size) {
        const std::size_t record_end = contents.find('\n', record_start);
        if (record_end == std::string::npos || record_end >= complete_size) {
            throw std::runtime_error("invalid ACK journal framing: " + journal_path_);
        }

        const std::string record = contents.substr(record_start, record_end - record_start);
        if (!record.empty() && record[0] == 'B') {
            std::vector<kvdb::Operation> operations;
            if (!parse_batch_record(record, operations)) {
                throw std::runtime_error("invalid ACK journal batch record: " + journal_path_);
            }
            for (const auto& operation : operations) {
                if (operation.type == kvdb::Operation::Type::Put) {
                    expected_state_[operation.key] = operation.value;
                } else {
                    expected_state_.erase(operation.key);
                }
            }
        } else {
            char operation = '\0';
            std::string key;
            std::string value;
            if (!parse_legacy_record(record, operation, key, value)) {
                throw std::runtime_error("invalid ACK journal record: " + journal_path_);
            }
            if (operation == 'P') {
                expected_state_[key] = value;
            } else {
                expected_state_.erase(key);
            }
        }
        record_start = record_end + 1;
    }

    if (complete_size != contents.size()) {
        std::error_code ec;
        std::filesystem::resize_file(journal_path_, complete_size, ec);
        if (ec) {
            throw std::runtime_error("cannot repair incomplete ACK journal tail: " + ec.message());
        }

        std::FILE* file = std::fopen(journal_path_.c_str(), "ab");
        if (!file) {
            throw std::runtime_error("cannot synchronize repaired ACK journal: " + journal_path_);
        }
        const bool synced = sync_file(file);
        const bool closed = std::fclose(file) == 0;
        if (!synced || !closed) {
            throw std::runtime_error("cannot synchronize repaired ACK journal: " + journal_path_);
        }
    }
}

bool AckOracle::append_record(const std::string& record, std::string& error) {
    std::FILE* file = std::fopen(journal_path_.c_str(), "ab");
    if (!file) {
        error = "cannot open ACK journal '" + journal_path_ + "'";
        return false;
    }

    const std::size_t written = std::fwrite(record.data(), 1, record.size(), file);
    const bool synced = written == record.size() && sync_file(file);
    const bool closed = std::fclose(file) == 0;
    if (!synced || !closed) {
        error = "cannot durably append to ACK journal '" + journal_path_ + "'";
        return false;
    }
    return true;
}

bool AckOracle::record_put(const std::string& key, const std::string& value, std::string& error) {
    if (key.empty()) {
        error = "cannot record an empty key in the ACK journal";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string record = "P\t" + encode_hex(key) + "\t" + encode_hex(value) + "\n";
    if (!append_record(record, error)) return false;
    expected_state_[key] = value;
    return true;
}

bool AckOracle::record_del(const std::string& key, std::string& error) {
    if (key.empty()) {
        error = "cannot record an empty key in the ACK journal";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!append_record("D\t" + encode_hex(key) + "\n", error)) return false;
    expected_state_.erase(key);
    return true;
}

bool AckOracle::record_batch(const std::vector<kvdb::Operation>& operations, std::string& error) {
    if (operations.empty() || operations.size() > kvdb::MAX_BATCH_OPERATIONS) {
        error = "ACK journal batch size must be between 1 and " +
            std::to_string(kvdb::MAX_BATCH_OPERATIONS);
        return false;
    }

    std::string record = "B\t" + std::to_string(operations.size());
    for (const auto& operation : operations) {
        if (operation.key.empty() || operation.key.size() > kvdb::MAX_KEY_SIZE ||
            (operation.type == kvdb::Operation::Type::Put &&
             operation.value.size() > kvdb::MAX_VALUE_SIZE) ||
            (operation.type != kvdb::Operation::Type::Put &&
             operation.type != kvdb::Operation::Type::Delete)) {
            error = "ACK journal batch contains an invalid operation";
            return false;
        }
        if (operation.type == kvdb::Operation::Type::Put) {
            record += "\tP\t" + encode_hex(operation.key) + "\t" + encode_hex(operation.value);
        } else {
            record += "\tD\t" + encode_hex(operation.key);
        }
    }
    record.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    auto updated_state = expected_state_;
    for (const auto& operation : operations) {
        if (operation.type == kvdb::Operation::Type::Put) {
            updated_state[operation.key] = operation.value;
        } else {
            updated_state.erase(operation.key);
        }
    }
    if (!append_record(record, error)) return false;
    expected_state_.swap(updated_state);
    return true;
}

std::optional<std::string> AckOracle::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = expected_state_.find(key);
    if (found == expected_state_.end()) return std::nullopt;
    return found->second;
}

std::unordered_map<std::string, std::string> AckOracle::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected_state_;
}
