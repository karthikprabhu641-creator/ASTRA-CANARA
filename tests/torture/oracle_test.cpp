#include "oracle.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void append_text(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    assert(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(output.good());
}

} // namespace

int main() {
    const std::string path = "oracle_phase7_test.ack.log";
    std::remove(path.c_str());

    {
        AckOracle oracle(path);
        std::string error;
        assert(oracle.record_put("legacy", "legacy-value", error));
        assert(oracle.record_del("deleted", error));

        const std::vector<kvdb::Operation> batch{
            {kvdb::Operation::Type::Put, "batch key", "value\nwith\ttabs\\"},
            {kvdb::Operation::Type::Delete, "legacy", {}},
            {kvdb::Operation::Type::Put, "empty value", ""}
        };
        assert(oracle.record_batch(batch, error));

        const std::string before_invalid = read_file(path);
        assert(!oracle.record_batch({}, error));
        assert(read_file(path) == before_invalid);

        const auto snapshot = oracle.snapshot();
        assert(snapshot.size() == 2);
        assert(snapshot.at("batch key") == "value\nwith\ttabs\\");
        assert(snapshot.at("empty value").empty());
        assert(snapshot.find("legacy") == snapshot.end());
    }

    const std::string complete_journal = read_file(path);
    assert(complete_journal.find("B\t3\t") != std::string::npos);
    const std::size_t batch_start = complete_journal.find("B\t3\t");
    const std::size_t batch_end = complete_journal.find('\n', batch_start);
    assert(batch_end != std::string::npos);
    assert(complete_journal.find('\n', batch_end + 1) == std::string::npos);

    {
        AckOracle recovered(path);
        const auto batch_value = recovered.get("batch key");
        assert(batch_value.has_value());
        assert(*batch_value == "value\nwith\ttabs\\");
        assert(!recovered.get("legacy").has_value());
    }

    append_text(path, "B\t2\tP\t746f726e2d6b6579\t7061727469616c");
    {
        AckOracle recovered(path);
        const auto value = recovered.get("batch key");
        assert(value.has_value());
        assert(*value == "value\nwith\ttabs\\");
        assert(!recovered.get("torn-key").has_value());
    }
    assert(read_file(path) == complete_journal);

    const std::string concurrent_path = "oracle_phase7_concurrent.ack.log";
    std::remove(concurrent_path.c_str());
    {
        AckOracle oracle(concurrent_path);
        std::vector<std::thread> writers;
        for (int worker = 0; worker < 4; ++worker) {
            writers.emplace_back([worker, &oracle] {
                for (int batch_index = 0; batch_index < 10; ++batch_index) {
                    std::vector<kvdb::Operation> batch{
                        {kvdb::Operation::Type::Put,
                         "worker-" + std::to_string(worker) + "-" + std::to_string(batch_index),
                         "value-" + std::to_string(batch_index)},
                        {kvdb::Operation::Type::Delete,
                         "deleted-" + std::to_string(worker) + "-" + std::to_string(batch_index),
                         {}}
                    };
                    std::string error;
                    assert(oracle.record_batch(batch, error));
                }
            });
        }
        for (auto& writer : writers) writer.join();
    }
    {
        AckOracle recovered(concurrent_path);
        assert(recovered.snapshot().size() == 40);
    }

    const std::string malformed_path = "oracle_phase7_malformed.ack.log";
    std::remove(malformed_path.c_str());
    append_text(malformed_path, "B\t2\tP\t6b\t76\n");
    bool rejected = false;
    try {
        AckOracle malformed(malformed_path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);

    std::remove(path.c_str());
    std::remove(concurrent_path.c_str());
    std::remove(malformed_path.c_str());
    return 0;
}
