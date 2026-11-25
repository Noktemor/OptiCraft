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
    void setWorld(const glm::mat4& gameOpenGlViewProjection,const glm::vec3& sky,const glm::vec3& eye,float ambient,float fogEnd,const glm::vec3& sunDirection,const std::array<float,4>& heldLight);
    void setMenuClear(const glm::vec3& color);
    void submitUi(std::span<const UiVertex> vertices,bool blocks){ui_.append(vertices,blocks);}
    template<class Vertex> void submitItems(const std::vector<Vertex>& vertices){
        items_.clear();items_.reserve(vertices.size());
        for(const auto& v:vertices)items_.push_back({v.x,v.y,v.z,v.u,v.v,v.nx,v.ny,v.nz,v.light});
    }
    void submitEntity(EntityDraw&& draw){entities_.push_back(std::move(draw));}
    void present();
    void shutdown() noexcept;
    bool closeRequested() const noexcept;
    const std::string& gpuName() const {return runtime_.gpuName();}
    VulkanFrameRuntime& worldgenRuntime() {return runtime_;}
    bool compactDraws() const {return scene_.compactDraws();}
    uint32_t residentCapacity() const { return scene_.residentCapacity(); }
    size_t pendingUploads() const { return scene_.pendingUploads(); }
    const auto& uploadStats() const {return scene_.uploadStats();}
    uint64_t denseVoxelBytes() const {return scene_.denseVoxelBytes();}
    uint64_t voxelAllocatedBytes() const {return scene_.voxelAllocatedBytes();}
    uint64_t voxelGrowthCount() const {return scene_.voxelGrowthCount();}
    uint64_t voxelShrinkCount() const {return scene_.voxelShrinkCount();}
    uint64_t voxelAdmissionMisses() const {return scene_.voxelAdmissionMisses();}
    size_t pendingMeshes() const {return scene_.pendingMeshes();}
    size_t inFlightMeshes() const {return scene_.inFlightMeshes();}
    uint32_t residentCount() const {return scene_.residentCount();}
    uint32_t workBudget() const {return scene_.workBudget();}
    double lastMeshGpuMs() const {return scene_.lastMeshGpuMs();}
    const auto& gpuTimes() const {return scene_.gpuTimes();}
    bool reusedMiscCommands() const {return scene_.reusedMiscCommands();}
    bool reusedOpaqueCommands() const {return scene_.reusedOpaqueCommands();}
    uint64_t framesPresented() const {return runtime_.framesPresented();}
    const auto& meshStats() const {return scene_.stats();}
    const auto& meshKinds() const { return scene_.meshKinds(); }

    VulkanGameChunkFeed::UploadSink uploadSink() { return scene_.uploadSink(); }
    VulkanGameChunkFeed::EvictSink evictSink() { return scene_.evictSink(); }
private:
    GLFWwindow* window_ = nullptr;
    VulkanFrameRuntime runtime_;
    VulkanLiveWorldRenderer scene_;
    UiBatch ui_;
    std::vector<ItemVertex> items_;
    std::vector<EntityDraw> entities_;
    glm::mat4 viewProjection_{1.f};
    WorldViewParams worldView_{};
    std::array<float,4> clear_{0.1f,0.1f,0.1f,1.f};
    bool drawWorld_=false;
};

} // namespace OptiCraft::Vulkan
