// SPDX-License-Identifier: MIT
#include "aoahid_player/recorder.hpp"

#include "aoahid_player/adb.hpp"
#include "aoahid_player/event_script.hpp"
#include "aoahid_player/getevent_parser.hpp"
#include "aoahid_player/paths.hpp"
#include "aoahid_player/process.hpp"
#include "aoahid_player/timing.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <vector>

namespace aoap {
namespace {

constexpr int read_slice_ms = 100;           // how quickly stop() is noticed
constexpr int64_t flush_interval_ns = 250'000'000;

void append_rows(std::string& text, const std::vector<EventRecord>& rows) {
    for (const EventRecord& record : rows) {
        const double wait_ms = static_cast<double>(record.wait_ns) / 1'000'000.0;
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, TouchEvent>)
                    append_touch_row(text, value.finger_id, value.state, value.x, value.y, wait_ms);
                else if constexpr (std::is_same_v<T, KeyEvent>)
                    append_key_row(text, value.usage, value.down, wait_ms);
            },
            record.payload);
    }
}

// adb's first real complaint; lines starting with '*' are server start-up chatter.
std::string first_complaint(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '.'))
            line.pop_back();
        if (!line.empty() && line.front() != '*')
            return line;
        start = end + 1;
    }
    return {};
}

} // namespace

std::string timestamped_record_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof name, "record-%Y%m%d-%H%M%S", &local);
    return name;
}

std::string resolve_record_path(std::string_view name) {
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
        name.remove_prefix(1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.remove_suffix(1);
    std::string file = name.empty() ? timestamped_record_name() : std::string(name);
    std::filesystem::path path = utf8_path(file);
    if (!path.has_extension())
        path += ".csv";
    if (!path.has_parent_path())
        path = utf8_path(script_directory()) / path;
    return path_utf8(path);
}

bool Recorder::run(const RecordOptions& options) {
    rows_.store(0, std::memory_order_relaxed);
    const auto note = [this](const Severity severity, const std::string& text) {
        if (sink_ != nullptr)
            sink_->message(severity, text);
    };

    // A stop() that arrived before run() still counts.
    if (stopping_.load(std::memory_order_relaxed))
        return false;

    const std::filesystem::path path = utf8_path(options.output_path);
    const std::string shown = path_utf8(path.filename());
    std::error_code code;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        note(Severity::error, "Cannot create " + options.output_path + ".");
        return false;
    }
    output << "# recorded by aoahid-player from `adb shell getevent -lt`\n"
              "# every row is lowercase, so the whole script repeats each loop\n";

    // The rows keep the touch panel's raw coordinates, which are not always
    // screen pixels; recording the panel's range lets playback scale them to
    // whatever touchscreen resolution is connected.
    {
        const CommandResult ranges = run_command(
            adb_command(options.adb_serial, {"shell", "getevent", "-lp"}), 8000);
        int32_t width = 0;
        int32_t height = 0;
        if (ranges.started && !ranges.timed_out && ranges.exit_code == 0 &&
            record::parse_touch_range(ranges.output, options.input_device, width, height)) {
            output << "# screen " << width << 'x' << height << '\n';
            note(Severity::info, "Touch panel range " + std::to_string(width) + "x" +
                                     std::to_string(height) +
                                     "; playback scales it to the connected touchscreen.");
        }
    }

    ChildProcess adb;
    std::string error;
    if (!adb.start(adb_command(options.adb_serial, {"shell", "getevent", "-lt"}), error)) {
        output.close();
        std::filesystem::remove(path, code);
        note(Severity::error, "Recording could not start: " + error +
                                  ". Install Android SDK Platform-Tools and add it to PATH.");
        return false;
    }

    started_ns_.store(Timing::now_ns(), std::memory_order_relaxed);
    active_.store(true, std::memory_order_relaxed);
    note(Severity::info, "Recording into " + shown + ". Use the phone, then stop.");

    record::GeteventParser parser;
    if (!options.input_device.empty())
        parser.set_device_filter(options.input_device);

    std::vector<EventRecord> rows;
    std::string text;
    std::string line;
    int64_t last_flush = Timing::now_ns();
    bool pending_flush = false;
    bool adb_ended = false;
    const auto write = [&] {
        if (rows.empty())
            return;
        text.clear();
        append_rows(text, rows);
        output << text;
        if (sink_ != nullptr && !text.empty())
            sink_->recorded_row(text);
        rows_.store(rows_.load(std::memory_order_relaxed) + rows.size(),
                    std::memory_order_relaxed);
        pending_flush = true;
    };

    while (!stopping_.load(std::memory_order_relaxed)) {
        const ChildProcess::Read read = adb.read_line(line, read_slice_ms);
        if (read == ChildProcess::Read::ended) {
            adb_ended = true;
            break;
        }
        if (read == ChildProcess::Read::line) {
            rows.clear();
            parser.feed(line, Timing::now_ns(), rows);
            write();
        }
        // Written through, so a crash or a pulled cable loses little.
        const int64_t now = Timing::now_ns();
        if (pending_flush && now - last_flush >= flush_interval_ns) {
            output.flush();
            last_flush = now;
            pending_flush = false;
        }
    }
    adb.finish(true);
    active_.store(false, std::memory_order_relaxed);

    rows.clear();
    parser.finish(rows);
    write();
    output.flush();
    const bool written = static_cast<bool>(output);
    output.close();

    const uint64_t total = rows_.load(std::memory_order_relaxed);
    const std::string adb_reason = first_complaint(adb.error_output());
    if (!written) {
        note(Severity::error, "Writing " + options.output_path + " failed.");
        return false;
    }
    if (total == 0) {
        std::filesystem::remove(path, code);
        if (adb_ended)
            note(Severity::error, "adb stopped before anything was recorded" +
                                      (adb_reason.empty() ? std::string()
                                                          : " (" + adb_reason + ")") +
                                      ". Check that USB debugging is on and this computer is "
                                      "authorised.");
        else
            note(Severity::warning,
                 "Nothing was captured, so no file was saved. Touch the screen while recording.");
        return false;
    }
    if (adb_ended)
        note(Severity::warning, "adb stopped unexpectedly" +
                                    (adb_reason.empty() ? std::string() : " (" + adb_reason + ")") +
                                    "; the rows captured so far were kept.");
    note(Severity::info, "Saved " + std::to_string(total) + " rows to " + shown + ".");
    if (parser.unmapped_keys() != 0) {
        note(Severity::warning, "Skipped " + std::to_string(parser.unmapped_keys()) +
                                    " key events with no keyboard equivalent (first: " +
                                    parser.first_unmapped_key() + ").");
    }
    return true;
}

} // namespace aoap
