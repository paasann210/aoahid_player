// SPDX-License-Identifier: MIT
#include "theme.hpp"

#include <cstdio>

// Generated at build time from Dear ImGui's bundled Roboto-Medium.ttf.
extern const unsigned char aoahid_player_font_data[];
extern const unsigned int aoahid_player_font_size;

namespace gui::theme {

void apply_style(const float dpi_scale) {
    ImGuiStyle style;
    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(10, 7);
    style.CellPadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(10, 9);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 22;
    style.ScrollbarSize = 9;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.WindowRounding = 0;
    style.ChildRounding = 10;
    style.FrameRounding = 6;
    style.PopupRounding = 8;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 4;
    style.TabRounding = 6;
    style.SeparatorTextBorderSize = 1;
    style.SelectableTextAlign = ImVec2(0, 0.5f);
    style.DisabledAlpha = 0.4f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = vec(text);
    c[ImGuiCol_TextDisabled] = vec(text_faint);
    c[ImGuiCol_WindowBg] = vec(background);
    c[ImGuiCol_ChildBg] = vec(surface);
    c[ImGuiCol_PopupBg] = vec(rgb(0x1A1B20));
    c[ImGuiCol_Border] = vec(border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = vec(field);
    c[ImGuiCol_FrameBgHovered] = vec(field_hover);
    c[ImGuiCol_FrameBgActive] = vec(field_active);
    c[ImGuiCol_TitleBg] = vec(background);
    c[ImGuiCol_TitleBgActive] = vec(background);
    c[ImGuiCol_TitleBgCollapsed] = vec(background);
    c[ImGuiCol_MenuBarBg] = vec(surface);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(border_strong);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(rgb(0x44454E));
    c[ImGuiCol_ScrollbarGrabActive] = vec(rgb(0x55565F));
    c[ImGuiCol_CheckMark] = vec(rgb(0xFFFFFF));
    c[ImGuiCol_SliderGrab] = vec(accent);
    c[ImGuiCol_SliderGrabActive] = vec(accent_hover);
    c[ImGuiCol_Button] = vec(field);
    c[ImGuiCol_ButtonHovered] = vec(field_hover);
    c[ImGuiCol_ButtonActive] = vec(field_active);
    c[ImGuiCol_Header] = vec(accent_soft);
    c[ImGuiCol_HeaderHovered] = vec(field_hover);
    c[ImGuiCol_HeaderActive] = vec(field_active);
    c[ImGuiCol_Separator] = vec(border);
    c[ImGuiCol_SeparatorHovered] = vec(accent);
    c[ImGuiCol_SeparatorActive] = vec(accent);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = vec(surface);
    c[ImGuiCol_TabHovered] = vec(field_hover);
    c[ImGuiCol_TabSelected] = vec(field);
    c[ImGuiCol_TextSelectedBg] = vec(rgb(0x6E5FD1, 110));
    c[ImGuiCol_NavCursor] = vec(accent);
    c[ImGuiCol_ModalWindowDimBg] = vec(rgb(0x000000, 140));

    style.ScaleAllSizes(dpi_scale);
    style.FontSizeBase = font_body;
    style.FontScaleDpi = dpi_scale;
    ImGui::GetStyle() = style;
}

void setup(const float dpi_scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // static data, never freed
    config.OversampleH = 2;
    std::snprintf(config.Name, sizeof config.Name, "Roboto Medium");
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(aoahid_player_font_data),
                                   static_cast<int>(aoahid_player_font_size), font_body, &config);
    apply_style(dpi_scale);
}

} // namespace gui::theme
