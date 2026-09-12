// SPDX-License-Identifier: MIT
#pragma once

#include <imgui.h>

namespace gui::theme {

// Flat dark palette: neutral charcoal surfaces in a few steps, one calm
// purple accent for what can be pressed or is selected, and muted status
// colours. No gradients, glows, or shadows.
inline constexpr ImU32 rgb(const unsigned hex, const unsigned alpha = 255U) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha);
}

inline constexpr ImU32 background = rgb(0x0E0F12);
inline constexpr ImU32 surface = rgb(0x16171B);    // cards
inline constexpr ImU32 surface_hi = rgb(0x1C1D22); // rows inside cards
inline constexpr ImU32 field = rgb(0x212228);      // inputs, tracks, quiet buttons
inline constexpr ImU32 field_hover = rgb(0x292A31);
inline constexpr ImU32 field_active = rgb(0x31323A);
inline constexpr ImU32 border = rgb(0x26272E);
inline constexpr ImU32 border_strong = rgb(0x34353D);
inline constexpr ImU32 text = rgb(0xECECF1);
inline constexpr ImU32 text_dim = rgb(0x9D9EA9);
inline constexpr ImU32 text_faint = rgb(0x696A75);

// White text on `accent` is about 4.9:1.
inline constexpr ImU32 accent = rgb(0x6E5FD1);
inline constexpr ImU32 accent_hover = rgb(0x7A6CDA);
inline constexpr ImU32 accent_active = rgb(0x6152C2);
inline constexpr ImU32 accent_text = rgb(0xAEA4F2); // accent used as text on dark
inline constexpr ImU32 accent_soft = rgb(0x6E5FD1, 40);
inline constexpr ImU32 accent_line = rgb(0x6E5FD1, 130);

inline constexpr ImU32 success = rgb(0x4DB885);
inline constexpr ImU32 warning = rgb(0xD8A445);
inline constexpr ImU32 danger = rgb(0xE0564E);
inline constexpr ImU32 danger_soft = rgb(0xE0564E, 36);

inline ImVec4 vec(const ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

// Font sizes in unscaled pixels; DPI scaling is applied by ImGui. Roboto's
// digits are tabular, so running clocks and counters do not jitter.
inline constexpr float font_body = 15.0f;
inline constexpr float font_small = 12.5f;
inline constexpr float font_title = 15.5f;
inline constexpr float font_display = 30.0f;

// Loads the embedded font and applies the style for `dpi_scale`.
void setup(float dpi_scale);
// Re-applies the style after the window moved to a monitor with another scale.
void apply_style(float dpi_scale);

} // namespace gui::theme
