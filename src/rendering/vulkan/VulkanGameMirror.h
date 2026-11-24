#pragma once

#include "rendering/vulkan/VulkanLiveWorldRenderer.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <memory>

struct GLFWwindow;
class Block_Registry;

namespace OptiCraft::Vulkan {

// Primary Vulkan presentation path. Window owns the borrowed GLFW_NO_API window.
// A hidden legacy GL context remains for the unported game resource classes.
class VulkanGameMirror final {
public:
    VulkanGameMirror() = default;
    ~VulkanGameMirror() { shutdown(); }
    VulkanGameMirror(const VulkanGameMirror&) = delete;
    VulkanGameMirror& operator=(const VulkanGameMirror&) = delete;

    void initialize(GLFWwindow* window,const Block_Registry& registry);
    void beginGameFrame();
    void setWorld(const glm::mat4& gameOpenGlViewProjection,const glm::vec3& sky);
    void setMenuClear(const glm::vec3& color);
    void submitUi(std::span<const UiVertex> vertices,bool blocks){ui_.append(vertices,blocks);}
    void present();
    void shutdown() noexcept;
    bool closeRequested() const noexcept;

    VulkanGameChunkFeed::UploadSink uploadSink() { return scene_.uploadSink(); }
    VulkanGameChunkFeed::EvictSink evictSink() { return scene_.evictSink(); }
private:
    GLFWwindow* window_ = nullptr;
    VulkanFrameRuntime runtime_;
    VulkanLiveWorldRenderer scene_;
    UiBatch ui_;
    glm::mat4 viewProjection_{1.f};
    std::array<float,4> clear_{0.1f,0.1f,0.1f,1.f};
    bool drawWorld_=false;
};

} // namespace OptiCraft::Vulkan
