// SPDX-License-Identifier: MIT
//
// The Live tab: a black surface with the phone's aspect ratio that shows what
// the connected devices are doing, and forwards the computer's pointer,
// keyboard, and gamepad to them.

#include "app.hpp"

#include "app_common.hpp"
#include "keymap.hpp"
#include "theme.hpp"

#include "aoahid_player/timing.hpp"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gui {
namespace {

using namespace detail;
using ui::px;
using Phase = Engine::Phase;

constexpr size_t live_log_limit = 200;
constexpr int64_t live_wheel_hold_ns = 700'000'000; // how long a scroll stays on screen
// The contact slot live touches use; script playback owns the others.
constexpr int live_finger = 0;

// Also used by the log panel.
const char* mouse_button_name(const uint32_t button) {
    switch (button) {
    case 1:
        return "Left";
    case 2:
        return "Right";
    case 3:
        return "Middle";
    case 4:
        return "Back";
    case 5:
        return "Forward";
    default:
        return "Button";
    }
}

// Removes `value` from `set`, or adds it when `present`; returns true when the
// set changed.
template <typename T>
bool set_member(std::vector<T>& set, const T value, const bool present) {
    const auto found = std::find(set.begin(), set.end(), value);
    if (present) {
        if (found != set.end())
            return false;
        set.push_back(value);
        return true;
    }
    if (found == set.end())
        return false;
    set.erase(found);
    return true;
}

std::string join_names(const std::vector<uint16_t>& usages) {
    std::string text;
    for (const uint16_t usage : usages) {
        if (!text.empty())
            text += " + ";
        text += hid_usage_name(usage);
    }
    return text;
}

// Why Live control cannot be used right now, shown under the preview so it
// is never just a disabled toggle with no explanation. Playing a script does
// not "stop" the player in any lasting sense — it is only that loading a
// script's events for playback and forwarding Live input are two uses of the
// same connection that cannot run at once — hence the wording below.
const char* live_unavailable_reason(const Engine::Phase phase, const bool connected) {
    switch (phase) {
    case Phase::playing:
        return "Live control is unavailable while a script is loaded and playing. Stop the "
               "player (not the connection) to use the phone from here.";
    case Phase::connected:
    case Phase::live:
        return connected ? "Turn on Live control below to use the phone from here." : "";
    default:
        return "Connect a device to use live control.";
    }
}

} // namespace

// The preview may show the phone turned; both directions of the mapping are
// spelled out so a click and the dot drawn for it always agree.
ImVec2 App::live_to_device(const ImVec2 preview) const noexcept {
    switch (live_rotation_ & 3) {
    case 1:
        return ImVec2(preview.y, 1.0f - preview.x);
    case 2:
        return ImVec2(1.0f - preview.x, 1.0f - preview.y);
    case 3:
        return ImVec2(1.0f - preview.y, preview.x);
    default:
        return preview;
    }
}

ImVec2 App::live_to_preview(const ImVec2 device) const noexcept {
    switch (live_rotation_ & 3) {
    case 1:
        return ImVec2(1.0f - device.y, device.x);
    case 2:
        return ImVec2(1.0f - device.x, 1.0f - device.y);
    case 3:
        return ImVec2(device.y, 1.0f - device.x);
    default:
        return device;
    }
}

float App::live_preview_aspect() const noexcept {
    // Connected: the screen in use. Not yet: the settings that would be used,
    // so the preview already has the right shape while setting up.
    const aoap::ProfileSetup setup =
        engine_.connected() ? engine_.connected_setup() : build_setup();
    float width = static_cast<float>(live_ratio_w_);
    float height = static_cast<float>(live_ratio_h_);
    if (width <= 0.0f || height <= 0.0f) {
        // Follow the touchscreen the connection declared.
        width = setup.touch.enabled && setup.touch.width > 0
                    ? static_cast<float>(setup.touch.width)
                    : 9.0f;
        height = setup.touch.enabled && setup.touch.height > 0
                     ? static_cast<float>(setup.touch.height)
                     : 16.0f;
    }
    // A quarter turn puts the long side across.
    return (live_rotation_ & 1) != 0 ? height / width : width / height;
}

bool App::live_ready() const {
    const Phase phase = engine_.phase();
    return phase == Phase::connected || phase == Phase::live;
}

void App::live_log(std::string text, const ImU32 color) {
    live_log_lines_.push_back(LiveLogEntry{std::move(text), color, aoap::Timing::now_ns()});
    if (live_log_lines_.size() > live_log_limit)
        live_log_lines_.erase(live_log_lines_.begin());
    ++live_log_version_;
}

void App::live_enable(const bool on) {
    if (on) {
        if (engine_.phase() != Phase::connected)
            return;
        engine_.set_observing(true);
        engine_.live_start();
        live_log("Live control started.", theme::text_dim);
        return;
    }
    live_capture_pointer(false);
    engine_.live_stop();
    live_capturing_ = false;
    live_touching_ = false;
    live_move_x_ = 0.0;
    live_move_y_ = 0.0;
}

// Grabs the OS pointer so mouse mode can keep receiving relative motion past
// the edge of the screen, or lets it go back to normal. GLFW's disabled
// cursor mode does exactly this, but the resulting cursor position is
// unbounded and invisible, so frame() pins io.MousePos off-screen for as
// long as this is captured — otherwise that unbounded position eventually
// drifts onto some other button in this window and clicks it. With
// io.MousePos pinned, ImGui's own io.MouseDelta no longer reflects the
// pointer's motion, so it is tracked independently via on_cursor() instead
// (see live_raw_delta_x_/y_) while captured.
void App::live_capture_pointer(const bool captured) {
    if (captured == live_mouse_captured_)
        return;
    GLFWwindow* window = glfwGetCurrentContext();
    if (window == nullptr)
        return;
    glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured && glfwRawMouseMotionSupported() == GLFW_TRUE)
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    live_mouse_captured_ = captured;
    // A fresh grab (or release) starts from zero so neither the next capture
    // nor the pointer coming back replays whatever motion accumulated
    // around the transition.
    live_move_x_ = 0.0;
    live_move_y_ = 0.0;
    live_raw_delta_x_ = 0.0;
    live_raw_delta_y_ = 0.0;
    live_raw_cursor_valid_ = false;
    if (captured)
        return;
    // Releasing mid-click would otherwise leave a button held on the phone
    // forever, since live_pointer() stops reading the mouse once let go.
    for (const uint32_t button : live_buttons_)
        engine_.live_send(aoap::MouseButton{button, false});
    live_buttons_.clear();
}

// Everything the worker actually sent, so the surface shows the device state
// rather than what the pointer did.
void App::drain_observed() {
    Observed event;
    while (engine_.take(event)) {
        if (event.kind == Observed::Kind::wheel) {
            live_wheel_ = event.wheel;
            live_wheel_at_ = event.time_ns;
            char text[48];
            std::snprintf(text, sizeof text, "Wheel %+d", event.wheel);
            live_log(text, theme::accent_text);
            continue;
        }
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, aoap::TouchEvent>) {
                    live_touching_ = value.state;
                    live_touch_x_ = value.x;
                    live_touch_y_ = value.y;
                } else if constexpr (std::is_same_v<T, aoap::MouseMove>) {
                    live_move_sent_x_ = value.dx;
                    live_move_sent_y_ = value.dy;
                    live_move_at_ = event.time_ns;
                } else if constexpr (std::is_same_v<T, aoap::MouseButton>) {
                    if (set_member(live_buttons_, value.button, value.pressed)) {
                        live_log(std::string(mouse_button_name(value.button)) +
                                     (value.pressed ? " down" : " up"),
                                 theme::text_dim);
                    }
                } else if constexpr (std::is_same_v<T, aoap::KeyEvent>) {
                    if (set_member(live_keys_, value.usage, value.down) && value.down)
                        live_log(std::string(hid_usage_name(value.usage)), theme::text);
                } else if constexpr (std::is_same_v<T, aoap::GamepadButton>) {
                    if (set_member(live_pad_, value.button, value.pressed) && value.pressed) {
                        live_log("Pad " + std::to_string(value.button), theme::text);
                    }
                }
                // Motion and axis values are shown as they are, not logged.
            },
            event.payload);
    }
}

void App::live_pointer(const ImVec2 surface_min, const ImVec2 surface_size) {
    if (engine_.phase() != Phase::live || surface_size.x <= 0.0f || surface_size.y <= 0.0f)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    const aoap::ProfileSetup setup = engine_.connected_setup();

    if (live_.touch && setup.touch.enabled) {
        // The pointer is a single finger. Where it sits inside the preview is
        // kept as a fraction, turned back into the phone's own orientation,
        // and only then scaled to the touchscreen's coordinates, so the
        // preview's shape never changes where a touch lands.
        const ImVec2 preview(
            std::clamp((io.MousePos.x - surface_min.x) / surface_size.x, 0.0f, 1.0f),
            std::clamp((io.MousePos.y - surface_min.y) / surface_size.y, 0.0f, 1.0f));
        const ImVec2 device = live_to_device(preview);
        const int32_t x = std::clamp(
            static_cast<int32_t>(device.x * static_cast<float>(setup.touch.width)), 0,
            setup.touch.width - 1);
        const int32_t y = std::clamp(
            static_cast<int32_t>(device.y * static_cast<float>(setup.touch.height)), 0,
            setup.touch.height - 1);
        const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (down != live_touching_ || (down && (x != live_touch_x_ || y != live_touch_y_)))
            engine_.live_send(aoap::TouchEvent{live_finger, down, x, y});
        return;
    }

    if (live_.mouse && setup.mouse.enabled) {
        // The release key (Escape by default, configurable in the controls
        // below) already let go of the pointer in on_key(), straight from
        // the window system before ImGui's key state could be cleared for
        // key forwarding; here only capturing on a click is left to do. The
        // release key is still forwarded to the phone afterwards like any
        // other key, same as before it released the capture.
        if (!live_mouse_captured_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            live_capture_pointer(true);
        if (!live_mouse_captured_)
            return;

        // Relative motion; the fraction left over is kept so slow movement is
        // not lost to truncation.
        // Sent as-is, unlike touch: the preview's rotation only remaps where a
        // position lands, and a mouse delta is not a position. The phone's own
        // OS applies its own orientation to the cursor it moves.
        // io.MouseDelta is not used here: io.MousePos is pinned off-screen
        // every frame while captured (see frame()), so ImGui never sees the
        // cursor actually move and io.MouseDelta would read zero. The raw
        // motion on_cursor() accumulated since the last frame is used
        // instead, then drained.
        live_move_x_ += live_raw_delta_x_;
        live_move_y_ += live_raw_delta_y_;
        live_raw_delta_x_ = 0.0;
        live_raw_delta_y_ = 0.0;
        const auto whole = [](double& value) {
            const double truncated = std::trunc(value);
            value -= truncated;
            return static_cast<int32_t>(std::clamp(truncated, -32767.0, 32767.0));
        };
        const int32_t dx = whole(live_move_x_);
        const int32_t dy = whole(live_move_y_);
        if (dx != 0 || dy != 0)
            engine_.live_send(aoap::MouseMove{dx, dy});
        for (int button = 0; button < 5 && button < static_cast<int>(setup.mouse.buttons);
             ++button) {
            const uint32_t index = static_cast<uint32_t>(button) + 1;
            const bool held = std::find(live_buttons_.begin(), live_buttons_.end(), index) !=
                              live_buttons_.end();
            const bool down = ImGui::IsMouseDown(button);
            if (down != held)
                engine_.live_send(aoap::MouseButton{index, down});
        }
        if (io.MouseWheel != 0.0f)
            engine_.live_scroll(static_cast<int32_t>(std::lround(io.MouseWheel)));
    }
}

void App::live_keyboard() {
    if (engine_.phase() != Phase::live || !live_.key)
        return;
    const aoap::ProfileSetup setup = engine_.connected_setup();
    if (!setup.key.enabled)
        return;
    // Every key GLFW reported this frame, mapped to its HID usage; the
    // release key is never forwarded. Key-ups are sent straight from
    // on_key() instead of queued here, so they still reach the phone after
    // switching away from this tab.
    for (const uint16_t usage : pressed_keys_)
        engine_.live_send(aoap::KeyEvent{usage, true});
    pressed_keys_.clear();
}

void App::live_paste_clipboard() {
    if (engine_.phase() != Phase::live || !live_.key)
        return;
    if (!engine_.connected_setup().key.enabled)
        return;
    if (live_paste_active_.load(std::memory_order_relaxed))
        return;
    const char* clipboard = ImGui::GetClipboardText();
    if (clipboard == nullptr || clipboard[0] == '\0') {
        live_log("Clipboard is empty.", theme::text_dim);
        return;
    }

    if (live_paste_thread_.joinable())
        live_paste_thread_.join();
    live_paste_active_.store(true, std::memory_order_relaxed);
    live_paste_thread_ = std::thread([this, text = std::string(clipboard)] {
        // Half of the ~8ms per-character budget on either side of the report
        // change, so Down and Up always land in separate reports instead of
        // cancelling out in the same one — Android drops the keystroke
        // otherwise. Left Shift rides along with the key itself so a
        // shifted character is still one physical-feeling press.
        constexpr int32_t step_delay_ms = 4;
        constexpr uint16_t left_shift = 0xE1;
        for (const char c : text) {
            uint16_t usage = 0;
            bool shift = false;
            if (!hid_usage_from_ascii(c, usage, shift))
                continue; // unsupported, including all non-ASCII — skipped
            if (shift)
                engine_.live_send(aoap::KeyEvent{left_shift, true});
            engine_.live_send(aoap::KeyEvent{usage, true});
            aoap::Timing::sleep_ms(step_delay_ms);
            engine_.live_send(aoap::KeyEvent{usage, false});
            if (shift)
                engine_.live_send(aoap::KeyEvent{left_shift, false});
            aoap::Timing::sleep_ms(step_delay_ms);
        }
        live_paste_active_.store(false, std::memory_order_relaxed);
    });
}

void App::live_gamepad() {
    if (engine_.phase() != Phase::live || !live_.gamepad)
        return;
    const aoap::ProfileSetup setup = engine_.connected_setup();
    if (!setup.gamepad.enabled)
        return;

    GLFWgamepadstate state{};
    int pad = -1;
    for (int id = GLFW_JOYSTICK_1; id <= GLFW_JOYSTICK_LAST && pad < 0; ++id) {
        if (glfwJoystickIsGamepad(id) == GLFW_TRUE && glfwGetGamepadState(id, &state) == GLFW_TRUE)
            pad = id;
    }
    if (pad < 0) {
        live_pad_connected_ = false;
        return;
    }
    if (!live_pad_connected_) {
        live_pad_connected_ = true;
        live_log(std::string("Gamepad: ") + glfwGetGamepadName(pad), theme::text_dim);
    }

    // Buttons in GLFW's order map onto the declared buttons one for one.
    const uint32_t count = std::min<uint32_t>(setup.gamepad.buttons, GLFW_GAMEPAD_BUTTON_LAST + 1);
    for (uint32_t index = 0; index < count; ++index) {
        const bool down = state.buttons[index] == GLFW_PRESS;
        const bool held =
            std::find(live_pad_.begin(), live_pad_.end(), index + 1) != live_pad_.end();
        if (down != held)
            engine_.live_send(aoap::GamepadButton{index + 1, down});
    }
    // The D-pad is separate from the buttons on this profile.
    const bool up = state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] == GLFW_PRESS;
    const bool down = state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] == GLFW_PRESS;
    const bool right = state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS;
    const bool left = state.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS;
    if (setup.gamepad.dpad != AOAHID_DPAD_NONE &&
        (up != live_dpad_.up || down != live_dpad_.down || right != live_dpad_.right ||
         left != live_dpad_.left)) {
        live_dpad_ = aoap::GamepadDpad{up, down, right, left};
        engine_.live_send(live_dpad_);
    }

    // Axes, in the order the connection declared them.
    for (size_t index = 0; index < setup.gamepad.axes.size(); ++index) {
        float raw = 0.0f;
        switch (setup.gamepad.axes[index]) {
        case AOAHID_AXIS_X:
            raw = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
            break;
        case AOAHID_AXIS_Y:
            raw = state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
            break;
        case AOAHID_AXIS_Z:
        case AOAHID_AXIS_RX:
            raw = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
            break;
        case AOAHID_AXIS_RZ:
        case AOAHID_AXIS_RY:
            raw = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
            break;
        case AOAHID_AXIS_SIMULATION_ACCELERATOR:
        case AOAHID_AXIS_SIMULATION_THROTTLE:
            // GLFW reports triggers as -1 released, +1 fully pressed.
            raw = (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
            break;
        case AOAHID_AXIS_SIMULATION_BRAKE:
            raw = (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
            break;
        default:
            continue; // no obvious stick for this role
        }
        // A small dead zone keeps a resting stick from sending traffic.
        if (std::fabs(raw) < 0.06f)
            raw = 0.0f;
        // The axis range the connection declared; unipolar roles start at 0.
        const int32_t extent =
            static_cast<int32_t>((int64_t{1} << (setup.gamepad.axis_bits - 1)) - 1);
        const bool unipolar = aoap::spec_detail::axis_is_unipolar(setup.gamepad.axes[index]);
        const int32_t value = static_cast<int32_t>(
            std::lround(std::clamp(raw, unipolar ? 0.0f : -1.0f, 1.0f) *
                        static_cast<float>(extent)));
        if (index >= live_axes_.size())
            live_axes_.resize(index + 1, 0);
        if (live_axes_[index] != value) {
            live_axes_[index] = value;
            engine_.live_send(aoap::GamepadAxis{index, value});
        }
    }
}

// --- Drawing ---------------------------------------------------------------

void App::draw_live() {
    drain_observed();

    const bool running = engine_.phase() == Phase::live;
    if (running) {
        live_keyboard();
        live_gamepad();
    }

    // The surface takes the height left over above the controls, and the log
    // a fixed column beside it.
    const float controls = ImGui::GetFrameHeight() * 2 + px(64);
    const float available = ImGui::GetContentRegionAvail().y - controls -
                            ImGui::GetStyle().ItemSpacing.y;
    const float height = std::max(available, px(200));
    const float log_width = px(210);
    const float spacing = px(10);
    const float surface_width =
        std::max(ImGui::GetContentRegionAvail().x - log_width - spacing, px(200));
    draw_live_surface(ImVec2(surface_width, height));
    ImGui::SameLine(0, spacing);
    draw_live_log(ImVec2(log_width, height));
    gap(2);
    draw_live_controls();
}

void App::draw_live_surface(const ImVec2 size) {
    const aoap::ProfileSetup setup = engine_.connected_setup();
    const float aspect = live_preview_aspect();


    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgb(0x000000));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // NoScrollbar/NoScrollWithMouse: the wheel belongs to mouse mode (it
    // scrolls on the phone), never to this child window. Without this, a
    // captured pointer's wheel could scroll the surface itself once its
    // content (the phone rectangle) is taller than the available space.
    ImGui::BeginChild("##live_surface", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    // The phone, centred and as large as it fits.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float margin = px(10);
    float width = area.x - margin * 2;
    float height = width / aspect;
    if (height > area.y - margin * 2) {
        height = area.y - margin * 2;
        width = height * aspect;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 p0(origin.x + (area.x - width) * 0.5f, origin.y + (area.y - height) * 0.5f);
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRect(p0, p1, ImGui::GetColorU32(theme::border_strong), px(10), px(1.5f));

    const bool running = engine_.phase() == Phase::live;
    // The whole surface takes the pointer while live control is on.
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##surface", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // ImGui clears the active id the instant a button is released, even when
    // the release lands outside the item, so a drag let go past the edge of
    // the preview would otherwise never be seen here and would stay "held".
    // Falling back to our own held-state keeps checking until it clears.
    const bool held_state = live_touching_ || !live_buttons_.empty();
    // The reference image takes the pointer for its own handles first, while
    // unlocked; Live control only sees what it does not use.
    const bool image_active = live_image_.interact(p0, ImVec2(width, height));
    // While the pointer is captured, GLFW reports unbounded virtual motion
    // (see GLFW_CURSOR_DISABLED), so MousePos drifts outside the item and
    // hovered/IsItemActive alone would drop input the moment it does.
    if (running && !image_active &&
        (hovered || ImGui::IsItemActive() || held_state || live_mouse_captured_))
        live_pointer(p0, ImVec2(width, height));

    live_image_.draw(list, p0, ImVec2(width, height));

    // The live contact.
    if (live_touching_ && setup.touch.enabled && setup.touch.width > 0) {
        const ImVec2 device(
            static_cast<float>(live_touch_x_) / static_cast<float>(setup.touch.width),
            static_cast<float>(live_touch_y_) / static_cast<float>(setup.touch.height));
        const ImVec2 preview = live_to_preview(device);
        const ImVec2 c(p0.x + preview.x * width, p0.y + preview.y * height);
        const float radius = px(18);
        list->AddCircleFilled(c, radius, ImGui::GetColorU32(theme::accent_soft), 32);
        list->AddCircle(c, radius, ImGui::GetColorU32(theme::accent), 32, px(2));
        list->AddCircleFilled(c, px(4), ImGui::GetColorU32(IM_COL32(255, 255, 255, 230)), 16);
        char text[48];
        std::snprintf(text, sizeof text, "%d, %d", live_touch_x_, live_touch_y_);
        ImGui::PushFont(nullptr, theme::font_small);
        list->AddText(ImVec2(c.x + radius + px(6), c.y - ImGui::GetFontSize() * 0.5f),
                      ImGui::GetColorU32(theme::text), text);
        ImGui::PopFont();
    }

    // Held keys ride along the bottom of the phone, where they do not cover
    // what the pointer is doing.
    if (!live_keys_.empty()) {
        ImGui::PushFont(nullptr, theme::font_small);
        const std::string keys = join_names(live_keys_);
        const ImVec2 text_size = ImGui::CalcTextSize(keys.c_str());
        const float line = ImGui::GetFontSize() + px(4);
        const ImVec2 box(p0.x + (width - text_size.x) * 0.5f - px(10), p1.y - line - px(16));
        list->AddRectFilled(box, ImVec2(box.x + text_size.x + px(20), box.y + line + px(6)),
                            ImGui::GetColorU32(theme::rgb(0xFFFFFF, 26)), px(6));
        list->AddText(ImVec2(box.x + px(10), box.y + px(3)), ImGui::GetColorU32(theme::text),
                      keys.c_str());
        ImGui::PopFont();
    }

    // Sticks, drawn small in the bottom-right corner.
    if (live_.gamepad && !live_axes_.empty()) {
        const float radius = px(22);
        ImVec2 centre(p1.x - radius - px(14), p1.y - radius - px(14));
        for (size_t index = 0; index + 1 < live_axes_.size() && index < 2; index += 2) {
            list->AddCircle(centre, radius, ImGui::GetColorU32(theme::border_strong), 32, px(1.5f));
            const float extent = static_cast<float>(
                (int64_t{1} << (engine_.connected_setup().gamepad.axis_bits - 1)) - 1);
            const float ax = static_cast<float>(live_axes_[index]) / extent;
            const float ay = static_cast<float>(live_axes_[index + 1]) / extent;
            list->AddCircleFilled(ImVec2(centre.x + ax * radius, centre.y + ay * radius), px(5),
                                  ImGui::GetColorU32(theme::accent), 20);
            centre.x -= radius * 2 + px(10);
        }
    }

    if (!running) {
        const char* hint = live_unavailable_reason(engine_.phase(), engine_.connected());
        if (hint[0] != '\0') {
            ImGui::PushFont(nullptr, theme::font_small);
            const float wrap_width = std::max(width - px(40), px(120));
            const ImVec2 text_size = ImGui::CalcTextSize(hint, nullptr, false, wrap_width);
            const ImVec2 text_pos(p0.x + (width - wrap_width) * 0.5f,
                                  p0.y + (height - text_size.y) * 0.5f);
            list->AddText(nullptr, 0.0f, text_pos, ImGui::GetColorU32(theme::text_faint), hint,
                          nullptr, wrap_width);
            ImGui::PopFont();
        }
    }

    // The way out of mouse mode's captured, hidden pointer, spelled out on
    // top of the preview itself — including fullscreen, where the controls
    // below it are out of sight.
    if (live_mouse_captured_) {
        char text[64];
        std::snprintf(text, sizeof text, "Press %s to release the pointer",
                     glfw_key_name(live_release_key_ != 0 ? live_release_key_
                                                          : GLFW_KEY_ESCAPE));
        ImGui::PushFont(nullptr, theme::font_small);
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        const float pad_x = px(10);
        const float pad_y = px(6);
        const ImVec2 box0(p0.x + (width - text_size.x) * 0.5f - pad_x, p0.y + px(12));
        const ImVec2 box1(box0.x + text_size.x + pad_x * 2, box0.y + text_size.y + pad_y * 2);
        list->AddRectFilled(box0, box1, ImGui::GetColorU32(IM_COL32(0, 0, 0, 170)), px(8));
        list->AddText(ImVec2(box0.x + pad_x, box0.y + pad_y), ImGui::GetColorU32(theme::text),
                      text);
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void App::draw_live_fullscreen() {
    drain_observed();
    if (engine_.phase() == Phase::live) {
        live_keyboard();
        live_gamepad();
    }

    // One slim bar holds the switches and the way out; the preview takes the
    // rest of the window.
    const float bar = ImGui::GetFrameHeight() + px(16);
    const float height = std::max(ImGui::GetContentRegionAvail().y - bar -
                                      ImGui::GetStyle().ItemSpacing.y,
                                  px(160));
    draw_live_surface(ImVec2(ImGui::GetContentRegionAvail().x, height));
    gap(2);

    ui::begin_card("##live_full_controls");
    const uint32_t available = engine_.connected_profiles();
    bool on = engine_.phase() == Phase::live;
    ImGui::BeginDisabled(!live_ready());
    if (ui::toggle("Live control", &on))
        live_enable(on);
    ImGui::EndDisabled();

    struct Entry {
        const char* label;
        bool* flag;
        aoap::Profile profile;
    };
    const Entry entries[] = {
        {"Touch##full", &live_.touch, aoap::Profile::touch},
        {"Mouse##full", &live_.mouse, aoap::Profile::mouse},
        {"Keyboard##full", &live_.key, aoap::Profile::key},
        {"Gamepad##full", &live_.gamepad, aoap::Profile::gamepad},
    };
    for (const Entry& entry : entries) {
        const bool present = (available & aoap::profile_bit(entry.profile)) != 0U;
        ImGui::SameLine(0, px(18));
        ImGui::BeginDisabled(!present);
        if (ui::toggle(entry.label, entry.flag) && *entry.flag) {
            if (entry.flag == &live_.touch) {
                live_.mouse = false;
                live_capture_pointer(false);
            } else if (entry.flag == &live_.mouse) {
                live_.touch = false;
            }
        }
        ImGui::EndDisabled();
    }

    const float exit_width = px(132);
    ImGui::SameLine();
    ui::align_right(exit_width);
    if (ui::button("Exit full screen", ImVec2(exit_width, 0)))
        live_fullscreen_ = false;
    ImGui::SetItemTooltip(
        "Esc also exits full screen, unless Keyboard is on (then Esc reaches the phone "
        "instead). The mouse mode release key (set below, in the normal view) always works "
        "here too.");
    ui::end_card();
}

void App::draw_live_log(const ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::surface);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(10)));
    ImGui::BeginChild("##live_log", size,
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ui::caption("Input");
    ImGui::SameLine();
    const float clear_width = px(52);
    ui::align_right(clear_width);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
    ImGui::BeginDisabled(live_log_lines_.empty());
    if (ui::button("Clear", ImVec2(clear_width, ImGui::GetFrameHeight() - px(6))))
        live_log_lines_.clear();
    ImGui::EndDisabled();
    gap(2);

    // What is held right now, then what happened, newest at the bottom.
    const aoap::ProfileSetup setup = engine_.connected_setup();
    ImGui::PushFont(nullptr, theme::font_small);
    if (live_touching_ && setup.touch.enabled) {
        ImGui::TextColored(theme::vec(theme::accent_text), "Touch %d, %d", live_touch_x_,
                           live_touch_y_);
    }
    if (!live_buttons_.empty()) {
        std::string text;
        for (const uint32_t button : live_buttons_)
            text += (text.empty() ? "" : " + ") + std::string(mouse_button_name(button));
        ImGui::TextColored(theme::vec(theme::text), "Mouse %s", text.c_str());
    }
    if (live_wheel_ != 0 && aoap::Timing::now_ns() - live_wheel_at_ < live_wheel_hold_ns)
        ImGui::TextColored(theme::vec(theme::accent_text), "Wheel %+d", live_wheel_);
    if (aoap::Timing::now_ns() - live_move_at_ < live_wheel_hold_ns) {
        // The raw delta actually sent; the preview's rotation does not affect it.
        ImGui::TextColored(theme::vec(theme::text), "Move %+d, %+d", live_move_sent_x_,
                           live_move_sent_y_);
    }
    if (!live_keys_.empty())
        ImGui::TextColored(theme::vec(theme::text), "Keys %s", join_names(live_keys_).c_str());
    if (!live_pad_.empty()) {
        std::string text;
        for (const uint32_t button : live_pad_)
            text += (text.empty() ? "" : " ") + std::to_string(button);
        ImGui::TextColored(theme::vec(theme::text), "Pad %s", text.c_str());
    }
    if (live_.gamepad && !live_axes_.empty()) {
        const float extent = static_cast<float>(
            (int64_t{1} << (std::max<uint32_t>(setup.gamepad.axis_bits, 2) - 1)) - 1);
        std::string text;
        for (size_t index = 0; index < live_axes_.size() && index < 4; ++index) {
            char value[16];
            std::snprintf(value, sizeof value, "%+.2f",
                          static_cast<double>(live_axes_[index]) / static_cast<double>(extent));
            text += (text.empty() ? "" : "  ") + std::string(value);
        }
        ImGui::TextColored(theme::vec(theme::text_dim), "Axes %s", text.c_str());
    }
    ImGui::PopFont();

    thin_rule(2.0f, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##live_entries", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PushFont(nullptr, theme::font_small);
    if (live_log_lines_.empty())
        ImGui::TextColored(theme::vec(theme::text_faint), "Nothing sent yet.");
    for (const LiveLogEntry& entry : live_log_lines_) {
        ImGui::PushStyleColor(ImGuiCol_Text, entry.color);
        ImGui::TextWrapped("%s", entry.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (live_log_version_ != live_log_seen_) {
        live_log_seen_ = live_log_version_;
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(20))
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

void App::draw_live_release_key_setting() {
    ui::caption("Mouse release key");
    const int effective_key = live_release_key_ != 0 ? live_release_key_ : GLFW_KEY_ESCAPE;
    const float set_width = px(120);

    if (live_release_key_picking_) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::accent_text));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Press any key...");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, px(10));
        if (ui::button("Cancel", ImVec2(set_width, 0)))
            live_release_key_picking_ = false;
        gap(1);
        small_dim("Press the key mouse mode should release the pointer with, or Esc to cancel "
                  "without changing it.");
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", glfw_key_name(effective_key));
    ImGui::SameLine(0, px(10));
    if (ui::button("Set...", ImVec2(set_width, 0)))
        live_release_key_picking_ = true;
    if (live_release_key_ != 0) {
        ImGui::SameLine(0, px(6));
        if (ui::button("Reset to Esc", ImVec2(px(96), 0)))
            live_release_key_ = 0;
    }
    gap(1);
    if (live_release_key_ != 0) {
        small_colored(theme::warning,
                     std::string("Only ") + glfw_key_name(effective_key) +
                         " releases the captured pointer now — Escape no longer does. Every "
                         "other key, including the mouse buttons, still goes straight to the "
                         "phone while captured.");
    } else {
        small_dim("Escape releases the captured pointer by default. Set a different key if a "
                  "script or the app on the phone needs Escape itself.");
    }
}

void App::draw_live_controls() {
    ui::begin_card("##live_controls");
    const aoap::ProfileSetup setup = engine_.connected_setup();
    const uint32_t available = engine_.connected_profiles();
    const bool running = engine_.phase() == Phase::live;

    // Live control on or off.
    bool on = running;
    ImGui::BeginDisabled(!live_ready());
    if (ui::toggle("Live control", &on))
        live_enable(on);
    ImGui::EndDisabled();

    // What is forwarded; only what the connection actually has.
    struct Entry {
        const char* label;
        bool* flag;
        aoap::Profile profile;
    };
    const Entry entries[] = {
        {"Touch##live", &live_.touch, aoap::Profile::touch},
        {"Mouse##live", &live_.mouse, aoap::Profile::mouse},
        {"Keyboard##live", &live_.key, aoap::Profile::key},
        {"Gamepad##live", &live_.gamepad, aoap::Profile::gamepad},
    };
    for (const Entry& entry : entries) {
        const bool present = (available & aoap::profile_bit(entry.profile)) != 0U;
        ImGui::SameLine(0, px(18));
        ImGui::BeginDisabled(!present);
        if (ui::toggle(entry.label, entry.flag) && *entry.flag) {
            // Touch and mouse share the pointer, so turning one on turns the
            // other off.
            if (entry.flag == &live_.touch) {
                live_.mouse = false;
                live_capture_pointer(false);
            } else if (entry.flag == &live_.mouse) {
                live_.touch = false;
            }
        }
        ImGui::EndDisabled();
        if (!present && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("This profile is not part of the connection.");
    }

    // Orientation and shape.
    const float rotate_width = px(96);
    const float ratio_width = px(54);
    const float reset_width = px(64);
    const float gap_x = px(6);
    const float full_width = ImGui::GetFrameHeight();
    ImGui::SameLine();
    ui::align_right(full_width + rotate_width + ratio_width * 2 + reset_width + px(14) +
                    gap_x * 5);
    if (ui::icon_button("##fullscreen", ui::Icon::expand, full_width, ui::Tone::quiet,
                        "Fill the window with the preview"))
        live_fullscreen_ = true;
    ImGui::SameLine(0, gap_x);

    char turn[32];
    std::snprintf(turn, sizeof turn, "Rotate %d\xC2\xB0", (live_rotation_ & 3) * 90);
    if (ui::button(turn, ImVec2(rotate_width, 0)))
        live_rotation_ = (live_rotation_ + 1) & 3;
    ImGui::SetItemTooltip("Turn the preview a quarter turn; touches follow it.");

    // The shape as width:height, which is how a screen size is written.
    ImGui::SameLine(0, gap_x);
    const bool following = live_ratio_w_ <= 0 || live_ratio_h_ <= 0;
    int shown_w = live_ratio_w_;
    int shown_h = live_ratio_h_;
    if (following) {
        const aoap::ProfileSetup shape =
            engine_.connected() ? setup : build_setup();
        shown_w = shape.touch.enabled && shape.touch.width > 0 ? shape.touch.width : 9;
        shown_h = shape.touch.enabled && shape.touch.height > 0 ? shape.touch.height : 16;
    }
    ImGui::SetNextItemWidth(ratio_width);
    if (ImGui::InputInt("##ratio_w", &shown_w, 0, 0)) {
        live_ratio_w_ = std::clamp(shown_w, 1, 65536);
        live_ratio_h_ = live_ratio_h_ > 0 ? live_ratio_h_ : shown_h;
    }
    ImGui::SetItemTooltip("Preview width, in the same units as the height.");
    ImGui::SameLine(0, px(4));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(":");
    ImGui::SameLine(0, px(4));
    ImGui::SetNextItemWidth(ratio_width);
    if (ImGui::InputInt("##ratio_h", &shown_h, 0, 0)) {
        live_ratio_h_ = std::clamp(shown_h, 1, 65536);
        live_ratio_w_ = live_ratio_w_ > 0 ? live_ratio_w_ : shown_w;
    }
    ImGui::SetItemTooltip("Preview height. Touch positions come from the connected "
                          "resolution, not from this ratio.");
    ImGui::SameLine(0, gap_x);
    ImGui::BeginDisabled(following && live_rotation_ == 0);
    if (ui::button("Reset", ImVec2(reset_width, 0))) {
        live_ratio_w_ = 0;
        live_ratio_h_ = 0;
        live_rotation_ = 0;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back to the connected screen's ratio, upright.");

    // Types the clipboard's text on the phone as keystrokes; same trigger as
    // Ctrl+Shift+V.
    gap(2);
    const bool paste_ready = running && live_.key && setup.key.enabled;
    ImGui::BeginDisabled(!paste_ready || live_paste_active_.load(std::memory_order_relaxed));
    if (ui::button("Paste Text", ImVec2(px(110), 0)))
        live_paste_clipboard();
    ImGui::EndDisabled();
    if (!paste_ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Turn on Live control and Keyboard to paste.");
    else
        ImGui::SetItemTooltip("Types the clipboard's text on the phone. Ctrl+Shift+V");

    // Reference image: an optional picture over the preview (a screenshot
    // works well) to line touches up against, not saved between runs.
    gap(2);
    ui::caption("Reference image");
    const float image_button_width = px(64);
    ImGui::SetNextItemWidth(px(260));
    ImGui::InputTextWithHint("##live_image_path", "Path to a .png/.jpg/.bmp image...",
                             &live_image_path_input_);
    ImGui::SameLine(0, gap_x);
    if (ui::button("Load", ImVec2(image_button_width, 0)) && !live_image_path_input_.empty()) {
        const std::string error = live_image_.load(live_image_path_input_);
        if (!error.empty())
            log_.message(aoap::Severity::warning, error);
    }
    ImGui::SameLine(0, gap_x);
    ImGui::BeginDisabled(!live_image_.loaded());
    if (ui::button("Clear", ImVec2(image_button_width, 0)))
        live_image_.clear();
    ImGui::SameLine(0, px(18));
    ui::toggle("Lock image", &live_image_.locked);
    ImGui::EndDisabled();
    if (live_image_.loaded()) {
        small_dim(live_image_.locked
                      ? "Locked: touches and clicks pass straight through it."
                      : "Drag it to move, its corner handle to resize, its top handle to "
                        "rotate.");
    } else {
        small_dim("Load a screenshot, or drop an image file on the preview above.");
    }

    // Mouse mode's release key: Escape by default, but any key can take over
    // so it never collides with a key the script or the target app needs.
    gap(2);
    draw_live_release_key_setting();

    gap(2);
    if (running) {
        std::string hint;
        if (live_.touch)
            hint = "Drag inside the preview to touch the phone.";
        else if (live_.mouse) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer, "Pointer captured; the wheel scrolls. Press %s "
                                                 "to let it go.",
                         glfw_key_name(live_release_key_ != 0 ? live_release_key_
                                                              : GLFW_KEY_ESCAPE));
            hint = live_mouse_captured_ ? buffer : "Click inside the preview to capture the "
                                                    "pointer.";
        }
        if (live_.key)
            hint += hint.empty() ? "Keys go to the phone while this tab is open."
                                 : "  Keys go to the phone while this tab is open.";
        if (hint.empty())
            hint = "Turn on what you want to forward.";
        small_dim(hint.c_str());
    } else {
        small_dim("Live control sends the pointer, keyboard, and gamepad straight to the phone. "
                  "It stops when playback starts.");
    }
    ui::end_card();
}

} // namespace gui
