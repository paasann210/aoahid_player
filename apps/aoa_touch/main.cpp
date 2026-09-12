// SPDX-License-Identifier: MIT
//
// aoa_touch — command-line front end. Device handling, scripts, and playback
// all live in aoahid_player_core; this file only does the terminal parts:
// numbered pickers, the "-> " offset prompt, and printing status.

#include "cli_options.hpp"

#include "aoahid_player/adb.hpp"
#include "aoahid_player/event_script.hpp"
#include "aoahid_player/events.hpp"
#include "aoahid_player/paths.hpp"
#include "aoahid_player/player.hpp"
#include "aoahid_player/session.hpp"
#include "aoahid_player/timing.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr double ns_per_ms = 1'000'000.0;

std::atomic<aoap::Player*> g_player{nullptr};

// The handler may only touch lock-free atomics and Player::stop(), which is
// an atomic OR plus a futex wake.
static_assert(std::atomic<aoap::Player*>::is_always_lock_free);

extern "C" void handle_interrupt(int) {
    aoap::Player* player = g_player.load(std::memory_order_relaxed);
    if (player != nullptr)
        player->stop();
}

// Prints core status. The progress line is rewritten in place with '\r', so
// a message that follows it starts on a fresh line.
class ConsoleSink final : public aoap::EventSink {
  public:
    void message(const aoap::Severity severity, const std::string_view text) override {
        const std::lock_guard lock(mutex_);
        FILE* stream = severity == aoap::Severity::info ? stdout : stderr;
        const char* tag = severity == aoap::Severity::info      ? "[INFO] "
                          : severity == aoap::Severity::warning ? "[WARN] "
                                                                : "[ERROR] ";
        if (progress_open_) {
            std::fputc('\n', stdout);
            progress_open_ = false;
        }
        std::fprintf(stream, "%s%.*s\n", tag, static_cast<int>(text.size()), text.data());
        std::fflush(stream);
    }

    void loop_completed(const uint64_t loops, const uint64_t reports) override {
        if (loops % 100 != 0)
            return;
        const std::lock_guard lock(mutex_);
        std::printf("\r[INFO] loops=%-8llu reports=%-10llu", static_cast<unsigned long long>(loops),
                    static_cast<unsigned long long>(reports));
        std::fflush(stdout);
        progress_open_ = true;
    }

    void end_progress() {
        const std::lock_guard lock(mutex_);
        if (progress_open_) {
            std::fputc('\n', stdout);
            progress_open_ = false;
        }
    }

  private:
    std::mutex mutex_;
    bool progress_open_{};
};

void fail(const std::string& text) { std::fprintf(stderr, "[ERROR] %s\n", text.c_str()); }

bool read_line(std::string& out) {
    char buffer[256];
    if (std::fgets(buffer, sizeof buffer, stdin) == nullptr)
        return false;
    out.assign(buffer);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return true;
}

// Parses "1,3" or "all" against a list of `count` entries into zero-based
// indices. Returns false and explains the problem on bad input.
bool select_indices(const std::string& text, const size_t count, std::vector<size_t>& out) {
    out.clear();
    if (text == "all") {
        for (size_t index = 0; index < count; ++index)
            out.push_back(index);
        return true;
    }
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const size_t end = comma == std::string::npos ? text.size() : comma;
        std::string piece = text.substr(start, end - start);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
            piece.erase(piece.begin());
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.pop_back();
        if (!piece.empty()) {
            char* stop = nullptr;
            const long number = std::strtol(piece.c_str(), &stop, 10);
            if (stop == piece.c_str() || *stop != '\0' || number < 1 ||
                static_cast<size_t>(number) > count) {
                fail("\"" + piece + "\" is not a listed number");
                return false;
            }
            const size_t zero_based = static_cast<size_t>(number) - 1;
            bool duplicate = false;
            for (const size_t existing : out)
                duplicate = duplicate || existing == zero_based;
            if (!duplicate)
                out.push_back(zero_based);
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    if (out.empty()) {
        fail("no device was selected");
        return false;
    }
    return true;
}

bool choose_devices(const aoap::cli::Options& options,
                    const std::vector<aoap::DeviceEntry>& devices, std::vector<size_t>& selection) {
    selection.clear();
    if (options.have_devices) {
        if (options.all_devices) {
            for (size_t index = 0; index < devices.size(); ++index)
                selection.push_back(index);
            return true;
        }
        for (const size_t number : options.devices) {
            if (number > devices.size()) {
                fail("--devices " + std::to_string(number) + " is out of range (" +
                     std::to_string(devices.size()) + " found)");
                return false;
            }
            selection.push_back(number - 1);
        }
        return true;
    }
    std::printf("AOA-capable devices:\n");
    for (size_t index = 0; index < devices.size(); ++index)
        std::printf("  %2zu  %s\n", index + 1, devices[index].label.c_str());
    std::printf("select devices (e.g. \"1,3\" or \"all\") [1]: ");
    std::fflush(stdout);
    std::string answer;
    if (!read_line(answer))
        return false;
    if (answer.empty())
        answer = "1";
    return select_indices(answer, devices.size(), selection);
}

bool choose_script(const aoap::cli::Options& options, std::string& path) {
    if (!options.script_path.empty()) {
        path = options.script_path;
        return true;
    }
    const std::string directory = aoap::script_directory();
    const std::vector<std::string> scripts = aoap::discover_csv_files(directory);
    if (scripts.empty()) {
        fail("no *.csv found in " + directory + "; pass a script path explicitly");
        return false;
    }
    std::printf("scripts in %s:\n", directory.c_str());
    for (size_t index = 0; index < scripts.size(); ++index)
        std::printf("  %2zu  %s\n", index + 1, aoap::display_name(scripts[index]).c_str());
    std::printf("select a script [1]: ");
    std::fflush(stdout);

    std::string answer;
    if (!read_line(answer))
        return false;
    if (answer.empty()) {
        path = scripts.front();
        return true;
    }
    char* stop = nullptr;
    const long number = std::strtol(answer.c_str(), &stop, 10);
    if (stop == answer.c_str() || *stop != '\0' || number < 1 ||
        static_cast<size_t>(number) > scripts.size()) {
        fail("\"" + answer + "\" is not a listed number");
        return false;
    }
    path = scripts[static_cast<size_t>(number) - 1];
    return true;
}

bool load_script(const aoap::cli::Options& options, std::string& path,
                 std::shared_ptr<aoap::EventScript>& script) {
    if (!choose_script(options, path))
        return false;
    auto loaded = std::make_shared<aoap::EventScript>();
    std::string error;
    if (!loaded->load(path, error)) {
        fail(error);
        return false;
    }
    std::printf("[INFO] %s: %zu intro rows, %zu loop rows\n", aoap::display_name(path).c_str(),
                loaded->once_rows.size(), loaded->loop_rows.size());
    script = std::move(loaded);
    return true;
}

// The "-> " prompt reads stdin on a detached thread, because a blocked read
// cannot be interrupted portably. It reaches the Player only through this
// bridge, which is cleared before the Player goes away.
struct OffsetBridge {
    std::mutex mutex;
    aoap::Player* player{};
};

void start_offset_prompt(const std::shared_ptr<OffsetBridge>& bridge) {
    std::thread([bridge] {
        char buffer[64];
        while (true) {
            {
                const std::lock_guard lock(bridge->mutex);
                if (bridge->player == nullptr)
                    return;
            }
            std::printf("-> ");
            std::fflush(stdout);
            if (std::fgets(buffer, sizeof buffer, stdin) == nullptr)
                return;
            char* end = nullptr;
            const double milliseconds = std::strtod(buffer, &end);
            if (end == buffer)
                continue;
            const std::lock_guard lock(bridge->mutex);
            if (bridge->player == nullptr)
                return;
            const int64_t total = bridge->player->adjust_offset_ns(
                static_cast<int64_t>(milliseconds * ns_per_ms));
            std::printf("[OFFSET] %+.3f ms\n", static_cast<double>(total) / ns_per_ms);
            std::fflush(stdout);
        }
    }).detach();
}

} // namespace

int run(int argc, char** argv) {
    const std::optional<aoap::cli::Options> parsed = aoap::cli::parse(argc, argv);
    if (!parsed)
        return 1;
    aoap::cli::Options options = *parsed;
    if (options.help)
        return 0;

    ConsoleSink sink;

    if (options.auto_resolution) {
        int32_t width = 0;
        int32_t height = 0;
        std::string error;
        if (aoap::adb_screen_size({}, width, height, error)) {
            options.profiles.touch.width = width;
            options.profiles.touch.height = height;
            std::printf("[INFO] adb reported %dx%d\n", width, height);
        } else if (options.profiles.touch.width <= 0) {
            fail("-A could not read the screen size (" + error + "); pass --touch-res");
            return 1;
        } else {
            std::fprintf(stderr, "[WARN] -A failed (%s); keeping --touch-res %dx%d\n",
                         error.c_str(), options.profiles.touch.width,
                         options.profiles.touch.height);
        }
        if (options.profiles.pen.enabled)
            aoap::resolve_pen_surface(options.profiles);
        std::printf("[INFO] stopping the adb server so it releases the USB device\n");
        std::fflush(stdout);
        if (!aoap::adb_kill_server(error))
            std::fprintf(stderr, "[WARN] %s\n", error.c_str());
        aoap::Timing::sleep_ms(1200);
    }

    std::string error;
    if (!aoap::validate_setup(options.profiles, error)) {
        fail(error);
        return 1;
    }

    // A script given on the command line is checked before any USB work.
    std::shared_ptr<aoap::EventScript> script;
    if (!options.script_path.empty()) {
        std::string ignored;
        if (!load_script(options, ignored, script))
            return 1;
    }

    aoap::Session session(&sink);
    std::vector<aoap::DeviceEntry> devices;
    if (!session.refresh(devices, error)) {
        fail(error);
        return 1;
    }
    if (devices.empty()) {
        fail("no AOA-capable device found.\n"
             "  1) is the cable a data cable, not charge-only?\n"
             "  2) is the phone's USB mode something other than \"Charge only\"?\n"
             "  3) on Linux, does the udev rule allow access to this device?");
        return 1;
    }

    std::vector<size_t> selection;
    if (!choose_devices(options, devices, selection))
        return 1;

    std::string script_path;
    if (!script && !load_script(options, script_path, script))
        return 1;

    if (!session.connect(selection, options.profiles, error)) {
        fail(error);
        return 1;
    }

    // A recording names the space its coordinates were taken in; map it onto
    // the connected touchscreen and pen surfaces before playing.
    {
        const aoap::ProfileSetup& setup = options.profiles;
        auto scaled = std::make_shared<aoap::EventScript>();
        if (aoap::scale_script(*script, setup.touch.enabled ? setup.touch.width : 0,
                               setup.touch.enabled ? setup.touch.height : 0,
                               setup.pen.enabled ? setup.pen.width : 0,
                               setup.pen.enabled ? setup.pen.height : 0, *scaled)) {
            std::printf("[INFO] scaling coordinates from %dx%d\n", script->screen_width,
                        script->screen_height);
            script = std::move(scaled);
        }
    }

    aoap::Player player(session.group(), &sink);
    player.load(script);
    player.set_speed(options.speed);
    player.set_loop_limit(options.loop_count);

    const auto bridge = std::make_shared<OffsetBridge>();
    if (!options.no_prompt) {
        bridge->player = &player;
        start_offset_prompt(bridge);
    }

    std::printf("[INFO] playing (Ctrl+C to stop)\n");
    std::fflush(stdout);
    g_player.store(&player, std::memory_order_relaxed);
    std::signal(SIGINT, handle_interrupt);
#if defined(SIGTERM)
    std::signal(SIGTERM, handle_interrupt);
#endif
    player.run();
    g_player.store(nullptr, std::memory_order_relaxed);
    std::signal(SIGINT, SIG_DFL);
    {
        const std::lock_guard lock(bridge->mutex);
        bridge->player = nullptr;
    }
    sink.end_progress();

    const aoap::PlaybackStatus status = player.status();
    std::printf("[INFO] stopped. loops=%llu reports=%llu\n",
                static_cast<unsigned long long>(status.loops),
                static_cast<unsigned long long>(status.reports));

    const std::vector<aoap::DeviceStatus> summary = session.group().snapshot();
    session.disconnect();

    bool had_errors = false;
    std::printf("[INFO] device summary:\n");
    for (const aoap::DeviceStatus& device : summary) {
        std::printf("  %-8llu reports, %-6llu errors  %s\n",
                    static_cast<unsigned long long>(device.reports),
                    static_cast<unsigned long long>(device.errors), device.label.c_str());
        if (device.errors != 0) {
            had_errors = true;
            std::printf("      last: %s%s\n", device.last_error.c_str(),
                        device.active ? "" : " [dropped from the group]");
        }
    }
    return had_errors ? 2 : 0;
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
