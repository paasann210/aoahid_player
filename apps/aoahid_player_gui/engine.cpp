// SPDX-License-Identifier: MIT
#include "engine.hpp"

#include "aoahid_player/adb.hpp"
#include "aoahid_player/input_state.hpp"
#include "aoahid_player/timing.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>

namespace gui {
namespace {

constexpr size_t live_inbox_limit = 4096;

std::string plural(const uint64_t count, const char* one, const char* many) {
    return std::to_string(count) + ' ' + (count == 1 ? one : many);
}

} // namespace

Engine::Engine(aoap::EventSink& sink, std::function<void()> wake)
    : sink_(sink), wake_(std::move(wake)), session_(&sink), player_(session_.group(), &sink) {
    player_.set_observer(this);
    live_inbox_.reserve(256);
    worker_ = std::thread([this] { loop(); });
}

Engine::~Engine() {
    {
        const std::lock_guard lock(mutex_);
        queue_.clear();
        playlist_abort_ = true;
        live_stop_ = true;
        if (running_play_)
            player_.stop();
        queue_.push_back(Command{Command::Kind::quit, {}, {}, false, {}, {}, {}, 0, {}, 0});
    }
    ready_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

void Engine::note(const aoap::Severity severity, const std::string& text) {
    sink_.message(severity, text);
}

void Engine::set_phase(const Phase phase) noexcept {
    phase_.store(phase, std::memory_order_release);
    if (wake_)
        wake_();
}

void Engine::push(Command command, const Phase optimistic) {
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(std::move(command));
        // Shown at once, so a double click cannot queue the same step twice.
        phase_.store(optimistic, std::memory_order_release);
    }
    ready_.notify_one();
}

void Engine::refresh() {
    if (phase() != Phase::idle)
        return;
    push(Command{Command::Kind::refresh, {}, {}, false, {}, {}, {}, 0, {}, 0}, Phase::refreshing);
}

void Engine::connect(std::vector<size_t> selection, aoap::ProfileSetup setup,
                     const bool stop_adb_server) {
    if (phase() != Phase::idle)
        return;
    push(Command{Command::Kind::connect, std::move(selection), std::move(setup), stop_adb_server,
                 {}, {}, {}, 0, {}, 0},
         Phase::connecting);
}

void Engine::disconnect() {
    if (!connected())
        return;
    {
        const std::lock_guard lock(mutex_);
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [](const Command& c) {
                                        return c.kind == Command::Kind::play ||
                                               c.kind == Command::Kind::playlist ||
                                               c.kind == Command::Kind::live;
                                    }),
                     queue_.end());
        playlist_abort_ = true;
        if (running_play_)
            player_.stop();
    }
    push(Command{Command::Kind::disconnect, {}, {}, false, {}, {}, {}, 0, {}, 0},
         Phase::disconnecting);
}

void Engine::play(std::shared_ptr<const aoap::EventScript> script, std::string name,
                  const aoap::PlaybackPosition start, const int64_t loops) {
    const Phase now = phase();
    if ((now != Phase::connected && now != Phase::live) || !script)
        return;
    live_stop();
    push(Command{Command::Kind::play, {}, {}, false, std::move(script), std::move(name), start,
                 loops, {}, 0},
         Phase::playing);
}

void Engine::play_playlist(std::vector<PlaylistStep> steps, const int64_t time_limit_ns) {
    const Phase now = phase();
    if ((now != Phase::connected && now != Phase::live) || steps.empty())
        return;
    live_stop();
    {
        const std::lock_guard lock(mutex_);
        playlist_abort_ = false;
    }
    push(Command{Command::Kind::playlist, {}, {}, false, {}, {}, {}, 0, std::move(steps),
                 time_limit_ns},
         Phase::playing);
}

void Engine::stop() {
    const std::lock_guard lock(mutex_);
    const auto queued = std::find_if(queue_.begin(), queue_.end(), [](const Command& c) {
        return c.kind == Command::Kind::play || c.kind == Command::Kind::playlist;
    });
    if (queued != queue_.end()) {
        queue_.erase(queued);
        if (!running_play_)
            phase_.store(Phase::connected, std::memory_order_release);
    }
    playlist_abort_ = true;
    if (running_play_)
        player_.stop();
}

void Engine::live_start() {
    if (phase() != Phase::connected)
        return;
    {
        const std::lock_guard lock(mutex_);
        live_stop_ = false;
        live_inbox_.clear();
    }
    push(Command{Command::Kind::live, {}, {}, false, {}, {}, {}, 0, {}, 0}, Phase::live);
}

void Engine::live_stop() {
    {
        const std::lock_guard lock(mutex_);
        live_stop_ = true;
        const auto queued = std::find_if(queue_.begin(), queue_.end(), [](const Command& c) {
            return c.kind == Command::Kind::live;
        });
        if (queued != queue_.end()) {
            queue_.erase(queued);
            if (phase_.load(std::memory_order_relaxed) == Phase::live)
                phase_.store(Phase::connected, std::memory_order_release);
        }
    }
    ready_.notify_one();
}

void Engine::live_send(const aoap::EventPayload& payload) {
    {
        const std::lock_guard lock(mutex_);
        if (phase_.load(std::memory_order_relaxed) != Phase::live)
            return;
        // Bursts are folded while the worker is busy with a report: moves add
        // up, and the latest contact position or axis value wins. Edges
        // (presses, releases, touch down and up) always keep their order.
        if (!live_inbox_.empty() && !live_inbox_.back().wheel) {
            aoap::EventPayload& last = live_inbox_.back().payload;
            if (const auto* move = std::get_if<aoap::MouseMove>(&payload)) {
                if (auto* previous = std::get_if<aoap::MouseMove>(&last)) {
                    previous->dx += move->dx;
                    previous->dy += move->dy;
                    ready_.notify_one();
                    return;
                }
            } else if (const auto* touch = std::get_if<aoap::TouchEvent>(&payload)) {
                auto* previous = std::get_if<aoap::TouchEvent>(&last);
                if (previous != nullptr && touch->state && previous->state &&
                    previous->finger_id == touch->finger_id) {
                    *previous = *touch;
                    ready_.notify_one();
                    return;
                }
            } else if (const auto* axis = std::get_if<aoap::GamepadAxis>(&payload)) {
                auto* previous = std::get_if<aoap::GamepadAxis>(&last);
                if (previous != nullptr && previous->axis_index == axis->axis_index) {
                    previous->value = axis->value;
                    ready_.notify_one();
                    return;
                }
            }
        }
        if (live_inbox_.size() >= live_inbox_limit)
            return;
        live_inbox_.push_back(LiveItem{false, payload, 0});
    }
    ready_.notify_one();
}

void Engine::live_scroll(const int32_t wheel) {
    if (wheel == 0)
        return;
    {
        const std::lock_guard lock(mutex_);
        if (phase_.load(std::memory_order_relaxed) != Phase::live)
            return;
        if (!live_inbox_.empty() && live_inbox_.back().wheel) {
            live_inbox_.back().wheel_delta += wheel;
        } else {
            if (live_inbox_.size() >= live_inbox_limit)
                return;
            live_inbox_.push_back(LiveItem{true, aoap::MouseMove{0, 0}, wheel});
        }
    }
    ready_.notify_one();
}

std::vector<aoap::DeviceEntry> Engine::devices() const {
    const std::lock_guard lock(mutex_);
    return devices_;
}

uint64_t Engine::devices_version() const {
    const std::lock_guard lock(mutex_);
    return devices_version_;
}

std::vector<aoap::DeviceStatus> Engine::device_status() const {
    return session_.group().snapshot();
}

aoap::ProfileSetup Engine::connected_setup() const {
    const std::lock_guard lock(mutex_);
    return setup_;
}

PlaylistProgress Engine::playlist_progress() const {
    const std::lock_guard lock(mutex_);
    return progress_;
}

// --- Sent events for the preview ---------------------------------------------

void Engine::observe(const Observed& event) noexcept {
    if (!observing_.load(std::memory_order_relaxed))
        return;
    const size_t head = ring_head_.load(std::memory_order_relaxed);
    const size_t next = (head + 1) % ring_size;
    if (next == ring_tail_.load(std::memory_order_acquire))
        return; // full: the preview drops this event rather than stall a send
    ring_[head] = event;
    ring_head_.store(next, std::memory_order_release);
}

bool Engine::take(Observed& out) noexcept {
    const size_t tail = ring_tail_.load(std::memory_order_relaxed);
    if (tail == ring_head_.load(std::memory_order_acquire))
        return false;
    out = ring_[tail];
    ring_tail_.store((tail + 1) % ring_size, std::memory_order_release);
    return true;
}

void Engine::sent(const aoap::EventPayload& payload) noexcept {
    if (!observing_.load(std::memory_order_relaxed))
        return;
    observe(Observed{Observed::Kind::payload, payload, 0, aoap::Timing::now_ns()});
}

// --- Worker --------------------------------------------------------------

std::shared_ptr<const aoap::EventScript>
Engine::prepare(const std::shared_ptr<const aoap::EventScript>& script) {
    const aoap::ProfileSetup& setup = session_.setup();
    auto scaled = std::make_shared<aoap::EventScript>();
    const bool touch = setup.touch.enabled;
    const bool pen = setup.pen.enabled;
    if (!aoap::scale_script(*script, touch ? setup.touch.width : 0, touch ? setup.touch.height : 0,
                            pen ? setup.pen.width : 0, pen ? setup.pen.height : 0, *scaled))
        return script;
    char text[128];
    std::snprintf(text, sizeof text, "Coordinates scaled from %dx%d to the connected surface.",
                  script->screen_width, script->screen_height);
    note(aoap::Severity::info, text);
    return scaled;
}

void Engine::loop() {
    while (true) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return !queue_.empty(); });
            command = std::move(queue_.front());
            queue_.pop_front();
            if (command.kind == Command::Kind::quit)
                break;
            running_play_ =
                command.kind == Command::Kind::play || command.kind == Command::Kind::playlist;
        }
        execute(command);
        {
            const std::lock_guard lock(mutex_);
            running_play_ = false;
        }
        if (wake_)
            wake_();
    }
    // Leaving: release whatever is held and unregister every HID device.
    session_.disconnect();
}

// After a run, the phase goes back to connected unless a queued command has
// already claimed it (a disconnect, another play).
void Engine::finish_run() {
    {
        const std::lock_guard lock(mutex_);
        if (queue_.empty())
            phase_.store(Phase::connected, std::memory_order_release);
    }
    if (wake_)
        wake_();
}

void Engine::run_live() {
    aoap::DeviceGroup& group = session_.group();
    aoap::InputState held;
    std::vector<LiveItem> batch;
    batch.reserve(256);

    const auto apply = [&](const LiveItem& item) {
        if (item.wheel) {
            aoahid_result result = group.scroll(item.wheel_delta);
            if (result == AOAHID_ERR_BUSY) {
                group.flush();
                result = group.scroll(item.wheel_delta);
            }
            if (result == AOAHID_OK)
                observe(Observed{Observed::Kind::wheel, aoap::MouseMove{0, 0}, item.wheel_delta,
                                 aoap::Timing::now_ns()});
            return;
        }
        aoahid_result result = group.apply(item.payload);
        if (result == AOAHID_ERR_BUSY) {
            // The opposite edge of the same control is still unreported.
            group.flush();
            result = group.apply(item.payload);
        }
        if (result == AOAHID_OK) {
            held.apply(item.payload);
            sent(item.payload);
        }
    };

    while (true) {
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,
                        [&] { return live_stop_ || !queue_.empty() || !live_inbox_.empty(); });
            if (live_stop_ || !queue_.empty())
                break; // stopped, or a play or disconnect takes over
            batch.swap(live_inbox_);
        }
        for (const LiveItem& item : batch)
            apply(item);
        batch.clear();
        // One report per burst; while it drains, new input folds in the inbox.
        group.flush();
        if (group.active_count() == 0) {
            note(aoap::Severity::error, "Every device stopped responding; live control stopped.");
            break;
        }
    }

    // Release everything live control pressed, releases in their own report.
    std::vector<aoap::EventPayload> releases;
    std::vector<aoap::EventPayload> presses;
    aoap::InputState::transition(held, aoap::InputState{}, releases, presses);
    for (const aoap::EventPayload& payload : releases)
        apply(LiveItem{false, payload, 0});
    group.flush();
    {
        const std::lock_guard lock(mutex_);
        live_inbox_.clear();
    }
}

void Engine::execute(Command& command) {
    std::string error;
    switch (command.kind) {
    case Command::Kind::refresh: {
        std::vector<aoap::DeviceEntry> found;
        const bool ok = session_.refresh(found, error);
        const size_t count = found.size();
        {
            const std::lock_guard lock(mutex_);
            devices_ = std::move(found);
            ++devices_version_;
        }
        if (!ok)
            note(aoap::Severity::error, error);
        else if (count == 0)
            note(aoap::Severity::warning,
                 "No AOA-capable device found. Use a data cable, set the phone's USB mode to "
                 "something other than \"Charge only\", and on Linux install the udev rule.");
        else
            note(aoap::Severity::info, "Found " + plural(count, "device.", "devices."));
        set_phase(Phase::idle);
        break;
    }
    case Command::Kind::connect: {
        if (command.stop_adb_server) {
            if (aoap::adb_kill_server(error)) {
                note(aoap::Severity::info, "Stopped the adb server so it releases the USB device.");
                aoap::Timing::sleep_ms(1200);
            }
            // adb missing or not running is normal here; nothing to report.
        }
        if (!session_.connect(command.selection, command.setup, error)) {
            note(aoap::Severity::error, error);
            profiles_.store(0U, std::memory_order_relaxed);
            set_phase(Phase::idle);
            break;
        }
        {
            const std::lock_guard lock(mutex_);
            setup_ = session_.setup();
        }
        setup_version_.fetch_add(1, std::memory_order_acq_rel);
        profiles_.store(session_.group().profile_mask(), std::memory_order_relaxed);
        note(aoap::Severity::info,
             "Ready: " + plural(session_.group().size(), "device", "devices") + " with " +
                 aoap::describe_profiles(session_.group().profile_mask()) + ".");
        set_phase(Phase::connected);
        break;
    }
    case Command::Kind::disconnect:
        session_.disconnect();
        profiles_.store(0U, std::memory_order_relaxed);
        note(aoap::Severity::info, "Disconnected.");
        set_phase(Phase::idle);
        break;
    case Command::Kind::play: {
        if (!session_.connected()) {
            set_phase(Phase::idle);
            break;
        }
        player_.set_stop_at(0);
        // The live offset belongs to one run: it starts at zero every time.
        player_.set_offset_ns(0);
        player_.set_loop_limit(command.loops);
        player_.load(prepare(command.script));
        note(aoap::Severity::info, "Playing " + command.name + ".");
        player_.run(command.start);
        const aoap::PlaybackStatus status = player_.status();
        note(aoap::Severity::info, "Stopped after " + plural(status.loops, "loop", "loops") +
                                       " (" + plural(status.reports, "report", "reports") + ").");
        finish_run();
        break;
    }
    case Command::Kind::playlist: {
        if (!session_.connected()) {
            set_phase(Phase::idle);
            break;
        }
        const int64_t stop_at =
            command.time_limit_ns > 0 ? aoap::Timing::now_ns() + command.time_limit_ns : 0;
        player_.set_stop_at(stop_at);
        const size_t count = command.steps.size();
        bool by_time = false;
        size_t finished = 0;
        for (size_t index = 0; index < count; ++index) {
            const PlaylistStep& step = command.steps[index];
            {
                const std::lock_guard lock(mutex_);
                if (playlist_abort_)
                    break;
                progress_ = PlaylistProgress{true, index, count, step.name, step.loops, stop_at};
            }
            if (wake_)
                wake_();
            player_.set_offset_ns(0);
            player_.set_loop_limit(std::max<int64_t>(step.loops, 1));
            player_.load(prepare(step.script));
            note(aoap::Severity::info, "Playlist " + std::to_string(index + 1) + "/" +
                                           std::to_string(count) + ": " + step.name + " (" +
                                           plural(static_cast<uint64_t>(step.loops), "loop",
                                                  "loops") +
                                           ").");
            player_.run({});
            if (player_.stopped_by_time()) {
                by_time = true;
                break;
            }
            if (session_.group().active_count() == 0)
                break;
            const std::lock_guard lock(mutex_);
            if (playlist_abort_)
                break;
            ++finished;
        }
        player_.set_stop_at(0);
        {
            const std::lock_guard lock(mutex_);
            progress_ = PlaylistProgress{};
        }
        if (by_time)
            note(aoap::Severity::info, "Time limit reached; the playlist stopped.");
        else if (finished == count)
            note(aoap::Severity::info, "Playlist finished.");
        else
            note(aoap::Severity::info, "Playlist stopped.");
        finish_run();
        break;
    }
    case Command::Kind::live:
        if (!session_.connected()) {
            set_phase(Phase::idle);
            break;
        }
        run_live();
        finish_run();
        break;
    case Command::Kind::quit:
        break;
    }
}

} // namespace gui
