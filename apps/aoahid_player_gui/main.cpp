// SPDX-License-Identifier: MIT
//
// aoahid_player_gui — Dear ImGui front end. The window is redrawn only when
// something happens (input, a worker finishing, a status change), so an idle
// window costs no CPU; while playback or a recording is running it redraws at
// a modest fixed rate to animate progress.

#include "app.hpp"
#include "theme.hpp"

#include "aoahid_player/timing.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_opengl3_loader.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr double tick_seconds = 1.0 / 30.0;      // redraw rate while something animates
constexpr double hover_tick_window = 1.0;        // keep ticking briefly for tooltips
constexpr int64_t wayland_frame_ns = 8'333'333;  // frame cap where vsync is not used

std::string g_glfw_error;
gui::App* g_app = nullptr;
std::atomic<bool> g_scale_changed{false};
// Bumped by every input callback and every worker wake-up. A change means the
// next frames must be drawn even if the event itself was consumed while
// polling, so layout that lags one frame (auto-sized cards) always settles.
std::atomic<uint32_t> g_activity{0};

void bump() noexcept { g_activity.fetch_add(1, std::memory_order_relaxed); }

void fatal(const std::string& text) {
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    MessageBoxW(nullptr, wide.c_str(), L"AOA HID Player", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "aoahid_player_gui: %s\n", text.c_str());
#endif
}

void on_glfw_error(int, const char* description) {
    g_glfw_error = description != nullptr ? description : "";
}

void on_drop(GLFWwindow*, const int count, const char** paths) {
    bump();
    if (g_app == nullptr || count <= 0)
        return;
    std::vector<std::string> list;
    list.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
        list.emplace_back(paths[index]);
    g_app->on_drop(std::move(list));
}

void on_focus(GLFWwindow*, const int focused) {
    bump();
    if (g_app == nullptr)
        return;
    if (focused == GLFW_TRUE)
        g_app->on_focus();
    else
        g_app->on_focus_lost();
}

void on_content_scale(GLFWwindow*, float, float) {
    g_scale_changed.store(true);
    bump();
}

void on_cursor(GLFWwindow*, const double x, const double y) {
    bump();
    if (g_app != nullptr)
        g_app->on_cursor(x, y);
}
void on_button(GLFWwindow*, int, int, int) { bump(); }
void on_scroll(GLFWwindow*, double, double) { bump(); }
void on_key(GLFWwindow*, const int key, const int scancode, const int action, const int mods) {
    bump();
    if (g_app == nullptr)
        return;
    if (action == GLFW_PRESS && key == GLFW_KEY_F && (mods & GLFW_MOD_CONTROL) != 0) {
        g_app->on_search_shortcut();
        return;
    }
    if (action != GLFW_REPEAT)
        g_app->on_key(key, scancode, action == GLFW_PRESS);
}
void on_char(GLFWwindow*, unsigned int) { bump(); }
void on_refresh(GLFWwindow*) { bump(); }
void on_size(GLFWwindow*, int, int) { bump(); }

// Where the compositor scales the framebuffer (Wayland, macOS) the UI is laid
// out at 1.0 and ImGui renders at the framebuffer scale; elsewhere the style
// and fonts follow the monitor's content scale.
bool framebuffer_scaled() {
#if defined(__APPLE__)
    return true;
#else
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#endif
}

float monitor_scale(GLFWmonitor* monitor) {
    if (monitor == nullptr || framebuffer_scaled())
        return 1.0f;
    float x = 1.0f;
    float y = 1.0f;
    glfwGetMonitorContentScale(monitor, &x, &y);
    return std::clamp(x, 1.0f, 4.0f);
}

float window_scale(GLFWwindow* window) {
    if (framebuffer_scaled())
        return 1.0f;
    float x = 1.0f;
    float y = 1.0f;
    glfwGetWindowContentScale(window, &x, &y);
    return std::clamp(x, 1.0f, 4.0f);
}

int run() {
    glfwSetErrorCallback(on_glfw_error);
    if (glfwInit() != GLFW_TRUE) {
        fatal("The window system could not be initialised." +
              (g_glfw_error.empty() ? std::string() : "\n\n" + g_glfw_error));
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    float scale = monitor_scale(monitor);
    int width = static_cast<int>(1360 * scale);
    int height = static_cast<int>(900 * scale);
    if (monitor != nullptr) {
        int x = 0;
        int y = 0;
        int work_width = 0;
        int work_height = 0;
        glfwGetMonitorWorkarea(monitor, &x, &y, &work_width, &work_height);
        if (work_width > 0 && work_height > 0) {
            width = std::min(width, static_cast<int>(work_width * 0.92));
            height = std::min(height, static_cast<int>(work_height * 0.92));
        }
    }

    GLFWwindow* window = glfwCreateWindow(width, height, "AOA HID Player", nullptr, nullptr);
    if (window == nullptr) {
        fatal("The window could not be created. OpenGL 3.0 or newer is required; updating the "
              "graphics driver usually fixes this." +
              (g_glfw_error.empty() ? std::string() : "\n\n" + g_glfw_error));
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, std::min(width, static_cast<int>(1000 * scale)),
                            std::min(height, static_cast<int>(640 * scale)), GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);

    // Wayland may block a vsync'd swap for as long as the window is hidden, so
    // there the frame rate is capped by hand instead.
    const bool wayland = glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
    glfwSwapInterval(wayland ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing is saved between runs
    io.LogFilename = nullptr;
    io.ConfigInputTextCursorBlink = false; // a blinking caret would need constant redraws

    // Callbacks set before the backend installs its own are chained by it.
    glfwSetWindowFocusCallback(window, on_focus);
    glfwSetDropCallback(window, on_drop);
    glfwSetWindowContentScaleCallback(window, on_content_scale);
    glfwSetCursorPosCallback(window, on_cursor);
    glfwSetMouseButtonCallback(window, on_button);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
    glfwSetCharCallback(window, on_char);
    glfwSetWindowRefreshCallback(window, on_refresh);
    glfwSetFramebufferSizeCallback(window, on_size);

    scale = window_scale(window);
    gui::theme::setup(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        fatal("OpenGL could not be initialised. OpenGL 3.0 or newer is required.");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    auto app = std::make_unique<gui::App>([] {
        bump();
        glfwPostEmptyEvent();
    });
    g_app = app.get();

    const ImVec4 clear = gui::theme::vec(gui::theme::background);
    int pending_frames = 2;
    bool shown = false;
    double last_motion = 0.0;
    bool tick_for_hover = false;
    int64_t last_frame = 0;
    uint32_t seen_activity = g_activity.load(std::memory_order_relaxed);

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        if (pending_frames > 0) {
            glfwPollEvents();
            --pending_frames;
        } else if (app->animating() || tick_for_hover) {
            glfwWaitEventsTimeout(tick_seconds);
        } else {
            glfwWaitEvents();
        }
        const uint32_t activity = g_activity.load(std::memory_order_relaxed);
        if (activity != seen_activity) {
            // ImGui settles hover, clicks, and auto-sized layout over a
            // couple of frames after anything changes.
            seen_activity = activity;
            pending_frames = std::max(pending_frames, 2);
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE) {
            glfwWaitEventsTimeout(0.25);
            continue;
        }
        if (wayland) {
            const int64_t since = aoap::Timing::now_ns() - last_frame;
            if (since < wayland_frame_ns)
                std::this_thread::sleep_for(std::chrono::nanoseconds(wayland_frame_ns - since));
        }
        if (g_scale_changed.exchange(false)) {
            scale = window_scale(window);
            gui::theme::apply_style(scale);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app->frame();
        ImGui::Render();

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        last_frame = aoap::Timing::now_ns();

        const double now = ImGui::GetTime();
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
            last_motion = now;
        tick_for_hover = ImGui::IsAnyItemHovered() && now - last_motion < hover_tick_window;

        if (!shown) {
            // Shown after the first frame so the window never flashes white.
            glfwShowWindow(window);
            shown = true;
            pending_frames = 2;
        }
    }

    // Workers may still wake the window while they wind down, so the App goes
    // first and GLFW last.
    g_app = nullptr;
    glfwHideWindow(window);
    app.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        fatal(std::string("Unexpected error: ") + error.what());
        return 1;
    } catch (...) {
        fatal("Unexpected error.");
        return 1;
    }
}
