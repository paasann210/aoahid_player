// SPDX-License-Identifier: MIT
#pragma once

// Small helpers shared by the App's source files: formatting, search,
// file-name rules, and a few layout shortcuts.

#include "engine.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "aoahid_player/paths.hpp"
#include "aoahid_player/spec_builder.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <future>
#include <string>
#include <string_view>
#include <vector>

namespace gui::detail {

using ui::px;
using Phase = Engine::Phase;

inline constexpr int64_t ns_per_ms = 1'000'000;
inline constexpr float label_width = 96.0f;

template <typename T>
inline bool ready(const std::future<T>& task) {
    return task.valid() && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

inline std::string format_time(int64_t ns) {
    ns = std::max<int64_t>(ns, 0);
    const int64_t total_ms = ns / ns_per_ms;
    char text[48];
    std::snprintf(text, sizeof text, "%" PRId64 ":%02" PRId64 ".%03" PRId64, total_ms / 60000,
                  (total_ms / 1000) % 60, total_ms % 1000);
    return text;
}

inline std::string format_count(const uint64_t value) {
    std::string digits = std::to_string(value);
    for (int index = static_cast<int>(digits.size()) - 3; index > 0; index -= 3)
        digits.insert(static_cast<size_t>(index), ",");
    return digits;
}

inline std::string axis_title(const aoahid_axis_role role) {
    const aoap::spec_detail::AxisIdentity* identity = aoap::spec_detail::find_axis(role);
    if (identity == nullptr)
        return "?";
    std::string name(identity->name);
    if (name.size() <= 2) {
        for (char& c : name)
            c = static_cast<char>(c - 32 * (c >= 'a' && c <= 'z'));
    } else {
        name[0] = static_cast<char>(name[0] - 32 * (name[0] >= 'a' && name[0] <= 'z'));
    }
    return name;
}

inline bool has_csv_extension(const std::string& path) {
    std::string extension = aoap::path_utf8(aoap::utf8_path(path).extension());
    for (char& c : extension)
        c = static_cast<char>(c + 32 * (c >= 'A' && c <= 'Z'));
    return extension == ".csv";
}

// Formats the Live tab's reference image can decode (stb_image supports
// more; this is the set worth advertising).
inline bool has_image_extension(const std::string& path) {
    std::string extension = aoap::path_utf8(aoap::utf8_path(path).extension());
    for (char& c : extension)
        c = static_cast<char>(c + 32 * (c >= 'A' && c <= 'Z'));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".bmp";
}

// ASCII case folding for search; other bytes compare as they are.
inline std::string fold(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(c + 32 * (c >= 'A' && c <= 'Z'));
    return out;
}

inline std::vector<std::string> search_terms(const std::string& query) {
    std::vector<std::string> terms;
    std::string term;
    for (const char c : fold(query)) {
        if (c == ' ' || c == '\t') {
            if (!term.empty())
                terms.push_back(std::move(term));
            term.clear();
        } else {
            term += c;
        }
    }
    if (!term.empty())
        terms.push_back(std::move(term));
    return terms;
}

// "1.2 KB  ·  2026-09-11 06:20" for the picker; empty when unreadable.
inline std::string file_meta(const std::string& path) {
    std::error_code code;
    const std::filesystem::path file = aoap::utf8_path(path);
    const uintmax_t bytes = std::filesystem::file_size(file, code);
    if (code)
        return {};
    char size[32];
    if (bytes < 1024)
        std::snprintf(size, sizeof size, "%u B", static_cast<unsigned>(bytes));
    else if (bytes < 1024 * 1024)
        std::snprintf(size, sizeof size, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(bytes) / 1048576.0);

    const auto written = std::filesystem::last_write_time(file, code);
    if (code)
        return size;
    // file_clock has no portable conversion before C++20 clock_cast is
    // everywhere; shifting by "now" on both clocks is accurate enough here.
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
    return std::string(size) + "  \xC2\xB7  " + when;
}

inline bool ends_with_csv(const std::string& name) {
    return name.size() >= 4 && fold(std::string_view(name).substr(name.size() - 4)) == ".csv";
}

// Why `name` cannot be a recording's file name, or empty when it can. The
// rules are the union of what Linux and Windows accept, so a recording
// made on one opens on the other.
inline std::string record_name_problem(const std::string& name) {
    if (name.empty())
        return {};
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) !=
                                                          std::string_view::npos)
            return "Use a plain name without / \\ : * ? \" < > |";
    }
    if (name.back() == '.' || name.back() == ' ' || name.front() == ' ')
        return "The name cannot start with a space or end with a space or a dot.";
    if (name.size() > 120)
        return "The name is too long.";
    std::string stem = fold(ends_with_csv(name) ? name.substr(0, name.size() - 4) : name);
    stem = stem.substr(0, stem.find('.'));
    static constexpr std::string_view reserved[] = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const std::string_view word : reserved) {
        if (stem == word)
            return "\"" + name + "\" is reserved on Windows; choose another name.";
    }
    if (stem.empty())
        return "Type a name before the dot.";
    return {};
}

// The file name a typed name saves as: ".csv" is added unless it is there.
inline std::string record_file_name(const std::string& name) {
    return ends_with_csv(name) ? name : name + ".csv";
}

// A dim label in a fixed column, then the cursor for the control after it.
inline void field(const char* label) {
    const float x = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(x + px(label_width));
}

inline void small_dim(const char* text) {
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_dim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

inline void small_colored(const ImU32 color, const std::string& text) {
    ImGui::PushFont(nullptr, theme::font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// A card heading on the left of a row with room for controls on its right.
inline void caption_row(const char* text) { ui::card_title(text); }

inline void gap(const float height) { ImGui::Dummy(ImVec2(0, px(height))); }

inline void thin_rule(const float before = 2.0f, const float after = 6.0f) {
    gap(before);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                                        ImGui::GetColorU32(theme::border));
    gap(after);
}


} // namespace gui::detail
