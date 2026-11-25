#pragma once
#include "world/generation/Gpu_Overlay_Reference.h"
#include "rendering/vulkan/SkyRoofPalette.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
namespace OptiCraft::Vulkan {
// Main graphics thread only. Strong references in queued source-only packets
// and frame slots protect bytes until their fence; weak references can expire
// or become stale at any time. A leased snapshot must not be modified.
struct OverlaySnapshot {
    static constexpr VkDeviceSize blockBytes=65536, metaBytes=32768;
    VulkanDeviceBuffer buffer;
    VkDevice device=VK_NULL_HANDLE;
    uint64_t ticket=0;
    bool ready=false;
    SkyRoofPalette skyRoof{};
};
inline std::shared_ptr<OverlaySnapshot> acquireOverlay(const OverlayReference& ref,VkDevice device) {
    auto p=ref.source.lock();
    if(!p || !ref.ticket || p->ticket!=ref.ticket || !p->ready || p->device!=device ||
        !p->buffer || p->buffer.size()!=OverlaySnapshot::blockBytes+OverlaySnapshot::metaBytes) return {};
    return p;
}
}
