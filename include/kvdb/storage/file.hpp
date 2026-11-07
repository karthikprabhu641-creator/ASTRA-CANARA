#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "kvdb/status.hpp"

namespace kvdb::storage {

class File {
public:
    File();
    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    // Open file at path; create_if_missing creates the file if it does not exist
    Status open(const std::string& path, bool create_if_missing = true);
    Status close();

    // Append data to the end of the file
    Status append(const std::uint8_t* data, std::size_t size);

    // Thread-safe positional read (does not modify file cursor)
    Status read_at(std::uint64_t offset, std::uint8_t* buffer, std::size_t size) const;

    // Flush and enforce physical durability (FlushFileBuffers / fsync)
    Status sync();

    // Physical truncation to new_size
    Status truncate(std::uint64_t new_size);

    // Query file state
    std::uint64_t size() const;
    bool is_open() const;
    const std::string& path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Cross-platform atomic file replacement
Status atomic_rename_file(const std::string& old_path, const std::string& new_path);

} // namespace kvdb::storage
