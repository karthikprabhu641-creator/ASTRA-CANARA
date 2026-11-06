#include "kvdb/database.hpp"
#include "kvdb/cli/batch_parser.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#if defined(KVDB_ENABLE_CRASH_HOOKS)
#include "crash_hook.hpp"
#endif

namespace {

constexpr int EXIT_CLI_SUCCESS = 0;
constexpr int EXIT_CLI_ERROR = 1;
constexpr int EXIT_CLI_NOT_FOUND = 2;

void print_usage(std::ostream& out = std::cout) {
    out << "Usage:\n"
        << "  kvdb open <path> [--max-bytes N]\n"
        << "  kvdb put <path> <key> [value]\n"
        << "  kvdb get <path> <key>\n"
        << "  kvdb del <path> <key>\n"
        << "  kvdb batch <path> <batch_file>\n"
        << "  kvdb dump <path>\n"
        << "  kvdb check <path>\n"
        << "  kvdb serve <path>\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(std::cerr);
        return EXIT_CLI_ERROR;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(std::cout);
        return EXIT_CLI_SUCCESS;
    }

    if (argc < 3) {
        std::cerr << "Error: database path required for command '" << command << "'\n";
        print_usage(std::cerr);
        return EXIT_CLI_ERROR;
    }

    std::string db_path = argv[2];
    kvdb::Database db;

    try {
        // --- 1. OPEN command ---
        if (command == "open") {
            std::uint64_t max_bytes = 0;
            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--max-bytes" && i + 1 < argc) {
                    try {
                        max_bytes = std::stoull(argv[i + 1]);
                    } catch (const std::exception&) {
                        std::cerr << "Error: invalid value for --max-bytes: " << argv[i + 1] << "\n";
                        return EXIT_CLI_ERROR;
                    }
                    i++;
                } else if (arg.rfind("--max-bytes=", 0) == 0) {
                    std::string val_str = arg.substr(12);
                    try {
                        max_bytes = std::stoull(val_str);
                    } catch (const std::exception&) {
                        std::cerr << "Error: invalid value for --max-bytes: " << val_str << "\n";
                        return EXIT_CLI_ERROR;
                    }
                } else {
                    std::cerr << "Error: unknown option: " << arg << "\n";
                    return EXIT_CLI_ERROR;
                }
            }

            kvdb::Status s = db.open(db_path, max_bytes);
            if (!kvdb::is_ok(s)) {
                std::cerr << "Error: " << kvdb::status_to_string(s) << "\n";
                return EXIT_CLI_ERROR;
            }
            std::cout << "OK\n";
            db.close();
            return EXIT_CLI_SUCCESS;
        }

        // --- CHECK command (read-only verification without modifying/truncating file) ---
        if (command == "check") {
            kvdb::Status s = kvdb::Database::check_file(db_path);
            if (kvdb::is_ok(s)) {
                std::cout << "INTEGRITY_OK\n";
                return EXIT_CLI_SUCCESS;
            } else {
                std::cerr << "INTEGRITY_FAILED: " << kvdb::status_to_string(s) << "\n";
                return EXIT_CLI_ERROR;
            }
        }

        // For all other commands, open the database first
        kvdb::Status s = db.open(db_path);
        if (!kvdb::is_ok(s)) {
            std::cerr << "Failed to open database: " << kvdb::status_to_string(s) << "\n";
            return EXIT_CLI_ERROR;
        }

        // --- 2. PUT command ---
        if (command == "put") {
            if (argc < 4) {
                std::cerr << "Usage: kvdb put <path> <key> [value]\n";
                return EXIT_CLI_ERROR;
            }
            std::string key = argv[3];
            std::string val = (argc >= 5) ? argv[4] : "";

            if (key.empty()) {
                std::cerr << "Error: key cannot be empty\n";
                return EXIT_CLI_ERROR;
            }

            s = db.put(key, val);
            if (!kvdb::is_ok(s)) {
                std::cerr << "Error: " << kvdb::status_to_string(s) << "\n";
                return EXIT_CLI_ERROR;
            }
            std::cout << "OK\n";
            db.close();
            return EXIT_CLI_SUCCESS;
        }

        // --- 3. GET command ---
        if (command == "get") {
            if (argc < 4) {
                std::cerr << "Usage: kvdb get <path> <key>\n";
                return EXIT_CLI_ERROR;
            }
            std::string key = argv[3];
            if (key.empty()) {
                std::cerr << "Error: key cannot be empty\n";
                return EXIT_CLI_ERROR;
            }

            auto res = db.get(key);
            db.close();

            if (res.status == kvdb::Status::NotFound) {
                std::cerr << "NOT_FOUND\n";
                return EXIT_CLI_NOT_FOUND;
            } else if (!kvdb::is_ok(res.status)) {
                std::cerr << "Error: " << kvdb::status_to_string(res.status) << "\n";
                return EXIT_CLI_ERROR;
            }

            std::cout << res.value << "\n";
            return EXIT_CLI_SUCCESS;
        }

        // --- 4. DEL command ---
        if (command == "del") {
            if (argc < 4) {
                std::cerr << "Usage: kvdb del <path> <key>\n";
                return EXIT_CLI_ERROR;
            }
            std::string key = argv[3];
            if (key.empty()) {
                std::cerr << "Error: key cannot be empty\n";
                return EXIT_CLI_ERROR;
            }

            s = db.remove(key);
            db.close();

            if (!kvdb::is_ok(s)) {
                std::cerr << "Error: " << kvdb::status_to_string(s) << "\n";
                return EXIT_CLI_ERROR;
            }
            std::cout << "OK\n";
            return EXIT_CLI_SUCCESS;
        }

        // --- 5. BATCH command ---
        if (command == "batch") {
            if (argc < 4) {
                std::cerr << "Usage: kvdb batch <path> <file>\n";
                return EXIT_CLI_ERROR;
            }
            std::string batch_file = argv[3];
            std::vector<kvdb::Operation> ops;
            s = kvdb::cli::BatchParser::parse_file(batch_file, ops);
            if (!kvdb::is_ok(s)) {
                std::cerr << "Invalid batch file\n";
                return EXIT_CLI_ERROR;
            }
            s = db.batch(ops);
            db.close();

            if (!kvdb::is_ok(s)) {
                std::cerr << "Batch error: " << kvdb::status_to_string(s) << "\n";
                return EXIT_CLI_ERROR;
            }
            std::cout << "OK\n";
            return EXIT_CLI_SUCCESS;
        }

        // --- 6. DUMP command ---
        if (command == "dump") {
            s = db.dump(std::cout);
            db.close();
            return kvdb::is_ok(s) ? EXIT_CLI_SUCCESS : EXIT_CLI_ERROR;
        }

        // --- 7. SERVE command ---
        if (command == "serve") {
            std::string line;
            while (std::getline(std::cin, line)) {
                // Strip CR if present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) continue;
                if (line == "QUIT") break;

                std::istringstream iss(line);
                std::string op, op_id;
                if (!(iss >> op)) {
                    continue;
                }

                if (op == "QUIT") {
                    break;
                }

                if (!(iss >> op_id)) {
                    std::cout << "ERR invalid_command\n";
                    std::cout.flush();
                    continue;
                }

                if (op == "PUT") {
                    std::string key, val;
                    if (!(iss >> key)) {
                        std::cout << "ERR " << op_id << " missing_key\n";
                    } else {
                        std::getline(iss, val);
                        if (!val.empty() && val[0] == ' ') {
                            val = val.substr(1);
                        }
                        std::string unescaped_val = kvdb::cli::BatchParser::unescape(val);
                        s = db.put(key, unescaped_val);
                        if (kvdb::is_ok(s)) {
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                            torture::crash_if_selected("before_ack");
#endif
                            // Durability guarantee: ACK only after durable sync
                            std::cout << "ACK " << op_id << "\n";
                            std::cout.flush();
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                            torture::crash_if_selected("after_ack");
#endif
                        } else {
                            std::cout << "ERR " << op_id << " " << kvdb::status_to_string(s) << "\n";
                        }
                    }
                } else if (op == "DEL") {
                    std::string key;
                    if (!(iss >> key)) {
                        std::cout << "ERR " << op_id << " missing_key\n";
                    } else {
                        s = db.remove(key);
                        if (kvdb::is_ok(s)) {
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                            torture::crash_if_selected("before_ack");
#endif
                            std::cout << "ACK " << op_id << "\n";
                            std::cout.flush();
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                            torture::crash_if_selected("after_ack");
#endif
                        } else {
                            std::cout << "ERR " << op_id << " " << kvdb::status_to_string(s) << "\n";
                        }
                    }
                } else if (op == "GET") {
                    std::string key;
                    if (!(iss >> key)) {
                        std::cout << "ERR " << op_id << " missing_key\n";
                    } else {
                        auto res = db.get(key);
                        if (kvdb::is_ok(res.status)) {
                            std::cout << "VALUE " << op_id << " " << res.value << "\n";
                        } else if (res.status == kvdb::Status::NotFound) {
                            std::cout << "NOT_FOUND " << op_id << "\n";
                        } else {
                            std::cout << "ERR " << op_id << " " << kvdb::status_to_string(res.status) << "\n";
                        }
                    }
                } else if (op == "BATCH") {
                    std::string batch_file;
                    if (!(iss >> batch_file)) {
                        std::cout << "ERR " << op_id << " missing_batch_file\n";
                    } else {
                        std::vector<kvdb::Operation> ops;
                        s = kvdb::cli::BatchParser::parse_file(batch_file, ops);
                        if (!kvdb::is_ok(s)) {
                            std::cout << "ERR " << op_id << " invalid_batch_file\n";
                        } else {
                            s = db.batch(ops);
                            if (kvdb::is_ok(s)) {
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                                torture::crash_if_selected("before_ack");
#endif
                                std::cout << "ACK " << op_id << "\n";
                                std::cout.flush();
#if defined(KVDB_ENABLE_CRASH_HOOKS)
                                torture::crash_if_selected("after_ack");
#endif
                            } else {
                                std::cout << "ERR " << op_id << " " << kvdb::status_to_string(s) << "\n";
                            }
                        }
                    }
                } else if (op == "COMPACT") {
                    s = db.compact();
                    if (kvdb::is_ok(s)) {
                        std::cout << "ACK " << op_id << "\n";
                    } else {
                        std::cout << "ERR " << op_id << " " << kvdb::status_to_string(s) << "\n";
                    }
                } else if (op == "CHECK") {
                    s = db.check();
                    if (kvdb::is_ok(s)) {
                        std::cout << "INTEGRITY_OK " << op_id << "\n";
                    } else {
                        std::cout << "INTEGRITY_FAILED " << op_id << " " << kvdb::status_to_string(s) << "\n";
                    }
                } else if (op == "DUMP") {
                    s = db.dump(std::cout);
                    if (kvdb::is_ok(s)) {
                        std::cout << "ACK " << op_id << "\n";
                    } else {
                        std::cout << "ERR " << op_id << " " << kvdb::status_to_string(s) << "\n";
                    }
                } else if (op == "PING") {
                    std::cout << "PONG " << op_id << "\n";
                } else {
                    std::cout << "ERR " << op_id << " unknown_command\n";
                }

                std::cout.flush();
            }
            db.close();
            return EXIT_CLI_SUCCESS;
        }

        std::cerr << "Unknown command: " << command << "\n";
        print_usage(std::cerr);
        return EXIT_CLI_ERROR;

    } catch (const std::exception& ex) {
        std::cerr << "Fatal unexpected error: " << ex.what() << "\n";
        return EXIT_CLI_ERROR;
    }
}
