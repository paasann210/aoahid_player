// SPDX-License-Identifier: MIT
#pragma once

#include "activity_log.hpp"
#include "engine.hpp"
#include "live_image.hpp"
#include "playlist.hpp"
#include "settings.hpp"
#include "widgets.hpp"

#include "aoahid_player/adb.hpp"
#include "aoahid_player/event_script.hpp"
#include "aoahid_player/player.hpp"
#include "aoahid_player/recorder.hpp"
#include "aoahid_player/spec_builder.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gui {

// The whole window: device and profile setup on the left, the player and the
// recorder on the right, the activity log at the bottom. Runs on the UI
// thread; everything slow happens on the Engine, the recording thread, or a
// short-lived adb task.
class App {
  public:
    explicit App(std::function<void()> wake);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void frame();
    void on_drop(std::vector<std::string> paths);
    void on_focus();
    // The window lost input focus; release a captured pointer so it does not
    // stay hidden and grabbed while some other window is in front.
    void on_focus_lost();
    // Ctrl+F, taken from the window system with the modifier state of that
    // exact key press, so even a chord sent in one burst is recognised.
    void on_search_shortcut();
    // Raw key presses from the window system, so live control can forward
    // keys ImGui reserves for itself (Tab, Escape, Space). `scancode` is the
    // platform scancode GLFW hands the key callback, used to recognise JIS
    // (Japanese) keyboard keys GLFW has no key constant for.
    void on_key(int glfw_key, int scancode, bool pressed);
    // Raw cursor position from the window system, tracked independently of
    // ImGui's own io.MousePos so mouse mode's relative motion survives
    // io.MousePos being pinned off-screen (see live_pointer()).
    void on_cursor(double x, double y);

    // True while something on screen moves on its own (progress, spinners,
    // a recording clock), so the main loop keeps redrawing without input.
    [[nodiscard]] bool animating() const;

  private:
    enum class Tab : int { player = 0, live = 1, playlist = 2, recorder = 3 };

    // What the Live tab forwards to the phone. Touch and mouse both use the
    // pointer, so only one of them can be on at a time.
    struct LiveToggles {
        bool touch{true};
        bool mouse{};
        bool key{};
        bool gamepad{};
    };
    // One line of the Live tab's input log.
    struct LiveLogEntry {
        std::string text;
        ImU32 color;
        int64_t time_ns;
    };

    struct ScreenSize {
        bool ok{};
        int32_t width{};
        int32_t height{};
        std::string error;
    };
    struct AdbList {
        bool ok{};
        std::vector<aoap::AdbDevice> devices;
        std::string error;
    };
    // One file of the csv folder, with what the picker shows and searches.
    struct ScriptFile {
        std::string path;
        std::string name;  // display name
        std::string lower; // name folded for case-insensitive search
        std::string meta;  // "1.2 KB  ·  2026-09-11 06:20"
    };

    // Layout pieces.
    void draw_header();
    void draw_sidebar();
    void draw_devices_card();
    void draw_profiles_card();
    void draw_touch_settings();
    void draw_mouse_settings();
    void draw_key_settings();
    void draw_gamepad_settings();
    void draw_pen_settings();
    void draw_connect_card();
    void draw_player();
    void draw_script_card();
    void draw_script_picker(float width);
    void draw_transport_card();
    void draw_live();
    void draw_live_surface(ImVec2 size);
    void draw_live_log(ImVec2 size);
    void draw_live_controls();
    // The "Mouse release key" row: shows the configured key, a button to
    // pick a new one, and a note that only that exact key releases the
    // captured pointer once changed.
    void draw_live_release_key_setting();
    // The Live preview filling the window, with only the input switches and
    // a way out.
    void draw_live_fullscreen();
    void draw_playlist();
    void draw_playlist_picker();
    void draw_recorder();
    void draw_log(float height);

    // Actions.
    void poll();
    void refresh_scripts();
    void load_script(const std::string& path);
    void delete_script(const std::string& path);
    void connect();
    void toggle_playback();
    void stop_playback();
    void play_playlist();
    void live_enable(bool on);
    void live_pointer(ImVec2 surface_min, ImVec2 surface_size);
    // Grabs or releases the OS pointer for relative mouse forwarding.
    void live_capture_pointer(bool captured);
    // Preview fractions to device fractions and back, following the rotation.
    [[nodiscard]] ImVec2 live_to_device(ImVec2 preview) const noexcept;
    [[nodiscard]] ImVec2 live_to_preview(ImVec2 device) const noexcept;
    // The shape the preview is drawn at, after the rotation.
    [[nodiscard]] float live_preview_aspect() const noexcept;
    void live_keyboard();
    void live_gamepad();
    void drain_observed();
    void live_log(std::string text, ImU32 color);
    [[nodiscard]] bool live_ready() const;
    void refresh_playlists();
    void load_playlist_named(const std::string& name);
    void save_current_playlist();
    [[nodiscard]] bool playlist_playable() const;
    void seek(aoap::PlaybackPosition target);
    void detect_screen_size();
    void refresh_adb_devices();
    void start_recording();
    void stop_recording();
    void finish_recording();
    [[nodiscard]] std::string adb_serial() const;
    [[nodiscard]] aoap::ProfileSetup build_setup() const;
    [[nodiscard]] Settings current_settings() const;
    void apply_settings(const Settings& settings);
    // Writes the settings when they differ from what was last saved.
    void persist_settings();
    [[nodiscard]] bool settings_locked() const;
    [[nodiscard]] bool recording() const;

    std::function<void()> wake_;
    ActivityLog log_;
    Engine engine_;
    Tab tab_{Tab::player};

    // Devices.
    std::vector<aoap::DeviceEntry> devices_;
    uint64_t devices_version_{~uint64_t{0}};
    std::set<std::string> selected_;
    bool stop_adb_{true};
    std::string connect_error_;
    float connect_height_{120.0f};
    Engine::Phase last_phase_{Engine::Phase::idle};

    // Profile settings; frozen while connected.
    bool use_touch_{true};
    bool use_mouse_{};
    bool use_key_{};
    bool use_gamepad_{};
    bool use_pen_{};
    int touch_width_{1080};
    int touch_height_{2400};
    int touch_contacts_{10};
    int mouse_buttons_{5};
    int key_min_{0x04};
    int key_max_{0x65};
    int pad_buttons_{16};
    int pad_bits_{16};
    int pad_dpad_{1}; // 0 none, 1 hat, 2 buttons
    std::vector<aoahid_axis_role> pad_axes_;
    int pen_mode_{0}; // 0 direct screen, 1 indirect tablet

    // Scripts.
    std::vector<ScriptFile> scripts_;
    std::string script_path_;
    std::shared_ptr<const aoap::EventScript> script_;
    aoap::Timeline timeline_;
    std::string script_error_;
    std::string path_input_;
    std::string script_filter_;
    bool focus_search_{};
    bool open_picker_{};
    int picker_cursor_{};
    bool picker_follow_{}; // scroll the list to the cursor row
    bool picker_typing_{}; // the search field was active last frame
    std::string pending_delete_;

    // Transport.
    aoap::PlaybackPosition cursor_{};
    float scrub_intro_{};
    float scrub_loop_{};
    float speed_{1.0f};
    int loop_limit_{};
    float offset_step_ms_{10.0f};

    // Live tab.
    LiveToggles live_{};
    bool live_capturing_{};   // the pointer belongs to the surface
    // While in mouse mode, the OS cursor is grabbed (hidden and unbounded) so
    // relative motion keeps flowing even past the edge of the screen. The
    // release key (Escape by default; see live_release_key_) releases it
    // without turning Live control or the Mouse toggle off; clicking the
    // preview again grabs it back.
    bool live_mouse_captured_{};
    // The GLFW key that releases the captured pointer; 0 means "not set",
    // which is treated as Escape. Only this exact key releases the capture
    // once changed — every other key (including the mouse's own buttons)
    // still goes straight to the phone, capture or not.
    int live_release_key_{};
    // True while "Set release key" is armed: the next key press this frame
    // is captured as the new live_release_key_ instead of being forwarded.
    bool live_release_key_picking_{};
    bool live_touching_{};    // a contact is down
    int32_t live_touch_x_{};  // last contact position, in device coordinates
    int32_t live_touch_y_{};
    // The preview's shape, as a width:height ratio; 0 follows the connected
    // touchscreen. `live_rotation_` is how many quarter turns clockwise the
    // phone is shown at, for landscape use.
    int live_ratio_w_{};
    int live_ratio_h_{};
    int live_rotation_{};
    bool live_fullscreen_{};
    std::vector<LiveLogEntry> live_log_lines_;
    uint64_t live_log_version_{};
    uint64_t live_log_seen_{};
    std::vector<uint16_t> live_keys_;      // held keys, for the on-screen list
    std::vector<uint32_t> live_buttons_;   // held mouse buttons
    std::vector<uint32_t> live_pad_;       // held gamepad buttons
    int64_t live_wheel_at_{};
    int32_t live_wheel_{};
    // The last relative motion the phone actually received, for the panel.
    int32_t live_move_sent_x_{};
    int32_t live_move_sent_y_{};
    int64_t live_move_at_{};
    bool live_pad_connected_{};
    aoap::GamepadDpad live_dpad_{};
    std::vector<int32_t> live_axes_;
    std::vector<uint16_t> pressed_keys_;  // HID usages seen since the last frame; key-ups go
                                           // straight to the engine instead (see on_key())
    double live_move_x_{};    // pointer motion not yet sent as whole pixels
    double live_move_y_{};
    // Raw cursor position from the window system and the motion accumulated
    // from it since live_pointer() last drained it. Tracked independently of
    // ImGui's io.MouseDelta: while the pointer is captured, io.MousePos is
    // pinned off-screen every frame (see frame()) so a captured, invisible,
    // unboundedly-moving cursor can never drift onto — and click — some
    // other button in this window. ImGui's own delta calculation depends on
    // io.MousePos actually moving, so once it is pinned this raw delta is
    // the only source of the pointer's motion.
    bool live_raw_cursor_valid_{};
    double live_raw_cursor_x_{};
    double live_raw_cursor_y_{};
    double live_raw_delta_x_{};
    double live_raw_delta_y_{};
    LiveImageOverlay live_image_;      // optional reference picture over the preview
    std::string live_image_path_input_;

    // Playlist.
    std::vector<std::string> playlist_names_;
    Playlist playlist_;
    std::string playlist_name_input_;
    std::string playlist_error_;
    bool playlist_dirty_{};
    int playlist_add_choice_{};
    bool open_playlist_picker_{};

    // Recorder.
    std::vector<aoap::AdbDevice> adb_devices_;
    int adb_choice_{}; // 0 = automatic, else adb_devices_[choice - 1]
    std::string record_name_;
    std::string record_input_;
    std::unique_ptr<aoap::Recorder> recorder_;
    std::thread record_thread_;
    std::atomic<bool> record_done_{};
    bool record_saved_{};
    std::string record_path_;
    std::string last_record_path_;
    uint64_t last_record_rows_{};

    // Short adb tasks.
    std::future<ScreenSize> size_task_;
    std::future<AdbList> adb_task_;

    // Saved between runs.
    std::filesystem::path settings_path_;
    Settings saved_settings_;
    bool settings_warned_{};

    // Log panel.
    bool log_open_{true};
    uint64_t log_seen_{};
    bool log_follow_{true};
};

} // namespace gui
