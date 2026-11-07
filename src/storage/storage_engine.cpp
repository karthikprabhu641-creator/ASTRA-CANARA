#include "kvdb/storage/storage_engine.hpp"
#include <vector>

namespace kvdb::storage {

StorageEngine::StorageEngine() = default;
StorageEngine::~StorageEngine() {
    close();
}

StorageEngine::StorageEngine(StorageEngine&&) noexcept = default;
StorageEngine& StorageEngine::operator=(StorageEngine&&) noexcept = default;

Status StorageEngine::open(const std::string& path, std::uint64_t max_bytes) {
    if (is_open_) {
        return Status::AlreadyOpen;
    }

    max_bytes_ = max_bytes;
    Status s = file_.open(path, true);
    if (!is_ok(s)) {
        return s;
    }

    if (file_.size() == 0) {
        // Initialize 32-byte header on newly created file
        auto hdr_bytes = serialize_file_header();
        s = file_.append(hdr_bytes.data(), hdr_bytes.size());
        if (!is_ok(s)) {
            file_.close();
            return s;
        }
        s = file_.sync();
        if (!is_ok(s)) {
            file_.close();
            return s;
        }
    } else {
        // Validate existing header
        std::vector<std::uint8_t> hdr_buf(FILE_HEADER_SIZE);
        s = file_.read_at(0, hdr_buf.data(), hdr_buf.size());
        if (!is_ok(s)) {
            file_.close();
            return s;
        }
        s = deserialize_file_header(hdr_buf.data(), hdr_buf.size());
        if (!is_ok(s)) {
            file_.close();
            return s;
        }
    }

    is_open_ = true;
    return Status::Ok;
}

Status StorageEngine::close() {
    if (!is_open_) return Status::Ok;

    file_.sync();
    file_.close();
    index_.clear();
    is_open_ = false;
    return Status::Ok;
}

bool StorageEngine::is_open() const {
    return is_open_;
}

Status StorageEngine::put(TxId tx_id, const std::string& key, const std::string& value) {
    if (!is_open_) return Status::NotOpen;
    if (key.empty() || key.size() > MAX_KEY_SIZE || value.size() > MAX_VALUE_SIZE) {
        return Status::InvalidArgument;
    }

    std::size_t begin_size = calculate_frame_size(0, 0);
    std::size_t put_size = calculate_frame_size(key.size(), value.size());
    std::size_t commit_size = calculate_frame_size(0, 0);
    std::size_t total_tx_size = begin_size + put_size + commit_size;

    if (max_bytes_ > 0 && file_.size() + total_tx_size > max_bytes_) {
        return Status::NoSpace;
    }

    std::uint64_t initial_file_offset = file_.size();
    std::uint64_t val_offset = initial_file_offset + begin_size + FRAME_HEADER_SIZE + key.size();

    std::vector<std::uint8_t> tx_buffer;
    tx_buffer.reserve(total_tx_size);

    Frame begin_frame{RecordType::Begin, tx_id, "", ""};
    serialize_frame_to(begin_frame, tx_buffer);

    Frame put_frame{RecordType::Put, tx_id, key, value};
    serialize_frame_to(put_frame, tx_buffer);

    Frame commit_frame{RecordType::Commit, tx_id, "", ""};
    serialize_frame_to(commit_frame, tx_buffer);

    // Append to file
    Status s = file_.append(tx_buffer.data(), tx_buffer.size());
    if (!is_ok(s)) return s;

    // Synchronize to physical storage
    s = file_.sync();
    if (!is_ok(s)) return s;

    // Publish to in-memory index
    index_.put(key, IndexEntry{val_offset, static_cast<std::uint64_t>(value.size()), tx_id});
    return Status::Ok;
}

Result StorageEngine::get(const std::string& key) const {
    Result res;
    if (!is_open_) {
        res.status = Status::NotOpen;
        return res;
    }
    if (key.empty() || key.size() > MAX_KEY_SIZE) {
        res.status = Status::InvalidArgument;
        return res;
    }

    auto entry_opt = index_.get(key);
    if (!entry_opt.has_value()) {
        res.status = Status::NotFound;
        return res;
    }

    const auto& entry = *entry_opt;
    if (entry.val_len == 0) {
        res.status = Status::Ok;
        res.value = "";
        return res;
    }

    res.value.resize(static_cast<std::size_t>(entry.val_len));
    Status s = file_.read_at(
        entry.val_file_offset,
        reinterpret_cast<std::uint8_t*>(&res.value[0]),
        static_cast<std::size_t>(entry.val_len)
    );

    if (!is_ok(s)) {
        res.status = s;
        res.value.clear();
        return res;
    }

    res.status = Status::Ok;
    return res;
}

Status StorageEngine::remove(TxId tx_id, const std::string& key) {
    if (!is_open_) return Status::NotOpen;
    if (key.empty() || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument;
    }

    std::size_t begin_size = calculate_frame_size(0, 0);
    std::size_t del_size = calculate_frame_size(key.size(), 0);
    std::size_t commit_size = calculate_frame_size(0, 0);
    std::size_t total_tx_size = begin_size + del_size + commit_size;

    if (max_bytes_ > 0 && file_.size() + total_tx_size > max_bytes_) {
        return Status::NoSpace;
    }

    std::vector<std::uint8_t> tx_buffer;
    tx_buffer.reserve(total_tx_size);

    Frame begin_frame{RecordType::Begin, tx_id, "", ""};
    serialize_frame_to(begin_frame, tx_buffer);

    Frame del_frame{RecordType::Delete, tx_id, key, ""};
    serialize_frame_to(del_frame, tx_buffer);

    Frame commit_frame{RecordType::Commit, tx_id, "", ""};
    serialize_frame_to(commit_frame, tx_buffer);

    Status s = file_.append(tx_buffer.data(), tx_buffer.size());
    if (!is_ok(s)) return s;

    s = file_.sync();
    if (!is_ok(s)) return s;

    index_.remove(key);
    return Status::Ok;
}

Status StorageEngine::read_value(const IndexEntry& entry, std::string& out_value) const {
    if (!is_open_) return Status::NotOpen;
    if (entry.val_len == 0) {
        out_value.clear();
        return Status::Ok;
    }

    out_value.resize(static_cast<std::size_t>(entry.val_len));
    return file_.read_at(
        entry.val_file_offset,
        reinterpret_cast<std::uint8_t*>(&out_value[0]),
        static_cast<std::size_t>(entry.val_len)
    );
}

Status StorageEngine::sync() {
    if (!is_open_) return Status::NotOpen;
    return file_.sync();
}

} // namespace kvdb::storage

