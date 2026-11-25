#include "rendering/vulkan/VulkanGameMirror.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
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
        VulkanLiveWorldRenderer::Config config;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(runtime_.physicalDevice(),&props);
        config.streaming.maxResident=4096;
        if(const char* slots=std::getenv("OPTICRAFT_VULKAN_MAX_RESIDENT")){
            char* end=nullptr;const auto value=std::strtoul(slots,&end,10);
            if(end!=slots && *end=='\0' && value>=8u && value<=4096u)
                config.streaming.maxResident=uint32_t(value);
        }
        config.streaming.uploadsPerFrame=8;
        config.meshing.batchLimit=16;
        if(const char* batches=std::getenv("OPTICRAFT_VULKAN_MESH_BATCHES"))
            if(std::string_view(batches)=="1")config.meshing.activeBatches=1;
        if(const char* cache=std::getenv("OPTICRAFT_VULKAN_MESH_CACHE"))
            if(std::string_view(cache)=="0")config.meshing.opaqueCacheRects=0;
        if(const char* coarse=std::getenv("OPTICRAFT_VULKAN_COARSE_CACHE"))
            if(std::string_view(coarse)=="0")config.meshing.coarseCache=false;
        if(const char* compact=std::getenv("OPTICRAFT_VULKAN_PLANE_COMPACTION"))
            if(std::string_view(compact)=="0")config.meshing.planeCompaction=false;
        if(const char* fast=std::getenv("OPTICRAFT_VULKAN_FAST_OPAQUE"))
            if(std::string_view(fast)=="0")config.meshing.fastOpaque=false;
        if(const char* mixed=std::getenv("OPTICRAFT_VULKAN_FAST_MIXED"))
            if(std::string_view(mixed)=="0")config.meshing.fastMiscQuads=0;
        config.meshing.workBudget=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?16u:32u;
        scene_.initialize(runtime_, registry, shaders,
                          "assets/texture/texture_atlas.png", config);
    } catch (...) {
        shutdown();
        throw;
    }
}

void VulkanGameMirror::beginGameFrame(){
    ui_.clear();items_.clear();entities_.clear();drawWorld_=false;clear_={0.1f,0.1f,0.1f,1.f};
}
void VulkanGameMirror::setWorld(const glm::mat4& gameOpenGlViewProjection,const glm::vec3& sky,const glm::vec3& eye,float ambient,float fogEnd,const glm::vec3& sunDirection,const std::array<float,4>& heldLight){
    viewProjection_=gameOpenGlViewProjection;drawWorld_=true;clear_={sky.r,sky.g,sky.b,1.f};
    worldView_.eyeAmbient={eye.x,eye.y,eye.z,std::clamp(ambient,0.f,1.f)};
    fogEnd=std::max(1.f,fogEnd);
    worldView_.fogColorStart={sky.r,sky.g,sky.b,fogEnd*0.70f};
    worldView_.fogEnd={fogEnd,sunDirection.x,sunDirection.y,sunDirection.z};
    worldView_.heldLight=heldLight;
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
    std::copy_n(data,16,worldView_.vp.begin());
    scene_.recordFrame(runtime_, frame, worldView_, frustum,drawWorld_,clear_,ui_,items_,entities_);
    runtime_.endFrame(frame);
}

bool VulkanGameMirror::closeRequested() const noexcept {
    return window_ && glfwWindowShouldClose(window_);
}

void VulkanGameMirror::detachWorld(){
    if(scene_.hasOwnedWorld()){if(runtime_.device())vkDeviceWaitIdle(runtime_.device());scene_.detachWorldIdle();}
}
void VulkanGameMirror::shutdown() noexcept {
    if (runtime_.device()) vkDeviceWaitIdle(runtime_.device());
    scene_.shutdown();
    runtime_.shutdown();
    window_ = nullptr;
}

} // namespace OptiCraft::Vulkan
