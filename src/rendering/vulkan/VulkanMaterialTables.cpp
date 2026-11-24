#include "rendering/vulkan/VulkanMaterialTables.h"
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace OptiCraft::Vulkan {
void VulkanMaterialTables::initialize(VkPhysicalDevice physical,VkDevice device,
                                      const GpuBlockCatalog& catalog) {
    if (device_ || !physical || !device || catalog.blocks.size()!=65536 ||
        catalog.opaqueMesherFlags.size()!=65536 ||catalog.faces.empty() ||
        catalog.shapeIndex.empty() ||catalog.shapeBoxes.empty())
        throw std::invalid_argument("VulkanMaterialTables invalid complete game catalog");
    device_=device;
    try {
        const VkDeviceSize lens[5]{
            VkDeviceSize(catalog.opaqueMesherFlags.size())*sizeof(uint32_t),
            VkDeviceSize(catalog.blocks.size())*sizeof(GpuBlockVisual),
            VkDeviceSize(catalog.shapeIndex.size())*sizeof(GpuShapeIndex),
            VkDeviceSize(catalog.shapeBoxes.size())*sizeof(GpuShapeBox),
            VkDeviceSize(catalog.faces.size())*sizeof(GpuFaceUv)};
        const void* sources[5]{catalog.opaqueMesherFlags.data(),catalog.blocks.data(),
            catalog.shapeIndex.data(),catalog.shapeBoxes.data(),catalog.faces.data()};
        VulkanDeviceBuffer* dest[5]{&opaqueFlags_,&blocks_,&indices_,&boxes_,&faces_};
        VkDeviceSize total=0;
        for(int i=0;i<5;i++){
            // Align offsets for vkCmdCopyBuffer and SSBO access.
            total=(total+15u)&~VkDeviceSize(15u);
            copies_[i]={total,0,lens[i]};
            if (lens[i] > std::numeric_limits<VkDeviceSize>::max()-total)
                throw std::overflow_error("Material catalog byte size overflow");
            total+=lens[i];
            dest[i]->create(physical,device,lens[i],
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        staging_.create(physical,device,total,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for(int i=0;i<5;i++)staging_.write(copies_[i].srcOffset,sources[i],lens[i]);
    } catch(...) {shutdown();throw;}
}
void VulkanMaterialTables::recordUpload(VkCommandBuffer primary) {
    if(!primary || !device_ || !staging_ || recorded_)
        throw std::logic_error("VulkanMaterialTables recordUpload requires initialized, unrecorded tables");
    const VkBuffer dest[5]{opaqueFlags_.buffer(),blocks_.buffer(),indices_.buffer(),
                           boxes_.buffer(),faces_.buffer()};
    for(int i=0;i<5;i++)vkCmdCopyBuffer(primary,staging_.buffer(),dest[i],1,&copies_[i]);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(primary,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    recorded_=true;
}
void VulkanMaterialTables::releaseStagingAfterCompletion() {
    if(!recorded_)throw std::logic_error("Material tables have not been submitted");
    staging_.destroy();
}
void VulkanMaterialTables::shutdown() noexcept {
    staging_.destroy();faces_.destroy();boxes_.destroy();indices_.destroy();
    blocks_.destroy();opaqueFlags_.destroy();device_=VK_NULL_HANDLE;recorded_=false;
}
} // namespace OptiCraft::Vulkan
