#include "child_process.hpp"
#include "oracle.hpp"
#include "workload_generator.hpp"
#include "kvdb/database.hpp"

#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

enum class Mode {
    Compaction,
    RandomCrash,
    Concurrency
};

struct Options {
    std::uint64_t seed{42};
    std::uint64_t cycles{500};
    std::uint64_t operations_per_worker{16};
    std::uint64_t max_bytes{0};
    std::uint32_t threads{4};
    std::uint32_t compact_rate{16};
    std::uint64_t crash_occurrence{1};
    std::string db_path{"torture_phase10.db"};
    std::string oracle_path{"torture_phase10.ack.log"};
    std::string report_path;
    std::string kvdb_path;
    std::string crash_binary_path;
    std::string crash_phase;
    std::string crash_operation;
    Mode mode{Mode::Compaction};
    bool db_path_explicit{false};
    bool oracle_path_explicit{false};
    bool large_values{false};
    bool large_batches{false};
    bool recovery_crash{false};
    bool compaction_crash{false};
    bool disk_full{false};
    bool verbose{false};
};

struct Request {
    torture::GeneratedOperation generated{};
    bool compact{false};
    std::string id;
    std::string batch_id;
    std::string batch_path;
    std::string description;
};

void print_usage(std::ostream& out) {
    out << "Usage: torture_tool_phase10 [options]\n"
        << "  --seed <uint>                    Workload and crash timing seed (default: 42)\n"
        << "  --iterations <int>               Crash/recovery cycles (default: 500)\n"
        << "  --mode <compaction|random-crash|concurrency|large-values|large-batches|recovery-crash|disk-full>\n"
        << "                                   Test mode (default: compaction)\n"
        << "  --workers <int>                  Concurrent workers (minimum 2 for concurrency/crash modes)\n"
        << "  --threads <int>                  Alias for --workers\n"
        << "  --operations-per-cycle <int>     Maximum requests per worker per cycle (default: 16)\n"
        << "  --compact-rate <int>             Average compaction interval in requests (default: 16)\n"
        << "  --max-bytes <uint>               Database file-size cap (0 = unlimited)\n"
        << "  --large-values                   Include 1 MiB PUT values\n"
        << "  --large-batches                  Include batches of 10,000 mutations\n"
        << "  --recovery-crash                 Randomly kill during serve startup/recovery\n"
        << "  --compaction-crash               Include compaction during random crash cycles\n"
        << "  --crash-phase <name>             Hard-kill at a reported persistence phase\n"
        << "  --crash-occurrence <int>          Pause at the Nth matching phase (default: 1)\n"
        << "  --crash-operation <put|delete|batch> Force operation type for crash-phase tests\n"
        << "  --disk-full                      Require --max-bytes and verify NoSpace recovery\n"
        << "  --verbose                        Print every crash cycle and request phase\n"
        << "  --report <path>                  Write a JSONL ACK/crash report (new file only)\n"
        << "  --db <path>                      Database path (default: torture_phase10.db)\n"
        << "  --oracle <path>                  ACK journal path (default: torture_phase10.ack.log)\n"
        << "  --kvdb <path>                    kvdb executable (default: beside this executable)\n"
        << "  --crash-binary <path>            Hook-enabled executable for recovery-crash mode\n"
        << "  --help                           Show this help\n";
}

bool parse_unsigned(const std::string& text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool is_close_crash_phase(const std::string& phase) {
    return phase.rfind("database_close_", 0) == 0;
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--large-values") {
            options.large_values = true;
            options.mode = Mode::RandomCrash;
            continue;
        }
        if (argument == "--large-batches") {
            options.large_batches = true;
            options.mode = Mode::RandomCrash;
            continue;
        }
        if (argument == "--recovery-crash") {
            options.recovery_crash = true;
            options.mode = Mode::RandomCrash;
            continue;
        }
        if (argument == "--compaction-crash") {
            options.compaction_crash = true;
            options.mode = Mode::RandomCrash;
            continue;
        }
        if (argument == "--disk-full") {
            options.disk_full = true;
            options.mode = Mode::RandomCrash;
            continue;
        }
        if (argument == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argument << "\n";
            return false;
        }
        const std::string value = argv[++i];
        std::uint64_t number = 0;
        if (argument == "--seed") {
            if (!parse_unsigned(value, options.seed)) {
                std::cerr << "Invalid unsigned seed: " << value << "\n";
                return false;
            }
        } else if (argument == "--iterations") {
            if (!parse_unsigned(value, options.cycles) || options.cycles == 0) {
                std::cerr << "Iterations must be a positive integer\n";
                return false;
            }
        } else if (argument == "--threads") {
            if (!parse_unsigned(value, number) || number == 0 ||
                number > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "Workers must be a positive 32-bit integer\n";
                return false;
            }
            options.threads = static_cast<std::uint32_t>(number);
        } else if (argument == "--workers") {
            if (!parse_unsigned(value, number) || number == 0 ||
                number > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "Workers must be a positive 32-bit integer\n";
                return false;
            }
            options.threads = static_cast<std::uint32_t>(number);
        } else if (argument == "--operations-per-cycle") {
            if (!parse_unsigned(value, options.operations_per_worker) ||
                options.operations_per_worker == 0) {
                std::cerr << "Operations per cycle must be a positive integer\n";
                return false;
            }
        } else if (argument == "--compact-rate") {
            if (!parse_unsigned(value, number) || number == 0 ||
                number > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "Compaction rate must be a positive 32-bit integer\n";
                return false;
            }
            options.compact_rate = static_cast<std::uint32_t>(number);
        } else if (argument == "--max-bytes") {
            if (!parse_unsigned(value, options.max_bytes)) {
                std::cerr << "Max bytes must be an unsigned integer\n";
                return false;
            }
        } else if (argument == "--crash-occurrence") {
            if (!parse_unsigned(value, options.crash_occurrence) ||
                options.crash_occurrence == 0) {
                std::cerr << "Crash occurrence must be a positive integer\n";
                return false;
            }
        } else if (argument == "--mode") {
            if (value == "compaction") options.mode = Mode::Compaction;
            else if (value == "random-crash") options.mode = Mode::RandomCrash;
            else if (value == "concurrency") options.mode = Mode::Concurrency;
            else if (value == "large-values") {
                options.mode = Mode::RandomCrash;
                options.large_values = true;
            } else if (value == "large-batches") {
                options.mode = Mode::RandomCrash;
                options.large_batches = true;
            } else if (value == "recovery-crash") {
                options.mode = Mode::RandomCrash;
                options.recovery_crash = true;
            } else if (value == "disk-full") {
                options.mode = Mode::RandomCrash;
                options.disk_full = true;
            } else {
                std::cerr << "Unsupported mode: " << value << "\n";
                return false;
            }
        } else if (argument == "--report") {
            options.report_path = value;
        } else if (argument == "--crash-phase") {
            options.crash_phase = value;
            options.mode = Mode::RandomCrash;
        } else if (argument == "--crash-operation") {
            options.crash_operation = value;
        } else if (argument == "--crash-binary") {
            options.crash_binary_path = value;
        } else if (argument == "--db") {
            options.db_path = value;
            options.db_path_explicit = true;
        } else if (argument == "--oracle") {
            options.oracle_path = value;
            options.oracle_path_explicit = true;
        } else if (argument == "--kvdb") {
            options.kvdb_path = value;
        } else {
            std::cerr << "Unknown option: " << argument << "\n";
            return false;
        }
    }

    const auto db = std::filesystem::path(options.db_path).lexically_normal();
    const auto oracle = std::filesystem::path(options.oracle_path).lexically_normal();
    if (options.db_path.empty() || options.oracle_path.empty() || db == oracle) {
        std::cerr << "Database and oracle paths must be non-empty and different\n";
        return false;
    }
    if (options.operations_per_worker >
        std::numeric_limits<std::uint64_t>::max() / options.threads) {
        std::cerr << "Total operations per cycle exceed the supported range\n";
        return false;
    }
    if (options.crash_occurrence > 1 && options.crash_phase.empty()) {
        std::cerr << "--crash-occurrence requires --crash-phase\n";
        return false;
    }
    if (!options.crash_operation.empty() &&
        options.crash_operation != "put" && options.crash_operation != "delete" &&
        options.crash_operation != "batch") {
        std::cerr << "Unsupported crash operation: " << options.crash_operation << "\n";
        return false;
    }
    if (!options.crash_operation.empty() && options.crash_phase.empty()) {
        std::cerr << "--crash-operation requires --crash-phase\n";
        return false;
    }
    if (options.crash_operation == "batch" && !options.large_batches) {
        std::cerr << "--crash-operation batch requires --large-batches\n";
        return false;
    }
    if (options.crash_operation == "delete" &&
        (options.large_values || options.large_batches)) {
        std::cerr << "--crash-operation delete cannot be combined with large workloads\n";
        return false;
    }
    if (options.disk_full && !options.crash_operation.empty()) {
        std::cerr << "--disk-full cannot be combined with --crash-operation\n";
        return false;
    }
    if (options.crash_occurrence > 1 && options.crash_operation == "delete") {
        std::cerr << "--crash-operation delete does not support repeated batch phases\n";
        return false;
    }
    if (options.mode == Mode::RandomCrash && options.threads < 2) {
        std::cerr << "Random crash testing requires at least two workers\n";
        return false;
    }
    if (options.mode == Mode::Concurrency && options.threads < 2) {
        std::cerr << "Concurrency testing requires at least two workers\n";
        return false;
    }
    if (options.large_values && options.large_batches) {
        std::cerr << "--large-values and --large-batches cannot be combined\n";
        return false;
    }
    if (!options.crash_phase.empty()) {
        static const std::array<const char*, 21> supported_phases{
            "transaction_serialized", "file_append_partial_write",
            "transaction_operation_written", "transaction_before_commit",
            "transaction_commit_written", "transaction_before_sync",
            "transaction_synced", "compaction_generation_started",
            "compaction_generation_completed", "compaction_before_file_sync",
            "compaction_after_file_sync", "compaction_before_temp_close",
            "compaction_after_temp_close", "compaction_before_database_close",
            "compaction_after_database_close", "compaction_before_replace",
            "compaction_after_replace", "database_close_before_sync",
            "database_close_after_sync", "database_close_before_file_close",
            "database_close_after_file_close"};
        bool supported = false;
        for (const char* phase : supported_phases) {
            if (options.crash_phase == phase) supported = true;
        }
        if (!supported && options.crash_phase != "compaction_before_directory_sync" &&
            options.crash_phase != "compaction_after_directory_sync" &&
            options.crash_phase != "compaction_old_file_replaced") {
            std::cerr << "Unsupported crash phase: " << options.crash_phase << "\n";
            return false;
        }
        if (options.crash_phase == "file_append_partial_write" &&
            !options.large_values && !options.large_batches) {
            std::cerr << "--crash-phase file_append_partial_write requires "
                         "--large-values or --large-batches\n";
            return false;
        }
        if (options.crash_occurrence > 1 &&
            options.crash_phase != "transaction_operation_written") {
            std::cerr << "--crash-occurrence greater than one is supported only for "
                         "transaction_operation_written\n";
            return false;
        }
        if (options.crash_occurrence > 1 && !options.large_batches) {
            std::cerr << "--crash-occurrence greater than one requires --large-batches\n";
            return false;
        }
        if (options.disk_full || options.recovery_crash) {
            std::cerr << "--crash-phase cannot be combined with --disk-full or "
                         "--recovery-crash\n";
            return false;
        }
#if defined(_WIN32)
        if (options.crash_phase == "compaction_before_directory_sync" ||
            options.crash_phase == "compaction_after_directory_sync") {
            std::cerr << "Directory fsync crash phases are unavailable on Windows; "
                         "the replacement uses MOVEFILE_WRITE_THROUGH\n";
            return false;
        }
#endif
    }
    if (options.disk_full && options.max_bytes < 512) {
        std::cerr << "--disk-full requires --max-bytes of at least 512 bytes\n";
        return false;
    }
    if (options.disk_full && !options.crash_phase.empty()) {
        std::cerr << "--disk-full cannot be combined with --crash-phase\n";
        return false;
    }
    if (options.disk_full &&
        (options.compaction_crash || options.recovery_crash ||
         options.large_values || options.large_batches)) {
        std::cerr << "--disk-full cannot be combined with other specialized crash workloads\n";
        return false;
    }
    if (options.mode == Mode::Compaction &&
        (options.large_values || options.large_batches || options.recovery_crash ||
         options.compaction_crash || options.disk_full || !options.crash_phase.empty())) {
        std::cerr << "Crash workload flags require --mode random-crash or a matching mode\n";
        return false;
    }
    if (options.mode == Mode::Compaction && !options.report_path.empty()) {
        std::cerr << "--report is currently supported for crash workload modes only\n";
        return false;
    }
    return true;
}

bool resolve_kvdb_path(const char* executable_path, std::string& path, std::string& error) {
    if (!path.empty()) return true;
    std::error_code ec;
    const auto runner = std::filesystem::absolute(executable_path, ec);
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

bool resolve_crash_binary_path(const char* executable_path, std::string& path,
                               std::string& error) {
    if (!path.empty()) return true;
    std::error_code ec;
    const auto runner = std::filesystem::absolute(executable_path, ec);
    if (ec) {
        error = "cannot resolve torture runner path: " + ec.message();
        return false;
    }
#if defined(_WIN32)
    path = (runner.parent_path() / "kvdb_crash_test.exe").string();
#else
    path = (runner.parent_path() / "kvdb_crash_test").string();
#endif
    return true;
}

bool path_exists(const std::string& path, bool& exists, std::string& error) {
    std::error_code ec;
    exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "cannot inspect path '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

bool prepare_random_paths(Options& options, std::uint64_t nonce, std::string& error) {
    if (!options.db_path_explicit) {
        std::error_code ec;
        const auto temp = std::filesystem::temp_directory_path(ec);
        if (ec) {
            error = "cannot resolve temporary directory: " + ec.message();
            return false;
        }
        options.db_path = (temp / ("kvdb_torture_" + std::to_string(options.seed) + "_" +
            std::to_string(nonce) + ".db")).string();
    }
    if (!options.oracle_path_explicit) {
        options.oracle_path = options.db_path + ".ack.log";
    }
    if (options.report_path.empty()) {
        options.report_path = options.db_path + ".report.jsonl";
    }

    const std::array<std::string, 3> paths{
        options.db_path, options.oracle_path, options.report_path};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        bool exists = false;
        if (!path_exists(paths[i], exists, error)) return false;
        if (exists) {
            error = "refusing to overwrite existing test artifact '" + paths[i] +
                "'; choose a fresh --db/--oracle/--report path";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (std::filesystem::path(paths[i]).lexically_normal() ==
                std::filesystem::path(paths[j]).lexically_normal()) {
                error = "database, oracle, and report paths must be different";
                return false;
            }
        }
    }
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

bool read_database_state(const Options& options, std::uint64_t cycle,
                         std::map<std::string, std::string>& state, std::string& error) {
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
        if (separator == std::string::npos || separator == 0 ||
            !state.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
            error = "cycle " + std::to_string(cycle) + " malformed/duplicate dump row: " + line;
            return false;
        }
    }

    output.clear();
    if (!run_cli(options.kvdb_path, {"check", options.db_path}, output, exit_code, error)) {
        error = "cycle " + std::to_string(cycle) + " integrity-check process failed: " + error;
        return false;
    }
    if (exit_code != 0 || output.size() != 1 || output.front() != "INTEGRITY_OK") {
        error = "cycle " + std::to_string(cycle) + " integrity check failed";
        if (!output.empty()) error += ": " + output.front();
        return false;
    }
    return true;
}

std::string describe_difference(const std::map<std::string, std::string>& expected,
                                const std::map<std::string, std::string>& actual) {
    auto expected_it = expected.begin();
    auto actual_it = actual.begin();
    while (expected_it != expected.end() || actual_it != actual.end()) {
        if (actual_it == actual.end() ||
            (expected_it != expected.end() && expected_it->first < actual_it->first)) {
            return "missing key '" + expected_it->first + "'";
        }
        if (expected_it == expected.end() || actual_it->first < expected_it->first) {
            return "unexpected key '" + actual_it->first + "'";
        }
        if (expected_it->second != actual_it->second) {
            return "value mismatch for key '" + expected_it->first +
                "': expected '" + expected_it->second + "', actual '" +
                actual_it->second + "'";
        }
        ++expected_it;
        ++actual_it;
    }
    return "state differs";
}

bool verify_state(const Options& options, const AckOracle& oracle, std::uint64_t cycle,
                  std::string& error) {
    std::map<std::string, std::string> actual;
    if (!read_database_state(options, cycle, actual, error)) return false;
    const auto snapshot = oracle.snapshot();
    const std::map<std::string, std::string> expected(snapshot.begin(), snapshot.end());
    if (actual == expected) return true;
    error = "cycle " + std::to_string(cycle) + " state mismatch: " +
        describe_difference(expected, actual) + " (expected " +
        std::to_string(expected.size()) + " keys, found " +
        std::to_string(actual.size()) + ")";
    return false;
}

class RecoveryAllowance {
public:
    void record_acknowledged(const Request& request) {
        update(request, false);
    }

    void record_unacknowledged_commit(const Request& request) {
        update(request, true);
    }

    std::optional<std::string> expected_value(const AckOracle& oracle,
                                              const std::string& key) const {
        const auto acknowledged_value = oracle.get(key);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto uncertain = unacknowledged_.find(key);
        return uncertain == unacknowledged_.end()
            ? acknowledged_value : uncertain->second;
    }

    std::map<std::string, std::string> expected_state(const AckOracle& oracle) const {
        const auto snapshot = oracle.snapshot();
        std::map<std::string, std::string> state(snapshot.begin(), snapshot.end());
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : unacknowledged_) {
            if (entry.second) state[entry.first] = *entry.second;
            else state.erase(entry.first);
        }
        return state;
    }

private:
    void update(const Request& request, bool unacknowledged_commit) {
        if (request.compact ||
            request.generated.type == torture::GeneratedOperation::Type::Get) {
            return;
        }
        const auto apply = [&](const std::string& key,
                               const std::optional<std::string>& value) {
            if (unacknowledged_commit) {
                unacknowledged_[key] = value;
            } else {
                unacknowledged_.erase(key);
            }
        };
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.generated.type == torture::GeneratedOperation::Type::Put) {
            apply(request.generated.key, request.generated.value);
        } else if (request.generated.type == torture::GeneratedOperation::Type::Delete) {
            apply(request.generated.key, std::nullopt);
        } else {
            for (const auto& operation : request.generated.batch) {
                if (operation.type == kvdb::Operation::Type::Put) {
                    apply(operation.key, operation.value);
                } else {
                    apply(operation.key, std::nullopt);
                }
            }
        }
    }

    mutable std::mutex mutex_;
    std::map<std::string, std::optional<std::string>> unacknowledged_;
};

bool verify_random_state(const Options& options, const AckOracle& oracle,
                         RecoveryAllowance& recovery_allowance,
                         const std::optional<Request>& pending,
                         std::uint64_t cycle, std::string& error) {
    std::map<std::string, std::string> actual;
    if (!read_database_state(options, cycle, actual, error)) return false;
    if (options.recovery_crash) {
        std::map<std::string, std::string> repeated_actual;
        if (!read_database_state(options, cycle, repeated_actual, error)) return false;
        if (repeated_actual != actual) {
            error = "cycle " + std::to_string(cycle) +
                " recovery was not idempotent: " +
                describe_difference(actual, repeated_actual);
            return false;
        }
    }

    const auto expected = recovery_allowance.expected_state(oracle);
    if (actual == expected) return true;

    if (pending && !pending->compact &&
        pending->generated.type != torture::GeneratedOperation::Type::Get) {
        auto either_state = expected;
        const auto apply = [&either_state](const kvdb::Operation& operation) {
            if (operation.type == kvdb::Operation::Type::Put) {
                either_state[operation.key] = operation.value;
            } else {
                either_state.erase(operation.key);
            }
        };
        if (pending->generated.type == torture::GeneratedOperation::Type::Put) {
            apply({kvdb::Operation::Type::Put, pending->generated.key,
                   pending->generated.value});
        } else if (pending->generated.type == torture::GeneratedOperation::Type::Delete) {
            apply({kvdb::Operation::Type::Delete, pending->generated.key, {}});
        } else {
            for (const auto& operation : pending->generated.batch) apply(operation);
        }
        if (actual == either_state) {
            recovery_allowance.record_unacknowledged_commit(*pending);
            return true;
        }
    }

    error = "cycle " + std::to_string(cycle) + " recovered state mismatch: " +
        describe_difference(expected, actual) + " (expected " +
        std::to_string(expected.size()) + " acknowledged keys, found " +
        std::to_string(actual.size()) + ")";
    if (pending) {
        error += "; only the in-flight operation " + pending->id +
            " may also have committed without an ACK";
    }
    return false;
}

bool verify_initial_state(const Options& options, const AckOracle& oracle,
                          std::string& error) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(options.db_path, ec);
    if (ec) {
        error = "cannot inspect database path '" + options.db_path + "': " + ec.message();
        return false;
    }
    const auto expected = oracle.snapshot();
    if (!exists) {
        if (!expected.empty()) {
            error = "database is missing but the ACK oracle contains " +
                std::to_string(expected.size()) + " keys";
            return false;
        }
        return true;
    }
    return verify_state(options, oracle, 0, error);
}

template <typename WriteReport>
bool run_concurrency_test(const Options& options, AckOracle& oracle,
                          WriteReport write_report, std::string& error) {
    kvdb::Database db;
    kvdb::Status status = db.open(options.db_path, options.max_bytes);
    if (!kvdb::is_ok(status)) {
        error = std::string("cannot open isolated concurrency database: ") +
            kvdb::status_to_string(status);
        return false;
    }

    constexpr std::size_t atomic_key_count = 8;
    const auto verify_snapshot = [&](std::uint64_t cycle,
                                     const std::set<std::string>& valid_hot_values,
                                     const std::set<std::string>& valid_atomic_generations,
                                     bool check_batch_atomicity,
                                     const std::string& atomic_prefix,
                                     bool compare_oracle) {
        std::ostringstream output;
        const kvdb::Status dump_status = db.dump(output);
        if (!kvdb::is_ok(dump_status)) {
            error = "concurrency cycle " + std::to_string(cycle) +
                " dump failed: " + kvdb::status_to_string(dump_status);
            return false;
        }

        std::map<std::string, std::string> actual;
        std::istringstream input(output.str());
        std::string line;
        while (std::getline(input, line)) {
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos || separator == 0 ||
                !actual.emplace(line.substr(0, separator),
                                line.substr(separator + 1)).second) {
                error = "concurrency cycle " + std::to_string(cycle) +
                    " dump contains a malformed or duplicate row";
                return false;
            }
        }

        const auto hot = actual.find("concurrency_hot_key");
        if (hot != actual.end() && valid_hot_values.count(hot->second) == 0) {
            error = "concurrency cycle " + std::to_string(cycle) +
                " hot key contains an ungenerated or torn value";
            return false;
        }
        actual.erase("concurrency_hot_key");

        if (check_batch_atomicity) {
            std::string generation;
            for (std::size_t i = 0; i < atomic_key_count; ++i) {
                const auto found = actual.find(atomic_prefix + std::to_string(i));
                if (found == actual.end()) {
                    error = "concurrency cycle " + std::to_string(cycle) +
                        " atomic batch snapshot is missing a marker key";
                    return false;
                }
                if (i == 0) generation = found->second;
                else if (found->second != generation) {
                    error = "concurrency cycle " + std::to_string(cycle) +
                        " reader observed a partially visible batch";
                    return false;
                }
                if (valid_atomic_generations.count(found->second) == 0) {
                    error = "concurrency cycle " + std::to_string(cycle) +
                        " atomic batch snapshot contains an unknown generation";
                    return false;
                }
            }
        }

        if (compare_oracle) {
            const auto snapshot = oracle.snapshot();
            const std::map<std::string, std::string> expected(snapshot.begin(), snapshot.end());
            if (actual != expected) {
                error = "concurrency cycle " + std::to_string(cycle) +
                    " state mismatch: " + describe_difference(expected, actual);
                return false;
            }
        }
        return true;
    };

    std::uint64_t total_operations = 0;
    std::set<std::string> historical_hot_values;
    for (std::uint64_t cycle = 0; cycle < options.cycles; ++cycle) {
        const std::string atomic_prefix =
            "concurrency_atomic_" + std::to_string(cycle) + "_";
        const std::size_t batch_rounds = static_cast<std::size_t>(
            std::max<std::uint64_t>(2, options.operations_per_worker));
        std::set<std::string> valid_atomic_generations{"generation_0"};
        for (std::size_t generation = 1; generation <= batch_rounds; ++generation) {
            valid_atomic_generations.insert(
                "generation_" + std::to_string(generation));
        }
        std::vector<kvdb::Operation> initial_atomic_batch;
        for (std::size_t i = 0; i < atomic_key_count; ++i) {
            initial_atomic_batch.push_back({kvdb::Operation::Type::Put,
                atomic_prefix + std::to_string(i), "generation_0"});
        }
        status = db.batch(initial_atomic_batch);
        if (!kvdb::is_ok(status)) {
            error = std::string("cannot initialize atomicity workload: ") +
                kvdb::status_to_string(status);
            return false;
        }
        if (!oracle.record_batch(initial_atomic_batch, error)) return false;

        std::vector<std::vector<torture::GeneratedOperation>> workloads(options.threads);
        std::map<std::string, std::set<std::string>> candidate_values;
        std::set<std::string> hot_values = historical_hot_values;
        for (const auto& entry : oracle.snapshot()) {
            candidate_values[entry.first].insert(entry.second);
        }
        for (std::uint32_t worker = 0; worker < options.threads; ++worker) {
            const std::uint64_t worker_seed = options.seed ^
                (0x9e3779b97f4a7c15ULL * (cycle + 1));
            torture::WorkloadGenerator generator(worker_seed, worker,
                                                   torture::WorkloadMode::All);
            auto& operations = workloads[worker];
            operations.reserve(static_cast<std::size_t>(options.operations_per_worker));
            for (std::uint64_t index = 0; index < options.operations_per_worker; ++index) {
                auto operation = generator.next();
                if (index % 4 == 0) {
                    operation = {torture::GeneratedOperation::Type::Put,
                        "concurrency_hot_key",
                        "worker_" + std::to_string(worker) + "_value_" +
                            std::to_string(cycle) + "_" + std::to_string(index), {}};
                    hot_values.insert(operation.value);
                } else if (operation.type == torture::GeneratedOperation::Type::Batch) {
                    for (auto& item : operation.batch) {
                        item.key = "worker_" + std::to_string(worker) + "_" + item.key;
                    }
                } else {
                    operation.key = "worker_" + std::to_string(worker) + "_" +
                        operation.key;
                }
                if (operation.type == torture::GeneratedOperation::Type::Put) {
                    candidate_values[operation.key].insert(operation.value);
                } else if (operation.type == torture::GeneratedOperation::Type::Batch) {
                    for (const auto& item : operation.batch) {
                        if (item.type == kvdb::Operation::Type::Put) {
                            candidate_values[item.key].insert(item.value);
                        }
                    }
                }
                operations.push_back(std::move(operation));
            }
        }
        historical_hot_values = hot_values;

        std::atomic<bool> running{true};
        std::atomic<bool> failed{false};
        std::atomic<std::uint64_t> cycle_operations{0};
        std::mutex snapshot_mutex;
        std::mutex error_mutex;
        std::string first_error;
        auto fail = [&](const std::string& message) {
            bool expected = false;
            if (failed.compare_exchange_strong(expected, true)) {
                std::lock_guard<std::mutex> lock(error_mutex);
                first_error = message;
            }
        };
        std::mutex start_mutex;
        std::condition_variable start_condition;
        bool start_workers = false;
        auto await_start = [&] {
            std::unique_lock<std::mutex> lock(start_mutex);
            start_condition.wait(lock, [&] { return start_workers; });
        };

        std::vector<std::thread> writers;
        writers.reserve(options.threads + 1);
        for (std::uint32_t worker = 0; worker < options.threads; ++worker) {
            writers.emplace_back([&, worker] {
                await_start();
                std::mt19937_64 schedule_rng(options.seed ^
                    (0xd1b54a32d192ed03ULL * (worker + 1)) ^ cycle);
                for (std::size_t index = 0;
                     index < workloads[worker].size() && !failed.load(); ++index) {
                    const auto& operation = workloads[worker][index];
                    kvdb::Status operation_status = kvdb::Status::Ok;
                    if (operation.type == torture::GeneratedOperation::Type::Put) {
                        operation_status = db.put(operation.key, operation.value);
                        if (kvdb::is_ok(operation_status) &&
                            operation.key != "concurrency_hot_key") {
                            std::string journal_error;
                            if (!oracle.record_put(operation.key, operation.value,
                                                   journal_error)) {
                                fail("cannot journal acknowledged PUT: " + journal_error);
                                break;
                            }
                        }
                    } else if (operation.type == torture::GeneratedOperation::Type::Delete) {
                        operation_status = db.remove(operation.key);
                        if (kvdb::is_ok(operation_status)) {
                            std::string journal_error;
                            if (!oracle.record_del(operation.key, journal_error)) {
                                fail("cannot journal acknowledged DEL: " + journal_error);
                                break;
                            }
                        }
                    } else if (operation.type == torture::GeneratedOperation::Type::Batch) {
                        operation_status = db.batch(operation.batch);
                        if (kvdb::is_ok(operation_status)) {
                            std::string journal_error;
                            if (!oracle.record_batch(operation.batch, journal_error)) {
                                fail("cannot journal acknowledged BATCH: " + journal_error);
                                break;
                            }
                        }
                    } else {
                        const auto result = db.get(operation.key);
                        const auto expected = oracle.get(operation.key);
                        if ((expected && (!kvdb::is_ok(result.status) ||
                                          result.value != *expected)) ||
                            (!expected && result.status != kvdb::Status::NotFound)) {
                            fail("concurrent GET returned an invalid state for " +
                                 operation.key);
                            break;
                        }
                    }
                    if (operation.type != torture::GeneratedOperation::Type::Get &&
                        !kvdb::is_ok(operation_status)) {
                        fail(std::string("concurrent mutation failed: ") +
                             kvdb::status_to_string(operation_status));
                        break;
                    }
                    if (index % 3 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(
                            std::uniform_int_distribution<int>(0, 200)(schedule_rng)));
                    }
                    ++cycle_operations;
                }
            });
        }

        writers.emplace_back([&, batch_rounds] {
            await_start();
            for (std::size_t generation = 1;
                 generation <= batch_rounds && !failed.load(); ++generation) {
                std::vector<kvdb::Operation> operations;
                operations.reserve(atomic_key_count);
                const std::string value = "generation_" + std::to_string(generation);
                for (std::size_t i = 0; i < atomic_key_count; ++i) {
                    operations.push_back({kvdb::Operation::Type::Put,
                        atomic_prefix + std::to_string(i), value});
                }
                const kvdb::Status batch_status = db.batch(operations);
                if (!kvdb::is_ok(batch_status)) {
                    fail(std::string("atomic batch writer failed: ") +
                         kvdb::status_to_string(batch_status));
                    break;
                }
                std::string oracle_error;
                if (!oracle.record_batch(operations, oracle_error)) {
                    fail("cannot journal atomic batch ACK: " + oracle_error);
                    break;
                }
                ++cycle_operations;
            }
        });

        std::vector<std::thread> readers;
        readers.reserve(2);
        for (unsigned int reader = 0; reader < 2; ++reader) {
            readers.emplace_back([&, reader] {
                await_start();
                std::mt19937_64 read_rng(options.seed ^
                    (0x94d049bb133111ebULL * (reader + 1)) ^ cycle);
                std::vector<std::string> keys;
                keys.reserve(candidate_values.size());
                for (const auto& candidate : candidate_values) {
                    if (candidate.first.rfind(atomic_prefix, 0) != 0 &&
                        candidate.first != "concurrency_hot_key") {
                        keys.push_back(candidate.first);
                    }
                }
                while (running.load() && !failed.load()) {
                    const auto hot = db.get("concurrency_hot_key");
                    if (kvdb::is_ok(hot.status) &&
                        hot_values.count(hot.value) == 0) {
                        fail("concurrent hot-key GET observed a torn or unknown value");
                        break;
                    }
                    if (!kvdb::is_ok(hot.status) &&
                        hot.status != kvdb::Status::NotFound) {
                        fail(std::string("concurrent hot-key GET failed: ") +
                             kvdb::status_to_string(hot.status));
                        break;
                    }

                    if (!keys.empty()) {
                        const auto& key = keys[std::uniform_int_distribution<std::size_t>(
                            0, keys.size() - 1)(read_rng)];
                        const auto result = db.get(key);
                        if (kvdb::is_ok(result.status)) {
                            const auto found = candidate_values.find(key);
                            if (found == candidate_values.end() ||
                                found->second.count(result.value) == 0) {
                                fail("concurrent GET observed an ungenerated value for key '" +
                                     key + "': '" + result.value + "'");
                                break;
                            }
                        } else if (result.status != kvdb::Status::NotFound) {
                            fail(std::string("concurrent GET failed: ") +
                                 kvdb::status_to_string(result.status));
                            break;
                        }
                    }
                    std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex);
                    std::string snapshot_error;
                    if (!verify_snapshot(cycle, hot_values, valid_atomic_generations,
                                         true, atomic_prefix, false)) {
                        snapshot_error = error;
                        fail(snapshot_error);
                        break;
                    }
                    std::this_thread::yield();
                }
            });
        }

        {
            std::lock_guard<std::mutex> lock(start_mutex);
            start_workers = true;
        }
        start_condition.notify_all();
        for (auto& writer : writers) writer.join();
        running.store(false);
        for (auto& reader : readers) reader.join();
        if (failed.load()) {
            std::lock_guard<std::mutex> lock(error_mutex);
            error = first_error;
            return false;
        }

        if (!verify_snapshot(cycle, hot_values, valid_atomic_generations, true,
                             atomic_prefix, true)) return false;
        status = db.close();
        if (!kvdb::is_ok(status)) {
            error = std::string("cannot close concurrency database: ") +
                kvdb::status_to_string(status);
            return false;
        }
        status = db.open(options.db_path, options.max_bytes);
        if (!kvdb::is_ok(status)) {
            error = std::string("cannot reopen concurrency database: ") +
                kvdb::status_to_string(status);
            return false;
        }
        if (!verify_snapshot(cycle, hot_values, valid_atomic_generations, true,
                             atomic_prefix, true)) return false;

        total_operations += cycle_operations.load();
        const std::string event = "{\"event\":\"concurrency_cycle_verified\","
            "\"seed\":" + std::to_string(options.seed) + ",\"cycle\":" +
            std::to_string(cycle) + ",\"workers\":" +
            std::to_string(options.threads) + ",\"operations\":" +
            std::to_string(cycle_operations.load()) + ",\"batch_readers\":2}";
        if (!write_report(event, error)) return false;
        std::cout << "[Torture] PASS concurrency cycle=" << cycle
                  << " workers=" << options.threads
                  << " operations=" << total_operations
                  << " batch_snapshots=verified\n";
    }

    status = db.close();
    if (!kvdb::is_ok(status)) {
        error = std::string("cannot close concurrency database: ") +
            kvdb::status_to_string(status);
        return false;
    }
    const std::string completion =
        "{\"event\":\"concurrency_run_complete\",\"seed\":" +
        std::to_string(options.seed) + ",\"iterations\":" +
        std::to_string(options.cycles) + ",\"workers\":" +
        std::to_string(options.threads) + ",\"operations\":" +
        std::to_string(total_operations) + "}";
    if (!write_report(completion, error)) return false;
    return true;
}

std::vector<kvdb::Operation> mutations_for(
    const torture::GeneratedOperation& generated) {
    if (generated.type == torture::GeneratedOperation::Type::Put) {
        return {{kvdb::Operation::Type::Put, generated.key, generated.value}};
    }
    if (generated.type == torture::GeneratedOperation::Type::Delete) {
        return {{kvdb::Operation::Type::Delete, generated.key, {}}};
    }
    if (generated.type == torture::GeneratedOperation::Type::Batch) {
        return generated.batch;
    }
    return {};
}

bool write_batch_file(const std::string& path, const std::vector<kvdb::Operation>& operations,
                      std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec) {
        error = ec ? "cannot inspect batch path '" + path + "': " + ec.message()
                   : "refusing to overwrite batch file '" + path + "'";
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = "cannot create batch file '" + path + "'";
        return false;
    }
    for (const auto& operation : operations) {
        if (operation.type == kvdb::Operation::Type::Put) {
            output << "PUT " << operation.key << " " << operation.value << "\n";
        } else {
            output << "DEL " << operation.key << "\n";
        }
    }
    output.close();
    if (!output) {
        error = "failed writing batch file '" + path + "'";
        return false;
    }
    return true;
}

std::string describe_request(const Request& request) {
    if (request.compact) return "COMPACT";
    switch (request.generated.type) {
    case torture::GeneratedOperation::Type::Put:
        return "PUT " + request.generated.key + " (" +
            std::to_string(request.generated.value.size()) + " bytes)";
    case torture::GeneratedOperation::Type::Get:
        return "GET " + request.generated.key;
    case torture::GeneratedOperation::Type::Delete:
        return "DEL " + request.generated.key;
    case torture::GeneratedOperation::Type::Batch:
        return "BATCH " + std::to_string(request.generated.batch.size()) + " ops";
    }
    return "unknown";
}

bool make_request(torture::WorkloadGenerator& generator, std::mt19937_64& compaction_rng,
                  std::uint32_t compact_rate, std::uint32_t worker_id,
                  std::uint64_t operation_index, std::uint64_t nonce,
                  std::uint64_t cycle, const std::string& crash_operation,
                  bool allow_compaction, bool large_values, bool large_batches,
                  bool large_batch_partial_write, bool force_disk_full,
                  bool force_compaction,
                  Request& request, std::string& error) {
    request.generated = generator.next();
    if (crash_operation == "delete") {
        request.generated = {torture::GeneratedOperation::Type::Delete,
            "torture_bootstrap_" + std::to_string(cycle), {}, {}};
    } else if (crash_operation == "put") {
        request.generated = {torture::GeneratedOperation::Type::Put,
            "crash_target_" + std::to_string(worker_id) + "_" +
                std::to_string(operation_index),
            "crash_target_value_" + std::to_string(cycle), {}};
    } else if (force_disk_full) {
        if (operation_index % 2 == 0) {
            request.generated = {torture::GeneratedOperation::Type::Put,
                "disk_full_" + std::to_string(worker_id) + "_" +
                    std::to_string(operation_index),
                std::string(64U * 1024U, 'D'), {}};
        } else {
            std::vector<kvdb::Operation> operations{
                {kvdb::Operation::Type::Put,
                 "disk_full_batch_" + std::to_string(worker_id) + "_" +
                     std::to_string(operation_index) + "_0",
                 std::string(64U * 1024U, 'B')},
                {kvdb::Operation::Type::Put,
                 "disk_full_batch_" + std::to_string(worker_id) + "_" +
                     std::to_string(operation_index) + "_1",
                 std::string(64U * 1024U, 'C')}};
            request.generated = {torture::GeneratedOperation::Type::Batch, {}, {},
                std::move(operations)};
        }
    } else if (large_values) {
        request.generated = {torture::GeneratedOperation::Type::Put,
            "large_value_" + std::to_string(worker_id) + "_" +
                std::to_string(operation_index),
            std::string(1024U * 1024U, 'V'), {}};
    } else if (large_batches) {
        std::vector<kvdb::Operation> operations;
        operations.reserve(kvdb::MAX_BATCH_OPERATIONS);
        for (std::size_t i = 0; i < kvdb::MAX_BATCH_OPERATIONS - 3; ++i) {
            const auto key_index = static_cast<std::uint64_t>(i % 1024);
            operations.push_back({kvdb::Operation::Type::Put,
                "large_batch_key_" + std::to_string(key_index),
                i == 0 && large_batch_partial_write
                    ? std::string(1024U * 1024U, 'B')
                    : "large_batch_value_" + std::to_string(operation_index) + "_" +
                          std::to_string(i)});
        }
        operations.push_back({kvdb::Operation::Type::Put, "batch_order_key", "before_delete"});
        operations.push_back({kvdb::Operation::Type::Delete, "batch_order_key", {}});
        operations.push_back({kvdb::Operation::Type::Put, "batch_order_key", "after_delete"});
        request.generated = {torture::GeneratedOperation::Type::Batch, {}, {},
            std::move(operations)};
    }
    request.id = std::to_string(worker_id) + "_" + std::to_string(operation_index);
    if (request.generated.type == torture::GeneratedOperation::Type::Batch) {
        request.batch_id = "batch_" + request.id;
    }
    request.compact = force_compaction ||
        (allow_compaction && std::uniform_int_distribution<std::uint32_t>(
            0, compact_rate - 1)(compaction_rng) == 0);
    if (!request.compact &&
        request.generated.type == torture::GeneratedOperation::Type::Batch) {
        request.batch_path = "torture_phase10_" + std::to_string(nonce) + "_w" +
            std::to_string(worker_id) + "_op" + std::to_string(operation_index) + ".batch";
        if (!write_batch_file(request.batch_path, request.generated.batch, error)) return false;
    }
    request.description = describe_request(request);
    return true;
}

std::vector<std::string> serve_arguments(const Options& options) {
    std::vector<std::string> arguments{"serve", options.db_path};
    if (options.max_bytes != 0) {
        arguments.push_back("--max-bytes");
        arguments.push_back(std::to_string(options.max_bytes));
    }
    return arguments;
}

class CompactionSession {
public:
    CompactionSession(const Options& options, AckOracle& oracle, std::uint64_t cycle,
                      std::uint64_t stop_after)
        : options_(options), oracle_(oracle), cycle_(cycle), stop_after_(stop_after) {}

    bool start(std::string& error) {
        if (!server_.start(options_.kvdb_path, serve_arguments(options_), error)) return false;
        if (!server_.write_line("PING phase10_ready", error)) return false;
        std::string response;
        bool eof = false;
        if (!server_.read_line(response, 30s, eof, error) || response != "PONG phase10_ready") {
            if (error.empty()) error = "unexpected server startup response: " + response;
            return false;
        }
        return true;
    }

    bool execute(const Request& request, std::uint32_t worker_id,
                 std::uint64_t operation_index, std::string& error) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (stopped_) return false;
        if (completed_ >= stop_after_) {
            if (!server_.request_termination(error)) {
                fatal_error_ = "could not inject cycle crash: " + error;
                return false;
            }
            stopped_ = true;
            return false;
        }

        const auto mutations = mutations_for(request.generated);
        std::string command;
        if (request.compact) {
            command = "COMPACT " + request.id;
        } else if (request.generated.type == torture::GeneratedOperation::Type::Put) {
            command = "PUT " + request.id + " " + request.generated.key + " " +
                request.generated.value;
        } else if (request.generated.type == torture::GeneratedOperation::Type::Get) {
            command = "GET " + request.id + " " + request.generated.key;
        } else if (request.generated.type == torture::GeneratedOperation::Type::Delete) {
            command = "DEL " + request.id + " " + request.generated.key;
        } else {
            command = "BATCH " + request.id + " " + request.batch_path;
        }

        std::string response;
        bool eof = false;
        if (!server_.write_line(command, error) ||
            !server_.read_line(response, 30s, eof, error)) {
            fatal_error_ = "child failed during " + request.description + ": " + error;
            add_history(worker_id, operation_index, request.description + " [no response]");
            return false;
        }
        if (!validate_response(request, response, error)) {
            fatal_error_ = "worker " + std::to_string(worker_id) + " operation " +
                std::to_string(operation_index) + ": " + error;
            add_history(worker_id, operation_index, request.description + " [invalid response]");
            return false;
        }

        if (!record_acknowledged_mutation(request.generated, request.compact, error)) {
            fatal_error_ = "ACK received but oracle update failed: " + error;
            add_history(worker_id, operation_index, request.description + " [oracle failure]");
            return false;
        }
        ++completed_;
        if (request.compact) ++compactions_;
        add_history(worker_id, operation_index, request.description + " [ACK]");
        return true;
    }

    bool stop(std::string& error) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (stopped_) {
            int exit_code = -1;
            return server_.wait(30s, exit_code, error);
        }
        stopped_ = true;
        return server_.terminate(error);
    }

    bool stopped() const {
        return stopped_.load();
    }

    std::string fatal_error() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return fatal_error_;
    }

    std::uint64_t acknowledgements() const {
        return completed_.load();
    }

    std::uint64_t compactions() const {
        return compactions_.load();
    }

    std::vector<std::string> history() const {
        std::lock_guard<std::mutex> lock(history_mutex_);
        return history_;
    }

private:
    bool validate_response(const Request& request, const std::string& response,
                           std::string& error) {
        if (!request.compact &&
            request.generated.type == torture::GeneratedOperation::Type::Get) {
            const auto expected = oracle_.get(request.generated.key);
            const std::string prefix = "VALUE " + request.id + " ";
            if (expected.has_value()) {
                if (response.compare(0, prefix.size(), prefix) != 0 ||
                    response.substr(prefix.size()) != *expected) {
                    error = "GET response mismatch for " + request.generated.key;
                    return false;
                }
            } else if (response != "NOT_FOUND " + request.id) {
                error = "GET expected NOT_FOUND for " + request.generated.key +
                    ", got: " + response;
                return false;
            }
            return true;
        }

        if (response != "ACK " + request.id) {
            error = "expected ACK " + request.id + ", got: " + response;
            return false;
        }
        return true;
    }

    bool record_acknowledged_mutation(const torture::GeneratedOperation& generated,
                                      bool compact, std::string& error) {
        if (compact || generated.type == torture::GeneratedOperation::Type::Get) return true;
        if (generated.type == torture::GeneratedOperation::Type::Put) {
            return oracle_.record_put(generated.key, generated.value, error);
        }
        if (generated.type == torture::GeneratedOperation::Type::Delete) {
            return oracle_.record_del(generated.key, error);
        }
        return oracle_.record_batch(generated.batch, error);
    }

    void add_history(std::uint32_t worker_id, std::uint64_t operation_index,
                     const std::string& text) {
        std::lock_guard<std::mutex> lock(history_mutex_);
        history_.push_back("cycle " + std::to_string(cycle_) + " worker " +
            std::to_string(worker_id) + " op " + std::to_string(operation_index) +
            ": " + text);
        if (history_.size() > 40) history_.erase(history_.begin());
    }

    const Options& options_;
    AckOracle& oracle_;
    std::uint64_t cycle_;
    std::uint64_t stop_after_;
    torture::ChildProcess server_;
    mutable std::mutex io_mutex_;
    mutable std::mutex history_mutex_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> compactions_{0};
    std::vector<std::string> history_;
    std::string fatal_error_;
};

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                static constexpr char digits[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(digits[ch >> 4]);
                escaped.push_back(digits[ch & 0x0f]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}

std::uint64_t unix_time_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

class CrashReport {
public:
    explicit CrashReport(const std::string& path) : path_(path) {}

    bool open(std::string& error) {
        if (path_.empty()) return true;
        output_.open(path_, std::ios::out | std::ios::trunc);
        if (!output_) {
            error = "cannot create crash report '" + path_ + "'";
            return false;
        }
        return true;
    }

    bool write(const std::string& json, std::string& error) {
        if (path_.empty()) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        output_ << json << '\n';
        output_.flush();
        if (!output_) {
            error = "failed writing crash report '" + path_ + "'";
            return false;
        }
        return true;
    }

private:
    std::string path_;
    std::ofstream output_;
    std::mutex mutex_;
};

class RandomCrashSession {
public:
    RandomCrashSession(const Options& options, AckOracle& oracle,
                       RecoveryAllowance& recovery_allowance, CrashReport& report,
                       std::uint64_t cycle, std::string compaction_target)
        : options_(options), oracle_(oracle), recovery_allowance_(recovery_allowance),
          report_(report), cycle_(cycle),
          compaction_target_(std::move(compaction_target)) {}

    bool start(std::string& error) {
        const bool phase_crash = !options_.crash_phase.empty() &&
            !is_close_crash_phase(options_.crash_phase);
        const std::string& executable = options_.compaction_crash || phase_crash
            ? options_.crash_binary_path : options_.kvdb_path;
        std::vector<std::pair<std::string, std::string>> environment;
        if (options_.compaction_crash || phase_crash) {
            environment.emplace_back("KVDB_TEST_REPORT_PHASES", "1");
        }
        if (!server_.start(executable, serve_arguments(options_), error, environment)) {
            return false;
        }
        const std::string op_id = "ready_" + std::to_string(cycle_);
        if (!server_.write_line("PING " + op_id, error)) return false;
        std::string response;
        bool eof = false;
        while (true) {
            eof = false;
            if (!server_.read_line(response, 30s, eof, error) || eof) {
                if (error.empty()) error = "server exited during startup";
                return false;
            }
            if (response.rfind("KVDB_TEST_PHASE ", 0) == 0) continue;
            if (response != "PONG " + op_id) {
                error = "unexpected server startup response: " + response;
                return false;
            }
            break;
        }
        Request bootstrap;
        bootstrap.id = "bootstrap_" + std::to_string(cycle_);
        bootstrap.generated = {torture::GeneratedOperation::Type::Put,
            "torture_bootstrap_" + std::to_string(cycle_),
            "seed_" + std::to_string(options_.seed), {}};
        bootstrap.description = describe_request(bootstrap);
        if (!server_.write_line("PUT " + bootstrap.id + " " +
                                bootstrap.generated.key + " " +
                                bootstrap.generated.value, error)) return false;
        while (true) {
            eof = false;
            if (!server_.read_line(response, 30s, eof, error) || eof) {
                if (error.empty()) error = "bootstrap PUT was not acknowledged";
                return false;
            }
            if (response.rfind("KVDB_TEST_PHASE ", 0) == 0) continue;
            if (response != "ACK " + bootstrap.id) {
                error = "bootstrap PUT was not acknowledged: " + response;
                return false;
            }
            break;
        }
        if (!oracle_.record_put(bootstrap.generated.key, bootstrap.generated.value, error)) {
            error = "bootstrap ACK could not be recorded in oracle: " + error;
            return false;
        }
        recovery_allowance_.record_acknowledged(bootstrap);
        const std::string bootstrap_event =
            "{\"event\":\"ack\",\"timestamp_unix_ms\":" +
            std::to_string(unix_time_ms()) + ",\"cycle\":" + std::to_string(cycle_) +
            ",\"worker\":-1,\"operation_id\":\"" + json_escape(bootstrap.id) +
            "\",\"batch_id\":\"\",\"operation\":\"bootstrap PUT\"}";
        if (!report_.write(bootstrap_event, error)) return false;
        if (options_.compaction_crash || phase_crash) {
            const std::string& target = options_.crash_phase.empty()
                ? compaction_target_ : options_.crash_phase;
            if (!server_.write_line("ARM " + target + " " +
                                    std::to_string(options_.crash_occurrence), error)) {
                return false;
            }
            while (true) {
                eof = false;
                if (!server_.read_line(response, 30s, eof, error) || eof) {
                    if (error.empty()) error = "server exited while arming crash phase";
                    return false;
                }
                if (response.rfind("KVDB_TEST_PHASE ", 0) == 0) continue;
                if (response != "ARMED " + target) {
                    error = "unexpected crash-phase arm response: " + response;
                    return false;
                }
                break;
            }
        }
        return true;
    }

    bool execute(const Request& request, std::uint32_t worker_id,
                 std::uint64_t operation_index, std::string& error) {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (crashed_.load() || failed_.load() || disk_full_complete()) return false;

        const std::string command = command_for(request);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            active_ = true;
            active_operation_ = request.description;
            active_id_ = request.id;
            active_phase_.clear();
            pending_request_ = request;
        }
        state_changed_.notify_all();

        std::string response;
        bool received = false;
        if (server_.write_line(command, error)) {
            while (true) {
                bool eof = false;
                if (!server_.read_line(response, 30s, eof, error)) break;
                if (eof) break;
                constexpr const char* phase_prefix = "KVDB_TEST_PHASE ";
                if (response.rfind(phase_prefix, 0) == 0) {
                    {
                        std::lock_guard<std::mutex> state_lock(state_mutex_);
                        active_phase_ = response.substr(std::char_traits<char>::length(
                            phase_prefix));
                    }
                    state_changed_.notify_all();
                    continue;
                }
                received = true;
                break;
            }
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            active_ = false;
            pending_request_.reset();
        }
        state_changed_.notify_all();
        if (!received) {
            if (crashed_.load()) return false;
            error = "child failed during operation " + request.id + " (" +
                request.description + "): " + (error.empty() ? "unexpected EOF" : error);
            failed_.store(true);
            set_fatal_error(error);
            state_changed_.notify_all();
            return false;
        }

        if (options_.disk_full && response == "ERR " + request.id + " NoSpace") {
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (request.generated.type == torture::GeneratedOperation::Type::Put) {
                    disk_full_put_seen_ = true;
                } else if (request.generated.type == torture::GeneratedOperation::Type::Batch) {
                    disk_full_batch_seen_ = true;
                }
                active_operation_ = request.description + " (NoSpace)";
                active_id_ = request.id;
            }
            const std::string event = "{\"event\":\"no_space\",\"timestamp_unix_ms\":" +
                std::to_string(unix_time_ms()) + ",\"cycle\":" +
                std::to_string(cycle_) + ",\"operation_id\":\"" +
                json_escape(request.id) + "\",\"batch_id\":\"" +
                json_escape(request.batch_id) + "\",\"operation\":\"" +
                json_escape(request.description) + "\"}";
            if (!report_.write(event, error)) {
                failed_.store(true);
                set_fatal_error(error);
                state_changed_.notify_all();
                return false;
            }
            state_changed_.notify_all();
            return true;
        }

        if (!validate_response(request, response, error)) {
            error = "worker " + std::to_string(worker_id) + " operation " +
                std::to_string(operation_index) + ": " + error;
            failed_.store(true);
            set_fatal_error(error);
            state_changed_.notify_all();
            return false;
        }

        if (!record_acknowledged_mutation(request, error)) {
            error = "ACK received but oracle update failed for " + request.id + ": " + error;
            failed_.store(true);
            set_fatal_error(error);
            state_changed_.notify_all();
            return false;
        }
        recovery_allowance_.record_acknowledged(request);
        ++acknowledgements_;
        const std::string event = "{\"event\":\"ack\",\"timestamp_unix_ms\":" +
            std::to_string(unix_time_ms()) + ",\"cycle\":" + std::to_string(cycle_) +
            ",\"worker\":" + std::to_string(worker_id) + ",\"operation_id\":\"" +
            json_escape(request.id) + "\",\"batch_id\":\"" +
            json_escape(request.batch_id) + "\",\"operation\":\"" +
            json_escape(request.description) + "\"}";
        if (!report_.write(event, error)) {
            failed_.store(true);
            set_fatal_error(error);
            state_changed_.notify_all();
            return false;
        }
        if (options_.verbose) {
            std::cout << "[Torture] cycle=" << cycle_ << " ACK op_id=" << request.id
                      << " phase=acknowledged\n";
        }
        return true;
    }

    bool wait_and_kill(std::mt19937_64& timing_rng, std::string& error) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        while (!crashed_.load() && !failed_.load()) {
            state_changed_.wait(lock, [&] {
                return active_ || (disk_full_put_seen_ && disk_full_batch_seen_) ||
                    crashed_.load() || failed_.load();
            });
            if (crashed_.load() || failed_.load()) break;
            if (disk_full_put_seen_ && disk_full_batch_seen_) {
                return kill_locked("disk-cap response", error);
            }
            if (options_.compaction_crash ||
                (!options_.crash_phase.empty() &&
                 !is_close_crash_phase(options_.crash_phase))) {
                const std::string& target = options_.crash_phase.empty()
                    ? compaction_target_ : options_.crash_phase;
                state_changed_.wait(lock, [&] {
                    return active_phase_ == target ||
                        (disk_full_put_seen_ && disk_full_batch_seen_) ||
                        failed_.load() || crashed_.load();
                });
                if (failed_.load() || crashed_.load()) break;
                if (disk_full_put_seen_ && disk_full_batch_seen_) {
                    return kill_locked("disk-cap response", error);
                }
                return kill_locked(target, error);
            }
            if (options_.disk_full) {
                state_changed_.wait(lock, [&] {
                    return (disk_full_put_seen_ && disk_full_batch_seen_) ||
                        failed_.load() || crashed_.load();
                });
                if (failed_.load() || crashed_.load()) break;
                return kill_locked("disk-cap response", error);
            }

            const auto delay = std::chrono::milliseconds(
                std::uniform_int_distribution<std::uint32_t>(1, 100)(timing_rng));
            const bool operation_finished = state_changed_.wait_for(lock, delay, [&] {
                return !active_ || failed_.load() || crashed_.load();
            });
            if (failed_.load() || crashed_.load()) break;
            if (operation_finished || !active_) continue;

            const std::string phase = active_operation_.empty()
                ? "request-in-flight" : "request-in-flight: " + active_operation_;
            return kill_locked(phase, error);
        }
        if (failed_.load()) {
            error = fatal_error_.empty()
                ? "a worker failed before the scheduled crash"
                : "worker failed before the scheduled crash: " + fatal_error_;
            return false;
        }
        return crashed_.load();
    }

    bool terminate_after_failure(std::string& error) {
        std::string termination_error;
        if (!server_.terminate(termination_error)) {
            error = "failed to reap child after worker failure: " + termination_error;
            return false;
        }
        return true;
    }

    void abort(const std::string& error) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!failed_.exchange(true)) fatal_error_ = error;
        state_changed_.notify_all();
    }

    bool reap(std::string& error) {
        int exit_code = -1;
        if (!server_.wait(30s, exit_code, error)) {
            std::string kill_error;
            if (!server_.terminate(kill_error)) error += "; " + kill_error;
            return false;
        }
        if (!crashed_.load() || exit_code == 0) {
            error = "crash cycle did not end with an abrupt process termination (exit " +
                std::to_string(exit_code) + ")";
            return false;
        }
        return true;
    }

    bool crashed() const { return crashed_.load(); }
    bool failed() const { return failed_.load(); }
    bool disk_full_complete() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return disk_full_put_seen_ && disk_full_batch_seen_;
    }
    std::optional<Request> crash_request() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return crash_request_;
    }
    std::uint64_t acknowledgements() const { return acknowledgements_.load(); }
    std::string fatal_error() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return fatal_error_;
    }

private:
    void set_fatal_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (fatal_error_.empty()) fatal_error_ = message;
    }

    static std::string command_for(const Request& request) {
        if (request.compact) return "COMPACT " + request.id;
        if (request.generated.type == torture::GeneratedOperation::Type::Put) {
            return "PUT " + request.id + " " + request.generated.key + " " +
                request.generated.value;
        }
        if (request.generated.type == torture::GeneratedOperation::Type::Get) {
            return "GET " + request.id + " " + request.generated.key;
        }
        if (request.generated.type == torture::GeneratedOperation::Type::Delete) {
            return "DEL " + request.id + " " + request.generated.key;
        }
        return "BATCH " + request.id + " " + request.batch_path;
    }

    bool validate_response(const Request& request, const std::string& response,
                           std::string& error) const {
        if (request.compact) {
            if (response != "ACK " + request.id) {
                error = "expected ACK " + request.id + ", got: " + response;
                return false;
            }
            return true;
        }
        if (request.generated.type == torture::GeneratedOperation::Type::Get) {
            const auto expected = recovery_allowance_.expected_value(
                oracle_, request.generated.key);
            const std::string prefix = "VALUE " + request.id + " ";
            if (expected) {
                if (response.compare(0, prefix.size(), prefix) != 0 ||
                    response.substr(prefix.size()) != *expected) {
                    error = "GET response mismatch for " + request.generated.key +
                        " (operation " + request.id + ")";
                    return false;
                }
            } else if (response != "NOT_FOUND " + request.id) {
                error = "GET expected NOT_FOUND for " + request.generated.key +
                    ", got: " + response;
                return false;
            }
            return true;
        }
        if (response != "ACK " + request.id) {
            error = "expected ACK " + request.id + ", got: " + response;
            return false;
        }
        return true;
    }

    bool record_acknowledged_mutation(const Request& request, std::string& error) {
        if (request.compact ||
            request.generated.type == torture::GeneratedOperation::Type::Get) return true;
        if (request.generated.type == torture::GeneratedOperation::Type::Put) {
            return oracle_.record_put(request.generated.key, request.generated.value, error);
        }
        if (request.generated.type == torture::GeneratedOperation::Type::Delete) {
            return oracle_.record_del(request.generated.key, error);
        }
        return oracle_.record_batch(request.generated.batch, error);
    }

    bool kill_locked(const std::string& phase, std::string& error) {
        if (crashed_.load()) return true;
        const std::string op_id = active_id_;
        crash_request_ = pending_request_;
        const std::uint64_t timestamp = unix_time_ms();
        crashed_.store(true);
        if (!server_.request_termination(error)) {
            crashed_.store(false);
            failed_.store(true);
            fatal_error_ = "could not inject SIGKILL/TerminateProcess: " + error;
            state_changed_.notify_all();
            return false;
        }
        active_ = false;
        const std::string event = "{\"event\":\"crash\",\"timestamp_unix_ms\":" +
            std::to_string(timestamp) + ",\"cycle\":" + std::to_string(cycle_) +
            ",\"operation_id\":\"" + json_escape(op_id) + "\",\"phase\":\"" +
            json_escape(phase) + "\",\"phase_occurrence\":" +
            std::to_string(options_.crash_occurrence) + "}";
        if (!report_.write(event, error)) {
            failed_.store(true);
            fatal_error_ = error;
            state_changed_.notify_all();
            return false;
        }
        std::cout << "[Torture] cycle=" << cycle_ << " crash_timestamp_unix_ms="
                  << timestamp << " op_id=" << (op_id.empty() ? "none" : op_id)
                  << " phase=" << phase
                  << " occurrence=" << options_.crash_occurrence << "\n";
        state_changed_.notify_all();
        return true;
    }

    const Options& options_;
    AckOracle& oracle_;
    RecoveryAllowance& recovery_allowance_;
    CrashReport& report_;
    std::uint64_t cycle_;
    std::string compaction_target_;
    torture::ChildProcess server_;
    std::mutex io_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    bool active_{false};
    bool disk_full_put_seen_{false};
    bool disk_full_batch_seen_{false};
    std::string active_operation_;
    std::string active_phase_;
    std::string active_id_;
    std::optional<Request> pending_request_;
    std::optional<Request> crash_request_;
    std::atomic<bool> crashed_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> acknowledgements_{0};
    std::string fatal_error_;
};

bool crash_during_recovery(const Options& options, CrashReport& report,
                           std::uint64_t cycle, std::string& error) {
    static const std::array<const char*, 3> phases{
        "recovery_scan", "recovery_commit_replayed",
        "file_truncate_after_size_change"};
    const std::string target_phase = phases[cycle % phases.size()];
    const bool inject_torn_tail = target_phase == "file_truncate_after_size_change";
    if (inject_torn_tail) {
        std::ofstream tail(options.db_path, std::ios::binary | std::ios::app);
        const char torn_byte = static_cast<char>(0xA5);
        tail.write(&torn_byte, 1);
        tail.flush();
        if (!tail) {
            error = "cannot append the one-byte torn tail to '" + options.db_path + "'";
            return false;
        }
        tail.close();
        if (!tail) {
            error = "cannot close the database after injecting a torn tail";
            return false;
        }
    }

    torture::ChildProcess recovery;
    if (!recovery.start(options.crash_binary_path, serve_arguments(options), error,
                        {{"KVDB_TEST_REPORT_PHASES", "1"},
                         {"KVDB_TEST_PAUSE_PHASE", target_phase}})) {
        error = "cannot start recovery crash child: " + error;
        return false;
    }

    bool marker_seen = false;
    while (!marker_seen) {
        std::string line;
        bool eof = false;
        if (!recovery.read_line(line, 30s, eof, error)) {
            if (eof) error = "recovery child exited before reporting its scan phase";
            else error = "failed waiting for recovery scan phase: " + error;
            std::string terminate_error;
            if (!recovery.terminate(terminate_error)) error += "; " + terminate_error;
            return false;
        }
        marker_seen = line == "KVDB_TEST_PHASE " + target_phase;
    }

    const std::uint64_t timestamp = unix_time_ms();
    if (!recovery.request_termination(error)) {
        error = "failed to kill recovery child at " + target_phase + ": " + error;
        std::string terminate_error;
        if (!recovery.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    int exit_code = -1;
    if (!recovery.wait(30s, exit_code, error)) {
        std::string terminate_error;
        if (!recovery.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    if (exit_code == 0) {
        error = "recovery crash injection unexpectedly exited cleanly";
        return false;
    }

    const std::string event = "{\"event\":\"recovery_crash\",\"timestamp_unix_ms\":" +
        std::to_string(timestamp) + ",\"cycle\":" + std::to_string(cycle) +
        ",\"operation_id\":\"recovery_" + std::to_string(cycle) +
        "\",\"phase\":\"" + target_phase +
        "\",\"injected_torn_tail_bytes\":" +
        (inject_torn_tail ? "1" : "0") + "}";
    if (!report.write(event, error)) return false;
    std::cout << "[Torture] cycle=" << cycle
              << " crash_timestamp_unix_ms=" << timestamp
              << " op_id=recovery_" << cycle << " phase=" << target_phase << "\n";
    return true;
}

bool crash_during_close(const Options& options, CrashReport& report,
                        std::uint64_t cycle, std::string& error) {
    torture::ChildProcess child;
    if (!child.start(options.crash_binary_path, serve_arguments(options), error,
                     {{"KVDB_TEST_REPORT_PHASES", "1"},
                      {"KVDB_TEST_PAUSE_PHASE", options.crash_phase}}) ||
        !child.write_line("PING close_ready_" + std::to_string(cycle), error)) {
        std::string terminate_error;
        if (!child.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }

    const std::string ready_id = "close_ready_" + std::to_string(cycle);
    std::string response;
    while (true) {
        bool eof = false;
        if (!child.read_line(response, 30s, eof, error) || eof) {
            if (error.empty()) error = "close-crash child exited before PING response";
            std::string terminate_error;
            if (!child.terminate(terminate_error)) error += "; " + terminate_error;
            return false;
        }
        if (response.rfind("KVDB_TEST_PHASE ", 0) == 0) continue;
        if (response != "PONG " + ready_id) {
            error = "unexpected close-crash startup response: " + response;
            std::string terminate_error;
            if (!child.terminate(terminate_error)) error += "; " + terminate_error;
            return false;
        }
        break;
    }

    if (!child.write_line("QUIT", error)) {
        std::string terminate_error;
        if (!child.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    const std::string target = "KVDB_TEST_PHASE " + options.crash_phase;
    while (true) {
        bool eof = false;
        if (!child.read_line(response, 30s, eof, error) || eof) {
            if (error.empty()) {
                error = "close-crash child exited before phase " + options.crash_phase;
            }
            std::string terminate_error;
            if (!child.terminate(terminate_error)) error += "; " + terminate_error;
            return false;
        }
        if (response == target) break;
    }

    const std::uint64_t timestamp = unix_time_ms();
    if (!child.request_termination(error)) {
        error = "failed to hard-kill close-crash child at " +
            options.crash_phase + ": " + error;
        std::string terminate_error;
        if (!child.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    int exit_code = -1;
    if (!child.wait(30s, exit_code, error)) {
        std::string terminate_error;
        if (!child.terminate(terminate_error)) error += "; " + terminate_error;
        return false;
    }
    if (exit_code == 0) {
        error = "close-crash child exited cleanly instead of being hard-killed";
        return false;
    }

    const std::string event = "{\"event\":\"close_crash\",\"timestamp_unix_ms\":" +
        std::to_string(timestamp) + ",\"cycle\":" + std::to_string(cycle) +
        ",\"operation_id\":\"close_" + std::to_string(cycle) +
        "\",\"phase\":\"" + json_escape(options.crash_phase) + "\"}";
    if (!report.write(event, error)) return false;
    std::cout << "[Torture] cycle=" << cycle
              << " crash_timestamp_unix_ms=" << timestamp
              << " op_id=close_" << cycle
              << " phase=" << options.crash_phase << "\n";
    return true;
}

bool run_cycle(const Options& options, AckOracle& oracle, std::uint64_t cycle,
               std::uint64_t nonce, std::mt19937_64& crash_rng,
               std::vector<std::unique_ptr<torture::WorkloadGenerator>>& generators,
               std::vector<std::mt19937_64>& compaction_rngs,
               std::vector<std::uint64_t>& operation_indices, std::uint64_t& compactions,
               std::string& error) {
    const std::uint64_t total_requests =
        options.operations_per_worker * options.threads;
    const std::uint64_t stop_after = std::uniform_int_distribution<std::uint64_t>(
        1, total_requests)(crash_rng);
    CompactionSession session(options, oracle, cycle, stop_after);
    if (!session.start(error)) {
        std::string stop_error;
        if (!session.stop(stop_error)) error += "; " + stop_error;
        return false;
    }

    std::atomic<bool> failed{false};
    std::vector<std::string> errors(options.threads);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    for (std::uint32_t worker_id = 0; worker_id < options.threads; ++worker_id) {
        workers.emplace_back([&, worker_id] {
            for (std::uint64_t count = 0; count < options.operations_per_worker; ++count) {
                if (failed.load() || session.stopped()) break;
                const std::uint64_t operation_index = operation_indices[worker_id]++;
                Request request;
                std::string operation_error;
                if (!make_request(*generators[worker_id], compaction_rngs[worker_id],
                                  options.compact_rate, worker_id, operation_index,
                                  nonce, cycle, {}, true, false, false, false,
                                  false, false,
                                  request, operation_error)) {
                    errors[worker_id] = operation_error;
                    failed.store(true);
                    break;
                }
                const bool completed =
                    session.execute(request, worker_id, operation_index, operation_error);
                if (!request.batch_path.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(request.batch_path, ec);
                    if (ec && operation_error.empty()) {
                        operation_error = "cannot remove batch file '" + request.batch_path +
                            "': " + ec.message();
                    }
                }
                if (!operation_error.empty()) {
                    errors[worker_id] = operation_error;
                    failed.store(true);
                    break;
                }
                if (!completed) {
                    if (!session.fatal_error().empty()) {
                        errors[worker_id] = session.fatal_error();
                        failed.store(true);
                    }
                    break;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::string stop_error;
    if (!session.stop(stop_error)) {
        error = "could not stop/reap child: " + stop_error;
        return false;
    }
    if (failed.load()) {
        for (const auto& item : errors) {
            if (!item.empty()) {
                error = item;
                break;
            }
        }
        for (const auto& item : session.history()) std::cerr << "[Torture]   " << item << "\n";
        return false;
    }
    compactions += session.compactions();
    return verify_state(options, oracle, cycle, error);
}

bool run_random_crash_cycle(
    const Options& options, AckOracle& oracle, RecoveryAllowance& recovery_allowance,
    CrashReport& report,
    std::uint64_t cycle, std::uint64_t nonce, std::mt19937_64& timing_rng,
    std::vector<std::unique_ptr<torture::WorkloadGenerator>>& generators,
    std::vector<std::mt19937_64>& compaction_rngs,
    std::vector<std::uint64_t>& operation_indices, std::string& error) {
    const std::string compaction_target = cycle % 2 == 0
        ? "compaction_before_replace" : "compaction_after_replace";
    RandomCrashSession session(options, oracle, recovery_allowance, report, cycle,
                               compaction_target);
    if (!session.start(error)) {
        std::string termination_error;
        if (!session.terminate_after_failure(termination_error)) {
            error += "; " + termination_error;
        }
        return false;
    }

    std::vector<std::string> worker_errors(options.threads);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    for (std::uint32_t worker_id = 0; worker_id < options.threads; ++worker_id) {
        workers.emplace_back([&, worker_id] {
            bool first_request = true;
            std::uint64_t disk_full_requests = 0;
            while (!session.crashed() && !session.failed() &&
                   !session.disk_full_complete() &&
                   (options.disk_full ||
                    disk_full_requests < options.operations_per_worker)) {
                const std::uint64_t operation_index = operation_indices[worker_id]++;
                Request request;
                std::string operation_error;
                if (!make_request(*generators[worker_id], compaction_rngs[worker_id],
                                  options.compact_rate, worker_id, operation_index,
                                  nonce, cycle, options.crash_operation,
                                  options.compaction_crash, options.large_values,
                                  options.large_batches,
                                  options.large_batches &&
                                      options.crash_phase == "file_append_partial_write",
                                  options.disk_full,
                                  (options.compaction_crash ||
                                   options.crash_phase.rfind("compaction_", 0) == 0) &&
                                      worker_id == 0 &&
                                      first_request,
                                  request, operation_error)) {
                    worker_errors[worker_id] = operation_error;
                    session.abort(operation_error);
                    break;
                }
                first_request = false;

                if (options.disk_full) {
                    ++disk_full_requests;
                    request.compact = false;
                    request.description = describe_request(request);
                }

                const bool completed = session.execute(
                    request, worker_id, operation_index, operation_error);
                if (completed) {
                    if (!request.batch_path.empty()) {
                        std::error_code ec;
                        if (!std::filesystem::remove(request.batch_path, ec) || ec) {
                            worker_errors[worker_id] = "cannot remove acknowledged batch file '" +
                                request.batch_path + "': " + ec.message();
                            session.abort(worker_errors[worker_id]);
                            break;
                        }
                    }
                    continue;
                }

                if (!operation_error.empty() && !session.crashed()) {
                    worker_errors[worker_id] = operation_error;
                    session.abort(operation_error);
                }
                break;
            }
        });
    }

    const bool crash_injected = session.wait_and_kill(timing_rng, error);
    for (auto& worker : workers) worker.join();
    if (!crash_injected || session.failed()) {
        for (const auto& worker_error : worker_errors) {
            if (!worker_error.empty()) {
                error = worker_error;
                break;
            }
        }
        const std::string fatal_error = session.fatal_error();
        if (!fatal_error.empty()) error = "worker failure: " + fatal_error;
        std::string termination_error;
        if (!session.terminate_after_failure(termination_error)) {
            error += "; " + termination_error;
        }
        return false;
    }

    if (options.disk_full && !session.disk_full_complete()) {
        error = "disk-full scenario did not observe NoSpace for both PUT and BATCH";
        std::string termination_error;
        if (!session.terminate_after_failure(termination_error)) {
            error += "; " + termination_error;
        }
        return false;
    }

    if (!session.reap(error)) return false;
    const auto pending = session.crash_request();
    if (options.recovery_crash &&
        !crash_during_recovery(options, report, cycle, error)) {
        return false;
    }
    if (is_close_crash_phase(options.crash_phase) &&
        !crash_during_close(options, report, cycle, error)) {
        return false;
    }
    if (!verify_random_state(options, oracle, recovery_allowance, pending,
                             cycle, error)) return false;

    const std::string event = "{\"event\":\"cycle_verified\",\"timestamp_unix_ms\":" +
        std::to_string(unix_time_ms()) + ",\"cycle\":" + std::to_string(cycle) +
        ",\"acknowledgements\":" + std::to_string(session.acknowledgements()) + "}";
    if (!report.write(event, error)) return false;
    std::cout << "[Torture] PASS cycle=" << cycle
              << " acknowledged=" << session.acknowledgements()
              << " recovered_state=verified";
    if (pending) std::cout << " in_flight_op=" << pending->id;
    if (options.disk_full) std::cout << " disk_full=put,batch-verified";
    std::cout << "\n";
    return true;
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
    std::string error;
    if (!resolve_kvdb_path(argv[0], options.kvdb_path, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    const std::uint64_t nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::unique_ptr<CrashReport> report;
    if (options.mode == Mode::RandomCrash || options.mode == Mode::Concurrency) {
        if (!prepare_random_paths(options, nonce, error)) {
            std::cerr << "[Torture] Refusing to start: " << error << "\n";
            return 1;
        }
        if (options.mode == Mode::RandomCrash &&
            (options.recovery_crash || options.compaction_crash ||
             !options.crash_phase.empty()) &&
            !resolve_crash_binary_path(argv[0], options.crash_binary_path, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        report = std::make_unique<CrashReport>(options.report_path);
        if (!report->open(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const std::string start_event =
            "{\"event\":\"run_start\",\"timestamp_unix_ms\":" +
            std::to_string(unix_time_ms()) + ",\"seed\":" +
            std::to_string(options.seed) + ",\"iterations\":" +
            std::to_string(options.cycles) + ",\"workers\":" +
            std::to_string(options.threads) + ",\"database\":\"" +
            json_escape(options.db_path) + "\",\"oracle\":\"" +
            json_escape(options.oracle_path) + "\",\"mode\":\"" +
            (options.mode == Mode::Concurrency ? "concurrency" : "random-crash") +
            "\"}";
        if (!report->write(start_event, error)) {
            std::cerr << error << "\n";
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
    if (options.mode == Mode::Compaction &&
        !verify_initial_state(options, *oracle, error)) {
        std::cerr << "[Torture] Refusing to start: " << error << "\n";
        return 1;
    }
    if (options.mode == Mode::Concurrency) {
        std::cout << "[Torture] Concurrent database test seed=" << options.seed
                  << " iterations=" << options.cycles
                  << " workers=" << options.threads
                  << " operations_per_worker=" << options.operations_per_worker
                  << " db=" << options.db_path
                  << " report=" << options.report_path << "\n";
        const auto write_report = [&](const std::string& event,
                                      std::string& report_error) {
            return report->write(event, report_error);
        };
        if (!run_concurrency_test(options, *oracle, write_report, error)) {
            std::string report_error;
            const std::string failure =
                "{\"event\":\"concurrency_failure\",\"timestamp_unix_ms\":" +
                std::to_string(unix_time_ms()) + ",\"seed\":" +
                std::to_string(options.seed) + ",\"error\":\"" +
                json_escape(error) + "\"}";
            if (!report->write(failure, report_error)) {
                error += "; could not write failure report: " + report_error;
            }
            std::cerr << "[Torture] FAIL seed=" << options.seed << ": " << error << "\n"
                      << "[Torture] Preserved database: " << options.db_path << "\n"
                      << "[Torture] Preserved oracle: " << options.oracle_path << "\n"
                      << "[Torture] Preserved report: " << options.report_path << "\n";
            return 1;
        }
        std::cout << "[Torture] PASS all " << options.cycles
                  << " concurrency cycles\n";
        return 0;
    }

    std::vector<std::unique_ptr<torture::WorkloadGenerator>> generators;
    std::vector<std::mt19937_64> compaction_rngs;
    std::vector<std::uint64_t> operation_indices(options.threads, 0);
    generators.reserve(options.threads);
    compaction_rngs.reserve(options.threads);
    for (std::uint32_t worker_id = 0; worker_id < options.threads; ++worker_id) {
        generators.push_back(std::make_unique<torture::WorkloadGenerator>(
            options.seed, worker_id, torture::WorkloadMode::All));
        compaction_rngs.emplace_back(options.seed ^
            (0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(worker_id) + 1)));
    }

    std::mt19937_64 crash_rng(options.seed);
    RecoveryAllowance recovery_allowance;
    std::uint64_t total_compactions = 0;
    if (options.mode == Mode::RandomCrash) {
        std::cout << "[Torture] Random process-crash test seed=" << options.seed
                  << " iterations=" << options.cycles
                  << " workers=" << options.threads
                  << " max_bytes=" << options.max_bytes
                  << " db=" << options.db_path
                  << " oracle=" << options.oracle_path
                  << " report=" << options.report_path << "\n";
        for (std::uint64_t cycle = 0; cycle < options.cycles; ++cycle) {
            const std::uint64_t cycle_seed = options.seed ^
                (0x9e3779b97f4a7c15ULL * (cycle + 1));
            std::mt19937_64 timing_rng(cycle_seed);
            if (!run_random_crash_cycle(options, *oracle, recovery_allowance,
                                        *report, cycle, nonce,
                                        timing_rng, generators, compaction_rngs,
                                        operation_indices, error)) {
                std::cerr << "[Torture] FAIL seed=" << options.seed
                          << " cycle=" << cycle << ": " << error << "\n"
                          << "[Torture] Preserved database: " << options.db_path << "\n"
                          << "[Torture] Preserved oracle: " << options.oracle_path << "\n"
                          << "[Torture] Preserved report: " << options.report_path << "\n";
                return 1;
            }
        }
        std::cout << "[Torture] PASS all " << options.cycles
                  << " random crash/recovery cycles\n";
        return 0;
    }

    std::cout << "[Torture] Phase 10 seed=" << options.seed
              << " cycles=" << options.cycles
              << " threads=" << options.threads
              << " operations_per_worker=" << options.operations_per_worker
              << " compact_rate=1/" << options.compact_rate << "\n";

    for (std::uint64_t cycle = 0; cycle < options.cycles; ++cycle) {
        if (!run_cycle(options, *oracle, cycle, nonce, crash_rng, generators,
                       compaction_rngs, operation_indices, total_compactions, error)) {
            std::cerr << "[Torture] FAILURE seed=" << options.seed
                      << " cycle=" << cycle << ": " << error << "\n";
            return 1;
        }
        if ((cycle + 1) % 25 == 0 || cycle + 1 == options.cycles) {
            std::cout << "[Torture] Verified " << cycle + 1 << "/"
                      << options.cycles << " crash/recovery cycles"
                      << " (compactions=" << total_compactions << ")\n";
        }
    }

    std::cout << "[Torture] Passed compaction-under-load crash testing\n";
    return 0;
}
