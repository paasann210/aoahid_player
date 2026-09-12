// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace aoap {

// A child process with its stdout and stderr on pipes and an empty stdin.
// argv[0] is looked up on PATH and every argument is passed as-is, with no
// shell in between. On Windows the child gets no console window, so a GUI
// can run it without a window flashing up, and it inherits only its pipes.
class ChildProcess {
  public:
    enum class Read { line, timeout, ended };

    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    // Arguments are UTF-8. Returns false with a readable reason when the
    // program cannot be started.
    bool start(const std::vector<std::string>& argv, std::string& error);

    // Waits up to `timeout_ms` for one stdout line, returned without its line
    // ending. `ended` means the child has exited and its output is drained;
    // a leftover partial line is returned as a line first.
    Read read_line(std::string& line, int timeout_ms);

    // Everything the child wrote to stderr so far.
    [[nodiscard]] const std::string& error_output() const noexcept { return errors_; }

    // Stops the child if `terminate` is set and it is still running, reaps it,
    // and returns its exit code (-1 when it was terminated or never ran).
    int finish(bool terminate) noexcept;

    [[nodiscard]] bool started() const noexcept;

  private:
    bool pump(int timeout_ms);
    bool exited() noexcept;

    std::string buffer_;
    std::string errors_;
    bool out_open_{};
    bool err_open_{};
    bool exited_{};
    int exit_code_{-1};

#if defined(_WIN32)
    void* process_{};
    void* out_{};
    void* err_{};
#else
    int pid_{-1};
    int out_{-1};
    int err_{-1};
#endif
};

struct CommandResult {
    bool started{};
    bool timed_out{};
    int exit_code{-1};
    std::string output;
    std::string error_output;
    std::string start_error;
};

// Runs a short command to completion and collects its output. The child is
// stopped after `timeout_ms`.
CommandResult run_command(const std::vector<std::string>& argv, int timeout_ms = 15000);

} // namespace aoap
