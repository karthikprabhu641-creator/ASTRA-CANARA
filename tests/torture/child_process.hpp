#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace torture {

class ChildProcess {
public:
    static constexpr std::size_t max_output_line_bytes = 64U * 1024U * 1024U;

    ChildProcess();
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& executable, const std::vector<std::string>& arguments,
               std::string& error,
               const std::vector<std::pair<std::string, std::string>>& environment = {});
    bool write_line(const std::string& line, std::string& error);
    bool read_line(std::string& line, std::chrono::milliseconds timeout, bool& eof,
                   std::string& error);
    bool close_input(std::string& error);
    bool wait(std::chrono::milliseconds timeout, int& exit_code, std::string& error);
    bool request_termination(std::string& error);
    bool terminate(std::string& error);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace torture
