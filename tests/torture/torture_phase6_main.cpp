#include "child_process.hpp"
#include "oracle.hpp"
#include "workload_generator.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

enum class Mode {
    All,
    Batch,
    Crash
};

struct Options {
    std::uint64_t seed{42};
    std::uint64_t iterations{500};
    std::string db_path{"torture_phase6.db"};
    std::string oracle_path{"torture_phase6.ack.log"};
    std::string kvdb_path;
    Mode mode{Mode::All};
    bool reset{false};
};

void print_usage(std::ostream& out) {
    out << "Usage: torture_tool_phase6 [options]\n"
        << "  --seed <uint>        PRNG seed (default: 42)\n"
        << "  --iterations <int>   Crash/restart cycles (default: 500)\n"
        << "  --db <path>          Database path (default: torture_phase6.db)\n"
        << "  --oracle <path>      ACK journal path (default: torture_phase6.ack.log)\n"
        << "  --kvdb <path>        kvdb executable (default: beside this executable)\n"
        << "  --mode <all|batch|crash> Workload selection (default: all)\n"
        << "  --reset              Remove the specified DB and oracle before starting\n"
        << "  --help               Show this help\n";
}

bool parse_unsigned(const std::string& text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--reset") {
            options.reset = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            return false;
        }
        const std::string value = argv[++i];
        if (arg == "--seed") {
            if (!parse_unsigned(value, options.seed)) {
                std::cerr << "Invalid unsigned seed: " << value << "\n";
                return false;
            }
        } else if (arg == "--iterations") {
            if (!parse_unsigned(value, options.iterations) || options.iterations == 0) {
                std::cerr << "Iterations must be a positive integer\n";
                return false;
            }
        } else if (arg == "--db") {
            options.db_path = value;
        } else if (arg == "--oracle") {
            options.oracle_path = value;
        } else if (arg == "--kvdb") {
            options.kvdb_path = value;
        } else if (arg == "--mode") {
            if (value == "all") options.mode = Mode::All;
            else if (value == "batch") options.mode = Mode::Batch;
            else if (value == "crash") options.mode = Mode::Crash;
            else {
                std::cerr << "Unsupported mode: " << value << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (options.db_path.empty() || options.oracle_path.empty()) {
        std::cerr << "Database and oracle paths cannot be empty\n";
        return false;
    }
    if (std::filesystem::path(options.db_path).lexically_normal() ==
        std::filesystem::path(options.oracle_path).lexically_normal()) {
        std::cerr << "Database and oracle paths must be different\n";
        return false;
    }
    return true;
}

bool default_kvdb_path(const char* executable_path, std::string& path, std::string& error) {
    std::error_code ec;
    const std::filesystem::path runner = std::filesystem::absolute(executable_path, ec);
    if (ec) {
        error = "cannot resolve torture runner path: " + ec.message();
        return false;
    }
#if defined(_WIN32)
    path = (runner.parent_path() / "kvdb.exe").string();
#else
    path = (runner.parent_path() / "kvdb").string();
#endif
    return true;
}

bool run_cli(const std::string& executable, const std::vector<std::string>& arguments,
             std::vector<std::string>& output, int& exit_code, std::string& error) {
    torture::ChildProcess child;
             if (!child.start(executable, arguments, error)) return false;
             if (!child.close_input(error)) {
                 std::string terminate_error;
                 if (!child.terminate(terminate_error)) error += "; " + terminate_error;
                 return false;
             }

    while (true) {
        std::string line;
        bool eof = false;
        if (!child.read_line(line, 30s, eof, error)) {
            if (eof) break;
            std::string terminate_error;
            if (!child.terminate(terminate_error)) error += "; " + terminate_error;
            return false;
        }
        output.push_back(std::move(line));
    }
    if (!child.wait(30s, exit_code, error)) {
        std::string terminate_error;
        if (!child.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    return true;
}

bool check_database(const Options& options, std::uint64_t cycle, std::string& error) {
    std::vector<std::string> output;
    int exit_code = -1;
    if (!run_cli(options.kvdb_path, {"check", options.db_path}, output, exit_code, error)) {
        error = "cycle " + std::to_string(cycle) + " integrity-check process failed: " + error;
        return false;
    }
    if (exit_code != 0 || output.size() != 1 || output.front() != "INTEGRITY_OK") {
        error = "cycle " + std::to_string(cycle) + " integrity check failed (exit " +
            std::to_string(exit_code) + ")";
        if (!output.empty()) error += ": " + output.front();
        return false;
    }
    return true;
}

bool read_database_dump(const Options& options, std::map<std::string, std::string>& state,
                        std::uint64_t cycle, std::string& error) {
    std::vector<std::string> output;
    int exit_code = -1;
    if (!run_cli(options.kvdb_path, {"dump", options.db_path}, output, exit_code, error)) {
        error = "cycle " + std::to_string(cycle) + " dump process failed: " + error;
        return false;
    }
    if (exit_code != 0) {
        error = "cycle " + std::to_string(cycle) + " dump exited with code " +
            std::to_string(exit_code);
        return false;
    }

    state.clear();
    for (const auto& line : output) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0) {
            error = "cycle " + std::to_string(cycle) + " malformed dump row: " + line;
            return false;
        }
        const std::string key = line.substr(0, separator);
        if (!state.emplace(key, line.substr(separator + 1)).second) {
            error = "cycle " + std::to_string(cycle) + " duplicate key in dump: " + key;
            return false;
        }
    }
    return true;
}

bool verify_against_oracle(const Options& options, const AckOracle& oracle,
                           std::uint64_t cycle, std::map<std::string, std::string>& actual,
                           bool& dump_available, std::string& error) {
    dump_available = false;
    if (!check_database(options, cycle, error)) return false;

    if (!read_database_dump(options, actual, cycle, error)) return false;
    dump_available = true;
    const auto expected = oracle.snapshot();
    for (const auto& entry : expected) {
        const auto found = actual.find(entry.first);
        if (found == actual.end()) {
            error = "cycle " + std::to_string(cycle) + " missing acknowledged key '" +
                entry.first + "'";
            return false;
        }
        if (found->second != entry.second) {
            error = "cycle " + std::to_string(cycle) + " value mismatch for acknowledged key '" +
                entry.first + "'";
            return false;
        }
    }
    for (const auto& entry : actual) {
        if (expected.find(entry.first) == expected.end()) {
            error = "cycle " + std::to_string(cycle) + " unexpected unacknowledged key '" +
                entry.first + "'";
            return false;
        }
    }
    return true;
}

std::string describe_operation(const torture::GeneratedOperation& operation) {
    switch (operation.type) {
        case torture::GeneratedOperation::Type::Put:
            return "PUT " + operation.key + "=" + operation.value;
        case torture::GeneratedOperation::Type::Get:
            return "GET " + operation.key;
        case torture::GeneratedOperation::Type::Delete:
            return "DEL " + operation.key;
        case torture::GeneratedOperation::Type::Batch:
            return "BATCH operations=" + std::to_string(operation.batch.size());
    }
    return "UNKNOWN";
}

void print_state_diff(const std::unordered_map<std::string, std::string>& expected,
                      const std::map<std::string, std::string>& actual) {
    std::cerr << "State diff (expected oracle vs recovered database):\n";
    std::size_t reported = 0;
    for (const auto& entry : expected) {
        const auto found = actual.find(entry.first);
        if (found == actual.end()) {
            std::cerr << "  missing key=" << entry.first << " expected=" << entry.second << "\n";
        } else if (found->second != entry.second) {
            std::cerr << "  changed key=" << entry.first << " expected=" << entry.second
                      << " actual=" << found->second << "\n";
        } else {
            continue;
        }
        if (++reported == 50) {
            std::cerr << "  (diff output limited to 50 entries)\n";
            return;
        }
    }
    for (const auto& entry : actual) {
        if (expected.find(entry.first) != expected.end()) continue;
        std::cerr << "  unexpected key=" << entry.first << " actual=" << entry.second << "\n";
        if (++reported == 50) {
            std::cerr << "  (diff output limited to 50 entries)\n";
            return;
        }
    }
    if (reported == 0) std::cerr << "  (no key/value differences)\n";
}

bool expect_response(torture::ChildProcess& server, const std::string& request,
                     const std::string& expected, std::string& error) {
    if (!server.write_line(request, error)) return false;
    std::string response;
    bool eof = false;
    if (!server.read_line(response, 30s, eof, error)) {
        if (eof) error = "server closed output while awaiting response to: " + request;
        return false;
    }
    if (response != expected) {
        error = "unexpected response to '" + request + "': " + response;
        return false;
    }
    return true;
}

bool execute_operation(torture::ChildProcess& server, AckOracle& oracle,
                       const torture::GeneratedOperation& operation, std::uint64_t op_id,
                       const std::filesystem::path& batch_file, std::string& error) {
    const std::string id = std::to_string(op_id);
    if (operation.type == torture::GeneratedOperation::Type::Put) {
        if (!expect_response(server, "PUT " + id + " " + operation.key + " " + operation.value,
                             "ACK " + id, error)) return false;
        return oracle.record_put(operation.key, operation.value, error);
    }
    if (operation.type == torture::GeneratedOperation::Type::Delete) {
        if (!expect_response(server, "DEL " + id + " " + operation.key,
                             "ACK " + id, error)) return false;
        return oracle.record_del(operation.key, error);
    }
    if (operation.type == torture::GeneratedOperation::Type::Get) {
        const auto expected = oracle.get(operation.key);
        const std::string response = expected.has_value()
            ? "VALUE " + id + " " + *expected
            : "NOT_FOUND " + id;
        return expect_response(server, "GET " + id + " " + operation.key, response, error);
    }

    {
        std::error_code exists_error;
        if (std::filesystem::exists(batch_file, exists_error) || exists_error) {
            error = exists_error ? "cannot inspect batch path: " + exists_error.message()
                                 : "batch path already exists: " + batch_file.string();
            return false;
        }
        std::ofstream file(batch_file, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot create batch input file: " + batch_file.string();
            return false;
        }
        for (const auto& item : operation.batch) {
            if (item.type == kvdb::Operation::Type::Put) {
                file << "PUT " << item.key << " " << item.value << "\n";
            } else {
                file << "DEL " << item.key << "\n";
            }
        }
        if (!file) {
            error = "failed writing batch input file: " + batch_file.string();
            return false;
        }
    }

    const bool acknowledged = expect_response(server, "BATCH " + id + " " + batch_file.string(),
                                               "ACK " + id, error);
    std::error_code remove_error;
    std::filesystem::remove(batch_file, remove_error);
    if (remove_error && acknowledged) {
        error = "cannot remove acknowledged batch input: " + remove_error.message();
        return false;
    }
    if (!acknowledged) return false;
    if (!oracle.record_batch(operation.batch, error)) {
        error = "BATCH was acknowledged but oracle persistence failed: " + error;
        return false;
    }
    return true;
}

bool create_interrupted_batch(const std::filesystem::path& path, std::uint64_t cycle,
                              std::size_t operations, std::string& error) {
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error) || exists_error) {
        error = exists_error ? "cannot inspect interrupted batch path: " + exists_error.message()
                             : "interrupted batch path already exists: " + path.string();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create interrupted batch file: " + path.string();
        return false;
    }
    for (std::size_t i = 0; i < operations; ++i) {
        file << "PUT phase6_pending_" << cycle << "_" << i
             << " pending_value_" << cycle << "_" << i << "\n";
    }
    if (!file) {
        error = "failed writing interrupted batch file: " + path.string();
        return false;
    }
    return true;
}

bool start_server(torture::ChildProcess& server, const Options& options, std::string& error) {
    if (!server.start(options.kvdb_path, {"serve", options.db_path}, error)) return false;
    if (!server.write_line("PING phase6_ready", error)) return false;
    std::string response;
    bool eof = false;
    if (!server.read_line(response, 30s, eof, error)) {
        if (eof) error = "server exited before startup handshake";
        return false;
    }
    if (response != "PONG phase6_ready") {
        error = "unexpected server startup response: " + response;
        return false;
    }
    return true;
}

void print_history(std::uint64_t seed, std::uint64_t cycle,
                   const std::vector<std::string>& history, const std::string& error) {
    std::cerr << "[Torture] FAILURE seed=" << seed << " cycle=" << cycle
              << ": " << error << "\nRecent operation history:\n";
    for (const auto& entry : history) std::cerr << "  " << entry << "\n";
}

bool terminate_server(torture::ChildProcess& server, std::string& error) {
    std::string terminate_error;
    if (server.terminate(terminate_error)) return true;
    if (!error.empty()) error += "; ";
    error += "failed to terminate child process: " + terminate_error;
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_usage(std::cout);
            return 0;
        }
    }

    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(std::cerr);
        return 1;
    }
    if (options.kvdb_path.empty()) {
        std::string error;
        if (!default_kvdb_path(argv[0], options.kvdb_path, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    if (options.reset) {
        std::error_code ec;
        std::filesystem::remove(options.db_path, ec);
        if (ec) {
            std::cerr << "Cannot remove database file: " << ec.message() << "\n";
            return 1;
        }
        std::filesystem::remove(options.oracle_path, ec);
        if (ec) {
            std::cerr << "Cannot remove oracle journal: " << ec.message() << "\n";
            return 1;
        }
    }

    std::unique_ptr<AckOracle> oracle;
    try {
        oracle = std::make_unique<AckOracle>(options.oracle_path);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load ACK oracle: " << ex.what() << "\n";
        return 1;
    }

    const auto run_nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path batch_prefix{"torture_phase6_batch_" +
        std::to_string(options.seed) + "_" + std::to_string(run_nonce) + "_"};

    std::mt19937_64 cycle_rng(options.seed ^ 0xd1b54a32d192ed03ULL);
    torture::WorkloadGenerator generator(options.seed, 0,
        options.mode == Mode::Batch ? torture::WorkloadMode::BatchOnly
                                    : torture::WorkloadMode::All);

    std::cout << "[Torture] Phase 6 seed=" << options.seed
              << " crash_cycles=" << options.iterations
              << " mode=" << (options.mode == Mode::All ? "all" :
                              options.mode == Mode::Batch ? "batch" : "crash") << "\n";

    std::uint64_t interrupted_batch_count = 0;
    std::uint64_t acknowledged_crash_batch_count = 0;
    for (std::uint64_t cycle = 0; cycle < options.iterations; ++cycle) {
        torture::ChildProcess server;
        std::string error;
        std::vector<std::string> history;
        error.clear();
        if (!start_server(server, options, error)) {
            print_history(options.seed, cycle, history, error);
            return 1;
        }

        const std::uint32_t operation_count =
            std::uniform_int_distribution<std::uint32_t>(10, 100)(cycle_rng);
        if (options.mode != Mode::Crash) {
            for (std::uint32_t index = 0; index < operation_count; ++index) {
                const auto operation = generator.next();
                const std::uint64_t op_id = cycle * 1000 + index;
                const std::filesystem::path batch_file = batch_prefix.string() +
                    std::to_string(cycle) + "_" + std::to_string(index) + ".txt";
                if (!execute_operation(server, *oracle, operation, op_id, batch_file, error)) {
                    history.push_back("op " + std::to_string(op_id) + " " +
                        describe_operation(operation) + " failed: " + error);
                    terminate_server(server, error);
                    print_history(options.seed, cycle, history, error);
                    return 1;
                }
                history.push_back("op " + std::to_string(op_id) + " " +
                    describe_operation(operation) + " completed");
                if (history.size() > 20) history.erase(history.begin());
            }
        }

        const auto before_crash = oracle->snapshot();
        const std::filesystem::path pending_file = batch_prefix.string() +
            std::to_string(cycle) + "_pending.txt";
        constexpr std::size_t pending_batch_size = 2000;
        if (!create_interrupted_batch(pending_file, cycle, pending_batch_size, error)) {
            terminate_server(server, error);
            print_history(options.seed, cycle, history, error);
            return 1;
        }

        const std::uint64_t pending_op_id = cycle * 1000 + operation_count + 100;
        const std::string request = "BATCH " + std::to_string(pending_op_id) + " " +
                                    pending_file.string();
        error.clear();
        if (!server.write_line(request, error)) {
            terminate_server(server, error);
            std::error_code ignored;
            std::filesystem::remove(pending_file, ignored);
            print_history(options.seed, cycle, history, error);
            return 1;
        }

        std::string response;
        bool eof = false;
        const bool got_response = server.read_line(response, 5ms, eof, error);
        bool pending_acknowledged = false;
        if (got_response) {
            if (response != "ACK " + std::to_string(pending_op_id)) {
                terminate_server(server, error);
                std::error_code ignored;
                std::filesystem::remove(pending_file, ignored);
                print_history(options.seed, cycle, history, "unexpected pending BATCH response: " + response);
                return 1;
            }
            pending_acknowledged = true;
            std::vector<kvdb::Operation> pending_operations;
            pending_operations.reserve(pending_batch_size);
            for (std::size_t i = 0; i < pending_batch_size; ++i) {
                pending_operations.push_back({kvdb::Operation::Type::Put,
                    "phase6_pending_" + std::to_string(cycle) + "_" + std::to_string(i),
                    "pending_value_" + std::to_string(cycle) + "_" + std::to_string(i)});
            }
            if (!oracle->record_batch(pending_operations, error)) {
                terminate_server(server, error);
                std::error_code ignored;
                std::filesystem::remove(pending_file, ignored);
                print_history(options.seed, cycle, history, "ACKed pending batch oracle error: " + error);
                return 1;
            }
        } else if (!error.empty() && error != "timed out waiting for child output") {
            terminate_server(server, error);
            std::error_code ignored;
            std::filesystem::remove(pending_file, ignored);
            print_history(options.seed, cycle, history, error);
            return 1;
        }

        history.push_back(pending_acknowledged ? "pending batch received ACK" :
                                                   "pending batch terminated before ACK");
        if (pending_acknowledged) ++acknowledged_crash_batch_count;
        else ++interrupted_batch_count;
        if (!terminate_server(server, error)) {
            std::error_code ignored;
            std::filesystem::remove(pending_file, ignored);
            print_history(options.seed, cycle, history, error);
            return 1;
        }
        error.clear();
        std::error_code remove_error;
        std::filesystem::remove(pending_file, remove_error);
        if (remove_error) {
            print_history(options.seed, cycle, history,
                          "cannot remove pending batch file: " + remove_error.message());
            return 1;
        }

        std::map<std::string, std::string> actual;
        bool dump_available = false;
        if (!verify_against_oracle(options, *oracle, cycle, actual, dump_available, error)) {
            if (dump_available) print_state_diff(oracle->snapshot(), actual);
            else std::cerr << "State diff unavailable because database dump failed\n";
            print_history(options.seed, cycle, history, error);
            return 1;
        }

        if (!pending_acknowledged) {
            if (actual.size() != before_crash.size()) {
                print_state_diff(before_crash, actual);
                print_history(options.seed, cycle, history,
                    "interrupted batch was not wholly absent: live key count changed from " +
                    std::to_string(before_crash.size()) + " to " + std::to_string(actual.size()));
                return 1;
            }
            for (const auto& entry : before_crash) {
                const auto found = actual.find(entry.first);
                if (found == actual.end() || found->second != entry.second) {
                    print_state_diff(before_crash, actual);
                    print_history(options.seed, cycle, history,
                        "interrupted batch changed pre-existing key '" + entry.first + "'");
                    return 1;
                }
            }
        }

        if ((cycle + 1) % 25 == 0 || cycle + 1 == options.iterations) {
            std::cout << "[Torture] Verified crash cycle " << (cycle + 1)
                      << "/" << options.iterations << "\n";
        }
    }

    std::cout << "[Torture] Passed " << options.iterations
              << " crash/restart cycles; " << interrupted_batch_count
              << " unacknowledged batches were absent after recovery, "
              << acknowledged_crash_batch_count
              << " crash batches were durably acknowledged\n";
    return 0;
}
