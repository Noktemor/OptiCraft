// OptiCraft 0.13.1 Vulkan presentation smoke test. This is intentionally NOT a
// replacement for Game until material/shape pipelines and world residency work.
// Launch OptiCraft_Vulkan from CLion: a real Vulkan window and frame submission.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "rendering/vulkan/VulkanFrameRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    using OptiCraft::Vulkan::VulkanFrameRuntime;
    auto preference = VulkanFrameRuntime::GpuPreference::Auto;
    bool validation = false;
    uint64_t maxFrames = 0; // interactive by default
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--integrated") preference = VulkanFrameRuntime::GpuPreference::Integrated;
        else if (arg == "--discrete") preference = VulkanFrameRuntime::GpuPreference::Discrete;
        else if (arg == "--validation") validation = true;
        else if (arg == "--frames" && i + 1 < argc) {
            const std::string count = argv[++i];
            long long parsed = 0;
            try { parsed = std::stoll(count); }
            catch (const std::exception&) { std::cerr << "--frames expects positive integer\n"; return 2; }
            if (parsed < 1) { std::cerr << "--frames expects positive integer\n"; return 2; }
            maxFrames = static_cast<uint64_t>(parsed);
        } else {
            std::cerr << "Usage: OptiCraft_Vulkan [--integrated|--discrete] [--validation] [--frames N]\n";
            return 2;
        }
    }
    if (!glfwInit()) { std::cerr << "Failed to initialize GLFW\n"; return 1; }
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OptiCraft 0.13.1 - Vulkan test 2 (presentation)", nullptr, nullptr);
    if (!window) { std::cerr << "Failed to create GLFW_NO_API window\n"; glfwTerminate(); return 1; }
    int code = 0;
    {
        VulkanFrameRuntime renderer;
        try {
            renderer.initialize(window, preference, validation);
            std::cout << "Vulkan device: " << renderer.gpuName()
                      << " | frames-in-flight: " << VulkanFrameRuntime::kFramesInFlight << '\n';
            while (!glfwWindowShouldClose(window) &&
                   (maxFrames == 0 || renderer.framesPresented() < maxFrames)) {
                glfwPollEvents();
                VulkanFrameRuntime::Frame frame;
                if (!renderer.beginFrame(frame)) {
                    int w = 0, h = 0;
                    glfwGetFramebufferSize(window, &w, &h);
                    if (!w || !h) glfwWaitEventsTimeout(0.1);
                    continue;
                }
                // Clear-only test: validates surface/present/frame fences/resize.
                // No dummy CPU mesh or fake world generation is introduced.
                const float phase = static_cast<float>(renderer.framesPresented() % 360) * 0.0174532925f;
                renderer.beginColorPass(frame, {0.06f + 0.035f*std::sin(phase),
                                                0.09f + 0.035f*std::cos(phase), 0.15f, 1.0f});
                renderer.endColorPass(frame);
                renderer.endFrame(frame);
            }
            std::cout << "Presented frames: " << renderer.framesPresented() << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Vulkan startup/frame failure: " << error.what() << '\n';
            code = 1;
        }
        renderer.shutdown();
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return code;
}
