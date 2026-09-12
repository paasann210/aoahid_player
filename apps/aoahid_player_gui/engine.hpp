// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_player/event_script.hpp"
#include "aoahid_player/events.hpp"
#include "aoahid_player/player.hpp"
#include "aoahid_player/session.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gui {

// One sent event, as the preview sees it.
struct Observed {
    enum class Kind : uint8_t { payload, wheel } kind{Kind::payload};
    aoap::EventPayload payload{aoap::MouseMove{0, 0}};
    int32_t wheel{};
    int64_t time_ns{};
};

// One script of a playlist run.
struct PlaylistStep {
    std::shared_ptr<const aoap::EventScript> script;
    std::string name;
    int64_t loops{1};
};

struct PlaylistProgress {
    bool active{};
    size_t index{};
    size_t count{};
    std::string name;
    int64_t loops{};
    int64_t ends_at_ns{}; // 0 = no time limit
};

// Runs every libaoahid call on one worker thread, which is what libaoahid's
// per-Context serialisation needs and keeps USB waits off the UI thread.
// Commands are queued; playback controls go straight to the thread-safe
// Player, and live input goes through a small coalescing inbox, so both take
// effect immediately.
class Engine final : private aoap::PlaybackObserver {
  public:
    enum class Phase : uint8_t {
        idle,
        refreshing,
        connecting,
        connected,
        playing,
        live,
        disconnecting
    };

    Engine(aoap::EventSink& sink, std::function<void()> wake);
    ~Engine() override;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void refresh();
    void connect(std::vector<size_t> selection, aoap::ProfileSetup setup, bool stop_adb_server);
    void disconnect();
    // `loops` 0 repeats until stopped.
    void play(std::shared_ptr<const aoap::EventScript> script, std::string name,
              aoap::PlaybackPosition start, int64_t loops);
    // Plays the steps in order; `time_limit_ns` 0 means no limit.
    void play_playlist(std::vector<PlaylistStep> steps, int64_t time_limit_ns);
    // Cancels a queued play or stops the running one, playlists included.
    void stop();

    // Live control: input from the preview goes straight to the devices, and
    // everything it pressed is released when it ends. live_send() may be
    // called for every pointer move; bursts are folded while a report drains.
    void live_start();
    void live_stop();
    void live_send(const aoap::EventPayload& payload);
    void live_scroll(int32_t wheel);

    [[nodiscard]] aoap::Player& player() noexcept { return player_; }
    [[nodiscard]] const aoap::Player& player() const noexcept { return player_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_.load(std::memory_order_acquire); }
    [[nodiscard]] bool connected() const noexcept {
        const Phase value = phase();
        return value == Phase::connected || value == Phase::playing || value == Phase::live;
    }
    [[nodiscard]] bool busy() const noexcept {
        const Phase value = phase();
        return value == Phase::refreshing || value == Phase::connecting ||
               value == Phase::disconnecting;
    }

    // Thread-safe snapshots for the UI.
    [[nodiscard]] std::vector<aoap::DeviceEntry> devices() const;
    [[nodiscard]] uint64_t devices_version() const;
    [[nodiscard]] std::vector<aoap::DeviceStatus> device_status() const;
    [[nodiscard]] uint32_t connected_profiles() const noexcept {
        return profiles_.load(std::memory_order_relaxed);
    }
    // The profiles of the current connection; changes only on connect.
    [[nodiscard]] aoap::ProfileSetup connected_setup() const;
    [[nodiscard]] uint64_t setup_version() const noexcept {
        return setup_version_.load(std::memory_order_acquire);
    }
    [[nodiscard]] PlaylistProgress playlist_progress() const;

    // Sent events for the preview. Nothing is recorded until observing is on;
    // take() belongs to the UI thread.
    void set_observing(bool on) noexcept { observing_.store(on, std::memory_order_relaxed); }
    bool take(Observed& out) noexcept;

  private:
    struct Command {
        enum class Kind { refresh, connect, disconnect, play, playlist, live, quit } kind;
        std::vector<size_t> selection;
        aoap::ProfileSetup setup;
        bool stop_adb_server{};
        std::shared_ptr<const aoap::EventScript> script;
        std::string name;
        aoap::PlaybackPosition start;
        int64_t loops{};
        std::vector<PlaylistStep> steps;
        int64_t time_limit_ns{};
    };
    struct LiveItem {
        bool wheel{};
        aoap::EventPayload payload{aoap::MouseMove{0, 0}};
        int32_t wheel_delta{};
    };

    void push(Command command, Phase optimistic);
    void loop();
    void execute(Command& command);
    void run_live();
    void finish_run();
    void set_phase(Phase phase) noexcept;
    void note(aoap::Severity severity, const std::string& text);
    std::shared_ptr<const aoap::EventScript>
    prepare(const std::shared_ptr<const aoap::EventScript>& script);
    void sent(const aoap::EventPayload& payload) noexcept override;
    void observe(const Observed& event) noexcept;

    aoap::EventSink& sink_;
    std::function<void()> wake_;
    aoap::Session session_;
    aoap::Player player_;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Command> queue_;
    bool running_play_{};
    bool playlist_abort_{};
    std::vector<aoap::DeviceEntry> devices_;
    uint64_t devices_version_{};
    aoap::ProfileSetup setup_;
    PlaylistProgress progress_;
    std::vector<LiveItem> live_inbox_;
    bool live_stop_{};

    std::atomic<Phase> phase_{Phase::idle};
    std::atomic<uint32_t> profiles_{};
    std::atomic<uint64_t> setup_version_{};

    // Single-producer (worker) single-consumer (UI) ring of sent events.
    static constexpr size_t ring_size = 4096;
    std::array<Observed, ring_size> ring_{};
    std::atomic<size_t> ring_head_{};
    std::atomic<size_t> ring_tail_{};
    std::atomic<bool> observing_{};

    std::thread worker_;
};

} // namespace gui
