#pragma once
#include "rendering/vulkan/ResidentLightBatch.h"
#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include <optional>

namespace OptiCraft::Vulkan {
class VulkanChunkStreamGpu;
struct StreamBatch;
// One bounded, closed-set transaction. This is a command recorder, not a
// lighting owner. Caller must order source writes before record() on the same
// queue and retain the external block/seed buffers and pages until its fence.
// record() needs seed-only light; recordGenerated() can read live sky
// from propagated light while rebuilding block-light seeds.
class VulkanResidentLight {
public:
    struct Sources {
        VkBuffer blocks=VK_NULL_HANDLE, seeds=VK_NULL_HANDLE;
        VkDeviceSize blockBytes=0, seedBytes=0;
        uint64_t epoch=0; // caller's immutable source snapshot generation
    };
    struct Result {
        VkBuffer buffer;
        VkDeviceSize offset, bytes;
        uint64_t epoch;
    };
    VulkanResidentLight()=default;
    ~VulkanResidentLight(); // owner must have waited for recorded work
    VulkanResidentLight(const VulkanResidentLight&)=delete;
    VulkanResidentLight& operator=(const VulkanResidentLight&)=delete;
    void initialize(VkPhysicalDevice physical,VkDevice device,uint32_t maxJobs,
                    std::span<const uint32_t> spirv);
    // slots must describe exactly the external buffers at the execution point.
    // Only closed, authoritative seed snapshots are supported. No publication.
    // Rejects invalid packets before updating descriptors or recording commands.
    bool record(VkCommandBuffer cmd,const Sources& sources,
                std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                std::span<const uint32_t> transparentBits);
    // Rebuild block-light seeds from current block IDs. sources.seeds refers
    // to pinned existing light pages, whose HIGH nibble alone is authoritative.
    // Closed-set only: outside light is intentionally not imported.
    bool recordGenerated(VkCommandBuffer cmd,const Sources& sources,
                std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                std::span<const uint32_t> transparentBits,std::span<const uint8_t> emission);
    // Rebuild skylight from actual resident blocks, then propagate 14 steps.
    // topOpen: 32 words per selected job in selection order, one X bit per Z
    // row. Top of EACH disconnected vertical chain must have an authoritative
    // open-sky mask (no opaque voxels above it). Interior jobs must have zero
    // masks; direct light is inherited by scanning the selected chain on GPU.
    // Unknown upper boundaries must not be submitted as known open/closed.
    // Side boundaries are closed; caller must provide sufficient halo for
    // world-exact core use. Preserves existing low nibble. No publication.
    bool recordSky(VkCommandBuffer cmd,const Sources& sources,
                std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                std::span<const uint32_t> skyTransparentBits,std::span<const uint32_t> topOpen);
    // Core IDs must be the prefix of selected. Extra vertical jobs are
    // permitted, but all in-world halo neighbours must be present. ResultCore()
    // then exposes only the exact core (given authoritative top masks).
    bool recordSkyCore(VkCommandBuffer cmd,const Sources& sources,
                std::span<const GpuChunkSlot> slots,std::span<const uint32_t> selected,
                uint32_t coreCount,std::span<const uint32_t> skyTransparentBits,
                std::span<const uint32_t> topOpen,LightVerticalBounds bounds={});
    // Strict one-chunk halo of every core, with core IDs first in job order.
    // Fails if any required in-world neighbour is absent, malformed, or over budget.
    // Same source lifetime contract as recordGenerated. Only resultCore()
    // excludes the artificially closed outer halo and is suitable for a
    // future publication step. It does not authorize publication on its own.
    bool recordGeneratedCore(VkCommandBuffer cmd,const Sources& sources,
                std::span<const GpuChunkSlot> slots,std::span<const uint32_t> core,
                std::span<const uint32_t> transparentBits,std::span<const uint8_t> emission,LightVerticalBounds bounds={});
    std::optional<Result> resultCore(uint64_t currentEpoch) const noexcept;
    // Stream-owned sources: record and consume in the SAME primary command
    // buffer after stream.record(). All subsequent stream transfers must be
    // submitted in order on that queue. No asynchronous external-page lease.
    // Core handles include generations; mixed dimensions are rejected.
    bool recordStreamCore(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
                std::span<const ChunkHandle> core,std::span<const uint32_t> transparentBits,
                std::span<const uint8_t> emission);
    // Caller certifies the actual world ceiling (highest chunk containing any
    // possible occluder), not the highest currently loaded chunk. No-sky is
    // explicit: only core pages are required in a no-sky dimension. Missing
    // in-world vertical data or halo rejects sky-enabled exact requests.
    // Provisional local emission when the full 26-neighbour halo is absent.
    bool recordStreamPreviewBlock(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
                ChunkHandle core,std::span<const uint32_t> bits,std::span<const uint8_t> emission,
                bool includeResidentHalo=false);
    // Render-only sky. Known masks are keyed by current stable-slot handle.
    // Unknown upper boundaries are closed; interior masks are ignored.
    struct PreviewSkySeed { ChunkHandle handle; std::array<uint32_t,32> top; };
    // Never an exact promotion certificate.
    bool recordStreamPreviewSky(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
                ChunkHandle core,std::span<const uint32_t> bits,std::span<const uint32_t> topOpen,
                std::span<const PreviewSkySeed> haloSeeds={},bool includeResidentHalo=false,
                std::optional<int32_t> previewCeiling=std::nullopt);
    struct SkyBoundary { bool hasSky; int32_t highestChunkY; };
    bool recordStreamSkyCore(VkCommandBuffer cmd,uint32_t frameSlot,const VulkanChunkStreamGpu& stream,
                std::span<const ChunkHandle> core,std::span<const uint32_t> skyTransparentBits,
                SkyBoundary boundary);
    std::optional<Result> resultStreamCore(VkCommandBuffer cmd,uint32_t frameSlot,
                const VulkanChunkStreamGpu& stream) const noexcept;
    // Publish only the original core, only to explicitly GPU-owned dense
    // light pages (sky additionally requires exclusive GPU world ownership), after stream.record() and BEFORE meshing consumes changed.
    // changed must be the StreamBatch of this same record. Caller must submit
    // the command buffer (or discard/reset the stream on cancellation).
    // No CPU gameplay light is modified. Whole request rejects before copying.
    bool publishStreamCore(VkCommandBuffer cmd,uint32_t frameSlot,
                VulkanChunkStreamGpu& stream,StreamBatch& changed);
    // Range for same-queue GPU consumption/copy, not a host pointer. Epoch
    // comparison is an additional guard; caller owns source revision checks.
    std::optional<Result> result(uint64_t currentEpoch) const noexcept;
    // Call only after fence (or after discarding unsubmitted command buffer).
    void retire() noexcept {busy_=false; count_=0; coreCount_=0; epoch_=0;stream_=nullptr;streamCore_.clear();streamDataSerials_.clear();streamSky_=false;published_=false;}
    bool busy() const noexcept{return busy_;}
    uint32_t capacity() const noexcept{return maxJobs_;}
private:
    bool recordImpl(VkCommandBuffer,const Sources&,std::span<const GpuChunkSlot>,
                    std::span<const uint32_t>,std::span<const uint32_t>,std::span<const uint8_t>,std::span<const uint32_t> topOpen={});
    std::optional<Result> resultRange(uint64_t currentEpoch) const noexcept;
    const VulkanChunkStreamGpu* stream_=nullptr;
    std::vector<ChunkHandle> streamCore_;
    std::vector<uint64_t> streamDataSerials_; // packed opposite-channel snapshot guard
    bool streamSky_=false,published_=false;
    void destroy() noexcept;
    VkDevice device_=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VulkanDeviceBuffer slots_,jobs_,transparency_,scratch_;
    uint32_t maxJobs_=0,count_=0,coreCount_=0;
    VkDeviceSize maxRange_=0;
    uint64_t epoch_=0;
    bool busy_=false;
};
}
