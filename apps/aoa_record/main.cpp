// SPDX-License-Identifier: MIT
//
// aoa_record — command-line front end for aoahid_player_core's Recorder:
// `adb shell getevent -lt` in, player-compatible CSV rows out, saved in the
// shared csv/ folder by default.

#include "cli_options.hpp"

#include "aoahid_player/events.hpp"
#include "aoahid_player/recorder.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::atomic<aoap::Recorder*> g_recorder{nullptr};
static_assert(std::atomic<aoap::Recorder*>::is_always_lock_free);

extern "C" void handle_interrupt(int) {
    aoap::Recorder* recorder = g_recorder.load(std::memory_order_relaxed);
    if (recorder != nullptr)
        recorder->stop();
}

class ConsoleSink final : public aoap::EventSink {
  public:
    explicit ConsoleSink(const bool echo) : echo_(echo) {}

    void message(const aoap::Severity severity, const std::string_view text) override {
        const std::lock_guard lock(mutex_);
        FILE* stream = severity == aoap::Severity::info ? stdout : stderr;
        const char* tag = severity == aoap::Severity::info      ? "[INFO] "
                          : severity == aoap::Severity::warning ? "[WARN] "
                                                                : "[ERROR] ";
        std::fprintf(stream, "%s%.*s\n", tag, static_cast<int>(text.size()), text.data());
        std::fflush(stream);
    }

    void recorded_row(const std::string_view row) override {
        if (!echo_)
            return;
        const std::lock_guard lock(mutex_);
        std::fwrite(row.data(), 1, row.size(), stdout);
        std::fflush(stdout);
    }

  private:
    std::mutex mutex_;
    bool echo_;
};

} // namespace

int run(int argc, char** argv) {
    const std::optional<aoap::record::cli::Options> parsed = aoap::record::cli::parse(argc, argv);
    if (!parsed)
        return 1;
    const aoap::record::cli::Options& options = *parsed;
    if (options.help)
        return 0;

    aoap::RecordOptions record;
    record.output_path = aoap::resolve_record_path(options.output);
    record.adb_serial = options.adb_serial;
    record.input_device = options.input_device;
    std::printf("[INFO] saving to %s (Ctrl+C to stop)\n", record.output_path.c_str());
    std::fflush(stdout);

    ConsoleSink sink(options.echo);
    aoap::Recorder recorder(&sink);
    g_recorder.store(&recorder, std::memory_order_relaxed);
    std::signal(SIGINT, handle_interrupt);
#if defined(SIGTERM)
    std::signal(SIGTERM, handle_interrupt);
#endif
    const bool saved = recorder.run(record);
    g_recorder.store(nullptr, std::memory_order_relaxed);
    return saved ? 0 : 1;
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[ERROR] %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[ERROR] unknown failure\n");
        return 1;
    }
}
