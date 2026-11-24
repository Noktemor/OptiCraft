#pragma once

#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanChunkMeshing.h"
#include "rendering/vulkan/VulkanGpuCuller.h"
#include "rendering/vulkan/VulkanOpaqueMaterialPipeline.h"
#include "rendering/vulkan/VulkanMiscPipeline.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include "rendering/vulkan/VulkanUiPipeline.h"
#include <array>
#include <filesystem>

class Block_Registry;
namespace OptiCraft::Vulkan {

// A real Vulkan world-frame consumer for Game::set_vulkan_chunk_sink().
// It owns the live SSBOs, compute/graphics pipelines, atlas and frame-local
// indirect buffers, but NOT the Game world or the GLFW window.
// The integration entrypoint needs VulkanFrameRuntime::beginFrame first and
// must call endFrame after recordFrame. All calls except packet submission/
// eviction take place on the render thread.
class VulkanLiveWorldRenderer {
public:
    struct Config {
        VulkanChunkStreamGpu::Config streaming{};
        VulkanChunkMeshing::Config meshing{};
    };
    VulkanLiveWorldRenderer()=default;
    ~VulkanLiveWorldRenderer(){shutdown();}
    VulkanLiveWorldRenderer(const VulkanLiveWorldRenderer&)=delete;
    VulkanLiveWorldRenderer& operator=(const VulkanLiveWorldRenderer&)=delete;

    void initialize(VulkanFrameRuntime& runtime,const Block_Registry& blocks,
                    const std::filesystem::path& shaderDirectory,
                    const std::filesystem::path& atlasPng, Config config);
    void recordFrame(VulkanFrameRuntime& runtime,const VulkanFrameRuntime::Frame& frame,
                     const float viewProjection[16],const GpuFrustum& frustum,
                     bool drawWorld,const std::array<float,4>& clearColor,const UiBatch& ui);
    VulkanGameChunkFeed::UploadSink uploadSink(){return stream_.uploadSink();}
    VulkanGameChunkFeed::EvictSink evictSink(){return stream_.evictSink();}
    void shutdown()noexcept; // AFTER Game callbacks detached and GPU idle
    const GpuMeshTransactions::Stats& stats()const{return meshing_.stats();}

private:
    struct FrameIndirect {
        VulkanDeviceBuffer opaque, misc;
        VulkanGpuCuller opaqueCuller, miscCuller;
    };
    void createGraphics(VkRenderPass renderPass);
    void recordIndirect(VkCommandBuffer cmd,VkBuffer buffer,uint32_t count)const;
    std::vector<uint32_t> shader(const char* name)const;
    VulkanFrameRuntime* runtime_=nullptr;
    std::filesystem::path shaderDirectory_;
    VkDevice device_=VK_NULL_HANDLE;
    VulkanChunkStreamGpu stream_;
    VulkanMaterialTables materials_;
    VulkanBlockAtlas atlas_;
    VulkanUiPipeline uiPipeline_;
    VulkanChunkMeshing meshing_;
    VulkanMiscPipeline miscPipeline_;
    VulkanOpaqueMaterialPipeline opaquePipeline_;
    std::array<FrameIndirect,VulkanFrameRuntime::kFramesInFlight> indirect_{};
    VkDescriptorSet atlasSet_=VK_NULL_HANDLE;
    uint64_t pipelineSwapchainGeneration_=0;
    uint32_t maxIndirectCount_=1;
    bool multiDraw_=false,initialUploadsRecorded_=false,staticStagingReleased_=false;
    uint32_t uploadFrameSlot_=UINT32_MAX;
};

} // namespace OptiCraft::Vulkan
