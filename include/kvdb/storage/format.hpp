#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "kvdb/types.hpp"
#include "kvdb/status.hpp"

namespace kvdb::storage {

// Endianness-safe encoding/decoding utilities (Standard Little-Endian)
inline void encode_le16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

inline void encode_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

inline void encode_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

inline std::uint16_t decode_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::uint32_t decode_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t decode_le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<std::uint64_t>(p[i]) << (i * 8));
    }
    return v;
}

#pragma pack(push, 1)
struct RawFileHeader {
    std::uint64_t magic;          // 8 bytes: "KVDB0001"
    std::uint32_t format_version; // 4 bytes: 1
    std::uint32_t header_length;  // 4 bytes: 32
    std::uint8_t  reserved[16];   // 16 bytes: zeros
};
static_assert(sizeof(RawFileHeader) == 32, "RawFileHeader must be exactly 32 bytes");

struct RawFrameHeader {
    std::uint32_t frame_magic;    // 4 bytes: "KVMF"
    std::uint16_t format_version; // 2 bytes: 1
    std::uint8_t  record_type;    // 1 byte: 1=BEGIN, 2=PUT, 3=DELETE, 4=COMMIT
    std::uint8_t  flags;          // 1 byte: 0
    std::uint64_t tx_id;          // 8 bytes
    std::uint32_t key_len;        // 4 bytes
    std::uint64_t val_len;        // 8 bytes
};
static_assert(sizeof(RawFrameHeader) == 28, "RawFrameHeader must be exactly 28 bytes");
#pragma pack(pop)

inline constexpr std::size_t FILE_HEADER_SIZE = sizeof(RawFileHeader);
inline constexpr std::size_t FRAME_HEADER_SIZE = sizeof(RawFrameHeader);
inline constexpr std::size_t CRC32_SIZE = sizeof(std::uint32_t);

// Decoded representation of a Frame Header
struct FrameHeader {
    RecordType type;
    std::uint8_t flags{0};
    TxId tx_id{0};
    std::uint32_t key_len{0};
    std::uint64_t val_len{0};

    std::size_t payload_size() const {
        return static_cast<std::size_t>(key_len) + static_cast<std::size_t>(val_len);
    }

    std::size_t total_frame_size() const {
        return FRAME_HEADER_SIZE + payload_size() + CRC32_SIZE;
    }
};

// Complete logical frame
struct Frame {
    RecordType type;
    TxId tx_id;
    std::string key;
    std::string value;
};

// Checksum calculation (IEEE 802.3 CRC32)
std::uint32_t calculate_crc32(const std::uint8_t* data, std::size_t size);

// Serialization functions
std::vector<std::uint8_t> serialize_file_header();
Status deserialize_file_header(const std::uint8_t* buffer, std::size_t size);

std::vector<std::uint8_t> serialize_frame(const Frame& frame);
void serialize_frame_to(const Frame& frame, std::vector<std::uint8_t>& out_buffer);

// Deserialization functions
Status deserialize_frame_header(const std::uint8_t* buffer, std::size_t size, FrameHeader& out_header);
Status deserialize_frame(const std::uint8_t* buffer, std::size_t size, Frame& out_frame, std::size_t& bytes_consumed);

// Utility to calculate total frame size before serialization
inline std::size_t calculate_frame_size(std::size_t key_len, std::size_t val_len) {
    return FRAME_HEADER_SIZE + key_len + val_len + CRC32_SIZE;
}

} // namespace kvdb::storage
