#include "child_process.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace torture {

namespace {

bool valid_timeout(std::chrono::milliseconds timeout) {
    return timeout.count() >= 0 &&
        timeout.count() <= std::numeric_limits<int>::max();
}

bool append_output(std::string& pending, const char* data, std::size_t size,
                   std::size_t& pending_offset, std::string& error) {
    pending.append(data, size);
    const auto newline = pending.find('\n', pending_offset);
    const auto line_size = newline == std::string::npos
        ? pending.size() - pending_offset : newline - pending_offset;
    if (line_size > ChildProcess::max_output_line_bytes) {
        pending.clear();
        pending_offset = 0;
        error = "child output line exceeds " +
            std::to_string(ChildProcess::max_output_line_bytes) + " bytes";
        return false;
    }
    return true;
}

bool take_final_output_line(std::string& pending, std::size_t& pending_offset,
                            std::string& line, bool& eof, std::string& error) {
    const auto remaining = pending.size() - pending_offset;
    if (remaining > ChildProcess::max_output_line_bytes) {
        pending.clear();
        pending_offset = 0;
        error = "child output line exceeds " +
            std::to_string(ChildProcess::max_output_line_bytes) + " bytes";
        return false;
    }
    if (remaining == 0) {
        eof = true;
        return false;
    }
    line.assign(pending, pending_offset, remaining);
    pending.clear();
    pending_offset = 0;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

} // namespace

struct ChildProcess::Impl {
    std::string pending;
    std::size_t pending_offset{0};
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
    const std::vector<std::pair<std::string, std::string>>& overrides,
    std::string& error) {
    std::map<std::string, std::string, EnvironmentKeyLess> variables;
    LPCH current = GetEnvironmentStringsA();
    if (!current) {
        error = "GetEnvironmentStrings failed: " + std::to_string(GetLastError());
        return {};
    }
    for (LPCH entry = current; *entry != '\0'; entry += std::strlen(entry) + 1) {
        const std::string item(entry);
        const std::size_t separator = !item.empty() && item[0] == '='
            ? item.find('=', 1) : item.find('=');
        if (separator != std::string::npos) {
            variables[item.substr(0, separator)] = item.substr(separator + 1);
        }
    }
    FreeEnvironmentStringsA(current);
    for (const auto& override_value : overrides) {
        if (override_value.first.empty() ||
            override_value.first.find('=') != std::string::npos ||
            override_value.first.find('\0') != std::string::npos ||
            override_value.second.find('\0') != std::string::npos) {
            error = "invalid environment override: names cannot be empty or contain "
                    "'=' or NUL; values cannot contain NUL";
            return {};
        }
        variables[override_value.first] = override_value.second;
    }

    std::vector<char> block;
    for (const auto& variable : variables) {
        const std::string item = variable.first + "=" + variable.second;
        block.insert(block.end(), item.begin(), item.end());
        block.push_back('\0');
    }
    if (block.empty()) block.push_back('\0');
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

bool make_cloexec_pipe(int pipe_fds[2], std::string& error) {
    if (pipe(pipe_fds) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        int& fd = pipe_fds[index];
        if (fd <= STDERR_FILENO) {
            const int replacement = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
            if (replacement < 0) {
                const int saved_errno = errno;
                close(pipe_fds[0]);
                close(pipe_fds[1]);
                pipe_fds[0] = pipe_fds[1] = -1;
                error = std::string("moving pipe descriptor above standard streams failed: ") +
                    std::strerror(saved_errno);
                return false;
            }
            close(fd);
            fd = replacement;
        }
        const int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            const int saved_errno = errno;
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            pipe_fds[0] = pipe_fds[1] = -1;
            error = std::string("setting close-on-exec on pipe failed: ") +
                std::strerror(saved_errno);
            return false;
        }
    }
    return true;
}

bool write_without_sigpipe(int fd, const std::string& data, std::string& error) {
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const int mask_result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (mask_result != 0) {
        error = std::string("blocking SIGPIPE failed: ") + std::strerror(mask_result);
        return false;
    }

    bool was_pending = false;
    if (sigpending(&pending) == 0) was_pending = sigismember(&pending, SIGPIPE) == 1;
    std::size_t offset = 0;
    int write_error = 0;
    while (offset < data.size()) {
        const ssize_t written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            write_error = written == 0 ? EIO : errno;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }

    if (write_error == EPIPE && !was_pending) {
        timespec no_wait{};
        while (sigtimedwait(&blocked, nullptr, &no_wait) < 0 && errno == EINTR) {}
    }
    const int restore_result = pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (write_error != 0) {
        error = std::string("write to child failed: ") + std::strerror(write_error);
        return false;
    }
    if (restore_result != 0) {
        error = std::string("restoring signal mask failed: ") +
            std::strerror(restore_result);
        return false;
    }
    return true;
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
    error.clear();
    if (executable.empty() || executable.find('\0') != std::string::npos) {
        error = "executable path must be non-empty and cannot contain NUL";
        return false;
    }
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            error = "child arguments cannot contain NUL";
            return false;
        }
    }
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
    impl_->pending_offset = 0;

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
    if (!SetHandleInformation(impl_->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0)) {
        error = "SetHandleInformation failed: " + std::to_string(GetLastError());
        close_handle(child_input);
        close_handle(child_output);
        close_handle(impl_->input);
        close_handle(impl_->output);
        return false;
    }

    std::string command_line = quote_argument(executable);
    for (const auto& argument : arguments) {
        command_line.push_back(' ');
        command_line += quote_argument(argument);
    }
    if (command_line.size() >= 32767) {
        error = "Windows command line exceeds the 32767-character limit";
        close_handle(child_input);
        close_handle(child_output);
        close_handle(impl_->input);
        close_handle(impl_->output);
        return false;
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
    std::vector<char> environment_block = make_environment_block(environment, error);
    if (!error.empty()) {
        close_handle(impl_->input);
        close_handle(impl_->output);
        return false;
    }
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
    int exec_pipe[2]{-1, -1};
    if (!make_cloexec_pipe(input_pipe, error) ||
        !make_cloexec_pipe(output_pipe, error) ||
        !make_cloexec_pipe(exec_pipe, error)) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        close_fd(exec_pipe[0]);
        close_fd(exec_pipe[1]);
        return false;
    }

    std::map<std::string, std::string> variables;
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const auto separator = entry.find('=');
        if (separator != std::string::npos) {
            variables[entry.substr(0, separator)] = entry.substr(separator + 1);
        }
    }
    for (const auto& variable : environment) {
        if (variable.first.empty() || variable.first.find('=') != std::string::npos ||
            variable.first.find('\0') != std::string::npos ||
            variable.second.find('\0') != std::string::npos) {
            error = "invalid environment override: names cannot be empty or contain "
                    "'=' or NUL; values cannot contain NUL";
            close_fd(input_pipe[0]);
            close_fd(input_pipe[1]);
            close_fd(output_pipe[0]);
            close_fd(output_pipe[1]);
            close_fd(exec_pipe[0]);
            close_fd(exec_pipe[1]);
            return false;
        }
        variables[variable.first] = variable.second;
    }
    std::vector<std::string> environment_entries;
    environment_entries.reserve(variables.size());
    for (const auto& variable : variables) {
        environment_entries.push_back(variable.first + "=" + variable.second);
    }
    std::vector<char*> envp;
    envp.reserve(environment_entries.size() + 1);
    for (auto& entry : environment_entries) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        close_fd(exec_pipe[0]);
        close_fd(exec_pipe[1]);
        return false;
    }
    if (child == 0) {
        close(exec_pipe[0]);
        if (dup2(input_pipe[0], STDIN_FILENO) < 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            const int child_error = errno;
            write(exec_pipe[1], &child_error, sizeof(child_error));
            _exit(126);
        }
        for (const int fd : {input_pipe[0], input_pipe[1],
                             output_pipe[0], output_pipe[1]}) {
            if (fd != STDIN_FILENO && fd != STDOUT_FILENO) close(fd);
        }
        execve(executable.c_str(), argv.data(), envp.data());
        const int child_error = errno;
        write(exec_pipe[1], &child_error, sizeof(child_error));
        _exit(127);
    }

    close_fd(input_pipe[0]);
    close_fd(output_pipe[1]);
    close_fd(exec_pipe[1]);
    int child_error = 0;
    std::size_t error_bytes = 0;
    while (error_bytes < sizeof(child_error)) {
        const ssize_t count = read(exec_pipe[0],
            reinterpret_cast<char*>(&child_error) + error_bytes,
            sizeof(child_error) - error_bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            error = std::string("reading child launch status failed: ") +
                std::strerror(errno);
            break;
        }
        if (count == 0) break;
        error_bytes += static_cast<std::size_t>(count);
    }
    close_fd(exec_pipe[0]);
    if (!error.empty() || error_bytes != 0) {
        if (error.empty()) {
            error = error_bytes == sizeof(child_error)
                ? std::string("execve failed: ") + std::strerror(child_error)
                : "child launch status was incomplete";
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        return false;
    }
    impl_->input = input_pipe[1];
    impl_->output = output_pipe[0];
    impl_->pid = child;
    return true;
#endif
}

bool ChildProcess::write_line(const std::string& line, std::string& error) {
    error.clear();
    if (line.find_first_of("\r\n") != std::string::npos ||
        line.find('\0') != std::string::npos) {
        error = "child request must contain exactly one line";
        return false;
    }
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
    return write_without_sigpipe(impl_->input, data, error);
#endif
}

bool ChildProcess::read_line(std::string& line, std::chrono::milliseconds timeout, bool& eof,
                             std::string& error) {
    error.clear();
    eof = false;
    line.clear();
    if (!valid_timeout(timeout)) {
        error = "timeout must be in 0..2147483647 milliseconds";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::size_t newline = impl_->pending.find('\n', impl_->pending_offset);
        if (newline != std::string::npos) {
            if (newline - impl_->pending_offset > max_output_line_bytes) {
                impl_->pending.clear();
                impl_->pending_offset = 0;
                error = "child output line exceeds " +
                    std::to_string(max_output_line_bytes) + " bytes";
                return false;
            }
            line = impl_->pending.substr(impl_->pending_offset,
                newline - impl_->pending_offset);
            impl_->pending_offset = newline + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (impl_->pending_offset == impl_->pending.size()) {
                impl_->pending.clear();
                impl_->pending_offset = 0;
            } else if (impl_->pending_offset >= 4096 &&
                       impl_->pending_offset >= impl_->pending.size() / 2) {
                impl_->pending.erase(0, impl_->pending_offset);
                impl_->pending_offset = 0;
            }
            return true;
        }
        if (impl_->pending.size() - impl_->pending_offset > max_output_line_bytes) {
            impl_->pending.clear();
            impl_->pending_offset = 0;
            error = "child output line exceeds " +
                std::to_string(max_output_line_bytes) + " bytes";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "timed out waiting for child output";
            return false;
        }

        char buffer[4096];
#if defined(_WIN32)
        if (!impl_->output) {
            return take_final_output_line(impl_->pending, impl_->pending_offset,
                                          line, eof, error);
        }
        DWORD available = 0;
        if (!PeekNamedPipe(impl_->output, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE) {
                return take_final_output_line(impl_->pending, impl_->pending_offset,
                                              line, eof, error);
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
                return take_final_output_line(impl_->pending, impl_->pending_offset,
                                              line, eof, error);
            }
            error = "ReadFile failed: " + std::to_string(code);
            return false;
        }
        if (!append_output(impl_->pending, buffer, received,
                           impl_->pending_offset, error)) return false;
#else
        if (impl_->output < 0) {
            return take_final_output_line(impl_->pending, impl_->pending_offset,
                                          line, eof, error);
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
            return take_final_output_line(impl_->pending, impl_->pending_offset,
                                          line, eof, error);
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            error = std::string("read from child failed: ") + std::strerror(errno);
            return false;
        }
        if (!append_output(impl_->pending, buffer,
                           static_cast<std::size_t>(received),
                           impl_->pending_offset, error)) return false;
#endif
    }
}

bool ChildProcess::close_input(std::string& error) {
    error.clear();
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
    error.clear();
    if (!valid_timeout(timeout)) {
        error = "timeout must be in 0..2147483647 milliseconds";
        return false;
    }
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
    while (true) {
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
        if (std::chrono::steady_clock::now() >= deadline) break;
        usleep(1000);
    }
    error = "timed out waiting for child process";
    return false;
#endif
}

bool ChildProcess::request_termination(std::string& error) {
    error.clear();
#if defined(_WIN32)
    if (!impl_->process) {
        error = "no child process to terminate";
        return false;
    }
    if (!TerminateProcess(impl_->process, 137) &&
        WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) {
        error = "TerminateProcess failed: " + std::to_string(GetLastError());
        return false;
    }
#else
    if (impl_->pid <= 0) {
        error = "no child process to terminate";
        return false;
    }
    if (kill(impl_->pid, SIGKILL) != 0 && errno != ESRCH) {
        error = std::string("SIGKILL failed: ") + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

bool ChildProcess::terminate(std::string& error) {
    error.clear();
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
    impl_->pending_offset = 0;
    return true;
}

} // namespace torture
