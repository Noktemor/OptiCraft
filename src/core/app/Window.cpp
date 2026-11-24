//
// Created by noktemor on 28.03.2026.
//

#include "utils/configs/Config_Render.h"
#include <glad/gl.h>
#include "core/app/Window.h"
#include "utils/logging/Logger.h"


Window::Window(int width, int height, const std::string& title)
    : m_window_ptr(nullptr)
    , m_width(width)
    , m_height(height)
    , m_title(title)
    , m_is_open(false)
{}

Window::~Window() {
    if (m_vk_window) glfwDestroyWindow(m_vk_window);
    if (m_gl_window) glfwDestroyWindow(m_gl_window);
    // glfwTerminate() вызывается в Game::~Game
}

bool Window::initialize(bool try_vulkan) {
    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    if (try_vulkan && glfwVulkanSupported()) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        m_vk_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
        if (m_vk_window) attach_callbacks(m_vk_window);
        else LOG_WARN("Vulkan window creation failed; selecting OpenGL");
    } else if (try_vulkan) {
        LOG_WARN("Vulkan loader unavailable; selecting OpenGL");
    }

    // Ask for a modern context first so EXT/NV mesh shading can be used when the driver
    // exposes it. Keep the GL 4.3 fallback because the existing compute mesher needs 4.3,
    // not 4.6; machines without mesh shaders therefore keep working unchanged.
    constexpr int requested_versions[][2] = {{4, 6}, {4, 5}, {4, 3}};
    for (const auto& version : requested_versions) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, version[0]);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, version[1]);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window_ptr = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
        if (m_window_ptr) {
            m_gl_window = m_window_ptr;
            LOG_INFO("Created OpenGL context request " + std::to_string(version[0]) + "." +
                     std::to_string(version[1]));
            break;
        }
    }
    if (!m_window_ptr) {
        LOG_ERROR("Failed to create an OpenGL 4.3+ core context");
        return false;
    }

    glfwMakeContextCurrent(m_window_ptr);
    // Make VSync explicit: if swap time remains near 16.6 ms with this disabled,
    // the remaining 60 FPS cap is imposed by the compositor or graphics driver.
    glfwSwapInterval(Config::vsync_enabled ? 1 : 0);
    LOG_INFO(std::string("VSync: ") + (Config::vsync_enabled ? "on" : "off"));

    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        LOG_ERROR("Failed to initialize GLAD");
        return false;
    }

    const auto* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    LOG_INFO(std::string("OpenGL driver version: ") + (gl_version ? gl_version : "unknown"));
    const auto* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    LOG_INFO(std::string("OpenGL vendor: ") + (gl_vendor ? gl_vendor : "unknown"));
    LOG_INFO(std::string("OpenGL renderer: ") + (gl_renderer ? gl_renderer : "unknown"));

    // Настройки OpenGL
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    attach_callbacks(m_gl_window);
    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetFramebufferSize(m_gl_window, &framebuffer_width, &framebuffer_height);
    update_framebuffer_size(framebuffer_width, framebuffer_height);
    m_is_open = true;
    return true;
}

void Window::attach_callbacks(GLFWwindow* window) {
    glfwSetWindowUserPointer(window, this);
    glfwSetCursorPosCallback(window, mouse_callback_static);
    glfwSetKeyCallback(window, key_callback_static);
    glfwSetCharCallback(window, char_callback_static);
    glfwSetScrollCallback(window, scroll_callback_static);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback_static);
}

void Window::select_vulkan(bool enabled) {
    GLFWwindow* next = enabled && m_vk_window ? m_vk_window : m_gl_window;
    if (!next) return;
    if (m_window_ptr && m_window_ptr != next) glfwHideWindow(m_window_ptr);
    m_window_ptr = next;
    // Keep the legacy resource context current, but never present it in Vulkan mode.
    glfwMakeContextCurrent(m_gl_window);
    int w=0,h=0;
    glfwGetFramebufferSize(next,&w,&h);
    update_framebuffer_size(w,h);
    glfwSetInputMode(next, GLFW_CURSOR, m_cursor_mode);
    glfwSetWindowTitle(next, m_title.c_str());
    glfwShowWindow(next);
    if (!enabled && m_vk_window) {
        glfwDestroyWindow(m_vk_window); m_vk_window = nullptr;
    }
}

bool Window::should_close() const {
    return glfwWindowShouldClose(m_window_ptr);
}

void Window::swap_buffers() {
    if (is_vulkan()) return;
    const double start_time = glfwGetTime();
    glfwSwapBuffers(m_window_ptr);
    const double elapsed = glfwGetTime() - start_time;
    m_swap_time_sum += elapsed;
    ++m_swap_time_samples;
    if (m_swap_time_samples == 120) {
        const double average_ms = (m_swap_time_sum / m_swap_time_samples) * 1000.0;
        LOG_INFO("Average glfwSwapBuffers time: " + std::to_string(average_ms) + " ms");
        m_swap_time_sum = 0.0;
        m_swap_time_samples = 0;
    }
}

void Window::poll_events() {
    glfwPollEvents();
}

void Window::set_cursor_mode(int mode) {
    m_cursor_mode = mode;
    glfwSetInputMode(m_window_ptr, GLFW_CURSOR, mode);
}

void Window::set_window_user_pointer(void* ptr) {
    glfwSetWindowUserPointer(m_window_ptr, ptr);
}

void Window::set_mouse_callback(std::function<void(double, double)> callback) {
    m_mouse_callback = callback;
}

void Window::set_key_callback(std::function<void(int, int, int)> callback) {
    m_key_callback = callback;
}

void Window::set_char_callback(std::function<void(unsigned int)> callback) {
    m_char_callback = callback;
}

void Window::set_scroll_callback(std::function<void(double, double)> callback) {
    m_scroll_callback = callback;
}

void Window::set_title(const std::string& title) {
    m_title = title;
    if (m_window_ptr) glfwSetWindowTitle(m_window_ptr, m_title.c_str());
}

// Статические коллбэки
void Window::scroll_callback_static(GLFWwindow* window, double xoffset, double yoffset) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_window_ptr == window && self->m_scroll_callback) {
        self->m_scroll_callback(xoffset, yoffset);
    }
}
void Window::mouse_callback_static(GLFWwindow* window, double xpos, double ypos) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_window_ptr == window && self->m_mouse_callback) {
        self->m_mouse_callback(xpos, ypos);
    }
}

void Window::key_callback_static(GLFWwindow* window, int key, int scancode, int action, int mods) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_window_ptr == window && self->m_key_callback) {
        self->m_key_callback(key, action, mods);
    }
}

void Window::char_callback_static(GLFWwindow* window, unsigned int codepoint) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_window_ptr == window && self->m_char_callback) {
        self->m_char_callback(codepoint);
    }
}

void Window::framebuffer_size_callback_static(GLFWwindow* window, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_window_ptr == window) {
        self->update_framebuffer_size(width, height);
    }
}

void Window::update_framebuffer_size(int width, int height) {
    m_width = width;
    m_height = height;
    if (!is_vulkan()) glViewport(0, 0, width, height);
}
