// SPDX-License-Identifier: MIT
#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>

namespace gui::ui {

enum class Icon {
    play,
    pause,
    stop,
    restart,
    record,
    refresh,
    chevron_down,
    chevron_right,
    close,
    up,
    down,
    check,
    search,
    trash,
    expand,
    collapse
};

// What a button is for, which decides how loud it looks.
enum class Tone {
    primary,   // the one main action of a card: solid accent
    secondary, // everyday actions: quiet filled
    quiet,     // toolbar icons: no fill until hovered
    danger,    // destructive but routine (disconnect): quiet with red text
    record,    // starts a recording: quiet filled with a red record dot
};

// Unscaled pixels to current pixels, following the font's DPI scale.
float px(float value);

// A flat card with an optional title. Height 0 fits the contents; negative
// fills the remaining space minus that much.
bool begin_card(const char* id, const char* title = nullptr, float height = 0.0f);
void end_card();

// Card heading, aligned to a frame-height row so controls can sit beside it.
void card_title(const char* text);
// Small dim label above a control.
void caption(const char* text);
void text_dim(const char* format, ...) IM_FMTARGS(1);
void text_colored(ImU32 color, const char* format, ...) IM_FMTARGS(2);

// Width 0 fits the label, negative fills the row minus that much; height 0
// uses the frame height.
bool button(const char* label, ImVec2 size = ImVec2(0, 0), Tone tone = Tone::secondary);
// A round button with a drawn icon.
bool icon_button(const char* id, Icon icon, float diameter, Tone tone,
                 const char* tooltip = nullptr);

// A search field with a magnifier and, once there is text, a clear button.
// Esc also clears it; `focus` puts the caret in it. Returns true on Enter.
// `active` reports whether the field is being typed in.
bool search_box(const char* id, std::string* text, float width, const char* hint, bool focus,
                bool* active = nullptr);

// True on the frame Escape goes down, even while a text field holds it.
[[nodiscard]] bool escape_pressed();

// Switch with a label on its right.
bool toggle(const char* label, bool* value);

void draw_icon(ImDrawList* list, Icon icon, ImVec2 center, float size, ImU32 color);
// A check box drawn at `p0`, for custom rows.
void draw_check(ImDrawList* list, ImVec2 p0, float size, bool checked);

// Status label with a coloured dot; `blink` fades the dot (recording).
void pill(const char* text, ImU32 dot, bool blink = false);
[[nodiscard]] float pill_width(const char* text);

void spinner(float radius, ImU32 color);

// Text tabs with an accent underline under the current one.
bool tabs(const char* id, const char* const* labels, int count, int* current);

// A seekable progress bar. While dragging, `preview` follows the pointer;
// returns true on the frame the pointer is released, with `preview` set to
// the chosen fraction.
struct ScrubState {
    bool hovered{};
    bool dragging{};
    float hover_fraction{};
};
bool scrub_bar(const char* id, float fraction, float width, float height, float* preview,
               ScrubState* state = nullptr);

// A thin track with a round knob and the value printed to its right, filling
// the rest of the row, e.g. "10 fingers" or "1.50x". Click or drag to set;
// the value snaps to `step`. Returns true while the value changes.
bool slider(const char* id, float* value, float minimum, float maximum, int decimals,
            const char* unit, float step = 0.0f, bool logarithmic = false);
bool slider_int(const char* id, int* value, int minimum, int maximum, const char* unit);

// Right-aligns the next item of `width` in the current line.
void align_right(float width);

} // namespace gui::ui
