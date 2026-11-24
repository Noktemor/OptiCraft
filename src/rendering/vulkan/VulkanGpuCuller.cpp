#include "rendering/vulkan/VulkanGpuCuller.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
}
}
VulkanGpuCuller::~VulkanGpuCuller() { shutdown(); }
void VulkanGpuCuller::initialize(VkPhysicalDevice physical, VkDevice device,
                                  const VulkanCullerBuffers& b,
                                  std::span<const uint32_t> spv) {
    if (device_ || !device || !physical || spv.empty() || spv.front() != 0x07230203u)
        throw std::invalid_argument("VulkanGpuCuller::initialize invalid device or SPIR-V");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    if (!props.limits.maxComputeWorkGroupCount[0] ||
        props.limits.maxComputeWorkGroupSize[0] < 64 ||
        props.limits.maxComputeWorkGroupInvocations < 64 ||
        props.limits.maxPushConstantsSize < sizeof(Params))
        throw std::runtime_error("VulkanGpuCuller: device does not support required compute dispatch");
    for (size_t i = 0; i < b.buffers.size(); ++i) {
        if (!b.buffers[i] || !b.byteSizes[i] ||
            b.byteSizes[i] > props.limits.maxStorageBufferRange)
            throw std::invalid_argument("VulkanGpuCuller: missing/oversized SSBO");
    }
    if ((b.chunkSlotStride!=64 && b.chunkSlotStride!=96) ||
        b.byteSizes[0] % b.chunkSlotStride || b.byteSizes[1] % 16 ||
        b.byteSizes[2] % 4 || b.byteSizes[3] % 16)
        throw std::invalid_argument("VulkanGpuCuller: invalid buffer element alignment");
    if (b.byteSizes[1] / 16u < b.byteSizes[0] / b.chunkSlotStride)
        throw std::invalid_argument("VulkanGpuCuller: published draw array smaller than chunk table");
    const uint64_t cap = std::min(b.byteSizes[2] / 4, b.byteSizes[3] / 16);
    if (!cap || cap > UINT32_MAX)
        throw std::invalid_argument("VulkanGpuCuller: invalid live slot capacity");
    maxJobs_ = static_cast<uint32_t>(cap);
    maxGroupsX_ = props.limits.maxComputeWorkGroupCount[0];
    device_ = device;
    buffers_ = b;
    try {
        std::array<VkDescriptorSetLayoutBinding,4> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        l.bindingCount = static_cast<uint32_t>(bindings.size()); l.pBindings = bindings.data();
        checked(vkCreateDescriptorSetLayout(device_, &l, nullptr, &descriptorLayout_),
                "vkCreateDescriptorSetLayout(culler)");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &poolSize;
        checked(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_),
                "vkCreateDescriptorPool(culler)");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool_; alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &descriptorLayout_;
        checked(vkAllocateDescriptorSets(device_, &alloc, &descriptorSet_),
                "vkAllocateDescriptorSets(culler)");
        std::array<VkDescriptorBufferInfo,4> infos{};
        std::array<VkWriteDescriptorSet,4> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            infos[i] = {b.buffers[i], 0, b.byteSizes[i]};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = descriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(),0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Params)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout_;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout(culler)");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spv.size_bytes(); moduleInfo.pCode = spv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        checked(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module),
                "vkCreateShaderModule(culler)");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = module; stage.pName = "main";
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = stage; pci.layout = pipelineLayout_;
        VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE,1,&pci,nullptr,&pipeline_);
        vkDestroyShaderModule(device_,module,nullptr);
        checked(result, "vkCreateComputePipelines(culler)");
    } catch (...) { shutdown(); throw; }
}
void VulkanGpuCuller::record(VkCommandBuffer cmd, uint32_t count,
                              const GpuFrustum& frustum) const {
    if (!count) return;
    if (!device_ || !cmd || count > maxJobs_)
        throw std::out_of_range("VulkanGpuCuller::record invalid draw count");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const uint64_t maxInvocations = uint64_t(maxGroupsX_) * 64;
    for (uint32_t first = 0; first < count;) {
        const uint32_t batch = uint32_t(std::min(maxInvocations, uint64_t(count - first)));
        Params params{};
        params.frustum = frustum; params.firstJob = first; params.totalJobs = count;
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(params), &params);
        vkCmdDispatch(cmd, (batch + 63u) / 64u, 1, 1);
        first += batch;
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &barrier,0,nullptr,0,nullptr);
}
void VulkanGpuCuller::shutdown() {
    if (!device_) return;
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE; pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE; descriptorLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
    maxJobs_ = maxGroupsX_ = 0;
}
} // namespace OptiCraft::Vulkan
