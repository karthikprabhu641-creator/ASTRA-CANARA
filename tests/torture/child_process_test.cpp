#include "child_process.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace {

bool read_expected(torture::ChildProcess& child, const std::string& expected,
                   std::chrono::milliseconds timeout, std::string& error) {
    std::string line;
    bool eof = false;
    return child.read_line(line, timeout, eof, error) && !eof && line == expected;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        const std::string mode = argv[1];
        if (mode == "--child-echo") {
            std::string line;
            while (std::getline(std::cin, line)) std::cout << "REPLY " << line << std::endl;
            return 0;
        }
        if (mode == "--child-exit") return 7;
        if (mode == "--child-partial") {
            std::cout << "FINAL WITHOUT NEWLINE" << std::flush;
            return 0;
        }
        if (mode == "--child-delayed") {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << "READY" << std::endl;
            return 0;
        }
        if (mode == "--child-env") {
            const char* value = std::getenv("KVDB_CHILD_TEST");
            std::cout << (value ? value : "<missing>") << std::endl;
            return 0;
        }
        if (mode == "--child-arg" && argc == 3) {
            std::cout << argv[2] << std::endl;
            return 0;
        }
        if (mode == "--child-wait") {
            char byte = 0;
            while (std::cin.get(byte)) {}
            return 0;
        }
        return 2;
    }

    const auto executable = std::filesystem::absolute(argv[0]).string();
    std::string error;
    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-echo"}, error));
        assert(child.write_line("quotes \" and spaces", error));
        assert(read_expected(child, "REPLY quotes \" and spaces",
                             std::chrono::seconds(2), error));
        assert(error.empty());
        assert(!child.write_line("two\nlines", error));
        assert(!child.write_line(std::string("nul\0byte", 8), error));
        assert(child.write_line("still alive", error));
        assert(read_expected(child, "REPLY still alive", std::chrono::seconds(2), error));
        assert(child.write_line("buffered one", error));
        assert(child.write_line("buffered two", error));
        assert(read_expected(child, "REPLY buffered one", std::chrono::seconds(2), error));
        assert(read_expected(child, "REPLY buffered two", std::chrono::seconds(2), error));
        assert(child.close_input(error));
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 0);
    }

    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-env"}, error,
                           {{"KVDB_CHILD_TEST", "child value"}}));
        assert(read_expected(child, "child value", std::chrono::seconds(2), error));
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 0);
    }

    {
        const std::string argument = " leading \"quoted\" text\\\\";
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-arg", argument}, error));
        assert(read_expected(child, argument, std::chrono::seconds(2), error));
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 0);
    }

    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-delayed"}, error));
        std::string line;
        bool eof = false;
        assert(!child.read_line(line, std::chrono::milliseconds(5), eof, error));
        assert(error == "timed out waiting for child output");
        assert(read_expected(child, "READY", std::chrono::seconds(2), error));
        assert(error.empty());
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 0);
    }

    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-partial"}, error));
        assert(read_expected(child, "FINAL WITHOUT NEWLINE",
                             std::chrono::seconds(2), error));
        std::string line;
        bool eof = false;
        assert(!child.read_line(line, std::chrono::seconds(2), eof, error));
        assert(eof);
        assert(error.empty());
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 0);
    }

#if !defined(_WIN32)
    {
        struct sigaction before {};
        struct sigaction after {};
        assert(sigaction(SIGPIPE, nullptr, &before) == 0);
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-exit"}, error));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(!child.write_line("closed pipe", error));
        assert(sigaction(SIGPIPE, nullptr, &after) == 0);
        assert(before.sa_handler == after.sa_handler);
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 7);
    }
#endif

    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-wait"}, error));
        assert(child.request_termination(error));
        int exit_code = -1;
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 137);
    }

    {
        torture::ChildProcess child;
        assert(child.start(executable, {"--child-exit"}, error));
        int exit_code = -1;
        assert(!child.wait(std::chrono::milliseconds(-1), exit_code, error));
        assert(!error.empty());
        assert(child.wait(std::chrono::seconds(2), exit_code, error));
        assert(exit_code == 7);
    }

    {
        torture::ChildProcess child;
        assert(!child.start(executable + ".missing", {}, error));
        assert(error.find("execve failed") != std::string::npos ||
               error.find("CreateProcess failed") != std::string::npos);
    }
    {
        torture::ChildProcess child;
        const std::string invalid_value("bad\0value", 9);
        assert(!child.start(executable, {"--child-env"}, error,
                            {{"KVDB_CHILD_TEST", invalid_value}}));
        assert(error.find("NUL") != std::string::npos);
    }
    return 0;
}
