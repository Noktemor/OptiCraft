#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
namespace OptiCraft::Vulkan {
// Only update the descriptor set of the fence-completed frame.
class FrameVoxelDescriptors {
public:
    static constexpr uint32_t kFrames=2;
    std::array<VkDescriptorSet,kFrames> sets{};
    VkDescriptorSet select(VkDevice device,uint32_t frame,uint64_t epoch,
        const std::array<uint32_t,3>& bindings,const std::array<VkBuffer,3>& buffers,
        const std::array<VkDeviceSize,3>& sizes){
        if(frame>=kFrames)throw std::out_of_range("Invalid voxel descriptor frame");
        if(epochs_[frame]!=epoch){
            std::array<VkDescriptorBufferInfo,3> info{};
            std::array<VkWriteDescriptorSet,3> writes{};
            for(size_t k=0;k<3;++k){
                info[k]={buffers[k],0,sizes[k]};
                writes[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[k].dstSet=sets[frame];writes[k].dstBinding=bindings[k];
                writes[k].descriptorCount=1;writes[k].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[k].pBufferInfo=&info[k];
            }
            vkUpdateDescriptorSets(device,uint32_t(writes.size()),writes.data(),0,nullptr);
            epochs_[frame]=epoch;
        }
        return sets[frame];
    }
    void reset() noexcept {sets={};epochs_={};}
private:
    std::array<uint64_t,kFrames> epochs_{};
};
}
