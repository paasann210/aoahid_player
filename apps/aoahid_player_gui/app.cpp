// SPDX-License-Identifier: MIT
#include "app.hpp"

#include "app_common.hpp"
#include "keymap.hpp"
#include "theme.hpp"

#include "aoahid_player/device.hpp"
#include "aoahid_player/paths.hpp"
#include "aoahid_player/timing.hpp"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string_view>

#ifndef AOAHID_PLAYER_VERSION
#define AOAHID_PLAYER_VERSION "dev"
#endif

namespace gui {
using namespace detail;


App::App(std::function<void()> wake)
    : wake_(std::move(wake)), log_(wake_), engine_(log_, wake_) {
    settings_path_ = settings_path();
    apply_settings(load_settings(settings_path_));
    saved_settings_ = current_settings();
    engine_.player().set_speed(speed_);
    engine_.player().set_loop_limit(loop_limit_);
    refresh_scripts();
    refresh_playlists();
    if (!scripts_.empty())
        load_script(scripts_.front().path);
    log_.message(aoap::Severity::info,
                 "Scripts folder: " + aoap::script_directory() + ". Searching for devices...");
    engine_.refresh();
}

App::~App() {
    live_capture_pointer(false);
    persist_settings();
    stop_recording();
    if (record_thread_.joinable())
        record_thread_.join();
}

bool App::recording() const { return record_thread_.joinable(); }

bool App::settings_locked() const { return engine_.phase() != Phase::idle; }

bool App::animating() const {
    const Phase phase = engine_.phase();
    if (engine_.busy() || recording() || connect_prep_task_.valid() || retry_task_.valid() ||
        adb_task_.valid())
        return true;
    if (phase == Phase::playing)
        return engine_.player().status().state != aoap::PlaybackState::paused ||
               engine_.playlist_progress().ends_at_ns != 0;
    // Live control polls the gamepad and shows what the phone is doing.
    return phase == Phase::live;
}

std::string App::adb_serial() const {
    if (adb_choice_ > 0 && static_cast<size_t>(adb_choice_) <= adb_devices_.size())
        return adb_devices_[static_cast<size_t>(adb_choice_) - 1].serial;
    return {};
}

Settings App::current_settings() const {
    Settings settings;
    settings.use_touch = use_touch_;
    settings.touch_width = touch_width_;
    settings.touch_height = touch_height_;
    settings.touch_contacts = touch_contacts_;
    settings.use_mouse = use_mouse_;
    settings.mouse_buttons = mouse_buttons_;
    settings.use_key = use_key_;
    settings.key_min = key_min_;
    settings.key_max = key_max_;
    settings.use_gamepad = use_gamepad_;
    settings.pad_buttons = pad_buttons_;
    settings.pad_bits = pad_bits_;
    settings.pad_dpad = pad_dpad_;
    settings.pad_axes = pad_axes_;
    settings.use_pen = use_pen_;
    settings.pen_mode = pen_mode_;
    settings.live_release_key = live_release_key_;
    return settings;
}

void App::apply_settings(const Settings& settings) {
    use_touch_ = settings.use_touch;
    touch_width_ = settings.touch_width;
    touch_height_ = settings.touch_height;
    touch_contacts_ = settings.touch_contacts;
    use_mouse_ = settings.use_mouse;
    mouse_buttons_ = settings.mouse_buttons;
    use_key_ = settings.use_key;
    key_min_ = settings.key_min;
    key_max_ = settings.key_max;
    use_gamepad_ = settings.use_gamepad;
    pad_buttons_ = settings.pad_buttons;
    pad_bits_ = settings.pad_bits;
    pad_dpad_ = settings.pad_dpad;
    pad_axes_ = settings.pad_axes;
    use_pen_ = settings.use_pen;
    pen_mode_ = settings.pen_mode;
    live_release_key_ = settings.live_release_key;
}

void App::persist_settings() {
    Settings current = current_settings();
    if (current == saved_settings_)
        return;
    std::string error;
    if (!save_settings(settings_path_, current, error) && !settings_warned_) {
        // Said once; a read-only folder should not flood the log.
        settings_warned_ = true;
        log_.message(aoap::Severity::warning,
                     "Settings cannot be saved (" + error + "); changes last until you close "
                     "the app.");
    }
    saved_settings_ = std::move(current);
}

aoap::ProfileSetup App::build_setup() const {
    aoap::ProfileSetup setup;
    setup.touch = {use_touch_, touch_width_, touch_height_, static_cast<uint32_t>(touch_contacts_)};
    setup.mouse = {use_mouse_, static_cast<uint32_t>(mouse_buttons_)};
    setup.key = {use_key_, static_cast<uint16_t>(key_min_), static_cast<uint16_t>(key_max_)};
    setup.gamepad.enabled = use_gamepad_;
    setup.gamepad.buttons = static_cast<uint32_t>(pad_buttons_);
    setup.gamepad.axis_bits = static_cast<uint32_t>(pad_bits_);
    setup.gamepad.dpad = pad_dpad_ == 0   ? AOAHID_DPAD_NONE
                         : pad_dpad_ == 1 ? AOAHID_DPAD_HAT
                                          : AOAHID_DPAD_BUTTONS;
    setup.gamepad.axes = pad_axes_;
    setup.pen.enabled = use_pen_;
    setup.pen.mode = pen_mode_ == 0 ? AOAHID_PEN_DIRECT_SCREEN : AOAHID_PEN_INDIRECT_TABLET;
    aoap::resolve_pen_surface(setup);
    return setup;
}

// --- Actions ---------------------------------------------------------------

void App::poll() {
    const uint64_t version = engine_.devices_version();
    if (version != devices_version_) {
        devices_version_ = version;
        devices_ = engine_.devices();
        // With exactly one phone there is nothing to choose.
        if (devices_.size() == 1 && selected_.empty())
            selected_.insert(devices_.front().key);
    }

    engine_.set_observing(tab_ == Tab::live);
    const Phase phase = engine_.phase();
    if (phase != last_phase_) {
        if (last_phase_ == Phase::playing)
            cursor_ = {};
        if (phase == Phase::connected && last_phase_ == Phase::connecting)
            connect_error_.clear();
        if (phase == Phase::idle && last_phase_ == Phase::refreshing && awaiting_connect_rescan_) {
            awaiting_connect_rescan_ = false;
            connect();
        }
        last_phase_ = phase;
    }

    if (ready(connect_prep_task_)) {
        const ConnectPrep result = connect_prep_task_.get();
        if (!result.skipped) {
            set_adb_status(result.status);
            if (result.size_ok) {
                touch_width_ = result.width;
                touch_height_ = result.height;
                log_.message(aoap::Severity::info, "Screen size from adb: " +
                                                       std::to_string(result.width) + " x " +
                                                       std::to_string(result.height) + ".");
            } else {
                log_.message(aoap::Severity::warning,
                             "Screen size not detected. " + result.size_error);
            }
        }
        awaiting_connect_rescan_ = true;
        engine_.refresh();
    }
    if (ready(retry_task_)) {
        const RetryPrep result = retry_task_.get();
        if (!result.skipped)
            set_adb_status(result.status);
        engine_.refresh();
    }
    if (ready(adb_task_)) {
        AdbList result = adb_task_.get();
        const std::string previous = adb_serial();
        adb_devices_ = std::move(result.devices);
        adb_choice_ = 0;
        for (size_t index = 0; index < adb_devices_.size(); ++index) {
            if (adb_devices_[index].serial == previous)
                adb_choice_ = static_cast<int>(index) + 1;
        }
        if (result.ok)
            set_adb_status(AdbStatus::running);
        else {
            log_.message(aoap::Severity::warning, result.error);
            if (aoap::adb_missing(result.error))
                set_adb_status(AdbStatus::not_found);
        }
    }
    if (record_done_.load(std::memory_order_acquire))
        finish_recording();
}

void App::refresh_scripts() {
    scripts_.clear();
    for (std::string& path : aoap::discover_csv_files(aoap::script_directory())) {
        ScriptFile file;
        file.name = aoap::display_name(path);
        file.lower = fold(file.name);
        file.meta = file_meta(path);
        file.path = std::move(path);
        scripts_.push_back(std::move(file));
    }
}

void App::delete_script(const std::string& path) {
    std::error_code code;
    const std::string name = aoap::display_name(path);
    if (!std::filesystem::remove(aoap::utf8_path(path), code) || code) {
        log_.message(aoap::Severity::error, "Could not delete " + name + ".csv" +
                                                (code ? ": " + code.message() : std::string()) +
                                                ".");
        refresh_scripts();
        return;
    }
    log_.message(aoap::Severity::info, "Deleted " + name + ".csv.");
    if (path == script_path_) {
        script_.reset();
        script_path_.clear();
        timeline_ = {};
        cursor_ = {};
    }
    if (path == last_record_path_)
        last_record_path_.clear();
    refresh_scripts();
}

void App::load_script(const std::string& path) {
    auto script = std::make_shared<aoap::EventScript>();
    std::string error;
    if (!script->load(path, error)) {
        script_error_ = error;
        log_.message(aoap::Severity::error, error);
        return;
    }
    timeline_ = aoap::build_timeline(*script);
    script_ = std::move(script);
    script_path_ = path;
    path_input_ = path;
    script_error_.clear();
    cursor_ = {};
}

void App::begin_connect() {
    if (connect_prep_task_.valid() || retry_task_.valid() || engine_.phase() != Phase::idle)
        return;
    connect_error_.clear();
    const bool skip_adb = recording();
    if (skip_adb)
        log_.message(aoap::Severity::info,
                     "The adb server is left running because a recording is in progress.");
    connect_prep_task_ =
        std::async(std::launch::async, [serial = adb_serial(), skip_adb, wake = wake_] {
            ConnectPrep result;
            if (skip_adb) {
                result.skipped = true;
                wake();
                return result;
            }
            std::string error;
            if (aoap::adb_start_server(error))
                result.status = AdbStatus::running;
            else
                result.status = aoap::adb_missing(error) ? AdbStatus::not_found
                                                          : AdbStatus::unknown;
            result.size_ok =
                aoap::adb_screen_size(serial, result.width, result.height, result.size_error);
            std::string stop_error;
            if (aoap::adb_kill_server(stop_error)) {
                if (result.status != AdbStatus::not_found)
                    result.status = AdbStatus::stopped;
                // Gives the OS a moment to actually release the USB
                // interface before the AOA handshake reaches for it.
                aoap::Timing::sleep_ms(1200);
            } else if (aoap::adb_missing(stop_error)) {
                result.status = AdbStatus::not_found;
            }
            wake();
            return result;
        });
}

void App::connect() {
    std::vector<size_t> selection;
    for (size_t index = 0; index < devices_.size(); ++index) {
        if (selected_.count(devices_[index].key) != 0)
            selection.push_back(index);
    }
    if (selection.empty()) {
        connect_error_ = devices_.empty() ? "No device found. Plug in a phone and refresh."
                                          : "Select at least one device.";
        return;
    }
    const aoap::ProfileSetup setup = build_setup();
    std::string error;
    if (!aoap::validate_setup(setup, error)) {
        connect_error_ = error;
        return;
    }
    engine_.connect(std::move(selection), setup);
}

void App::retry() {
    if (connect_prep_task_.valid() || retry_task_.valid() || engine_.phase() != Phase::idle)
        return;
    connect_error_.clear();
    const bool skip_adb = recording();
    if (skip_adb)
        log_.message(aoap::Severity::info,
                     "The adb server is left running because a recording is in progress.");
    retry_task_ = std::async(std::launch::async, [skip_adb, wake = wake_] {
        RetryPrep result;
        if (!skip_adb) {
            std::string error;
            if (aoap::adb_kill_server(error))
                result.status = AdbStatus::stopped;
            else
                result.status =
                    aoap::adb_missing(error) ? AdbStatus::not_found : AdbStatus::unknown;
        } else {
            result.skipped = true;
        }
        wake();
        return result;
    });
}

void App::set_adb_status(const AdbStatus status) {
    if (status == adb_status_)
        return;
    adb_status_ = status;
    switch (status) {
    case AdbStatus::not_found:
        log_.message(aoap::Severity::warning, "adb not found on PATH.");
        break;
    case AdbStatus::running:
        log_.message(aoap::Severity::info, "adb server running.");
        break;
    case AdbStatus::stopped:
        log_.message(aoap::Severity::info, "adb server stopped.");
        break;
    case AdbStatus::unknown:
        break;
    }
}

void App::toggle_playback() {
    const Phase phase = engine_.phase();
    if (phase == Phase::connected) {
        if (script_)
            engine_.play(script_, aoap::display_name(script_path_), cursor_, loop_limit_);
        return;
    }
    if (phase != Phase::playing)
        return;
    const aoap::PlaybackState state = engine_.player().status().state;
    if (state == aoap::PlaybackState::paused)
        engine_.player().resume();
    else if (state == aoap::PlaybackState::playing)
        engine_.player().pause();
}

bool App::playlist_playable() const {
    if (playlist_.entries.empty())
        return false;
    for (const Playlist::Entry& entry : playlist_.entries) {
        const std::string path = resolve_script_reference(entry.script);
        std::error_code code;
        if (!std::filesystem::exists(aoap::utf8_path(path), code))
            return false;
    }
    return true;
}

void App::refresh_playlists() { playlist_names_ = list_playlists(); }

void App::load_playlist_named(const std::string& name) {
    Playlist loaded;
    std::string error;
    if (!load_playlist(name, loaded, error)) {
        playlist_error_ = error;
        log_.message(aoap::Severity::error, error);
        refresh_playlists();
        return;
    }
    playlist_ = std::move(loaded);
    playlist_name_input_ = playlist_.name;
    playlist_dirty_ = false;
    playlist_error_.clear();
}

void App::save_current_playlist() {
    Playlist saving = playlist_;
    saving.name = playlist_name_input_;
    const std::string problem = record_name_problem(saving.name);
    if (saving.name.empty()) {
        playlist_error_ = "Give the playlist a name before saving.";
        return;
    }
    if (!problem.empty()) {
        playlist_error_ = problem;
        return;
    }
    std::string error;
    if (!save_playlist(saving, error)) {
        playlist_error_ = error;
        log_.message(aoap::Severity::error, error);
        return;
    }
    playlist_ = std::move(saving);
    playlist_dirty_ = false;
    playlist_error_.clear();
    refresh_playlists();
    log_.message(aoap::Severity::info, "Saved the playlist \"" + playlist_.name + "\".");
}

void App::play_playlist() {
    std::vector<PlaylistStep> steps;
    steps.reserve(playlist_.entries.size());
    for (const Playlist::Entry& entry : playlist_.entries) {
        const std::string path = resolve_script_reference(entry.script);
        auto script = std::make_shared<aoap::EventScript>();
        std::string error;
        if (!script->load(path, error)) {
            playlist_error_ = error;
            log_.message(aoap::Severity::error, error);
            return;
        }
        steps.push_back(PlaylistStep{std::move(script), aoap::display_name(path), entry.loops});
    }
    playlist_error_.clear();
    const int64_t limit = static_cast<int64_t>(playlist_.time_limit_minutes) * 60 *
                          1'000'000'000LL;
    engine_.play_playlist(std::move(steps), limit);
}

void App::stop_playback() {
    engine_.stop();
    cursor_ = {};
}

void App::seek(const aoap::PlaybackPosition target) {
    if (engine_.phase() == Phase::playing &&
        engine_.player().status().state != aoap::PlaybackState::stopped)
        engine_.player().seek(target);
    else
        cursor_ = target;
}

void App::refresh_adb_devices() {
    if (adb_task_.valid())
        return;
    adb_task_ = std::async(std::launch::async, [wake = wake_] {
        AdbList result;
        result.ok = aoap::adb_list_devices(result.devices, result.error);
        wake();
        return result;
    });
}

void App::start_recording() {
    if (recording())
        return;
    if (!record_name_problem(record_name_).empty())
        return;
    aoap::RecordOptions options;
    options.output_path = aoap::resolve_record_path(
        record_name_.empty() ? std::string() : record_file_name(record_name_));
    options.adb_serial = adb_serial();
    options.input_device = record_input_;
    record_path_ = options.output_path;
    record_saved_ = false;
    record_done_.store(false, std::memory_order_relaxed);
    recorder_ = std::make_unique<aoap::Recorder>(&log_);
    record_thread_ = std::thread([this, options] {
        record_saved_ = recorder_->run(options);
        record_done_.store(true, std::memory_order_release);
        wake_();
    });
}

void App::stop_recording() {
    if (recorder_)
        recorder_->stop();
}

void App::finish_recording() {
    if (record_thread_.joinable())
        record_thread_.join();
    record_done_.store(false, std::memory_order_relaxed);
    if (recorder_ && record_saved_) {
        last_record_path_ = record_path_;
        last_record_rows_ = recorder_->rows();
        record_name_.clear();
        refresh_scripts();
    }
    recorder_.reset();
}

void App::on_drop(std::vector<std::string> paths) {
    for (const std::string& path : paths) {
        if (has_csv_extension(path)) {
            if (engine_.phase() == Phase::playing) {
                log_.message(aoap::Severity::warning,
                             "Stop playback before loading another script.");
                return;
            }
            tab_ = Tab::player;
            load_script(path);
            return;
        }
    }
    if (tab_ == Tab::live) {
        for (const std::string& path : paths) {
            if (has_image_extension(path)) {
                const std::string error = live_image_.load(path);
                if (!error.empty())
                    log_.message(aoap::Severity::warning, error);
                return;
            }
        }
        log_.message(aoap::Severity::warning,
                     "Drop a .csv script, or a .png/.jpg/.bmp image for the reference "
                     "overlay, here.");
        return;
    }
    log_.message(aoap::Severity::warning, "Only .csv scripts can be dropped here.");
}

void App::on_focus() {
    refresh_scripts();
    refresh_playlists();
}

void App::on_focus_lost() { live_capture_pointer(false); }

void App::on_key(const int glfw_key, const int scancode, const bool pressed) {
    if (live_release_key_picking_) {
        // Capturing the next press for "Set release key"; nothing else
        // touches this key press. Escape cancels back to the default instead
        // of becoming the release key itself, since that would make Escape
        // unreachable as a way to back out of the picker.
        if (pressed) {
            live_release_key_picking_ = false;
            live_release_key_ = glfw_key == GLFW_KEY_ESCAPE ? 0 : glfw_key;
        }
        return;
    }
    // The release key lets go of the captured pointer no matter which
    // profiles are forwarding, so it works in mouse-only mode too. Like Esc
    // used to, it is still forwarded to the phone afterwards if Keyboard is
    // also on — releasing the capture does not consume the press.
    if (pressed && live_mouse_captured_ && engine_.phase() == Engine::Phase::live &&
        is_release_key(glfw_key, live_release_key_))
        live_capture_pointer(false);

    if (!live_.key || engine_.phase() != Engine::Phase::live)
        return;
    if (is_release_key(glfw_key, live_release_key_) || ImGui::GetIO().WantTextInput)
        return;
    // GLFW has no key constant for the keys a JIS (Japanese) keyboard adds
    // over a US layout — Henkan, Muhenkan, Kana, Zenkaku/Hankaku, Ro — so
    // they decode to GLFW_KEY_UNKNOWN and hid_usage_from_glfw() cannot see
    // them. This is not an Android-side limitation: the key never left this
    // app. The platform scancode still identifies them on Linux.
    const uint16_t usage = hid_usage_from_glfw(glfw_key);
    const uint16_t resolved = usage != 0 ? usage : hid_usage_from_scancode(scancode);
    if (resolved == 0)
        return;
    if (!pressed) {
        // Always deliver key-ups, even off the Live tab (live_keyboard() only
        // drains the queue while that tab is drawn): otherwise a key held
        // while switching tabs stays stuck down on the phone until Live
        // control is turned off entirely.
        engine_.live_send(aoap::KeyEvent{resolved, false});
        return;
    }
    if (tab_ != Tab::live)
        return; // new presses only start while the Live tab is open
    pressed_keys_.push_back(resolved);
}

void App::on_cursor(const double x, const double y) {
    // Only needed while the pointer is captured (see live_pointer() and
    // frame()); otherwise ImGui's own io.MouseDelta is used as before, so
    // this stays a no-op the rest of the time and adds no per-frame cost.
    if (!live_mouse_captured_) {
        live_raw_cursor_valid_ = false;
        return;
    }
    if (live_raw_cursor_valid_) {
        live_raw_delta_x_ += x - live_raw_cursor_x_;
        live_raw_delta_y_ += y - live_raw_cursor_y_;
    }
    live_raw_cursor_x_ = x;
    live_raw_cursor_y_ = y;
    live_raw_cursor_valid_ = true;
}

void App::on_search_shortcut() {
    if (engine_.phase() == Phase::playing)
        return;
    tab_ = Tab::player;
    open_picker_ = true;
}

// --- Frame -----------------------------------------------------------------

void App::frame() {
    poll();

    ImGuiIO& io = ImGui::GetIO();
    // Mouse mode's captured pointer is invisible and moves without limit
    // (see live_capture_pointer()), so io.MousePos would otherwise
    // eventually drift onto some other button in this window and click it —
    // the whole point of the capture is that the pointer belongs to the
    // preview alone. Pinning it off-screen (ImGui's own "no mouse" sentinel)
    // makes every button and tab in this window correctly see no hover and
    // no click for as long as the capture lasts; live_pointer() tracks the
    // actual motion itself instead, from on_cursor()'s raw deltas.
    if (live_mouse_captured_) {
        constexpr ImVec2 off_screen(-FLT_MAX, -FLT_MAX);
        io.MousePos = off_screen;
        io.MousePosPrev = off_screen;
    }
    const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (tab_ == Tab::player && !io.WantTextInput && !popup_open &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
        toggle_playback();
    const bool forwarding_keys = engine_.phase() == Phase::live && live_.key;
    // The pointer capture's own release key is handled in on_key(), straight
    // from the window system, so it works whichever key is configured and
    // whether or not Keyboard forwarding is on. Full screen still uses plain
    // Esc regardless of the configured release key — it is a window chrome
    // shortcut, not part of live control.
    if (live_fullscreen_ && !forwarding_keys && ui::escape_pressed())
        live_fullscreen_ = false;
    // Live control owns the keyboard while it is forwarding keys.
    if (forwarding_keys)
        io.ClearInputKeys();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(16)));
    constexpr ImGuiWindowFlags root_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##root", nullptr, root_flags);
    ImGui::PopStyleVar();

    if (live_fullscreen_) {
        // Everything but the preview and its switches is out of the way.
        draw_live_fullscreen();
        if (!ImGui::IsAnyItemActive())
            persist_settings();
        ImGui::End();
        return;
    }

    draw_header();
    gap(4);

    const float log_height = log_open_ ? px(136) : ImGui::GetFrameHeight() + px(30);
    const float body = std::max(ImGui::GetContentRegionAvail().y - log_height -
                                    ImGui::GetStyle().ItemSpacing.y - px(4),
                                px(120));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##sidebar", ImVec2(px(392), body), ImGuiChildFlags_None);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine(0, px(16));
    ImGui::BeginChild("##main", ImVec2(0, body), ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    {
        static const char* const tabs[] = {"Player", "Live", "Playlist", "Recorder"};
        int current = static_cast<int>(tab_);
        if (ui::tabs("##tabs", tabs, 4, &current)) {
            tab_ = static_cast<Tab>(current);
            if (tab_ != Tab::live)
                live_capture_pointer(false);
        }
        gap(4);
        if (tab_ == Tab::player)
            draw_player();
        else if (tab_ == Tab::live)
            draw_live();
        else if (tab_ == Tab::playlist)
            draw_playlist();
        else
            draw_recorder();
    }
    ImGui::EndChild();

    gap(4);
    draw_log(log_height);

    // Saved once an edit is finished, not on every keystroke or drag step.
    if (!ImGui::IsAnyItemActive())
        persist_settings();
    ImGui::End();
}

void App::draw_header() {
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float size = px(30);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    list->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), theme::accent, px(7));
    ui::draw_icon(list, ui::Icon::play, ImVec2(p0.x + size * 0.53f, p0.y + size * 0.5f),
                  size * 0.5f, IM_COL32_WHITE);
    ImGui::Dummy(ImVec2(size, size));

    ImGui::SameLine(0, px(12));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, theme::font_title);
    ImGui::TextUnformatted("AOA HID Player");
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_faint);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(5));
    ImGui::TextUnformatted("Android Open Accessory input player  ·  v" AOAHID_PLAYER_VERSION);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();

    // Status pills, right-aligned.
    const Phase phase = engine_.phase();
    std::string connection;
    ImU32 connection_dot = theme::text_faint;
    bool connection_pulse = false;
    switch (phase) {
    case Phase::idle:
        connection = "Not connected";
        break;
    case Phase::refreshing:
        connection = "Searching";
        connection_dot = theme::accent;
        connection_pulse = true;
        break;
    case Phase::connecting:
        connection = "Connecting";
        connection_dot = theme::accent;
        connection_pulse = true;
        break;
    case Phase::connected:
    case Phase::playing:
    case Phase::live: {
        const size_t count = engine_.device_status().size();
        connection = std::to_string(count) + (count == 1 ? " device" : " devices") + " connected";
        connection_dot = theme::success;
        break;
    }
    case Phase::disconnecting:
        connection = "Disconnecting";
        connection_dot = theme::warning;
        connection_pulse = true;
        break;
    }
    const aoap::PlaybackState state =
        phase == Phase::playing ? engine_.player().status().state : aoap::PlaybackState::stopped;
    const char* playback = state == aoap::PlaybackState::playing  ? "Playing"
                           : state == aoap::PlaybackState::paused ? "Paused"
                                                                  : "Stopped";
    const ImU32 playback_dot = state == aoap::PlaybackState::playing  ? theme::accent
                               : state == aoap::PlaybackState::paused ? theme::warning
                                                                      : theme::text_faint;
    const char* adb_label = adb_status_ == AdbStatus::running    ? "adb: running"
                            : adb_status_ == AdbStatus::stopped   ? "adb: stopped"
                            : adb_status_ == AdbStatus::not_found ? "adb: not found"
                                                                  : "adb: unknown";
    const ImU32 adb_dot = adb_status_ == AdbStatus::running    ? theme::success
                         : adb_status_ == AdbStatus::stopped   ? theme::text_faint
                         : adb_status_ == AdbStatus::not_found ? theme::danger
                                                                : theme::warning;
    const float spacing = px(8);
    float width = ui::pill_width(adb_label) + spacing + ui::pill_width(connection.c_str()) +
                  spacing + ui::pill_width(playback);
    if (recording())
        width += spacing + ui::pill_width("Recording");

    const float pill_height =
        ImGui::GetFontSize() * (theme::font_small / theme::font_body) + px(12);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size - pill_height) * 0.5f);
    ui::align_right(width);
    ui::pill(adb_label, adb_dot);
    ImGui::SetItemTooltip("Whether the adb server is running. Connect and Retry manage it "
                          "automatically.");
    ImGui::SameLine(0, spacing);
    if (recording()) {
        ui::pill("Recording", theme::danger, true);
        ImGui::SameLine(0, spacing);
    }
    ui::pill(connection.c_str(), connection_dot, connection_pulse);
    ImGui::SameLine(0, spacing);
    ui::pill(playback, playback_dot);
}

// --- Sidebar ---------------------------------------------------------------

void App::draw_sidebar() {
    // The connect card stays pinned below; its height is last frame's.
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float scroll = std::max(ImGui::GetContentRegionAvail().y - connect_height_ - spacing,
                                  px(80));
    ImGui::BeginChild("##setup", ImVec2(0, scroll), ImGuiChildFlags_None);
    draw_devices_card();
    gap(2);
    draw_profiles_card();
    ImGui::EndChild();
    draw_connect_card();
    connect_height_ = ImGui::GetItemRectSize().y;
}

void App::draw_devices_card() {
    ui::begin_card("##devices");
    caption_row("Devices");
    const Phase phase = engine_.phase();
    const float button = ImGui::GetFrameHeight();
    ui::align_right(button);
    if (phase == Phase::refreshing || retry_task_.valid()) {
        ui::spinner(button * 0.4f, theme::accent);
    } else {
        ImGui::BeginDisabled(phase != Phase::idle || connect_prep_task_.valid());
        if (ui::icon_button("##refresh_devices", ui::Icon::refresh, button, ui::Tone::quiet,
                            "Kill the adb server and search for AOA-capable devices again"))
            retry();
        ImGui::EndDisabled();
    }
    gap(2);

    if (devices_.empty()) {
        small_dim(phase == Phase::refreshing
                      ? "Searching..."
                      : "No device found. Connect a phone with a data cable and press refresh.");
    }
    const bool locked = settings_locked();
    const std::vector<aoap::DeviceStatus> status =
        engine_.connected() ? engine_.device_status() : std::vector<aoap::DeviceStatus>{};
    for (const aoap::DeviceEntry& device : devices_) {
        ImGui::PushID(device.key.c_str());
        bool selected = selected_.count(device.key) != 0;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float row = ImGui::GetTextLineHeight() * 2.4f + px(8);
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginDisabled(locked);
        if (ImGui::InvisibleButton("##row", ImVec2(width, row))) {
            if (selected)
                selected_.erase(device.key);
            else
                selected_.insert(device.key);
            selected = !selected;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();

        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + width, p0.y + row);
        list->AddRectFilled(p0, p1,
                            ImGui::GetColorU32(selected             ? theme::accent_soft
                                               : hovered && !locked ? theme::field
                                                                    : theme::surface_hi),
                            px(8));
        if (selected)
            list->AddRect(p0, p1, ImGui::GetColorU32(theme::accent_line), px(8));

        // Check box.
        const float box = px(18);
        const ImVec2 b0(p0.x + px(12), p0.y + (row - box) * 0.5f);
        const ImVec2 b1(b0.x + box, b0.y + box);
        ui::draw_check(list, b0, box, selected);

        const float text_x = b1.x + px(12);
        const float line = ImGui::GetTextLineHeight();
        list->AddText(ImVec2(text_x, p0.y + row * 0.5f - line - px(1)),
                      ImGui::GetColorU32(theme::text), device.product.c_str());
        char detail[160];
        std::snprintf(detail, sizeof detail, "%04x:%04x%s%s", device.vendor_id, device.product_id,
                      device.serial.empty() ? "" : "  ·  ", device.serial.c_str());
        list->AddText(ImVec2(text_x, p0.y + row * 0.5f + px(1)),
                      ImGui::GetColorU32(theme::text_faint), detail);

        // Live state of a connected device: a dot, the report count, and the
        // last failure as a tooltip.
        for (const aoap::DeviceStatus& entry : status) {
            if (entry.label != device.label)
                continue;
            const ImU32 dot = entry.active ? theme::success : theme::danger;
            const ImVec2 c(p1.x - px(16), p0.y + row * 0.5f - line * 0.5f);
            list->AddCircleFilled(c, px(4), ImGui::GetColorU32(dot));
            const std::string counts =
                entry.active ? format_count(entry.reports) + " reports" : std::string("dropped");
            ImGui::PushFont(nullptr, theme::font_small);
            const ImVec2 size = ImGui::CalcTextSize(counts.c_str());
            list->AddText(ImVec2(p1.x - px(12) - size.x, p0.y + row * 0.5f + px(2)),
                          ImGui::GetColorU32(entry.active ? theme::text_faint : theme::danger),
                          counts.c_str());
            ImGui::PopFont();
            if (!entry.last_error.empty() &&
                ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered())
                ImGui::SetTooltip("%s", entry.last_error.c_str());
        }
        ImGui::PopID();
    }
    ui::end_card();
}

void App::draw_profiles_card() {
    ui::begin_card("##profiles");
    caption_row("Profiles");
    const bool locked = settings_locked();
    if (locked) {
        const char* note = "Locked while connected";
        ImGui::PushFont(nullptr, theme::font_small);
        const float width = ImGui::CalcTextSize(note).x;
        ImGui::PopFont();
        ui::align_right(width);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(nullptr, theme::font_small);
        ImGui::TextColored(theme::vec(theme::warning), "%s", note);
        ImGui::PopFont();
    }
    gap(2);

    ImGui::BeginDisabled(locked);
    struct Entry {
        const char* label;
        bool* enabled;
        void (App::*settings)();
        std::string summary;
    };
    char touch_summary[48];
    std::snprintf(touch_summary, sizeof touch_summary, "%d x %d", touch_width_, touch_height_);
    char key_summary[32];
    std::snprintf(key_summary, sizeof key_summary, "0x%02X-0x%02X", key_min_, key_max_);
    const Entry entries[] = {
        {"Touchscreen##touch", &use_touch_, &App::draw_touch_settings, touch_summary},
        {"Mouse##mouse", &use_mouse_, &App::draw_mouse_settings,
         std::to_string(mouse_buttons_) + " buttons"},
        {"Keyboard##key", &use_key_, &App::draw_key_settings, key_summary},
        {"Gamepad##gamepad", &use_gamepad_, &App::draw_gamepad_settings,
         std::to_string(pad_buttons_) + " buttons, " + std::to_string(pad_axes_.size()) + " axes"},
        {"Pen##pen", &use_pen_, &App::draw_pen_settings,
         pen_mode_ == 0 ? "on screen" : "tablet"},
    };
    bool first = true;
    for (const Entry& entry : entries) {
        if (!first)
            thin_rule(0.0f, 0.0f);
        first = false;
        if (ui::toggle(entry.label, entry.enabled))
            connect_error_.clear();
        if (!*entry.enabled || locked) {
            ImGui::PushFont(nullptr, theme::font_small);
            const float width = ImGui::CalcTextSize(entry.summary.c_str()).x;
            ImGui::PopFont();
            ui::align_right(width);
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(nullptr, theme::font_small);
            ImGui::TextColored(theme::vec(*entry.enabled ? theme::text_dim : theme::text_faint),
                               "%s",
                               entry.summary.c_str());
            ImGui::PopFont();
        }
        if (*entry.enabled && !locked) {
            gap(2);
            ImGui::Indent(px(4));
            ImGui::PushID(entry.label);
            (this->*entry.settings)();
            ImGui::PopID();
            ImGui::Unindent(px(4));
        }
    }
    ImGui::EndDisabled();
    ui::end_card();
}

void App::draw_touch_settings() {
    field("Resolution");
    const float number = px(66);
    ImGui::SetNextItemWidth(number);
    if (ImGui::InputInt("##width", &touch_width_, 0, 0))
        touch_width_ = std::clamp(touch_width_, 1, 65536);
    ImGui::SameLine(0, px(6));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("x");
    ImGui::SameLine(0, px(6));
    ImGui::SetNextItemWidth(number);
    if (ImGui::InputInt("##height", &touch_height_, 0, 0))
        touch_height_ = std::clamp(touch_height_, 1, 65536);
    ImGui::SetItemTooltip("Connect reads this with adb (wm size) automatically. An override "
                          "size wins; type here to set it by hand instead.");

    field("Contacts");
    ui::slider_int("##contacts", &touch_contacts_, 1, 16, " fingers");
}

void App::draw_mouse_settings() {
    field("Buttons");
    ui::slider_int("##buttons", &mouse_buttons_, 1, 16, "");
}

void App::draw_key_settings() {
    field("Usages");
    const float number = px(62);
    ImGui::SetNextItemWidth(number);
    if (ImGui::InputScalar("##min", ImGuiDataType_S32, &key_min_, nullptr, nullptr, "%02X",
                           ImGuiInputTextFlags_CharsHexadecimal))
        key_min_ = std::clamp(key_min_, 0x04, 0xDF);
    ImGui::SameLine(0, px(6));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("to");
    ImGui::SameLine(0, px(6));
    ImGui::SetNextItemWidth(number);
    if (ImGui::InputScalar("##max", ImGuiDataType_S32, &key_max_, nullptr, nullptr, "%02X",
                           ImGuiInputTextFlags_CharsHexadecimal))
        key_max_ = std::clamp(key_max_, 0x04, 0xDF);
    ImGui::SetItemTooltip("HID keyboard usages in hex. 04 (A) to 65 (Menu) covers the usual "
                          "keys; modifiers are always included. Raise the upper end to 94 to "
                          "also forward the extra keys on a JIS (Japanese) keyboard — Henkan, "
                          "Muhenkan, Kana, Zenkaku/Hankaku, and Ro (87-94).");
}

void App::draw_gamepad_settings() {
    field("Buttons");
    ui::slider_int("##buttons", &pad_buttons_, 1, 32, "");

    field("Axis bits");
    ui::slider_int("##bits", &pad_bits_, 2, 32, " bits");

    field("D-pad");
    static const char* const dpad_modes[] = {"None", "Hat switch", "Four buttons"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##dpad", &pad_dpad_, dpad_modes, 3);

    field("Axes");
    ImGui::BeginGroup();
    const float button = ImGui::GetFrameHeight() * 0.8f;
    for (size_t index = 0; index < pad_axes_.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu", index);
        ImGui::SameLine(0, px(8));
        ImGui::TextUnformatted(axis_title(pad_axes_[index]).c_str());
        ImGui::SameLine();
        ui::align_right(button * 3 + px(8));
        ImGui::BeginDisabled(index == 0);
        if (ui::icon_button("##up", ui::Icon::up, button, ui::Tone::quiet))
            std::swap(pad_axes_[index], pad_axes_[index - 1]);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(4));
        ImGui::BeginDisabled(index + 1 == pad_axes_.size());
        if (ui::icon_button("##down", ui::Icon::down, button, ui::Tone::quiet))
            std::swap(pad_axes_[index], pad_axes_[index + 1]);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(4));
        const bool required =
            pad_axes_[index] == AOAHID_AXIS_X || pad_axes_[index] == AOAHID_AXIS_Y;
        ImGui::BeginDisabled(required);
        bool removed = false;
        if (ui::icon_button("##remove", ui::Icon::close, button, ui::Tone::quiet,
                            required ? nullptr : "Remove axis")) {
            pad_axes_.erase(pad_axes_.begin() + static_cast<std::ptrdiff_t>(index));
            removed = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        if (removed)
            break;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##add", "Add axis", ImGuiComboFlags_HeightLarge)) {
        for (const aoap::spec_detail::AxisIdentity& identity : aoap::spec_detail::axis_table) {
            if (std::find(pad_axes_.begin(), pad_axes_.end(), identity.role) != pad_axes_.end())
                continue;
            if (ImGui::Selectable(axis_title(identity.role).c_str()))
                pad_axes_.push_back(identity.role);
        }
        ImGui::EndCombo();
    }
    small_dim("The number is the axis column of \"a\" rows.");
    ImGui::EndGroup();
}

void App::draw_pen_settings() {
    field("Mode");
    static const char* const modes[] = {"On screen (direct)", "Tablet (indirect)"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##mode", &pen_mode_, modes, 2);
    small_dim(use_touch_ ? "Uses the touchscreen resolution."
                         : "Uses a 0-32767 surface the phone maps onto its screen.");
}

void App::draw_connect_card() {
    ui::begin_card("##connect");
    const Phase phase = engine_.phase();
    const ImVec2 size(-FLT_MIN, ImGui::GetFrameHeight() + px(14));
    switch (phase) {
    case Phase::idle:
        if (connect_prep_task_.valid()) {
            ImGui::BeginDisabled();
            ui::button("Connecting...##busy", size, ui::Tone::primary);
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(retry_task_.valid());
            if (ui::button("Connect", size, ui::Tone::primary))
                begin_connect();
            ImGui::EndDisabled();
        }
        break;
    case Phase::connected:
    case Phase::playing:
    case Phase::live:
        if (ui::button("Disconnect", size, ui::Tone::danger))
            engine_.disconnect();
        break;
    case Phase::refreshing:
    case Phase::connecting:
    case Phase::disconnecting: {
        ImGui::BeginDisabled();
        ui::button(phase == Phase::connecting      ? "Connecting...##busy"
                   : phase == Phase::disconnecting ? "Disconnecting...##busy"
                                                   : "Searching...##busy",
                   size, ui::Tone::primary);
        ImGui::EndDisabled();
        break;
    }
    }
    if (!connect_error_.empty()) {
        gap(2);
        small_colored(theme::danger, connect_error_);
    }
    ui::end_card();
}

// --- Player ----------------------------------------------------------------

void App::draw_player() {
    draw_script_card();
    gap(2);
    draw_transport_card();
}

void App::draw_script_card() {
    ui::begin_card("##script");
    caption_row("Script");
    const float button = ImGui::GetFrameHeight();
    ui::align_right(button);
    if (ui::icon_button("##rescan", ui::Icon::refresh, button, ui::Tone::quiet,
                        "Rescan the csv folder"))
        refresh_scripts();

    // The loaded script; clicking it opens the picker below it.
    const bool playing = engine_.phase() == Phase::playing;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() + px(12);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImGui::BeginDisabled(playing);
    const bool clicked = ImGui::InvisibleButton("##selector", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop playback to change the script");
    const bool picker_open = ImGui::IsPopupOpen("##picker");
    if ((clicked || open_picker_) && !playing) {
        ImGui::OpenPopup("##picker");
        focus_search_ = true;
        pending_delete_.clear();
        picker_cursor_ = 0;
        for (size_t index = 0; index < scripts_.size(); ++index) {
            if (scripts_[index].path == script_path_)
                picker_cursor_ = static_cast<int>(index);
        }
        script_filter_.clear();
        picker_follow_ = true;
    }
    open_picker_ = false;

    ImDrawList* list = ImGui::GetWindowDrawList();
    const float rounding = px(8);
    ImGui::BeginDisabled(playing);
    list->AddRectFilled(p0, p1,
                        ImGui::GetColorU32(hovered || picker_open ? theme::field_hover
                                                                  : theme::field),
                        rounding);
    if (picker_open)
        list->AddRect(p0, p1, ImGui::GetColorU32(theme::accent_line), rounding);
    const float text_y = p0.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    if (script_) {
        const std::string name = aoap::display_name(script_path_);
        ImGui::PushFont(nullptr, theme::font_title);
        list->AddText(ImVec2(p0.x + px(14), p0.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                      ImGui::GetColorU32(theme::text), name.c_str());
        ImGui::PopFont();
    } else {
        list->AddText(ImVec2(p0.x + px(14), text_y), ImGui::GetColorU32(theme::text_faint),
                      scripts_.empty() ? "No scripts yet: record one or drop a .csv file here"
                                       : "Choose a script");
    }
    char count[48];
    std::snprintf(count, sizeof count, "%zu %s   Ctrl+F", scripts_.size(),
                  scripts_.size() == 1 ? "script" : "scripts");
    const float count_width = ImGui::CalcTextSize(count).x;
    list->AddText(ImVec2(p1.x - px(40) - count_width, text_y),
                  ImGui::GetColorU32(theme::text_faint), count);
    ui::draw_icon(list, ui::Icon::chevron_down, ImVec2(p1.x - px(20), p0.y + height * 0.5f),
                  px(20), ImGui::GetColorU32(theme::text_dim));
    ImGui::EndDisabled();

    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + px(4)));
    draw_script_picker(width);

    if (!script_error_.empty()) {
        gap(2);
        small_colored(theme::danger, script_error_);
    }
    if (script_) {
        gap(2);
        char summary[200];
        std::snprintf(summary, sizeof summary,
                      "%zu intro rows (%s)  ·  %zu loop rows (%s)  ·  uses the %s",
                      script_->once_rows.size(),
                      format_time(timeline_.duration(aoap::Segment::intro)).c_str(),
                      script_->loop_rows.size(),
                      format_time(timeline_.duration(aoap::Segment::loop)).c_str(),
                      aoap::describe_profiles(script_->required_profiles()).c_str());
        small_dim(summary);

        const uint32_t available = engine_.connected() ? engine_.connected_profiles()
                                                       : aoap::enabled_profiles(build_setup());
        const uint32_t missing = script_->required_profiles() & ~available;
        if (missing != 0U) {
            small_colored(theme::warning, "Rows for the " + aoap::describe_profiles(missing) +
                                              " are skipped unless that profile is connected.");
        }
    }
    ui::end_card();
}

void App::draw_script_picker(const float width) {
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::surface_hi);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border_strong);
    const bool open = ImGui::BeginPopup("##picker", ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!open)
        return;

    // Filter first so the keys below act on what is shown.
    const std::vector<std::string> terms = search_terms(script_filter_);
    std::vector<int> shown;
    shown.reserve(scripts_.size());
    for (size_t index = 0; index < scripts_.size(); ++index) {
        bool match = true;
        for (const std::string& term : terms)
            match = match && scripts_[index].lower.find(term) != std::string::npos;
        if (match)
            shown.push_back(static_cast<int>(index));
    }

    // Escape cancels a pending delete, clears text being typed (the field
    // does that itself), and otherwise closes the picker.
    const bool escape = ui::escape_pressed();
    const bool field_clears = picker_typing_ && !script_filter_.empty();
    const std::string before = script_filter_;
    const bool entered = ui::search_box("##search", &script_filter_,
                                        ImGui::GetContentRegionAvail().x, "Search scripts",
                                        focus_search_, &picker_typing_);
    focus_search_ = false;
    if (script_filter_ != before) {
        picker_cursor_ = 0;
        picker_follow_ = true;
        pending_delete_.clear();
    }

    // The cursor is an index into `shown`.
    int cursor = -1;
    if (!shown.empty()) {
        if (script_filter_ == before) {
            // Opening puts the cursor on the loaded script; keep it in range.
            auto found = std::find(shown.begin(), shown.end(), picker_cursor_);
            cursor = found != shown.end() ? static_cast<int>(found - shown.begin()) : 0;
        } else {
            cursor = 0;
        }
    }
    if (cursor >= 0 && pending_delete_.empty()) {
        const int last = static_cast<int>(shown.size()) - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            cursor = std::min(cursor + 1, last);
            picker_follow_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            cursor = std::max(cursor - 1, 0);
            picker_follow_ = true;
        }
        picker_cursor_ = shown[static_cast<size_t>(cursor)];
    }
    if (escape) {
        if (!pending_delete_.empty())
            pending_delete_.clear();
        else if (!field_clears)
            ImGui::CloseCurrentPopup();
    }

    std::string choose;
    std::string remove; // deleted after the list is drawn
    if (entered && cursor >= 0 && pending_delete_.empty())
        choose = scripts_[static_cast<size_t>(picker_cursor_)].path;

    // The list: up to eight rows are visible, the rest scroll.
    const float row = px(44);
    const float gap_y = px(2);
    const size_t visible = std::clamp<size_t>(shown.size(), 1, 8);
    const float list_height = static_cast<float>(visible) * (row + gap_y) - gap_y;
    gap(2);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##rows", ImVec2(0, list_height), ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, gap_y));
    if (shown.empty()) {
        ImGui::SetCursorPos(ImVec2(px(10), (row - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextDisabled("%s", scripts_.empty() ? "The csv folder is empty."
                                                   : "No script matches this search.");
    }
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    const bool mouse_moved = delta.x != 0.0f || delta.y != 0.0f;
    for (size_t position = 0; position < shown.size(); ++position) {
        const ScriptFile& file = scripts_[static_cast<size_t>(shown[position])];
        const bool loaded = file.path == script_path_;
        const bool deleting = pending_delete_ == file.path;
        ImGui::PushID(file.path.c_str());
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float row_width = ImGui::GetContentRegionAvail().x;
        const ImVec2 r1(r0.x + row_width, r0.y + row);
        const bool mouse_here =
            ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r0, r1, false);
        if (mouse_here && mouse_moved)
            picker_cursor_ = shown[position];
        const bool current = picker_cursor_ == shown[position];

        const ImU32 fill = deleting ? theme::danger_soft
                           : current ? theme::field_hover
                                     : 0;
        if (fill != 0)
            list->AddRectFilled(r0, r1, ImGui::GetColorU32(fill), px(6));
        if (loaded)
            list->AddRectFilled(ImVec2(r0.x, r0.y + px(10)), ImVec2(r0.x + px(3), r1.y - px(10)),
                                ImGui::GetColorU32(theme::accent), px(1.5f));

        const float line = ImGui::GetTextLineHeight();
        const float name_y = r0.y + row * 0.5f - line + px(1);
        float x = r0.x + px(14);
        if (deleting) {
            const std::string question = "Delete " + file.name + ".csv?";
            list->AddText(ImVec2(x, r0.y + (row - line) * 0.5f), ImGui::GetColorU32(theme::text),
                          question.c_str());
        } else {
            // The name, with the first search term picked out.
            size_t hit = std::string::npos;
            size_t hit_length = 0;
            if (!terms.empty()) {
                hit = file.lower.find(terms.front());
                hit_length = terms.front().size();
            }
            const char* text = file.name.c_str();
            const auto segment = [&](const char* begin, const char* end, const ImU32 color) {
                if (begin == end)
                    return;
                list->AddText(ImVec2(x, name_y), ImGui::GetColorU32(color), begin, end);
                x += ImGui::CalcTextSize(begin, end).x;
            };
            const ImU32 base = loaded || current ? theme::text : theme::text_dim;
            if (hit == std::string::npos) {
                segment(text, text + file.name.size(), base);
            } else {
                segment(text, text + hit, base);
                segment(text + hit, text + hit + hit_length, theme::accent_text);
                segment(text + hit + hit_length, text + file.name.size(), base);
            }
            ImGui::PushFont(nullptr, theme::font_small);
            const std::string meta = loaded ? "Loaded  \xC2\xB7  " + file.meta : file.meta;
            list->AddText(ImVec2(r0.x + px(14), r0.y + row * 0.5f + px(3)),
                          ImGui::GetColorU32(theme::text_faint), meta.c_str());
            ImGui::PopFont();
        }

        // Row actions go in before the row itself, so they always win the
        // click over it, even when the pointer arrives and clicks at once.
        const float button = ImGui::GetFrameHeight();
        if (deleting) {
            const float confirm_w = px(78);
            const float cancel_w = px(70);
            ImGui::SetCursorScreenPos(
                ImVec2(r1.x - confirm_w - cancel_w - px(14), r0.y + (row - button) * 0.5f));
            if (ui::button("Cancel", ImVec2(cancel_w, 0)))
                pending_delete_.clear();
            ImGui::SameLine(0, px(6));
            if (ui::button("Delete", ImVec2(confirm_w, 0), ui::Tone::danger)) {
                remove = file.path;
                pending_delete_.clear();
            }
        } else if (current) {
            ImGui::SetCursorScreenPos(ImVec2(r1.x - button - px(8), r0.y + (row - button) * 0.5f));
            if (ui::icon_button("##delete", ui::Icon::trash, button, ui::Tone::quiet,
                                "Delete this file"))
                pending_delete_ = file.path;
        }

        ImGui::SetCursorScreenPos(r0);
        const bool pressed = ImGui::InvisibleButton("##row", ImVec2(row_width, row));
        if (static_cast<int>(position) == cursor && picker_follow_) {
            ImGui::SetScrollHereY(0.5f);
            picker_follow_ = false;
        }
        if (pressed && !deleting && pending_delete_.empty())
            choose = file.path;
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (!remove.empty())
        delete_script(remove);

    // Anything outside the csv folder can still be opened by path.
    thin_rule(2.0f, 4.0f);
    const float open_width = px(76);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - open_width -
                            ImGui::GetStyle().ItemSpacing.x);
    const bool path_entered =
        ImGui::InputTextWithHint("##path", "Open another file by path (or drop it on the window)",
                                 &path_input_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ui::button("Open", ImVec2(open_width, 0)) || path_entered) && !path_input_.empty())
        choose = path_input_;
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::TextDisabled("Up/Down to move, Enter to open, Esc to close");
    ImGui::PopFont();

    if (!choose.empty()) {
        load_script(choose);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::draw_transport_card() {
    ui::begin_card("##transport");
    const Phase phase = engine_.phase();
    const aoap::PlaybackStatus status = engine_.player().status();
    const bool live = phase == Phase::playing && status.state != aoap::PlaybackState::stopped;
    const aoap::PlaybackPosition position = live ? status.position : cursor_;
    const bool have_script = script_ != nullptr;
    const int64_t intro_ns = have_script ? timeline_.duration(aoap::Segment::intro) : 0;
    const int64_t loop_ns = have_script ? timeline_.duration(aoap::Segment::loop) : 0;
    const bool has_intro = have_script && timeline_.rows(aoap::Segment::intro) > 0;
    const bool has_loop = have_script && timeline_.rows(aoap::Segment::loop) > 0;

    // Time readout and loop counter.
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(nullptr, theme::font_display);
    const float big_line = ImGui::GetTextLineHeight();
    ImGui::TextUnformatted(format_time(position.time_ns).c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, px(10));
    ImGui::SetCursorPosY(top + big_line - ImGui::GetTextLineHeight() - px(4));
    const int64_t segment_ns = position.segment == aoap::Segment::intro ? intro_ns : loop_ns;
    ImGui::TextDisabled("/ %s", format_time(segment_ns).c_str());

    std::string where;
    if (!have_script)
        where = "No script";
    else if (position.segment == aoap::Segment::intro && has_intro)
        where = "Intro";
    else {
        const uint64_t current = (live ? status.loops : 0) + 1;
        where = "Loop " + format_count(current);
        if (loop_limit_ > 0)
            where += " of " + format_count(static_cast<uint64_t>(loop_limit_));
    }
    const std::string reports = format_count(live ? status.reports : 0) + " reports";
    const float right = std::max(ImGui::CalcTextSize(where.c_str()).x,
                                 ImGui::CalcTextSize(reports.c_str()).x);
    ImGui::SameLine();
    ImGui::SetCursorPosY(top + px(2));
    ui::align_right(right);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(where.c_str());
    ImGui::TextDisabled("%s", reports.c_str());
    ImGui::EndGroup();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + big_line + px(4)));

    // Timeline bars; releasing a bar seeks.
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_w = px(52);
    const float length_w = ImGui::CalcTextSize("00:00.000").x;
    const float bar_w = std::max(width - label_w - length_w - px(20), px(40));
    const auto bar = [&](const char* id, const char* label, const aoap::Segment segment,
                         const int64_t duration, float* scrub) {
        const bool here = position.segment == segment;
        float fraction = 0.0f;
        if (duration > 0 && here)
            fraction = static_cast<float>(static_cast<double>(position.time_ns) /
                                          static_cast<double>(duration));
        else if (segment == aoap::Segment::intro && position.segment == aoap::Segment::loop)
            fraction = 1.0f;
        const float row_top = ImGui::GetCursorPosY();
        const float row_left = ImGui::GetCursorPosX();
        const float bar_row = std::max(px(6) + px(12), px(20));
        ImGui::SetCursorPosY(row_top + (bar_row - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, here ? theme::text : theme::text_faint);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(row_left + label_w);
        ImGui::SetCursorPosY(row_top);
        ui::ScrubState state;
        ImGui::BeginDisabled(!have_script);
        const bool released = ui::scrub_bar(id, fraction, bar_w, px(6), scrub, &state);
        ImGui::EndDisabled();
        if (state.hovered || state.dragging) {
            const float at = state.dragging ? *scrub : state.hover_fraction;
            ImGui::SetTooltip("%s", format_time(static_cast<int64_t>(
                                        static_cast<double>(duration) * at))
                                        .c_str());
        }
        ImGui::SameLine(0, px(20));
        ImGui::SetCursorPosY(row_top + (bar_row - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("%s", format_time(duration).c_str());
        ImGui::SetCursorPosY(row_top + bar_row + px(2));
        if (released && have_script)
            seek({segment, static_cast<int64_t>(static_cast<double>(duration) * *scrub)});
    };
    if (has_intro)
        bar("##intro", "Intro", aoap::Segment::intro, intro_ns, &scrub_intro_);
    if (has_loop || !have_script)
        bar("##loop", "Loop", aoap::Segment::loop, loop_ns, &scrub_loop_);

    // Transport buttons, centred.
    gap(2);
    const float small = px(38);
    const float large = px(52);
    const float spacing = px(18);
    const float total = small * 2 + large + spacing * 2;
    const float row_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (ImGui::GetContentRegionAvail().x - total) * 0.5f);
    ImGui::SetCursorPosY(row_y + (large - small) * 0.5f);
    ImGui::BeginDisabled(!have_script);
    if (ui::icon_button("##restart", ui::Icon::restart, small, ui::Tone::secondary,
                        "Back to the start"))
        seek({});
    ImGui::EndDisabled();

    ImGui::SameLine(0, spacing);
    ImGui::SetCursorPosY(row_y);
    const bool can_start = phase == Phase::connected && have_script;
    const bool can_toggle = phase == Phase::playing && status.state != aoap::PlaybackState::stopped;
    const bool pausing = can_toggle && status.state == aoap::PlaybackState::playing;
    ImGui::BeginDisabled(!can_start && !can_toggle);
    if (ui::icon_button("##play", pausing ? ui::Icon::pause : ui::Icon::play, large,
                        ui::Tone::primary, pausing ? "Pause (Space)" : "Play (Space)"))
        toggle_playback();
    ImGui::EndDisabled();
    if (!can_start && !can_toggle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!engine_.connected() ? "Connect a device first"
                                               : "Choose a script first");

    ImGui::SameLine(0, spacing);
    ImGui::SetCursorPosY(row_y + (large - small) * 0.5f);
    ImGui::BeginDisabled(phase != Phase::playing);
    if (ui::icon_button("##stop", ui::Icon::stop, small, ui::Tone::secondary,
                        "Stop and release everything"))
        stop_playback();
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(row_y + large);
    thin_rule(2.0f, 2.0f);

    // Speed, loop limit, live offset.
    if (ImGui::BeginTable("##settings", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ui::caption("Speed");
        // Typed in directly; the value takes effect as soon as it parses.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##speed", &speed_, 0.0f, 0.0f, "%.2fx",
                              ImGuiInputTextFlags_CharsScientific)) {
            speed_ = std::clamp(speed_, static_cast<float>(aoap::Player::min_speed),
                                static_cast<float>(aoap::Player::max_speed));
            engine_.player().set_speed(speed_);
        }
        static constexpr float presets[] = {0.5f, 1.0f, 2.0f, 4.0f};
        static const char* const preset_labels[] = {"0.5x", "1x", "2x", "4x"};
        const float preset_width = (ImGui::GetContentRegionAvail().x - px(6) * 3) / 4.0f;
        for (int index = 0; index < 4; ++index) {
            if (index > 0)
                ImGui::SameLine(0, px(6));
            if (ui::button(preset_labels[index], ImVec2(preset_width, 0))) {
                speed_ = presets[index];
                engine_.player().set_speed(speed_);
            }
        }

        ImGui::TableNextColumn();
        ui::caption("Loops");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("##loops", &loop_limit_, 1, 10)) {
            loop_limit_ = std::max(loop_limit_, 0);
            engine_.player().set_loop_limit(loop_limit_);
        }
        small_dim(loop_limit_ == 0 ? "Repeats until stopped" : "Stops after this many loops");

        ImGui::TableNextColumn();
        ui::caption("Offset");
        // The offset belongs to the run in progress: it starts at zero and
        // can only be nudged while something is playing.
        const bool live_offset = engine_.phase() == Phase::playing;
        const double offset_ms = static_cast<double>(engine_.player().offset_ns()) / 1e6;
        ImGui::BeginDisabled(!live_offset);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%+.1f ms", offset_ms);
        ImGui::SameLine();
        ui::align_right(px(60));
        if (ui::button("Reset", ImVec2(px(60), 0)))
            engine_.player().set_offset_ns(0);
        static constexpr int steps[] = {-10, -1, 1, 10};
        const float step_width = (ImGui::GetContentRegionAvail().x - px(6) * 3) / 4.0f;
        for (int index = 0; index < 4; ++index) {
            if (index > 0)
                ImGui::SameLine(0, px(6));
            char label[16];
            std::snprintf(label, sizeof label, "%+d", steps[index]);
            if (ui::button(label, ImVec2(step_width, 0)))
                engine_.player().adjust_offset_ns(static_cast<int64_t>(steps[index]) * ns_per_ms);
        }
        ImGui::EndDisabled();
        if (!live_offset && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Available while playing; it resets at every start.");
        ImGui::EndTable();
    }
    ui::end_card();
}

// --- Playlist --------------------------------------------------------------

void App::draw_playlist() {
    const bool running = engine_.phase() == Phase::playing;
    const PlaylistProgress progress = engine_.playlist_progress();

    ui::begin_card("##playlist_file");
    caption_row("Playlist");
    const float button = ImGui::GetFrameHeight();
    ui::align_right(button);
    if (ui::icon_button("##rescan_playlists", ui::Icon::refresh, button, ui::Tone::quiet,
                        "Rescan the playlists folder"))
        refresh_playlists();
    gap(2);

    field("Name");
    const float open_width = px(86);
    const float save_width = px(76);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - open_width - save_width -
                            spacing * 2);
    if (ImGui::InputTextWithHint("##playlist_name", "Untitled", &playlist_name_input_))
        playlist_dirty_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(playlist_names_.empty());
    if (ui::button("Open", ImVec2(open_width, 0)))
        open_playlist_picker_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(playlist_.entries.empty() || playlist_name_input_.empty());
    if (ui::button("Save", ImVec2(save_width, 0), ui::Tone::primary))
        save_current_playlist();
    ImGui::EndDisabled();
    draw_playlist_picker();

    if (!playlist_error_.empty())
        small_colored(theme::danger, playlist_error_);
    else if (playlist_dirty_ && !playlist_.entries.empty())
        small_colored(theme::warning, "Unsaved changes.");
    else
        small_dim(("Playlists are kept in " + aoap::path_utf8(playlist_directory())).c_str());
    ui::end_card();

    gap(2);
    ui::begin_card("##playlist_steps");
    caption_row("Scripts");
    char total[64];
    int64_t loops_total = 0;
    for (const Playlist::Entry& entry : playlist_.entries)
        loops_total += entry.loops;
    std::snprintf(total, sizeof total, "%zu %s  ·  %lld loops", playlist_.entries.size(),
                  playlist_.entries.size() == 1 ? "script" : "scripts",
                  static_cast<long long>(loops_total));
    ImGui::PushFont(nullptr, theme::font_small);
    const float total_width = ImGui::CalcTextSize(total).x;
    ImGui::PopFont();
    ui::align_right(total_width);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::TextColored(theme::vec(theme::text_faint), "%s", total);
    ImGui::PopFont();
    gap(2);

    if (playlist_.entries.empty())
        small_dim("Add scripts below; they play in this order, each for its own number of loops.");

    // Rows: order, name, loops, remove.
    size_t remove_at = playlist_.entries.size();
    for (size_t index = 0; index < playlist_.entries.size(); ++index) {
        Playlist::Entry& entry = playlist_.entries[index];
        ImGui::PushID(static_cast<int>(index));
        const bool current = running && progress.active && progress.index == index;
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float row = ImGui::GetFrameHeight() + px(10);
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* list = ImGui::GetWindowDrawList();
        if (current) {
            list->AddRectFilled(r0, ImVec2(r0.x + width, r0.y + row),
                                ImGui::GetColorU32(theme::accent_soft), px(6));
        }

        ImGui::SetCursorScreenPos(
            ImVec2(r0.x + px(10), r0.y + (row - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(current ? theme::accent_text : theme::text_faint), "%zu",
                           index + 1);
        ImGui::SameLine(0, px(10));

        const std::string path = resolve_script_reference(entry.script);
        std::error_code code;
        const bool missing = !std::filesystem::exists(aoap::utf8_path(path), code);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, missing ? theme::danger : theme::text);
        ImGui::TextUnformatted(aoap::display_name(entry.script).c_str());
        ImGui::PopStyleColor();
        if (missing)
            ImGui::SetItemTooltip("This file is no longer in the csv folder.");

        const float loops_width = px(92);
        ui::align_right(loops_width + button + px(6));
        ImGui::SetNextItemWidth(loops_width);
        ImGui::BeginDisabled(running);
        int loops = static_cast<int>(entry.loops);
        if (ImGui::InputInt("##loops", &loops, 1, 10)) {
            entry.loops = std::clamp(loops, 1, 1000000);
            playlist_dirty_ = true;
        }
        ImGui::SetItemTooltip("How many times this script loops");
        ImGui::SameLine(0, px(6));
        if (ui::icon_button("##remove", ui::Icon::close, button, ui::Tone::quiet, "Remove"))
            remove_at = index;
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + row + px(2)));
        ImGui::PopID();
    }
    if (remove_at < playlist_.entries.size()) {
        playlist_.entries.erase(playlist_.entries.begin() +
                                static_cast<std::ptrdiff_t>(remove_at));
        playlist_dirty_ = true;
    }

    // Add a script from the csv folder.
    gap(2);
    thin_rule(0.0f, 4.0f);
    ImGui::BeginDisabled(running || scripts_.empty());
    const float add_width = px(76);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - add_width - spacing);
    const char* preview = scripts_.empty() ? "No scripts in the csv folder"
                                           : scripts_[static_cast<size_t>(std::clamp(
                                                          playlist_add_choice_, 0,
                                                          static_cast<int>(scripts_.size()) - 1))]
                                                 .name.c_str();
    if (ImGui::BeginCombo("##add_script", preview)) {
        for (size_t index = 0; index < scripts_.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(scripts_[index].name.c_str(),
                                  playlist_add_choice_ == static_cast<int>(index)))
                playlist_add_choice_ = static_cast<int>(index);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ui::button("Add", ImVec2(add_width, 0)) && !scripts_.empty()) {
        const size_t choice = static_cast<size_t>(
            std::clamp(playlist_add_choice_, 0, static_cast<int>(scripts_.size()) - 1));
        playlist_.entries.push_back(
            Playlist::Entry{script_reference(scripts_[choice].path), 1});
        playlist_dirty_ = true;
    }
    ImGui::EndDisabled();
    ui::end_card();

    gap(2);
    ui::begin_card("##playlist_run");
    caption_row("Run");
    gap(2);
    field("Time limit");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(px(120));
    if (ImGui::InputInt("##limit", &playlist_.time_limit_minutes, 1, 10)) {
        playlist_.time_limit_minutes = std::clamp(playlist_.time_limit_minutes, 0, 100000);
        playlist_dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, px(10));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(playlist_.time_limit_minutes == 0
                            ? "minutes (0 plays the whole playlist)"
                            : "minutes, then playback stops");

    gap(6);
    if (running) {
        if (progress.active) {
            char line[160];
            std::snprintf(line, sizeof line, "Playing %zu/%zu: %s", progress.index + 1,
                          progress.count, progress.name.c_str());
            ImGui::TextUnformatted(line);
            if (progress.ends_at_ns != 0) {
                const int64_t left = progress.ends_at_ns - aoap::Timing::now_ns();
                small_dim(("Time limit: " + format_time(std::max<int64_t>(left, 0)) + " left")
                              .c_str());
            }
            gap(4);
        }
        if (ui::button("Stop playlist", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + px(12)),
                       ui::Tone::danger))
            stop_playback();
    } else {
        const bool ready_to_play = engine_.phase() == Phase::connected && playlist_playable();
        ImGui::BeginDisabled(!ready_to_play);
        if (ui::button("Play playlist", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + px(12)),
                       ui::Tone::primary))
            play_playlist();
        ImGui::EndDisabled();
        if (!ready_to_play && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", playlist_.entries.empty() ? "Add a script first"
                                    : !engine_.connected()   ? "Connect a device first"
                                                             : "One of the scripts is missing");
        }
    }
    ui::end_card();
}

void App::draw_playlist_picker() {
    if (open_playlist_picker_) {
        ImGui::OpenPopup("##playlist_picker");
        open_playlist_picker_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::surface_hi);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border_strong);
    const bool open = ImGui::BeginPopup("##playlist_picker");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!open)
        return;

    std::string chosen;
    std::string remove;
    for (const std::string& name : playlist_names_) {
        ImGui::PushID(name.c_str());
        const float button = ImGui::GetFrameHeight();
        if (ImGui::Selectable("##row", name == playlist_.name, ImGuiSelectableFlags_None,
                              ImVec2(px(260), button)))
            chosen = name;
        const ImVec2 r0 = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(r0.x + px(8), r0.y + (button - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(theme::text), name.c_str());
        ImGui::SameLine();
        ui::align_right(button);
        if (ui::icon_button("##delete", ui::Icon::trash, button, ui::Tone::quiet, "Delete"))
            remove = name;
        ImGui::PopID();
    }
    if (!chosen.empty()) {
        load_playlist_named(chosen);
        ImGui::CloseCurrentPopup();
    }
    if (!remove.empty()) {
        std::string error;
        if (delete_playlist(remove, error))
            log_.message(aoap::Severity::info, "Deleted the playlist \"" + remove + "\".");
        else
            log_.message(aoap::Severity::error, error);
        refresh_playlists();
    }
    ImGui::EndPopup();
}

// --- Recorder --------------------------------------------------------------

void App::draw_recorder() {
    const bool active = recording();

    ui::begin_card("##phone");
    caption_row("Phone");
    gap(2);
    ImGui::BeginDisabled(active);
    field("Device");
    const float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button -
                            ImGui::GetStyle().ItemSpacing.x);
    std::string current = "Automatic (the only phone adb sees)";
    if (adb_choice_ > 0 && static_cast<size_t>(adb_choice_) <= adb_devices_.size()) {
        const aoap::AdbDevice& device = adb_devices_[static_cast<size_t>(adb_choice_) - 1];
        current = device.model.empty() ? device.serial : device.model + "  ·  " + device.serial;
    }
    if (ImGui::BeginCombo("##adb", current.c_str())) {
        if (ImGui::Selectable("Automatic (the only phone adb sees)", adb_choice_ == 0))
            adb_choice_ = 0;
        for (size_t index = 0; index < adb_devices_.size(); ++index) {
            const aoap::AdbDevice& device = adb_devices_[index];
            std::string label = device.model.empty() ? device.serial
                                                     : device.model + "  ·  " + device.serial;
            if (device.state != "device")
                label += "  (" + device.state + ")";
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(label.c_str(), adb_choice_ == static_cast<int>(index) + 1))
                adb_choice_ = static_cast<int>(index) + 1;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (adb_task_.valid())
        ui::spinner(button * 0.4f, theme::accent);
    else if (ui::icon_button("##adb_refresh", ui::Icon::refresh, button, ui::Tone::quiet,
                             "List phones with adb devices"))
        refresh_adb_devices();

    field("Input");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##input", "Every input device (or /dev/input/eventN)",
                             &record_input_);
    ImGui::EndDisabled();
    gap(2);
    small_dim("Recording reads touches and keys with adb over USB debugging. The result plays "
              "back with the touchscreen and keyboard profiles.");
    ui::end_card();

    gap(2);
    ui::begin_card("##record");
    caption_row("Recording");
    gap(2);
    if (!active) {
        // The name is chosen before recording; blank takes the date and time.
        field("File name");
        const bool typed_csv = ends_with_csv(record_name_);
        const float suffix = typed_csv ? 0.0f : ImGui::CalcTextSize(".csv").x + px(8);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - suffix);
        ImGui::InputTextWithHint("##name", "Leave blank for record-<date>-<time>", &record_name_);
        if (!typed_csv) {
            ImGui::SameLine(0, px(8));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(".csv");
        }
        const std::string problem = record_name_problem(record_name_);
        bool replaces = false;
        if (!problem.empty()) {
            small_colored(theme::danger, problem);
        } else {
            const std::string target =
                record_name_.empty()
                    ? aoap::path_utf8(aoap::utf8_path(aoap::script_directory()) /
                                      "record-<date>-<time>.csv")
                    : aoap::resolve_record_path(record_file_name(record_name_));
            small_dim(("Saves to " + target).c_str());
            std::error_code code;
            replaces = !record_name_.empty() &&
                       std::filesystem::exists(aoap::utf8_path(target), code);
            if (replaces)
                small_colored(theme::warning,
                              "A script with this name exists; recording replaces it.");
        }
        gap(6);
        ImGui::BeginDisabled(!problem.empty());
        if (ui::button(replaces ? "Replace and record" : "Start recording",
                       ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + px(12)), ui::Tone::record))
            start_recording();
        ImGui::EndDisabled();
    } else {
        const int64_t started = recorder_ ? recorder_->started_ns() : 0;
        const bool capturing = recorder_ && recorder_->active() && started != 0;
        const int64_t elapsed = capturing ? aoap::Timing::now_ns() - started : 0;

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(nullptr, theme::font_display);
        const float line = ImGui::GetTextLineHeight();
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.2) / 1.2);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + px(8), p.y + line * 0.5f), px(7),
            ImGui::GetColorU32(theme::rgb(0xE0564E, static_cast<unsigned>(255 - phase * 150))));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(26));
        char clock[32];
        const int64_t seconds = elapsed / 1'000'000'000;
        std::snprintf(clock, sizeof clock, "%02" PRId64 ":%02" PRId64 ".%01" PRId64, seconds / 60,
                      seconds % 60, (elapsed / 100'000'000) % 10);
        ImGui::TextUnformatted(capturing ? clock : "Starting");
        ImGui::PopFont();
        const std::string rows =
            format_count(recorder_ ? recorder_->rows() : 0) + " rows captured  ·  " +
            aoap::display_name(record_path_);
        small_dim(rows.c_str());
        gap(6);
        if (ui::button("Stop and save", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() + px(12)),
                       ui::Tone::primary))
            stop_recording();
    }

    if (!last_record_path_.empty() && !active) {
        thin_rule();
        const std::string saved = "Saved " + aoap::display_name(last_record_path_) + "  ·  " +
                                  format_count(last_record_rows_) + " rows";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(theme::success), "%s", saved.c_str());
        ui::align_right(px(130));
        ImGui::BeginDisabled(engine_.phase() == Phase::playing);
        if (ui::button("Open in player", ImVec2(px(130), 0))) {
            load_script(last_record_path_);
            tab_ = Tab::player;
        }
        ImGui::EndDisabled();
    }
    ui::end_card();
}

// --- Activity log ----------------------------------------------------------

void App::draw_log(const float height) {
    ui::begin_card("##log", nullptr, height);
    const float button = ImGui::GetFrameHeight();
    if (ui::icon_button("##fold", log_open_ ? ui::Icon::chevron_down : ui::Icon::chevron_right,
                        button, ui::Tone::quiet, log_open_ ? "Hide" : "Show"))
        log_open_ = !log_open_;
    ImGui::SameLine(0, px(6));
    caption_row("Activity");

    const ActivityLog::Entry problem = log_.latest_problem();
    if (!log_open_ && !problem.text.empty()) {
        ImGui::SameLine(0, px(14));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, problem.severity == aoap::Severity::error
                                                 ? theme::danger
                                                 : theme::warning);
        ImGui::TextUnformatted(problem.text.c_str());
        ImGui::PopStyleColor();
    }
    ui::align_right(px(64));
    if (ui::button("Clear", ImVec2(px(64), 0)))
        log_.clear();

    if (log_open_) {
        gap(2);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_None);
        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        log_.visit([](const ActivityLog::Entry& entry) {
            ImGui::PushFont(nullptr, theme::font_small);
            ImGui::TextColored(theme::vec(theme::text_faint), "%s", entry.time.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0, px(10));
            const ImU32 color = entry.severity == aoap::Severity::error     ? theme::danger
                                : entry.severity == aoap::Severity::warning ? theme::warning
                                                                            : theme::text_dim;
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", entry.text.c_str());
            ImGui::PopStyleColor();
        });
        const uint64_t version = log_.version();
        if (version != log_seen_ && (at_bottom || log_follow_))
            ImGui::SetScrollHereY(1.0f);
        log_follow_ = false;
        log_seen_ = version;
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ui::end_card();
}

} // namespace gui
