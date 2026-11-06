#include "child_process.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace torture {

struct ChildProcess::Impl {
    std::string pending;
#if defined(_WIN32)
    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    HANDLE input{nullptr};
    HANDLE output{nullptr};
#else
    pid_t pid{-1};
    int input{-1};
    int output{-1};
#endif
};

#if defined(_WIN32)
namespace {

std::string quote_argument(const std::string& argument) {
    std::string result = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('"');
            backslashes = 0;
        } else {
            result.append(backslashes, '\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, '\\');
    result.push_back('"');
    return result;
}

void close_handle(HANDLE& handle) {
    if (handle) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

struct EnvironmentKeyLess {
    bool operator()(const std::string& left, const std::string& right) const {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](unsigned char a, unsigned char b) {
                return std::tolower(a) < std::tolower(b);
            });
    }
};

std::vector<char> make_environment_block(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::map<std::string, std::string, EnvironmentKeyLess> variables;
    LPCH current = GetEnvironmentStringsA();
    if (current) {
        for (LPCH entry = current; *entry != '\0'; entry += std::strlen(entry) + 1) {
            const std::string item(entry);
            const std::size_t separator = item[0] == '=' ? item.find('=', 1) : item.find('=');
            if (separator != std::string::npos) {
                variables[item.substr(0, separator)] = item.substr(separator + 1);
            }
        }
        FreeEnvironmentStringsA(current);
    }
    for (const auto& override_value : overrides) {
        variables[override_value.first] = override_value.second;
    }

    std::vector<char> block;
    for (const auto& variable : variables) {
        const std::string item = variable.first + "=" + variable.second;
        block.insert(block.end(), item.begin(), item.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

} // namespace
#else
namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

} // namespace
#endif

ChildProcess::ChildProcess() : impl_(new Impl()) {}

ChildProcess::~ChildProcess() {
    std::string ignored_error;
    terminate(ignored_error);
    delete impl_;
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments,
                         std::string& error,
                         const std::vector<std::pair<std::string, std::string>>& environment) {
    if (
#if defined(_WIN32)
        impl_->process != nullptr
#else
        impl_->pid > 0
#endif
    ) {
        error = "child process is already running";
        return false;
    }
    impl_->pending.clear();

#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE child_input = nullptr;
    HANDLE child_output = nullptr;
    if (!CreatePipe(&child_input, &impl_->input, &security, 0) ||
        !CreatePipe(&impl_->output, &child_output, &security, 0)) {
        error = "CreatePipe failed: " + std::to_string(GetLastError());
        close_handle(child_input);
        close_handle(child_output);
        close_handle(impl_->input);
        close_handle(impl_->output);
        return false;
    }
    SetHandleInformation(impl_->input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0);

    std::string command_line = quote_argument(executable);
    for (const auto& argument : arguments) {
        command_line.push_back(' ');
        command_line += quote_argument(argument);
    }
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process_info{};
    std::vector<char> environment_block = make_environment_block(environment);
    const BOOL created = CreateProcessA(
        executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, environment_block.data(), nullptr, &startup, &process_info);
    close_handle(child_input);
    close_handle(child_output);
    if (!created) {
        error = "CreateProcess failed: " + std::to_string(GetLastError());
        close_handle(impl_->input);
        close_handle(impl_->output);
        return false;
    }
    impl_->process = process_info.hProcess;
    impl_->thread = process_info.hThread;
    return true;
#else
    int input_pipe[2]{-1, -1};
    int output_pipe[2]{-1, -1};
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        return false;
    }
    if (child == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        for (const auto& variable : environment) {
            if (setenv(variable.first.c_str(), variable.second.c_str(), 1) != 0) {
                _exit(126);
            }
        }
        execv(executable.c_str(), argv.data());
        _exit(127);
    }

    close_fd(input_pipe[0]);
    close_fd(output_pipe[1]);
    impl_->input = input_pipe[1];
    impl_->output = output_pipe[0];
    impl_->pid = child;
    std::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

bool ChildProcess::write_line(const std::string& line, std::string& error) {
    const std::string data = line + "\n";
#if defined(_WIN32)
    if (!impl_->input) {
        error = "child stdin is closed";
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        if (!WriteFile(impl_->input, data.data() + offset, chunk, &written, nullptr) || written == 0) {
            error = "WriteFile failed: " + std::to_string(GetLastError());
            return false;
        }
        offset += written;
    }
    return true;
#else
    if (impl_->input < 0) {
        error = "child stdin is closed";
        return false;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = write(impl_->input, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            error = std::string("write to child failed: ") + std::strerror(errno);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
#endif
}

bool ChildProcess::read_line(std::string& line, std::chrono::milliseconds timeout, bool& eof,
                             std::string& error) {
    eof = false;
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::size_t newline = impl_->pending.find('\n');
        if (newline != std::string::npos) {
            line = impl_->pending.substr(0, newline);
            impl_->pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "timed out waiting for child output";
            return false;
        }

        char buffer[4096];
#if defined(_WIN32)
        if (!impl_->output) {
            eof = true;
            return false;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(impl_->output, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE) {
                eof = true;
                return false;
            }
            error = "PeekNamedPipe failed: " + std::to_string(code);
            return false;
        }
        if (available == 0) {
            Sleep(1);
            continue;
        }
        DWORD received = 0;
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(impl_->output, buffer, requested, &received, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE) {
                eof = true;
                return false;
            }
            error = "ReadFile failed: " + std::to_string(code);
            return false;
        }
        impl_->pending.append(buffer, received);
#else
        if (impl_->output < 0) {
            eof = true;
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor{impl_->output, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
        if (ready == 0) {
            error = "timed out waiting for child output";
            return false;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            error = std::string("poll failed: ") + std::strerror(errno);
            return false;
        }
        const ssize_t received = read(impl_->output, buffer, sizeof(buffer));
        if (received == 0) {
            eof = true;
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            error = std::string("read from child failed: ") + std::strerror(errno);
            return false;
        }
        impl_->pending.append(buffer, static_cast<std::size_t>(received));
#endif
    }
}

bool ChildProcess::close_input(std::string& error) {
#if defined(_WIN32)
    if (!impl_->input) return true;
    if (!CloseHandle(impl_->input)) {
        error = "CloseHandle for child stdin failed: " + std::to_string(GetLastError());
        impl_->input = nullptr;
        return false;
    }
    impl_->input = nullptr;
#else
    if (impl_->input < 0) return true;
    if (close(impl_->input) != 0) {
        error = std::string("close child stdin failed: ") + std::strerror(errno);
        impl_->input = -1;
        return false;
    }
    impl_->input = -1;
#endif
    return true;
}

bool ChildProcess::wait(std::chrono::milliseconds timeout, int& exit_code, std::string& error) {
#if defined(_WIN32)
    if (!impl_->process) {
        error = "no child process to wait for";
        return false;
    }
    const DWORD result = WaitForSingleObject(impl_->process, static_cast<DWORD>(timeout.count()));
    if (result != WAIT_OBJECT_0) {
        error = result == WAIT_TIMEOUT ? "timed out waiting for child process"
                                     : "WaitForSingleObject failed: " + std::to_string(GetLastError());
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) {
        error = "GetExitCodeProcess failed: " + std::to_string(GetLastError());
        return false;
    }
    exit_code = static_cast<int>(code);
    close_handle(impl_->process);
    close_handle(impl_->thread);
    close_handle(impl_->input);
    close_handle(impl_->output);
    return true;
#else
    if (impl_->pid <= 0) {
        error = "no child process to wait for";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
        if (result == impl_->pid) {
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
            impl_->pid = -1;
            close_fd(impl_->input);
            close_fd(impl_->output);
            return true;
        }
        if (result < 0 && errno != EINTR) {
            error = std::string("waitpid failed: ") + std::strerror(errno);
            return false;
        }
        usleep(1000);
    }
    error = "timed out waiting for child process";
    return false;
#endif
}

bool ChildProcess::terminate(std::string& error) {
#if defined(_WIN32)
    if (impl_->process) {
        if (!TerminateProcess(impl_->process, 137) &&
            WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) {
            error = "TerminateProcess failed: " + std::to_string(GetLastError());
            return false;
        }
        if (WaitForSingleObject(impl_->process, INFINITE) != WAIT_OBJECT_0) {
            error = "waiting for terminated child failed: " + std::to_string(GetLastError());
            return false;
        }
    }
    close_handle(impl_->process);
    close_handle(impl_->thread);
    close_handle(impl_->input);
    close_handle(impl_->output);
#else
    if (impl_->pid > 0) {
        if (kill(impl_->pid, SIGKILL) != 0 && errno != ESRCH) {
            error = std::string("SIGKILL failed: ") + std::strerror(errno);
            return false;
        }
        int status = 0;
        pid_t result = -1;
        do {
            result = waitpid(impl_->pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != ECHILD) {
            error = std::string("waitpid after SIGKILL failed: ") + std::strerror(errno);
            return false;
        }
        impl_->pid = -1;
    }
    close_fd(impl_->input);
    close_fd(impl_->output);
#endif
    impl_->pending.clear();
    return true;
}

} // namespace torture
