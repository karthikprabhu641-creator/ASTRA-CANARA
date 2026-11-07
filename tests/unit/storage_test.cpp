#include "kvdb/storage/format.hpp"
#include "kvdb/storage/file.hpp"
#include "kvdb/storage/index.hpp"
#include "kvdb/storage/storage_engine.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>

using namespace kvdb;
using namespace kvdb::storage;

void test_binary_serialization() {
    std::cout << "[Test] Running binary serialization & CRC32 tests...\n";

    // 1. File Header
    auto hdr_bytes = serialize_file_header();
    assert(hdr_bytes.size() == FILE_HEADER_SIZE);
    assert(is_ok(deserialize_file_header(hdr_bytes.data(), hdr_bytes.size())));

    // Tamper with magic
    hdr_bytes[0] ^= 0xFF;
    assert(deserialize_file_header(hdr_bytes.data(), hdr_bytes.size()) == Status::Corruption);
    hdr_bytes[0] ^= 0xFF; // Restore

    // 2. Frame serialization & deserialization
    Frame f1{RecordType::Put, 101, "test_key", "test_value_12345"};
    auto frame_bytes = serialize_frame(f1);
    assert(frame_bytes.size() == calculate_frame_size(f1.key.size(), f1.value.size()));

    Frame out_frame;
    std::size_t consumed = 0;
    assert(is_ok(deserialize_frame(frame_bytes.data(), frame_bytes.size(), out_frame, consumed)));
    assert(consumed == frame_bytes.size());
    assert(out_frame.type == RecordType::Put);
    assert(out_frame.tx_id == 101);
    assert(out_frame.key == "test_key");
    assert(out_frame.value == "test_value_12345");

    // 3. CRC32 corruption detection
    frame_bytes[FRAME_HEADER_SIZE + 2] ^= 0x55; // Corrupt payload byte
    assert(deserialize_frame(frame_bytes.data(), frame_bytes.size(), out_frame, consumed) == Status::Corruption);

    std::cout << "  -> Binary serialization & CRC32 passed.\n";
}

void test_put_get_delete() {
    std::cout << "[Test] Running PUT, GET, DELETE operations...\n";

    std::string test_db = "storage_crud_test.db";
    std::remove(test_db.c_str());

    StorageEngine engine;
    assert(is_ok(engine.open(test_db)));

    // Basic PUT and GET
    assert(is_ok(engine.put(1, "user:1", "Alice")));
    Result r1 = engine.get("user:1");
    assert(is_ok(r1.status));
    assert(r1.value == "Alice");

    // Overwrite existing key
    assert(is_ok(engine.put(2, "user:1", "Alice Updated")));
    Result r2 = engine.get("user:1");
    assert(is_ok(r2.status));
    assert(r2.value == "Alice Updated");

    // Empty value handling (distinct from NotFound)
    assert(is_ok(engine.put(3, "empty_key", "")));
    Result r3 = engine.get("empty_key");
    assert(is_ok(r3.status));
    assert(r3.value.empty());

    // Non-existent key
    Result r4 = engine.get("non_existent");
    assert(r4.status == Status::NotFound);

    // Binary key & value with embedded null bytes
    std::string bin_key("bin\0key", 7);
    std::string bin_val("val\0\1\2\3val", 10);
    assert(is_ok(engine.put(4, bin_key, bin_val)));
    Result r5 = engine.get(bin_key);
    assert(is_ok(r5.status));
    assert(r5.value == bin_val);

    // DELETE key
    assert(is_ok(engine.remove(5, "user:1")));
    Result r6 = engine.get("user:1");
    assert(r6.status == Status::NotFound);

    // DELETE missing key (idempotent)
    assert(is_ok(engine.remove(6, "non_existent")));

    assert(is_ok(engine.close()));
    std::remove(test_db.c_str());

    std::cout << "  -> PUT, GET, DELETE passed.\n";
}

void test_large_keys_and_values() {
    std::cout << "[Test] Running large keys & values tests...\n";

    std::string test_db = "storage_large_test.db";
    std::remove(test_db.c_str());

    StorageEngine engine;
    assert(is_ok(engine.open(test_db)));

    // 1. Max Key Size (64 KB)
    std::string large_key(MAX_KEY_SIZE, 'K');
    assert(is_ok(engine.put(1, large_key, "max_key_value")));
    Result r_key = engine.get(large_key);
    assert(is_ok(r_key.status));
    assert(r_key.value == "max_key_value");

    // Exceed Max Key Size (64 KB + 1)
    std::string too_large_key(MAX_KEY_SIZE + 1, 'X');
    assert(engine.put(2, too_large_key, "val") == Status::InvalidArgument);

    // 2. Large Value (1 MB)
    std::string val_1mb(1024 * 1024, 'A');
    for (std::size_t i = 0; i < val_1mb.size(); ++i) {
        val_1mb[i] = static_cast<char>('A' + (i % 26));
    }
    assert(is_ok(engine.put(3, "large:1mb", val_1mb)));
    Result r_1mb = engine.get("large:1mb");
    assert(is_ok(r_1mb.status));
    assert(r_1mb.value.size() == val_1mb.size());
    assert(r_1mb.value == val_1mb);

    // 3. Large Value (4 MB)
    std::string val_4mb(4 * 1024 * 1024, 'B');
    for (std::size_t i = 0; i < val_4mb.size(); ++i) {
        val_4mb[i] = static_cast<char>('B' + (i % 26));
    }
    assert(is_ok(engine.put(4, "large:4mb", val_4mb)));
    Result r_4mb = engine.get("large:4mb");
    assert(is_ok(r_4mb.status));
    assert(r_4mb.value == val_4mb);

    // 4. Max Allowed Value Size (16 MB)
    std::string val_16mb(16 * 1024 * 1024, 'C');
    val_16mb[0] = 'H';
    val_16mb[val_16mb.size() - 1] = 'T';
    assert(is_ok(engine.put(5, "large:16mb", val_16mb)));
    Result r_16mb = engine.get("large:16mb");
    assert(is_ok(r_16mb.status));
    assert(r_16mb.value.size() == val_16mb.size());
    assert(r_16mb.value[0] == 'H');
    assert(r_16mb.value.back() == 'T');

    // Exceed Max Value Size (16 MB + 1)
    std::string too_large_val(MAX_VALUE_SIZE + 1, 'Z');
    assert(engine.put(6, "too_large", too_large_val) == Status::InvalidArgument);

    assert(is_ok(engine.close()));
    std::remove(test_db.c_str());

    std::cout << "  -> Large keys & values tests passed.\n";
}

void test_one_million_keys_index() {
    std::cout << "[Test] Running 1,000,000 keys in-memory index scale test...\n";

    Index index;
    const std::size_t TOTAL_KEYS = 1000000;

    auto t0 = std::chrono::high_resolution_clock::now();
    index.reserve(TOTAL_KEYS);

    for (std::size_t i = 0; i < TOTAL_KEYS; ++i) {
        std::string key = "user:" + std::to_string(i);
        IndexEntry entry{i * 100, 32, i + 1};
        index.put(key, entry);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double insert_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    assert(index.size() == TOTAL_KEYS);
    std::cout << "  -> Inserted 1,000,000 keys in: " << insert_ms << " ms (" 
              << (TOTAL_KEYS / (insert_ms / 1000.0)) << " keys/sec)\n";

    // Lookups
    auto t2 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < 10000; ++i) {
        std::string query_key = "user:" + std::to_string(i * 97 % TOTAL_KEYS);
        auto opt = index.get(query_key);
        assert(opt.has_value());
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double lookup_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::cout << "  -> 10,000 random index lookups in: " << lookup_ms << " ms ("
              << (10000.0 / (lookup_ms / 1000.0)) << " lookups/sec)\n";

    // Deletions
    for (std::size_t i = 0; i < 50000; ++i) {
        std::string del_key = "user:" + std::to_string(i);
        assert(index.remove(del_key));
    }
    assert(index.size() == TOTAL_KEYS - 50000);
    assert(!index.contains("user:0"));
    assert(index.contains("user:50000"));

    std::cout << "  -> 1,000,000 keys index test passed.\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   KVDB STORAGE ENGINE VERIFICATION     \n";
    std::cout << "========================================\n";

    test_binary_serialization();
    test_put_get_delete();
    test_large_keys_and_values();
    test_one_million_keys_index();

    std::cout << "\n========================================\n";
    std::cout << "  ALL STORAGE ENGINE TESTS PASSED!     \n";
    std::cout << "========================================\n";
    return 0;
}

