#pragma once

#include "rendering/vulkan/VulkanChunkStreamPlan.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace OptiCraft::Vulkan {

// Stage 5: real, persistent Vulkan allocation + GPU transfer of original block
// state from Game. GPU compute mesher consumes these buffers directly.
// Single graphics+compute queue only; caller must record between beginFrame and
// endColorPass, and call record AFTER VulkanFrameRuntime::beginFrame waits the
// frame fence. No device-idle, VkAllocateMemory or vkCreateBuffer per chunk.
class VulkanChunkStreamGpu {
public:
    static constexpr uint32_t kFrameSlots = 2;
    struct Config {
        uint32_t maxResident = 512;
        uint32_t uploadsPerFrame = 8;
        uint32_t maxQueued = 1024;
    };
    VulkanChunkStreamGpu() = default;
    ~VulkanChunkStreamGpu() { shutdown(); }
    VulkanChunkStreamGpu(const VulkanChunkStreamGpu&) = delete;
    VulkanChunkStreamGpu& operator=(const VulkanChunkStreamGpu&) = delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, Config config);
    // Safe for producer threads, including Game::VulkanGameChunkFeed callbacks.
    void submit(std::vector<LiveChunkPacket>&& packets);
    void evict(ChunkKey key);
    // Connect these directly to Game::set_vulkan_chunk_sink(). The GPU store
    // must outlive the callbacks; unregister before destroying it.
    VulkanGameChunkFeed::UploadSink uploadSink();
    VulkanGameChunkFeed::EvictSink evictSink();
    // Record one batched transfer into the frame's primary command buffer.
    // Returns dirty slot IDs to enqueue for GPU count/emit meshing. No work
    // submitted when inputs are unchanged. Returns frame-local data by value.
    StreamBatch record(VkCommandBuffer primary, uint32_t frameSlot);
    // Geometry owner updates these before the next record, after allocating the
    // corresponding persistent GPU ranges. They MUST match the current generation.
    void setOpaqueFaceRange(uint32_t slot, uint32_t generation, uint32_t base, uint32_t capacity);
    void setMiscQuadRange(uint32_t slot, uint32_t generation, uint32_t base, uint32_t capacity);
    void shutdown() noexcept; // must be called after all GPU submissions complete

    VkBuffer blocks() const noexcept { return blockWords_.buffer(); }
    VkBuffer metadata() const noexcept { return metaWords_.buffer(); }
    VkBuffer lights() const noexcept { return lightWords_.buffer(); }
    VkBuffer opaqueSlots() const noexcept { return opaqueSlots_.buffer(); }
    VkBuffer miscSlots() const noexcept { return miscSlots_.buffer(); }
    VkBuffer publishedDraws() const noexcept { return publishedDraws_.buffer(); }
    VkBuffer liveSlotIndices() const noexcept { return liveSlots_.buffer(); }
    uint32_t liveSlotCount() const noexcept { return liveSlotCount_; }
    VulkanChunkStreamPlan& plan() { return *planner_; }
    const VulkanChunkStreamPlan& plan() const { return *planner_; }
private:
    struct FaceRange {uint32_t generation=0,base=0,count=0;};
    struct StagingFrame { VulkanDeviceBuffer buffer; };
    struct RangeCopies {
        std::vector<VkBufferCopy> blocks, meta, light, opaque, misc, live;
    };
    VkDevice device_ = VK_NULL_HANDLE;
    Config config_{};
    uint32_t liveSlotCount_ = 0;
    std::unique_ptr<VulkanChunkStreamPlan> planner_;
    VulkanDeviceBuffer blockWords_, metaWords_, lightWords_;
    VulkanDeviceBuffer opaqueSlots_, miscSlots_, publishedDraws_, liveSlots_;
    std::array<StagingFrame,kFrameSlots> staging_;
    std::vector<FaceRange> opaqueFaces_, miscFaces_;
    std::vector<uint32_t> dirtyGeometry_;
    std::vector<std::byte> stagingBytes_;
};

} // namespace OptiCraft::Vulkan
