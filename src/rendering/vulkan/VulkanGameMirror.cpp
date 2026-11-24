#include "rendering/vulkan/VulkanGameMirror.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <filesystem>
#include <cstdlib>
#include <string_view>
#include "utils/configs/Config_Render.h"

namespace OptiCraft::Vulkan {

void VulkanGameMirror::initialize(GLFWwindow* window,const Block_Registry& registry) {
    if (window_ || !window) throw std::logic_error("Vulkan game window invalid");
    window_ = window; // borrowed; Window owns and destroys it after Vulkan shutdown
    try {
        auto gpu = VulkanFrameRuntime::GpuPreference::Auto;
        if (const char* choice=std::getenv("OPTICRAFT_VULKAN_GPU")) {
            if (std::string_view(choice)=="integrated") gpu=VulkanFrameRuntime::GpuPreference::Integrated;
            else if (std::string_view(choice)=="discrete") gpu=VulkanFrameRuntime::GpuPreference::Discrete;
        }
        const char* validation=std::getenv("OPTICRAFT_VULKAN_VALIDATION");
        runtime_.initialize(window_,gpu,validation && std::string_view(validation)=="1",Config::vsync_enabled);
        // Game's resource directory already contains the full existing atlas.
        // SPIR-V is compiled by CMake at build time into this directory.
        std::filesystem::path shaders = "assets/vulkan_shaders";
#if defined(OPTICRAFT_VULKAN_SHADER_DIR)
        // CLion can launch from the project directory instead of the build
        // directory. Prefer the generated SPIR-V next to the CMake build.
        if (!std::filesystem::exists(shaders / "voxel_count.comp.spv"))
            shaders = OPTICRAFT_VULKAN_SHADER_DIR;
#endif
        scene_.initialize(runtime_, registry, shaders,
                          "assets/texture/texture_atlas.png", {});
    } catch (...) {
        shutdown();
        throw;
    }
}

void VulkanGameMirror::beginGameFrame(){
    ui_.clear();drawWorld_=false;clear_={0.1f,0.1f,0.1f,1.f};
}
void VulkanGameMirror::setWorld(const glm::mat4& gameOpenGlViewProjection,const glm::vec3& sky){
    viewProjection_=gameOpenGlViewProjection;drawWorld_=true;clear_={sky.r,sky.g,sky.b,1.f};
}
void VulkanGameMirror::setMenuClear(const glm::vec3& color){
    drawWorld_=false;ui_.clear();clear_={color.r,color.g,color.b,1.f};
}
void VulkanGameMirror::present() {
    if (!window_ || closeRequested()) return;
    VulkanFrameRuntime::Frame frame{};
    if (!runtime_.beginFrame(frame)) return;
    glm::mat4 clip(1.0f);
    clip[1][1] = -1.0f;
    clip[2][2] = 0.5f;
    clip[3][2] = 0.5f;
    const glm::mat4 vp = clip * viewProjection_;
    const float* data = glm::value_ptr(vp);
    const auto frustum = GpuFrustum::fromVulkanViewProjection(data);
    scene_.recordFrame(runtime_, frame, data, frustum,drawWorld_,clear_,ui_);
    runtime_.endFrame(frame);
}

bool VulkanGameMirror::closeRequested() const noexcept {
    return window_ && glfwWindowShouldClose(window_);
}

void VulkanGameMirror::shutdown() noexcept {
    if (runtime_.device()) vkDeviceWaitIdle(runtime_.device());
    scene_.shutdown();
    runtime_.shutdown();
    window_ = nullptr;
}

} // namespace OptiCraft::Vulkan
