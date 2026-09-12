// SPDX-License-Identifier: MIT
#include "widgets.hpp"

#include "theme.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace gui::ui {
namespace {

constexpr float pi = 3.14159265358979f;

// Applies the style alpha, so custom drawing fades with BeginDisabled().
ImU32 faded(const ImU32 color) { return ImGui::GetColorU32(color); }

ImU32 with_alpha(const ImU32 color, const unsigned alpha) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

struct Colors {
    ImU32 fill;
    ImU32 fill_hover;
    ImU32 fill_active;
    ImU32 text;
};

Colors colors_for(const Tone tone) {
    switch (tone) {
    case Tone::primary:
        return {theme::accent, theme::accent_hover, theme::accent_active, IM_COL32_WHITE};
    case Tone::record:
        return {theme::field, theme::field_hover, theme::field_active, theme::text};
    case Tone::danger:
        return {theme::field, theme::danger_soft, theme::rgb(0xE0564E, 56), theme::danger};
    case Tone::quiet:
        return {0, theme::field_hover, theme::field_active, theme::text_dim};
    case Tone::secondary:
    default:
        return {theme::field, theme::field_hover, theme::field_active, theme::text};
    }
}

ImU32 state_fill(const Colors& colors, const bool hovered, const bool held) {
    return held ? colors.fill_active : hovered ? colors.fill_hover : colors.fill;
}

ImVec2 resolve_size(const ImVec2 size, const ImVec2 content) {
    ImVec2 result = size;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (result.x == 0.0f)
        result.x = content.x + px(28);
    else if (result.x < 0.0f)
        result.x = std::max(avail + result.x, px(10));
    if (result.y == 0.0f)
        result.y = ImGui::GetFrameHeight();
    return result;
}

} // namespace

float px(const float value) { return value * ImGui::GetStyle().FontScaleDpi; }

bool begin_card(const char* id, const char* title, const float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::surface);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(12)));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height == 0.0f)
        flags |= ImGuiChildFlags_AutoResizeY;
    const bool open = ImGui::BeginChild(id, ImVec2(0, height), flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (open && title != nullptr)
        card_title(title);
    return open;
}

void end_card() { ImGui::EndChild(); }

void card_title(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, theme::font_title);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void caption(const char* text) {
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_dim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void text_dim(const char* format, ...) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_dim);
    va_list args;
    va_start(args, format);
    ImGui::TextWrappedV(format, args);
    va_end(args);
    ImGui::PopStyleColor();
}

void text_colored(const ImU32 color, const char* format, ...) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    va_list args;
    va_start(args, format);
    ImGui::TextWrappedV(format, args);
    va_end(args);
    ImGui::PopStyleColor();
}

bool button(const char* label, const ImVec2 requested, const Tone tone) {
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 text_size = ImGui::CalcTextSize(label, label_end);
    const ImVec2 size = resolve_size(requested, text_size);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    const Colors colors = colors_for(tone);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImU32 fill = state_fill(colors, hovered, held);
    if ((fill & IM_COL32_A_MASK) != 0U)
        list->AddRectFilled(p0, p1, faded(fill), rounding);
    if (tone == Tone::danger && hovered)
        list->AddRect(p0, p1, faded(with_alpha(theme::danger, 90)), rounding);
    // The record tone leads with a red dot; the pair is centred together.
    const float dot = tone == Tone::record ? px(10) : 0.0f;
    const float lead = dot > 0.0f ? dot + px(9) : 0.0f;
    const float x = p0.x + (size.x - text_size.x - lead) * 0.5f;
    if (dot > 0.0f)
        list->AddCircleFilled(ImVec2(x + dot * 0.5f, p0.y + size.y * 0.5f), dot * 0.5f,
                              faded(theme::danger), 20);
    list->AddText(ImVec2(x + lead, p0.y + (size.y - text_size.y) * 0.5f), faded(colors.text),
                  label, label_end);
    return pressed;
}

void draw_icon(ImDrawList* list, const Icon icon, const ImVec2 c, const float s,
               const ImU32 color) {
    const float t = std::max(1.5f, s * 0.11f);
    switch (icon) {
    case Icon::play:
        list->AddTriangleFilled(ImVec2(c.x - s * 0.30f, c.y - s * 0.40f),
                                ImVec2(c.x - s * 0.30f, c.y + s * 0.40f),
                                ImVec2(c.x + s * 0.42f, c.y), color);
        break;
    case Icon::pause: {
        const float w = s * 0.20f;
        const float h = s * 0.38f;
        const float gap = s * 0.10f;
        list->AddRectFilled(ImVec2(c.x - gap - w, c.y - h), ImVec2(c.x - gap, c.y + h), color,
                            w * 0.25f);
        list->AddRectFilled(ImVec2(c.x + gap, c.y - h), ImVec2(c.x + gap + w, c.y + h), color,
                            w * 0.25f);
        break;
    }
    case Icon::stop: {
        const float h = s * 0.30f;
        list->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), color, h * 0.2f);
        break;
    }
    case Icon::restart: {
        const float h = s * 0.32f;
        list->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x - h + t * 1.3f, c.y + h), color,
                            t * 0.3f);
        list->AddTriangleFilled(ImVec2(c.x + h, c.y - h), ImVec2(c.x + h, c.y + h),
                                ImVec2(c.x - h + t * 1.6f, c.y), color);
        break;
    }
    case Icon::record:
        list->AddCircleFilled(c, s * 0.32f, color, 32);
        break;
    case Icon::refresh: {
        const float r = s * 0.34f;
        const float a0 = -pi * 0.35f;
        const float a1 = pi * 1.35f;
        list->PathArcTo(c, r, a0, a1, 24);
        list->PathStroke(color, t);
        const ImVec2 tip(c.x + r * std::cos(a0), c.y + r * std::sin(a0));
        const float h = s * 0.18f;
        list->AddTriangleFilled(ImVec2(tip.x - h * 0.2f, tip.y - h * 1.1f),
                                ImVec2(tip.x + h * 1.1f, tip.y + h * 0.1f),
                                ImVec2(tip.x - h * 0.6f, tip.y + h * 0.6f), color);
        break;
    }
    case Icon::chevron_down:
    case Icon::chevron_right: {
        const float h = s * 0.20f;
        ImVec2 points[3];
        if (icon == Icon::chevron_down) {
            points[0] = ImVec2(c.x - h * 1.4f, c.y - h * 0.6f);
            points[1] = ImVec2(c.x, c.y + h * 0.8f);
            points[2] = ImVec2(c.x + h * 1.4f, c.y - h * 0.6f);
        } else {
            points[0] = ImVec2(c.x - h * 0.6f, c.y - h * 1.4f);
            points[1] = ImVec2(c.x + h * 0.8f, c.y);
            points[2] = ImVec2(c.x - h * 0.6f, c.y + h * 1.4f);
        }
        list->AddPolyline(points, 3, color, t);
        break;
    }
    case Icon::close: {
        const float h = s * 0.24f;
        list->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), color, t);
        list->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), color, t);
        break;
    }
    case Icon::up:
    case Icon::down: {
        const float h = s * 0.22f;
        const float d = icon == Icon::up ? -1.0f : 1.0f;
        list->AddTriangleFilled(ImVec2(c.x - h * 1.2f, c.y - d * h * 0.6f),
                                ImVec2(c.x + h * 1.2f, c.y - d * h * 0.6f),
                                ImVec2(c.x, c.y + d * h * 0.8f), color);
        break;
    }
    case Icon::search: {
        const float r = s * 0.24f;
        const ImVec2 lens(c.x - s * 0.07f, c.y - s * 0.07f);
        list->AddCircle(lens, r, color, 20, t);
        const float d = r * 0.7071f;
        list->AddLine(ImVec2(lens.x + d, lens.y + d), ImVec2(c.x + s * 0.33f, c.y + s * 0.33f),
                      color, t * 1.1f);
        break;
    }
    case Icon::expand:
    case Icon::collapse: {
        // Two corner brackets, pointing out to enlarge and in to shrink.
        const float out = s * 0.34f;
        const float in = s * 0.12f;
        const float a = icon == Icon::expand ? out : in;
        const float b = icon == Icon::expand ? in : out;
        for (int corner = 0; corner < 2; ++corner) {
            const float sx = corner == 0 ? -1.0f : 1.0f;
            const ImVec2 tip(c.x + sx * a, c.y + sx * a);
            list->AddLine(tip, ImVec2(c.x + sx * b, c.y + sx * a), color, t);
            list->AddLine(tip, ImVec2(c.x + sx * a, c.y + sx * b), color, t);
        }
        break;
    }
    case Icon::trash: {
        const float w = s * 0.24f;
        const float h = s * 0.30f;
        // Lid with its handle, then the body.
        list->AddLine(ImVec2(c.x - w * 1.3f, c.y - h * 0.80f),
                      ImVec2(c.x + w * 1.3f, c.y - h * 0.80f), color, t);
        list->AddLine(ImVec2(c.x - w * 0.45f, c.y - h * 1.15f),
                      ImVec2(c.x + w * 0.45f, c.y - h * 1.15f), color, t);
        list->AddRect(ImVec2(c.x - w, c.y - h * 0.55f), ImVec2(c.x + w, c.y + h), color, t * 0.8f,
                      t);
        break;
    }
    case Icon::check: {
        const ImVec2 points[3] = {ImVec2(c.x - s * 0.26f, c.y + s * 0.02f),
                                  ImVec2(c.x - s * 0.07f, c.y + s * 0.20f),
                                  ImVec2(c.x + s * 0.27f, c.y - s * 0.18f)};
        list->AddPolyline(points, 3, color, std::max(1.5f, s * 0.12f));
        break;
    }
    }
}

void draw_check(ImDrawList* list, const ImVec2 p0, const float size, const bool checked) {
    const ImVec2 p1(p0.x + size, p0.y + size);
    const float rounding = size * 0.25f;
    if (checked) {
        list->AddRectFilled(p0, p1, faded(theme::accent), rounding);
        draw_icon(list, Icon::check, ImVec2(p0.x + size * 0.5f, p0.y + size * 0.5f), size,
                  faded(IM_COL32_WHITE));
    } else {
        list->AddRect(p0, p1, faded(theme::border_strong), rounding, px(1.5f));
    }
}

bool icon_button(const char* id, const Icon icon, const float diameter, const Tone tone,
                 const char* tooltip) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(diameter, diameter));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 c(p0.x + diameter * 0.5f, p0.y + diameter * 0.5f);
    ImDrawList* list = ImGui::GetWindowDrawList();

    const Colors colors = colors_for(tone);
    const ImU32 fill = state_fill(colors, hovered, held);
    if ((fill & IM_COL32_A_MASK) != 0U)
        list->AddCircleFilled(c, diameter * 0.5f, faded(fill), 48);
    const ImU32 glyph = tone == Tone::quiet && hovered ? theme::text : colors.text;
    const float scale = tone == Tone::quiet ? 0.62f : 0.48f;
    draw_icon(list, icon, c, diameter * scale, faded(glyph));
    if (tooltip != nullptr)
        ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

bool search_box(const char* id, std::string* text, const float width, const char* hint,
                const bool focus, bool* active_out) {
    ImGui::PushID(id);
    const float row = ImGui::GetFrameHeight();
    const float icon_room = px(28);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(icon_room, ImGui::GetStyle().FramePadding.y));
    if (focus)
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextItemAllowOverlap();
    const bool entered = ImGui::InputTextWithHint(
        "##field", hint, text,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll);
    const bool active = ImGui::IsItemActive();
    if (active_out != nullptr)
        *active_out = active;
    ImGui::PopStyleVar();

    ImDrawList* list = ImGui::GetWindowDrawList();
    draw_icon(list, Icon::search, ImVec2(p0.x + icon_room * 0.55f, p0.y + row * 0.5f), row * 0.72f,
              faded(active || !text->empty() ? theme::text_dim : theme::text_faint));
    if (!text->empty()) {
        const float size = row - px(8);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + width - size - px(4), p0.y + px(4)));
        if (ImGui::InvisibleButton("##clear", ImVec2(size, size)))
            text->clear();
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 c(p0.x + width - size * 0.5f - px(4), p0.y + row * 0.5f);
        if (hovered)
            list->AddCircleFilled(c, size * 0.5f, faded(theme::field_active), 20);
        draw_icon(list, Icon::close, c, size, faded(hovered ? theme::text : theme::text_dim));
        ImGui::SetItemTooltip("Clear (Esc)");
        // End on a zero-width item at the field's right edge, so the layout
        // (SameLine included) matches the field without the button.
        ImGui::SetCursorScreenPos(ImVec2(p0.x + width, p0.y));
        ImGui::Dummy(ImVec2(0, row));
    }
    ImGui::PopID();
    return entered;
}

bool escape_pressed() {
    // Read from the key's own state: an active text field owns Escape, and
    // the owner-aware queries would hide the press from the picker.
    const ImGuiKeyData* key = ImGui::GetKeyData(ImGuiKey_Escape);
    return key != nullptr && key->Down && key->DownDuration == 0.0f;
}

bool toggle(const char* label, bool* value) {
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 text_size = ImGui::CalcTextSize(label, label_end);
    const float row = ImGui::GetFrameHeight();
    const float height = std::round(row * 0.66f);
    const float width = std::round(height * 1.75f);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x + px(4);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width + spacing + text_size.x, row));
    if (pressed)
        *value = !*value;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 t0(p0.x, p0.y + (row - height) * 0.5f);
    const ImVec2 t1(t0.x + width, t0.y + height);
    const float radius = height * 0.5f;
    const ImU32 track = *value ? (hovered ? theme::accent_hover : theme::accent)
                               : (hovered ? theme::field_active : theme::field_hover);
    list->AddRectFilled(t0, t1, faded(track), radius);
    const float knob_x = *value ? t1.x - radius : t0.x + radius;
    list->AddCircleFilled(ImVec2(knob_x, t0.y + radius), radius - px(3),
                          faded(*value ? IM_COL32_WHITE : theme::text_dim), 24);
    list->AddText(ImVec2(t1.x + spacing, p0.y + (row - text_size.y) * 0.5f), faded(theme::text),
                  label, label_end);
    return pressed;
}

float pill_width(const char* text) {
    ImGui::PushFont(nullptr, theme::font_small);
    const float width = ImGui::CalcTextSize(text).x + px(3.5f) * 2 + px(24);
    ImGui::PopFont();
    return width;
}

void pill(const char* text, const ImU32 dot, const bool blink) {
    ImGui::PushFont(nullptr, theme::font_small);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float height = text_size.y + px(12);
    const float dot_r = px(3.5f);
    const ImVec2 size(text_size.x + dot_r * 2 + px(24), height);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    list->AddRectFilled(p0, p1, faded(theme::surface), height * 0.5f);
    list->AddRect(p0, p1, faded(theme::border), height * 0.5f);
    const ImVec2 c(p0.x + px(10) + dot_r, p0.y + height * 0.5f);
    ImU32 dot_color = dot;
    if (blink) {
        const float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.2) / 1.2);
        const float level = 0.45f + 0.55f * (0.5f + 0.5f * std::cos(phase * 2.0f * pi));
        dot_color = with_alpha(dot, static_cast<unsigned>(level * 255.0f));
    }
    list->AddCircleFilled(c, dot_r, faded(dot_color), 16);
    list->AddText(ImVec2(c.x + dot_r + px(7), p0.y + (height - text_size.y) * 0.5f),
                  faded(theme::text_dim), text);
    ImGui::PopFont();
}

void spinner(const float radius, const ImU32 color) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float row = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(radius * 2, row));
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + radius, p0.y + row * 0.5f);
    list->AddCircle(c, radius - px(1), faded(theme::field_active), 24, px(2));
    const float start = static_cast<float>(ImGui::GetTime() * 5.0);
    list->PathArcTo(c, radius - px(1), start, start + pi * 0.6f, 12);
    list->PathStroke(faded(color), px(2));
}

bool tabs(const char* id, const char* const* labels, const int count, int* current) {
    ImGui::PushID(id);
    const float height = ImGui::GetFrameHeight() + px(8);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    ImDrawList* list = ImGui::GetWindowDrawList();
    // A hairline under the whole strip; the current tab sits on it.
    list->AddLine(ImVec2(origin.x, origin.y + height - 0.5f),
                  ImVec2(origin.x + full, origin.y + height - 0.5f), faded(theme::border));
    bool changed = false;
    float x = origin.x;
    for (int index = 0; index < count; ++index) {
        const ImVec2 text_size = ImGui::CalcTextSize(labels[index]);
        const float width = text_size.x + px(24);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(index);
        if (ImGui::InvisibleButton("##tab", ImVec2(width, height)) && *current != index) {
            *current = index;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = *current == index;
        list->AddText(ImVec2(x + px(12), origin.y + (height - text_size.y) * 0.5f - px(1)),
                      faded(selected  ? theme::text
                            : hovered ? theme::text_dim
                                      : theme::text_faint),
                      labels[index]);
        if (selected) {
            list->AddRectFilled(ImVec2(x + px(8), origin.y + height - px(2)),
                                ImVec2(x + width - px(8), origin.y + height), faded(theme::accent),
                                px(1));
        }
        x += width + px(4);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(full, height));
    ImGui::PopID();
    return changed;
}

bool scrub_bar(const char* id, const float fraction, const float width, const float height,
               float* preview, ScrubState* state) {
    const float row = std::max(height + px(14), px(20));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, row));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool released = ImGui::IsItemDeactivated();
    const float mouse = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / width, 0.0f, 1.0f);
    if (active)
        *preview = mouse;
    if (state != nullptr) {
        state->hovered = hovered;
        state->dragging = active;
        state->hover_fraction = mouse;
    }

    const float shown = std::clamp(active ? *preview : fraction, 0.0f, 1.0f);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float cy = std::round(p0.y + row * 0.5f);
    const float radius = height * 0.5f;
    const ImVec2 t0(p0.x, cy - radius);
    const ImVec2 t1(p0.x + width, cy + radius);
    list->AddRectFilled(t0, t1, faded(theme::field_active), radius);
    const float fill_x = p0.x + width * shown;
    if (hovered && !active) {
        // Where a click would land.
        const float x = p0.x + width * mouse;
        list->AddLine(ImVec2(x, cy - height * 1.6f), ImVec2(x, cy + height * 1.6f),
                      faded(theme::text_dim), px(1));
    }
    if (fill_x > t0.x + 0.5f)
        list->AddRectFilled(t0, ImVec2(std::max(fill_x, t0.x + height), t1.y), faded(theme::accent),
                            radius);
    const float knob = (hovered || active) ? height * 1.5f : height * 1.2f;
    list->AddCircleFilled(ImVec2(fill_x, cy), knob, faded(IM_COL32(245, 245, 248, 255)), 24);
    return released;
}

bool slider(const char* id, float* value, const float minimum, const float maximum,
            const int decimals, const char* unit, const float step, const bool logarithmic) {
    const auto to_fraction = [&](const float v) {
        if (logarithmic)
            return std::log(v / minimum) / std::log(maximum / minimum);
        return (v - minimum) / (maximum - minimum);
    };
    const auto from_fraction = [&](const float f) {
        float v = logarithmic ? minimum * std::pow(maximum / minimum, f)
                              : minimum + (maximum - minimum) * f;
        if (step > 0.0f)
            v = std::round(v / step) * step;
        return std::clamp(v, minimum, maximum);
    };

    const auto print = [&](char* out, const size_t size, const float v) {
        std::snprintf(out, size, "%.*f%s", decimals, static_cast<double>(v), unit);
    };
    char text[48];
    print(text, sizeof text, *value);
    char widest[48];
    print(widest, sizeof widest, maximum);
    const float value_width = std::max(ImGui::CalcTextSize(widest).x, px(34));

    const float row = ImGui::GetFrameHeight();
    const float total = ImGui::GetContentRegionAvail().x;
    const float track_width = std::max(total - value_width - px(14), px(40));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float knob = px(7);
    ImGui::InvisibleButton(id, ImVec2(track_width, row));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active) {
        const float inner = track_width - knob * 2;
        const float f =
            std::clamp((ImGui::GetIO().MousePos.x - p0.x - knob) / std::max(inner, 1.0f), 0.0f,
                       1.0f);
        const float next = from_fraction(f);
        if (next != *value) {
            *value = next;
            changed = true;
            print(text, sizeof text, *value);
        }
    }

    ImDrawList* list = ImGui::GetWindowDrawList();
    const float cy = std::round(p0.y + row * 0.5f);
    const float track = px(4);
    const float x0 = p0.x + knob;
    const float x1 = p0.x + track_width - knob;
    const float x = x0 + (x1 - x0) * std::clamp(to_fraction(*value), 0.0f, 1.0f);
    list->AddRectFilled(ImVec2(x0, cy - track * 0.5f), ImVec2(x1, cy + track * 0.5f),
                        faded(theme::field_active), track * 0.5f);
    list->AddRectFilled(ImVec2(x0, cy - track * 0.5f), ImVec2(x, cy + track * 0.5f),
                        faded(theme::accent), track * 0.5f);
    const float r = (hovered || active) ? knob : knob - px(1);
    list->AddCircleFilled(ImVec2(x, cy), r, faded(IM_COL32(245, 245, 248, 255)), 24);

    ImGui::SameLine(0, px(14));
    ImGui::AlignTextToFramePadding();
    const float text_x = ImGui::GetCursorPosX() + value_width - ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(text_x);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::text : theme::text_dim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    return changed;
}

bool slider_int(const char* id, int* value, const int minimum, const int maximum,
                const char* unit) {
    float v = static_cast<float>(*value);
    const bool changed = slider(id, &v, static_cast<float>(minimum), static_cast<float>(maximum),
                                0, unit, 1.0f);
    if (changed)
        *value = static_cast<int>(std::lround(v));
    return changed;
}

void align_right(const float width) {
    ImGui::SameLine();
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
    ImGui::SetCursorPosX(std::max(x, ImGui::GetCursorPosX()));
}

} // namespace gui::ui
