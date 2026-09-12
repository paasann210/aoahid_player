// SPDX-License-Identifier: MIT
#include "aoahid_player/process.hpp"

#include "aoahid_player/timing.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aoap {
namespace {

constexpr int64_t ns_per_ms = 1'000'000;

void strip_line_end(std::string& line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
}

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            length);
    }
    return wide;
}

// Quotes one argument the way CommandLineToArgvW and the MSVC runtime split it.
void append_argument(std::wstring& line, const std::wstring& argument) {
    if (!line.empty())
        line += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"')
            line.append(backslashes * 2 + 1, L'\\');
        else
            line.append(backslashes, L'\\');
        backslashes = 0;
        line += character;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

void close_handle(void*& handle) noexcept {
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

#else

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

#endif

} // namespace

ChildProcess::~ChildProcess() { finish(true); }

bool ChildProcess::started() const noexcept {
#if defined(_WIN32)
    return process_ != nullptr;
#else
    return pid_ > 0;
#endif
}

#if defined(_WIN32)

bool ChildProcess::start(const std::vector<std::string>& argv, std::string& error) {
    finish(true);
    buffer_.clear();
    errors_.clear();
    exited_ = false;
    exit_code_ = -1;
    if (argv.empty()) {
        error = "no program was given";
        return false;
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &inheritable, 1 << 16) ||
        !CreatePipe(&err_read, &err_write, &inheritable, 1 << 16)) {
        for (HANDLE handle : {out_read, out_write, err_read, err_write}) {
            if (handle != nullptr)
                CloseHandle(handle);
        }
        error = "could not create a pipe";
        return false;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &inheritable, OPEN_EXISTING, 0, nullptr);

    // Only the child's three standard handles are inherited, so it cannot
    // keep anything else of ours open.
    HANDLE inherited[3] = {out_write, err_write, null_input};
    const DWORD inherited_count = null_input != INVALID_HANDLE_VALUE ? 3 : 2;
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<unsigned char> list_storage(list_size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    const bool have_list =
        InitializeProcThreadAttributeList(list, 1, 0, &list_size) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                  inherited_count * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : nullptr;
    startup.StartupInfo.hStdOutput = out_write;
    startup.StartupInfo.hStdError = err_write;
    startup.lpAttributeList = have_list ? list : nullptr;

    std::wstring command_line;
    for (const std::string& argument : argv)
        append_argument(command_line, widen(argument));

    PROCESS_INFORMATION information{};
    const DWORD flags = CREATE_NO_WINDOW | (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, flags,
                                        nullptr, nullptr, &startup.StartupInfo, &information);
    const DWORD failure = created ? 0 : GetLastError();
    if (have_list)
        DeleteProcThreadAttributeList(list);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (null_input != INVALID_HANDLE_VALUE)
        CloseHandle(null_input);

    if (!created) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND)
            error = "\"" + argv[0] + "\" was not found on PATH";
        else
            error = "\"" + argv[0] + "\" could not be started (Windows error " +
                    std::to_string(failure) + ")";
        return false;
    }
    CloseHandle(information.hThread);
    process_ = information.hProcess;
    out_ = out_read;
    err_ = err_read;
    out_open_ = true;
    err_open_ = true;
    return true;
}

bool ChildProcess::pump(const int timeout_ms) {
    const int64_t deadline = Timing::now_ns() + static_cast<int64_t>(timeout_ms) * ns_per_ms;
    while (true) {
        bool progressed = false;
        const auto drain = [&](void*& handle, bool& open, std::string& sink) {
            if (!open)
                return;
            while (true) {
                DWORD available = 0;
                if (!PeekNamedPipe(static_cast<HANDLE>(handle), nullptr, 0, nullptr, &available,
                                   nullptr)) {
                    // Broken pipe: every writer has closed it.
                    close_handle(handle);
                    open = false;
                    progressed = true;
                    return;
                }
                if (available == 0)
                    return;
                char chunk[4096];
                DWORD read = 0;
                const DWORD wanted = std::min(available, static_cast<DWORD>(sizeof chunk));
                if (!ReadFile(static_cast<HANDLE>(handle), chunk, wanted, &read, nullptr) ||
                    read == 0) {
                    close_handle(handle);
                    open = false;
                    progressed = true;
                    return;
                }
                sink.append(chunk, read);
                progressed = true;
            }
        };
        drain(out_, out_open_, buffer_);
        drain(err_, err_open_, errors_);
        if (progressed)
            return true;
        const int64_t remaining = deadline - Timing::now_ns();
        if (remaining <= 0 || (!out_open_ && !err_open_))
            return false;
        // Anonymous pipes cannot be waited on, so they are polled. Only the
        // recorder and short adb commands use this, never the playback path.
        Sleep(static_cast<DWORD>(std::clamp<int64_t>(remaining / ns_per_ms, 1, 10)));
    }
}

bool ChildProcess::exited() noexcept {
    if (exited_)
        return true;
    if (process_ == nullptr)
        return true;
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0)
        return false;
    DWORD code = 0;
    exit_code_ = GetExitCodeProcess(static_cast<HANDLE>(process_), &code) ? static_cast<int>(code)
                                                                          : -1;
    exited_ = true;
    return true;
}

int ChildProcess::finish(const bool terminate) noexcept {
    if (process_ != nullptr) {
        if (!exited()) {
            if (terminate) {
                TerminateProcess(static_cast<HANDLE>(process_), 1);
                WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
                exited_ = true;
                exit_code_ = -1;
            } else {
                WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
                exited();
            }
        }
        try {
            while (pump(0)) {
            }
        } catch (...) {
        }
        close_handle(process_);
    }
    close_handle(out_);
    close_handle(err_);
    out_open_ = false;
    err_open_ = false;
    return exit_code_;
}

#else

bool ChildProcess::start(const std::vector<std::string>& argv, std::string& error) {
    finish(true);
    buffer_.clear();
    errors_.clear();
    exited_ = false;
    exit_code_ = -1;
    if (argv.empty()) {
        error = "no program was given";
        return false;
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            if (fd >= 0)
                ::close(fd);
        }
        error = std::string("could not create a pipe: ") + std::strerror(errno);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // Nothing else of ours (USB or GL descriptors) may leak into adb or the
    // adb server it may start.
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
#endif

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t signals;
    sigemptyset(&signals);
    posix_spawnattr_setsigmask(&attributes, &signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &signals);
    // Its own process group: a terminal Ctrl+C reaches only us, and finish()
    // can stop the whole group.
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                              POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> arguments;
    arguments.reserve(argv.size() + 1);
    for (const std::string& argument : argv)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);

    pid_t pid = -1;
    const int result = posix_spawnp(&pid, arguments[0], &actions, &attributes, arguments.data(),
                                    environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (result != 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        if (result == ENOENT)
            error = "\"" + argv[0] + "\" was not found on PATH";
        else
            error = "\"" + argv[0] + "\" could not be started: " + std::strerror(result);
        return false;
    }
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);
    pid_ = pid;
    out_ = out_pipe[0];
    err_ = err_pipe[0];
    out_open_ = true;
    err_open_ = true;
    return true;
}

bool ChildProcess::pump(const int timeout_ms) {
    pollfd waiting[2]{};
    std::string* sinks[2]{};
    int* fds[2]{};
    bool* open[2]{};
    nfds_t count = 0;
    if (out_open_) {
        waiting[count] = {out_, POLLIN, 0};
        sinks[count] = &buffer_;
        fds[count] = &out_;
        open[count] = &out_open_;
        ++count;
    }
    if (err_open_) {
        waiting[count] = {err_, POLLIN, 0};
        sinks[count] = &errors_;
        fds[count] = &err_;
        open[count] = &err_open_;
        ++count;
    }
    if (count == 0)
        return false;
    const int ready = poll(waiting, count, timeout_ms);
    if (ready <= 0)
        return false;

    bool progressed = false;
    for (nfds_t index = 0; index < count; ++index) {
        if ((waiting[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;
        while (true) {
            char chunk[4096];
            const ssize_t taken = ::read(*fds[index], chunk, sizeof chunk);
            if (taken > 0) {
                sinks[index]->append(chunk, static_cast<size_t>(taken));
                progressed = true;
                continue;
            }
            if (taken < 0 && errno == EINTR)
                continue;
            if (taken < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            close_fd(*fds[index]); // end of stream or a hard error
            *open[index] = false;
            progressed = true;
            break;
        }
    }
    return progressed;
}

bool ChildProcess::exited() noexcept {
    if (exited_)
        return true;
    if (pid_ <= 0)
        return true;
    int status = 0;
    const pid_t reaped = waitpid(pid_, &status, WNOHANG);
    if (reaped == 0)
        return false;
    exited_ = true;
    exit_code_ = reaped == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

int ChildProcess::finish(const bool terminate) noexcept {
    if (pid_ > 0) {
        if (!exited()) {
            if (terminate) {
                kill(-pid_, SIGTERM);
                for (int attempt = 0; attempt < 100 && !exited(); ++attempt)
                    Timing::sleep_ms(10);
                if (!exited()) {
                    kill(-pid_, SIGKILL);
                    int status = 0;
                    waitpid(pid_, &status, 0);
                    exited_ = true;
                }
                exit_code_ = -1;
            } else {
                int status = 0;
                const pid_t reaped = waitpid(pid_, &status, 0);
                exited_ = true;
                exit_code_ = reaped == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }
        try {
            while (pump(0)) {
            }
        } catch (...) {
        }
        pid_ = -1;
    }
    close_fd(out_);
    close_fd(err_);
    out_open_ = false;
    err_open_ = false;
    return exit_code_;
}

#endif

ChildProcess::Read ChildProcess::read_line(std::string& line, const int timeout_ms) {
    const int64_t deadline = Timing::now_ns() + static_cast<int64_t>(timeout_ms) * ns_per_ms;
    while (true) {
        const size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line.assign(buffer_, 0, newline);
            buffer_.erase(0, newline + 1);
            strip_line_end(line);
            return Read::line;
        }
        if (!out_open_) {
            if (!buffer_.empty()) {
                line.swap(buffer_);
                buffer_.clear();
                strip_line_end(line);
                return Read::line;
            }
            return Read::ended;
        }
        if (exited()) {
            // The child is gone but something it started (the adb server)
            // may still hold the pipe, so stop at what is already there.
            while (pump(0)) {
            }
            if (buffer_.find('\n') == std::string::npos) {
#if defined(_WIN32)
                close_handle(out_);
#else
                close_fd(out_);
#endif
                out_open_ = false;
            }
            continue;
        }
        const int64_t remaining = deadline - Timing::now_ns();
        if (remaining <= 0)
            return Read::timeout;
        // Capped so an exit is noticed even while a daemon holds the pipe.
        pump(static_cast<int>(std::min<int64_t>(remaining / ns_per_ms + 1, 100)));
    }
}

CommandResult run_command(const std::vector<std::string>& argv, const int timeout_ms) {
    CommandResult result;
    ChildProcess child;
    result.started = child.start(argv, result.start_error);
    if (!result.started)
        return result;
    const int64_t deadline = Timing::now_ns() + static_cast<int64_t>(timeout_ms) * ns_per_ms;
    std::string line;
    while (true) {
        const int64_t remaining = (deadline - Timing::now_ns()) / ns_per_ms;
        if (remaining <= 0) {
            result.timed_out = true;
            break;
        }
        const ChildProcess::Read read = child.read_line(line, static_cast<int>(remaining));
        if (read == ChildProcess::Read::line) {
            result.output += line;
            result.output += '\n';
        } else if (read == ChildProcess::Read::timeout) {
            result.timed_out = true;
            break;
        } else {
            break;
        }
    }
    result.exit_code = child.finish(result.timed_out);
    result.error_output = child.error_output();
    return result;
}

} // namespace aoap
