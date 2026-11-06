#include "child_process.hpp"
#include "oracle.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Options {
    std::uint64_t seed{42};
    std::uint64_t iterations{500};
    std::string db_prefix{"torture_phase8.db"};
    std::string oracle_prefix{"torture_phase8.ack.log"};
    std::string kvdb_path;
    std::string crash_binary_path;
};

const std::vector<std::string>& crash_points() {
    static const std::vector<std::string> points{
        "after_begin",
        "after_operations",
        "after_commit",
        "after_sync",
        "before_ack",
        "after_ack"
    };
    return points;
}

void print_usage(std::ostream& out) {
    out << "Usage: torture_tool_phase8 [options]\n"
        << "  --seed <uint>        Deterministic crash-point order and keys (default: 42)\n"
        << "  --iterations <int>   Crash scenarios (default: 500)\n"
        << "  --db <prefix>        Generated per-scenario DB filename prefix\n"
        << "  --oracle <prefix>    Generated per-scenario oracle filename prefix\n"
        << "  --kvdb <path>        Normal kvdb executable for recovery checks\n"
        << "  --crash-binary <path> Hook-enabled kvdb executable\n"
        << "  --help               Show this help\n";
}

bool parse_unsigned(const std::string& text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argument << "\n";
            return false;
        }
        const std::string value = argv[++i];
        if (argument == "--seed") {
            if (!parse_unsigned(value, options.seed)) {
                std::cerr << "Invalid unsigned seed: " << value << "\n";
                return false;
            }
        } else if (argument == "--iterations") {
            if (!parse_unsigned(value, options.iterations) || options.iterations == 0) {
                std::cerr << "Iterations must be a positive integer\n";
                return false;
            }
        } else if (argument == "--db") {
            options.db_prefix = value;
        } else if (argument == "--oracle") {
            options.oracle_prefix = value;
        } else if (argument == "--kvdb") {
            options.kvdb_path = value;
        } else if (argument == "--crash-binary") {
            options.crash_binary_path = value;
        } else {
            std::cerr << "Unknown option: " << argument << "\n";
            return false;
        }
    }

    if (options.db_prefix.empty() || options.oracle_prefix.empty()) {
        std::cerr << "Database and oracle prefixes cannot be empty\n";
        return false;
    }
    return true;
}

bool absolute_path(const std::string& input, std::string& output, std::string& error) {
    std::error_code ec;
    output = std::filesystem::absolute(input, ec).string();
    if (ec) {
        error = "cannot resolve path '" + input + "': " + ec.message();
        return false;
    }
    return true;
}

bool resolve_executable_paths(const char* runner_path, Options& options, std::string& error) {
    std::string runner_absolute;
    if (!absolute_path(runner_path, runner_absolute, error)) return false;
    const std::filesystem::path directory =
        std::filesystem::path(runner_absolute).parent_path();
#if defined(_WIN32)
    if (options.kvdb_path.empty()) options.kvdb_path = (directory / "kvdb.exe").string();
    if (options.crash_binary_path.empty()) {
        options.crash_binary_path = (directory / "kvdb_crash_test.exe").string();
    }
#else
    if (options.kvdb_path.empty()) options.kvdb_path = (directory / "kvdb").string();
    if (options.crash_binary_path.empty()) {
        options.crash_binary_path = (directory / "kvdb_crash_test").string();
    }
#endif
    return true;
}

bool run_cli(const std::string& executable, const std::vector<std::string>& arguments,
             std::vector<std::string>& output, int& exit_code, std::string& error) {
    torture::ChildProcess child;
    if (!child.start(executable, arguments, error)) return false;
    if (!child.close_input(error)) {
        std::string termination_error;
        if (!child.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }
    while (true) {
        std::string line;
        bool eof = false;
        if (!child.read_line(line, 30s, eof, error)) {
            if (eof) break;
            std::string termination_error;
            if (!child.terminate(termination_error)) error += "; " + termination_error;
            return false;
        }
        output.push_back(std::move(line));
    }
    if (!child.wait(30s, exit_code, error)) {
        std::string termination_error;
        if (!child.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }
    return true;
}

bool get_database_state(const Options& options, const std::string& db_path,
                        std::uint64_t cycle, std::map<std::string, std::string>& state,
                        std::string& error) {
    std::vector<std::string> output;
    int exit_code = -1;
    if (!run_cli(options.kvdb_path, {"check", db_path}, output, exit_code, error)) {
        error = "cycle " + std::to_string(cycle) + " integrity check process failed: " + error;
        return false;
    }
    if (exit_code != 0 || output.size() != 1 || output.front() != "INTEGRITY_OK") {
        error = "cycle " + std::to_string(cycle) + " integrity check failed";
        if (!output.empty()) error += ": " + output.front();
        return false;
    }

    output.clear();
    if (!run_cli(options.kvdb_path, {"dump", db_path}, output, exit_code, error)) {
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
    return true;
}

bool cleanup_file(const std::string& path, std::string& error) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
        error = "cannot remove test file '" + path + "': " + ec.message();
        return false;
    }
    if (!removed && std::filesystem::exists(path, ec)) {
        error = "test file remains after cleanup: " + path;
        return false;
    }
    return true;
}

bool run_scenario(const Options& options, const std::string& point, std::uint64_t cycle,
                  const std::string& db_path, const std::string& oracle_path,
                  std::string& error) {
    const std::string key = "phase8_" + std::to_string(options.seed) + "_" +
        std::to_string(cycle);
    const std::string value = "value_" + std::to_string(options.seed) + "_" +
        std::to_string(cycle);
    const std::string operation_id = std::to_string(cycle + 1);

    AckOracle oracle(oracle_path);
    torture::ChildProcess server;
    if (!server.start(options.crash_binary_path, {"serve", db_path}, error,
                      {{"KVDB_TEST_CRASH_POINT", point}})) {
        return false;
    }
    if (!server.write_line("PING phase8_ready", error)) {
        std::string termination_error;
        if (!server.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }

    std::string response;
    bool eof = false;
    if (!server.read_line(response, 30s, eof, error) || response != "PONG phase8_ready") {
        if (error.empty()) error = "unexpected startup response: " + response;
        std::string termination_error;
        if (!server.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }
    if (!server.write_line("PUT " + operation_id + " " + key + " " + value, error)) {
        std::string termination_error;
        if (!server.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }

    bool acknowledged = false;
    error.clear();
    if (server.read_line(response, 30s, eof, error)) {
        acknowledged = true;
        if (response != "ACK " + operation_id) {
            error = "unexpected mutation response at " + point + ": " + response;
            std::string termination_error;
            if (!server.terminate(termination_error)) error += "; " + termination_error;
            return false;
        }
        if (!oracle.record_put(key, value, error)) {
            std::string termination_error;
            if (!server.terminate(termination_error)) error += "; " + termination_error;
            return false;
        }
    } else if (!eof) {
        error = "timed out waiting for injected crash at " + point;
        std::string termination_error;
        if (!server.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }

    int exit_code = -1;
    if (!server.wait(30s, exit_code, error)) {
        std::string termination_error;
        if (!server.terminate(termination_error)) error += "; " + termination_error;
        return false;
    }
    if (exit_code != 86) {
        error = "crash hook '" + point + "' exited with " + std::to_string(exit_code) +
            " instead of 86";
        return false;
    }

    const bool expects_present = point == "after_sync" || point == "before_ack" ||
                                 point == "after_ack";
    if (acknowledged != (point == "after_ack")) {
        error = "ACK observation did not match crash point '" + point + "'";
        return false;
    }

    std::map<std::string, std::string> actual;
    if (!get_database_state(options, db_path, cycle, actual, error)) return false;
    const bool present = actual.find(key) != actual.end();
    if ((expects_present && !present) ||
        ((point == "after_begin" || point == "after_operations") && present)) {
        error = "recovered mutation presence violates crash-point contract at '" + point + "'";
        return false;
    }
    if (point == "after_commit" && !actual.empty() &&
        (actual.size() != 1 || !present || actual.at(key) != value)) {
        error = "post-COMMIT recovery was neither wholly absent nor wholly present";
        return false;
    }
    if (present && actual.at(key) != value) {
        error = "recovered mutation has a partial or incorrect value at '" + point + "'";
        return false;
    }
    const bool allowed_present = expects_present || point == "after_commit";
    if (!allowed_present && !actual.empty()) {
        error = "unacknowledged mutation unexpectedly changed recovered state at '" + point + "'";
        return false;
    }
    if (acknowledged) {
        const auto expected = oracle.snapshot();
        if (actual.size() != expected.size() || expected.size() != 1 ||
            actual.find(key) == actual.end() || actual.at(key) != value) {
            error = "ACKed state does not match the persistent oracle after '" + point + "'";
            return false;
        }
    }

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
    if (!resolve_executable_paths(argv[0], options, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::vector<std::string> points = crash_points();
    std::mt19937_64 random(options.seed);
    std::shuffle(points.begin(), points.end(), random);
    const auto run_nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    std::cout << "[Torture] Phase 8 seed=" << options.seed
              << " scenarios=" << options.iterations << "\n";

    for (std::uint64_t cycle = 0; cycle < options.iterations; ++cycle) {
        const std::string& point = points[static_cast<std::size_t>(cycle % points.size())];
        const std::string suffix = ".phase8_" + std::to_string(options.seed) + "_" +
            std::to_string(run_nonce) + "_" + std::to_string(cycle);
        const std::string db_path = options.db_prefix + suffix;
        const std::string oracle_path = options.oracle_prefix + suffix;

        std::error_code exists_error;
        if (std::filesystem::exists(db_path, exists_error) ||
            std::filesystem::exists(oracle_path, exists_error) || exists_error) {
            std::cerr << "Refusing to overwrite an existing Phase 8 test file\n";
            return 1;
        }

        if (!run_scenario(options, point, cycle, db_path, oracle_path, error)) {
            std::cerr << "[Torture] FAILURE seed=" << options.seed << " scenario=" << cycle
                      << " crash_point=" << point << ": " << error << "\n";
            return 1;
        }

        if (!cleanup_file(db_path, error) || !cleanup_file(oracle_path, error)) {
            std::cerr << "[Torture] FAILURE seed=" << options.seed << " scenario=" << cycle
                      << " cleanup: " << error << "\n";
            return 1;
        }
        if ((cycle + 1) % 25 == 0 || cycle + 1 == options.iterations) {
            std::cout << "[Torture] Verified " << (cycle + 1) << "/"
                      << options.iterations << " scenarios\n";
        }
    }

    std::cout << "[Torture] Passed all deterministic crash-point scenarios\n";
    return 0;
}
