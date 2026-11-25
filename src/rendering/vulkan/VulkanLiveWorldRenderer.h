#pragma once

#include "rendering/vulkan/VulkanFrameRuntime.h"
#include "rendering/vulkan/VulkanChunkMeshing.h"
#include "rendering/vulkan/VulkanGpuCuller.h"
#include "rendering/vulkan/VulkanHiZ.h"
#include "rendering/vulkan/VulkanAlphaSort.h"
#include "rendering/vulkan/VulkanOpaqueMaterialPipeline.h"
#include "rendering/vulkan/VulkanMiscPipeline.h"
#include "rendering/vulkan/VulkanBlockAtlas.h"
#include "rendering/vulkan/VulkanMaterialTables.h"
#include "rendering/vulkan/VulkanUiPipeline.h"
#include "rendering/vulkan/VulkanItemPipeline.h"
#include "rendering/vulkan/VulkanEntityPipeline.h"
#include "rendering/vulkan/MeshWorkBudget.h"
#include "rendering/vulkan/WorldViewParams.h"
#include "rendering/vulkan/StaticDrawCache.h"
#include "rendering/vulkan/ResidentLightScheduler.h"
#include "rendering/vulkan/OwnedWorldRuntime.h"
#include <array>
#include <filesystem>
#include <memory>

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
                     const WorldViewParams& view,const GpuFrustum& frustum,
                     bool drawWorld,const std::array<float,4>& clearColor,const UiBatch& ui,std::span<const ItemVertex> items,std::span<const EntityDraw> entities);
    VulkanGameChunkFeed::UploadSink uploadSink(){return stream_.uploadSink();}
    VulkanGameChunkFeed::EvictSink evictSink(){return stream_.evictSink();}
    VulkanGameChunkFeed::SnapshotLeaseSink snapshotLeaseSink(){return stream_.snapshotLeaseSink();}
    void attachWorld(Chunk_Manager& manager);
    void detachWorldIdle();
    bool hasOwnedWorld()const noexcept{return bool(ownedWorld_);}
    uint32_t cpuMirrorCapacity() const {return residentCapacity()-(ownedWorld_?ownedWorld_->capacity():0);}
    void shutdown()noexcept; // AFTER Game callbacks detached and GPU idle
    const auto& entityStats() const noexcept {return entityPipeline_.stats();}
    const auto& itemStats() const noexcept {return itemPipeline_.stats();}
    bool compactDraws() const {return compactDraws_;}
    bool residentLightPilotAvailable() const noexcept {return stream_.indirectVoxelPages() && !residentLightUnavailable_;}
    size_t residentLightPending() const noexcept {return residentLightScheduler_.pendingCount();}
    uint64_t residentLightRecordedCores() const noexcept {return residentLightRecordedCores_;}
    uint32_t residentCapacity() const { return stream_.plan().capacity(); }
    size_t pendingUploads() const { return stream_.plan().pendingCount(); }
    const auto& uploadStats() const {return stream_.uploadStats();}
    uint64_t denseVoxelBytes() const {return stream_.denseVoxelBytes();}
    uint64_t voxelAllocatedBytes() const {return stream_.voxelAllocatedBytes();}
    uint64_t voxelGrowthCount() const {return stream_.voxelGrowthCount();}
    uint64_t voxelShrinkCount() const {return stream_.voxelShrinkCount();}
    uint64_t voxelAdmissionMisses() const {return stream_.voxelAdmissionMisses();}
    size_t pendingMeshes() const {return meshing_.pendingCount();}
    size_t inFlightMeshes() const {return meshing_.inFlightCount();}
    uint32_t residentCount() const {return stream_.liveSlotCount();}
    uint32_t workBudget() const {return workBudget_.budget();}
    uint32_t ownedCapacity()const noexcept{return ownedWorld_?ownedWorld_->capacity():0;}
    size_t ownedCount()const noexcept{return ownedWorld_?ownedWorld_->count():0;}
    bool ownedPressure()const noexcept{return ownedWorld_ && ownedWorld_->pressure();}
    double ownedQuotaMs()const noexcept{return ownedWorld_?ownedWorkBudget_.quotaMs():0;}
    const auto& ownedGrants()const noexcept{return ownedGrants_;}
    double lastMeshGpuMs() const {return workBudget_.lastMs();}
    // Completed frame: total, cull, alpha, Hi-Z; -1 if timestamps unsupported.
    const std::array<double,4>& gpuTimes() const {return gpuTimes_;}
    bool reusedMiscCommands() const {return reusedMisc_;}
    bool reusedOpaqueCommands() const {return reusedMisc_ && opaqueCacheEnabled_;}
    const std::array<uint8_t,65536>& meshKinds() const { return meshKinds_; }
    const GpuMeshTransactions::Stats& stats()const{return meshing_.stats();}

private:
    struct FrameIndirect {
        VulkanDeviceBuffer opaque, misc, opaqueCount, miscCount;
        VulkanGpuCuller opaqueCuller, miscCuller;
    };
    void createGraphics(VkRenderPass renderPass);
    void createHiZ();
    void recordIndirect(VkCommandBuffer cmd,VkBuffer buffer,uint32_t count,VkBuffer countBuffer=VK_NULL_HANDLE)const;
    std::vector<uint32_t> shader(const char* name)const;
    VulkanFrameRuntime* runtime_=nullptr;
    std::filesystem::path shaderDirectory_;
    VkDevice device_=VK_NULL_HANDLE;
    VulkanChunkStreamGpu stream_;
    ResidentLightScheduler residentLightScheduler_;
    std::unique_ptr<OwnedWorldRuntime> ownedWorld_;
    std::array<std::unique_ptr<VulkanResidentLight>,VulkanFrameRuntime::kFramesInFlight> ownedSky_{};
    bool ownedSkyUnavailable_=false;
    std::array<std::unique_ptr<VulkanResidentLight>,VulkanFrameRuntime::kFramesInFlight> residentLight_{};
    std::array<uint32_t,2048> lightTransparency_{};
    std::array<uint8_t,65536> lightEmission_{};
    bool residentLightUnavailable_=false;
    uint64_t residentLightRecordedCores_=0;
    VulkanMaterialTables materials_;
    VulkanBlockAtlas atlas_;
    VulkanUiPipeline uiPipeline_;
    VulkanItemPipeline itemPipeline_;
    VulkanEntityPipeline entityPipeline_;
    VulkanChunkMeshing meshing_;
    VulkanMiscPipeline miscPipeline_;
    VulkanOpaqueMaterialPipeline opaquePipeline_;
    std::array<FrameIndirect,VulkanFrameRuntime::kFramesInFlight> indirect_{};
    VulkanHiZ hiz_;
    VulkanAlphaSort alphaSort_;
    std::array<VulkanCullerBuffers,VulkanFrameRuntime::kFramesInFlight> hizBuffers_{};
    VkDescriptorSet atlasSet_=VK_NULL_HANDLE;
    uint64_t pipelineSwapchainGeneration_=0;
    std::array<uint8_t,65536> meshKinds_{};
    MeshWorkBudget workBudget_;
    AdaptiveWorldWorkBudget ownedWorkBudget_;
    AdaptiveWorldWorkBudget::Counts ownedGrants_{};
    uint64_t workSampleSerial_=0;
    VkQueryPool meshQueries_=VK_NULL_HANDLE;
    uint32_t timestampBits_=0;
    double timestampNs_=0;
    std::array<uint32_t,VulkanFrameRuntime::kFramesInFlight> timestampWork_{};
    std::array<bool,VulkanFrameRuntime::kFramesInFlight> timestampCount_{};
    static constexpr uint32_t kQueriesPerFrame=16;
    std::array<AdaptiveWorldWorkBudget::Counts,VulkanFrameRuntime::kFramesInFlight> ownedTimedWork_{};
    std::array<bool,VulkanFrameRuntime::kFramesInFlight> ownedTimingValid_{};
    std::array<uint32_t,VulkanFrameRuntime::kFramesInFlight> timestampFlags_{};
    std::array<double,4> gpuTimes_{{-1,-1,-1,-1}};
    std::array<StaticDrawCache,VulkanFrameRuntime::kFramesInFlight> miscCache_{};
    uint64_t drawRevision_=0;
    bool reusedMisc_=false,opaqueCacheEnabled_=true;
    uint32_t maxIndirectCount_=1,hizMinResidents_=256;
    bool compactDraws_=false, multiDraw_=false,initialUploadsRecorded_=false,staticStagingReleased_=false;
    uint32_t uploadFrameSlot_=UINT32_MAX;
};

} // namespace OptiCraft::Vulkan
