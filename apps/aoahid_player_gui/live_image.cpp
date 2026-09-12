// SPDX-License-Identifier: MIT
#include "live_image.hpp"

#include "widgets.hpp"

#include "aoahid_player/paths.hpp"

#include <imgui_impl_opengl3_loader.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include <stb_image.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace gui {
namespace {

using ui::px;

// Handles sit a little outside the image itself; both are generous so they
// stay easy to grab even on a small preview.
constexpr float rotate_handle_gap = 26.0f;
constexpr float handle_hit_radius = 15.0f;
constexpr float click_margin = 48.0f; // lets the rotate handle poke past the edge
constexpr float min_half_width_frac = 0.02f;
constexpr float max_half_width_frac = 4.0f;

float distance(const ImVec2 a, const ImVec2 b) noexcept {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// Reads the whole file into memory; stb_image is built with STBI_NO_STDIO so
// decoding never has to guess how to open a UTF-8 path on Windows, the same
// filesystem boundary the rest of the project crosses through utf8_path().
bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream file(aoap::utf8_path(path), std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const std::streamoff size = file.tellg();
    if (size < 0)
        return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!out.empty() && !file.read(reinterpret_cast<char*>(out.data()),
                                   static_cast<std::streamsize>(out.size())))
        return false;
    return true;
}

} // namespace

LiveImageOverlay::~LiveImageOverlay() { clear(); }

std::string LiveImageOverlay::load(const std::string& path) {
    std::vector<unsigned char> bytes;
    if (!read_file(path, bytes) || bytes.empty())
        return "Could not read \"" + path + "\".";

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                  &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return "Could not decode \"" + path + "\"" +
               (reason != nullptr ? std::string(": ") + reason : std::string()) + ".";
    }

    clear();
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    texture_ = texture;
    pixel_width_ = width;
    pixel_height_ = height;
    path_ = path;
    center_ = ImVec2(0.5f, 0.5f);
    half_width_frac_ = 0.35f;
    rotation_ = 0.0f;
    dragging_ = Drag::none;
    return {};
}

void LiveImageOverlay::clear() {
    if (texture_ != 0) {
        const GLuint texture = static_cast<GLuint>(texture_);
        glDeleteTextures(1, &texture);
        texture_ = 0;
    }
    pixel_width_ = 0;
    pixel_height_ = 0;
    path_.clear();
    dragging_ = Drag::none;
}

bool LiveImageOverlay::interact(const ImVec2 surface_min, const ImVec2 surface_size) {
    if (!loaded() || locked) {
        dragging_ = Drag::none;
        return false;
    }

    const ImVec2 center_px(surface_min.x + center_.x * surface_size.x,
                           surface_min.y + center_.y * surface_size.y);
    const float half_w = half_width_frac_ * surface_size.x;
    const float half_h = half_w * (static_cast<float>(pixel_height_) /
                                   static_cast<float>(std::max(pixel_width_, 1)));
    const float cosr = std::cos(rotation_);
    const float sinr = std::sin(rotation_);
    const auto to_screen = [&](const ImVec2 local) {
        return ImVec2(center_px.x + local.x * cosr - local.y * sinr,
                      center_px.y + local.x * sinr + local.y * cosr);
    };
    const ImVec2 resize_handle = to_screen(ImVec2(half_w, half_h));
    const ImVec2 rotate_handle = to_screen(ImVec2(0.0f, -half_h - px(rotate_handle_gap)));

    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (dragging_ == Drag::none) {
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            return false;
        const ImVec2 mouse = io.MousePos;
        // A coarse gate first, so a click nowhere near the preview never
        // starts a drag just because the math below would technically hit.
        if (mouse.x < surface_min.x - px(click_margin) ||
            mouse.x > surface_min.x + surface_size.x + px(click_margin) ||
            mouse.y < surface_min.y - px(click_margin) ||
            mouse.y > surface_min.y + surface_size.y + px(click_margin))
            return false;

        const float hit_radius = px(handle_hit_radius);
        if (distance(mouse, rotate_handle) <= hit_radius) {
            dragging_ = Drag::rotate;
            drag_start_rotation_ = rotation_;
            drag_start_angle_ = std::atan2(mouse.y - center_px.y, mouse.x - center_px.x);
            return true;
        }
        if (distance(mouse, resize_handle) <= hit_radius) {
            dragging_ = Drag::resize;
            drag_start_half_width_ = half_width_frac_;
            drag_start_distance_ = std::max(1.0f, distance(mouse, center_px));
            return true;
        }
        // Inside the (possibly rotated) rectangle: undo the rotation, then
        // it is an ordinary axis-aligned bounds check.
        const float dx = mouse.x - center_px.x;
        const float dy = mouse.y - center_px.y;
        const float local_x = dx * cosr + dy * sinr;
        const float local_y = -dx * sinr + dy * cosr;
        if (std::fabs(local_x) > half_w || std::fabs(local_y) > half_h)
            return false;
        dragging_ = Drag::move;
        drag_offset_ = ImVec2(dx, dy);
        return true;
    }

    if (!mouse_down || !ImGui::IsMousePosValid(&io.MousePos)) {
        // The second condition also covers the pointer being captured for
        // mouse mode mid-drag (its io.MousePos is pinned to ImGui's "no
        // mouse" sentinel — see App::frame()): the drag simply stops rather
        // than snapping to whatever that sentinel resolves to.
        dragging_ = Drag::none;
        return true; // still consume the release frame
    }
    const ImVec2 mouse = io.MousePos;
    switch (dragging_) {
    case Drag::move: {
        const ImVec2 new_center(mouse.x - drag_offset_.x, mouse.y - drag_offset_.y);
        center_.x = std::clamp((new_center.x - surface_min.x) / surface_size.x, 0.0f, 1.0f);
        center_.y = std::clamp((new_center.y - surface_min.y) / surface_size.y, 0.0f, 1.0f);
        break;
    }
    case Drag::resize: {
        const float dist = std::max(1.0f, distance(mouse, center_px));
        half_width_frac_ = std::clamp(drag_start_half_width_ * (dist / drag_start_distance_),
                                      min_half_width_frac, max_half_width_frac);
        break;
    }
    case Drag::rotate: {
        const float angle = std::atan2(mouse.y - center_px.y, mouse.x - center_px.x);
        rotation_ = drag_start_rotation_ + (angle - drag_start_angle_);
        break;
    }
    case Drag::none:
        break;
    }
    return true;
}

void LiveImageOverlay::draw(ImDrawList* list, const ImVec2 surface_min,
                           const ImVec2 surface_size) const {
    if (!loaded())
        return;

    const ImVec2 center_px(surface_min.x + center_.x * surface_size.x,
                           surface_min.y + center_.y * surface_size.y);
    const float half_w = half_width_frac_ * surface_size.x;
    const float half_h = half_w * (static_cast<float>(pixel_height_) /
                                   static_cast<float>(std::max(pixel_width_, 1)));
    const float cosr = std::cos(rotation_);
    const float sinr = std::sin(rotation_);
    const auto to_screen = [&](const ImVec2 local) {
        return ImVec2(center_px.x + local.x * cosr - local.y * sinr,
                      center_px.y + local.x * sinr + local.y * cosr);
    };
    const ImVec2 tl = to_screen(ImVec2(-half_w, -half_h));
    const ImVec2 tr = to_screen(ImVec2(half_w, -half_h));
    const ImVec2 br = to_screen(ImVec2(half_w, half_h));
    const ImVec2 bl = to_screen(ImVec2(-half_w, half_h));

    list->AddImageQuad(texture_, tl, tr, br, bl);
    if (locked)
        return;

    const ImU32 outline = IM_COL32(255, 255, 255, 210);
    const ImU32 handle_fill = IM_COL32(64, 170, 255, 255);
    list->AddQuad(tl, tr, br, bl, outline, px(1.5f));

    const ImVec2 resize_handle = to_screen(ImVec2(half_w, half_h));
    const ImVec2 rotate_handle = to_screen(ImVec2(0.0f, -half_h - px(rotate_handle_gap)));
    list->AddLine(to_screen(ImVec2(0.0f, -half_h)), rotate_handle, outline, px(1.5f));

    const float handle_radius = px(7.0f);
    list->AddCircleFilled(resize_handle, handle_radius, handle_fill, 16);
    list->AddCircle(resize_handle, handle_radius, outline, 16, px(1.5f));
    list->AddCircleFilled(rotate_handle, handle_radius, handle_fill, 16);
    list->AddCircle(rotate_handle, handle_radius, outline, 16, px(1.5f));
}

} // namespace gui
