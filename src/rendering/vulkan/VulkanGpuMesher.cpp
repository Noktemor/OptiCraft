#include "rendering/vulkan/VulkanGpuMesher.h"
#include "rendering/vulkan/MeshDispatchRuns.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace OptiCraft::Vulkan {
namespace {
void require(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("VulkanGpuMesher: ") + action +
                                 " VkResult=" + std::to_string(result));
}
VkShaderModule createModule(VkDevice dev, std::span<const uint32_t> code) {
    if (code.empty() || code.front() != 0x07230203u)
        throw std::invalid_argument("VulkanGpuMesher: invalid/missing SPIR-V");
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size_bytes();
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    require(vkCreateShaderModule(dev, &ci, nullptr, &module), "vkCreateShaderModule");
    return module;
}
}

VulkanGpuMesher::~VulkanGpuMesher() { shutdown(); }

void VulkanGpuMesher::initialize(VkPhysicalDevice physical, VkDevice device,
                                const GpuMesherBuffers& buffers,
                                std::span<const uint32_t> countSpv,
                                std::span<const uint32_t> emitSpv,
                                std::span<const uint32_t> finalizeSpv,
                                 std::span<const uint32_t> publishSpv) {
    if (device_ != VK_NULL_HANDLE || physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        throw std::invalid_argument("VulkanGpuMesher: bad device or already initialized");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props); // once at initialization
    dispatchYLimit_ = props.limits.maxComputeWorkGroupCount[1];
    finalizeXLimit_ = props.limits.maxComputeWorkGroupCount[0];
    if (!dispatchYLimit_ || !finalizeXLimit_ || finalizeXLimit_ < 192 ||
        props.limits.maxComputeWorkGroupSize[0] < 64 ||
        props.limits.maxComputeWorkGroupInvocations < 64)
        throw std::runtime_error("VulkanGpuMesher: device cannot dispatch 32^3 voxel groups");

    for (size_t i = 0; i < buffers.buffers.size(); ++i) {
        if (!buffers.buffers[i] || !buffers.byteSizes[i])
            throw std::invalid_argument("VulkanGpuMesher: missing storage buffer or size");
        if (buffers.byteSizes[i] > props.limits.maxStorageBufferRange)
            throw std::invalid_argument("VulkanGpuMesher: descriptor exceeds maxStorageBufferRange; split arena into buffer segments");
    }
    if (buffers.byteSizes[0] % 4u || buffers.byteSizes[1] % sizeof(GpuChunkSlot) ||
        buffers.byteSizes[2] % 4u || buffers.byteSizes[3] % 8u ||
        buffers.byteSizes[4] % 4u || buffers.byteSizes[5] % 16u ||
        buffers.byteSizes[6] % 4u || buffers.byteSizes[7] % 16u || buffers.byteSizes[8] % 4u || buffers.byteSizes[9] % 4u || buffers.byteSizes[10] % 4u)
        throw std::invalid_argument("VulkanGpuMesher: misaligned SSBO sizes");
    if (buffers.byteSizes[5] / 16u < buffers.byteSizes[1] / sizeof(GpuChunkSlot))
        throw std::invalid_argument("VulkanGpuMesher: active draw buffer smaller than stable chunk table");
    if (buffers.byteSizes[4] < 65536u * sizeof(uint32_t))
        throw std::invalid_argument("VulkanGpuMesher: incomplete 16-bit material-flags table");
    const uint64_t maxJobs = std::min({buffers.byteSizes[2] / 4u,
        buffers.byteSizes[6] / 4u, buffers.byteSizes[7] / 16u});
    if (!maxJobs || maxJobs > UINT32_MAX)
        throw std::invalid_argument("VulkanGpuMesher: invalid batch capacity");
    // A descriptor can include alignment padding; cap usable jobs by actual cache storage.
    if(buffers.planeCompaction && !buffers.coarseCache)
        throw std::invalid_argument("Plane compaction requires coarse cache");
    const uint64_t cacheJobBytes=4ull+8ull*buffers.cacheRectsPerJob+
        (buffers.coarseCache?4ull*kCoarseWordsPerJob:0ull)+
        (buffers.planeCompaction?4ull*kPlaneWordsPerJob:0ull);
    const bool hasCache=buffers.cacheRectsPerJob||buffers.coarseCache;
    const uint64_t usable=hasCache?std::min(maxJobs,buffers.byteSizes[10]/cacheJobBytes):maxJobs;
    if(!usable || usable>=kPlaneCompactFlag || (hasCache && usable*cacheJobBytes/4>UINT32_MAX))
        throw std::invalid_argument("VulkanGpuMesher: invalid rectangle cache size");
    maxJobs_ = static_cast<uint32_t>(usable);

    device_ = device;
    buffers_ = buffers;
    try {
        std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            if(i<10)bindings[i].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = static_cast<uint32_t>(bindings.size());
        dl.pBindings = bindings.data();
        require(vkCreateDescriptorSetLayout(device_, &dl, nullptr, &descriptorLayout_),
                "vkCreateDescriptorSetLayout");

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      static_cast<uint32_t>(bindings.size())*FrameVoxelDescriptors::kFrames};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = FrameVoxelDescriptors::kFrames;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &poolSize;
        require(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptorPool_),
                "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool_;
        const std::array<VkDescriptorSetLayout,2> frameLayouts{descriptorLayout_,descriptorLayout_};
        alloc.descriptorSetCount = FrameVoxelDescriptors::kFrames;
        alloc.pSetLayouts = frameLayouts.data();
        require(vkAllocateDescriptorSets(device_, &alloc, frameDescriptors_.sets.data()),
                "vkAllocateDescriptorSets");
        descriptorSet_=frameDescriptors_.sets[0];

        std::array<VkDescriptorBufferInfo, 11> infos{};
        std::array<VkWriteDescriptorSet, 11> writes{};
        for (uint32_t i = 0; i < infos.size(); ++i) {
            infos[i].buffer = buffers.buffers[i];
            infos[i].offset = 0;
            infos[i].range = buffers.byteSizes[i];
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = descriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        for(auto set:frameDescriptors_.sets){
            for(auto& w:writes)w.dstSet=set;
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),writes.data(),0,nullptr);
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(Params);
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &descriptorLayout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        require(vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout");

        const std::array<std::span<const uint32_t>,4> codes{countSpv, emitSpv, finalizeSpv, publishSpv};
        for (size_t i = 0; i < codes.size(); ++i) {
            VkShaderModule shader = createModule(device_, codes[i]);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader;
            stage.pName = "main";
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = stage;
            ci.layout = pipelineLayout_;
            VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE,
                                                       1, &ci, nullptr, &pipelines_[i]);
            vkDestroyShaderModule(device_, shader, nullptr);
            require(result, "vkCreateComputePipelines");
        }
    } catch (...) { shutdown(); throw; }
}

void VulkanGpuMesher::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto& pipeline : pipelines_) {
        if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;frameDescriptors_.reset();
    maxJobs_ = 0;
    device_ = VK_NULL_HANDLE;
}

void VulkanGpuMesher::barrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                              VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                              VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
}

void VulkanGpuMesher::resetCounts(VkCommandBuffer cmd, uint32_t jobs) const {
    if (jobs > maxJobs_) throw std::out_of_range("VulkanGpuMesher: too many jobs");
    vkCmdFillBuffer(cmd, buffers_.buffers[2], 0, VkDeviceSize(jobs) * sizeof(uint32_t), 0u);
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void VulkanGpuMesher::dispatchMeshes(VkCommandBuffer cmd, uint32_t jobs,
                                     VkPipeline pipeline, std::span<const uint8_t> kinds) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const bool coarse=buffers_.coarseCache && std::any_of(kinds.begin(),kinds.end(),
        [](uint8_t k){return (k&kCoarseOpaqueMesh)!=0;});
    const bool compact=buffers_.planeCompaction && std::any_of(kinds.begin(),kinds.end(),compactPlaneKind);
    std::vector<uint8_t> directKinds;
    if(compact){
        if(kinds.size()!=jobs)throw std::invalid_argument("Invalid compact plane job mask");
        directKinds.assign(kinds.begin(),kinds.end());
        for(uint32_t job=0;job<jobs;++job)if(compactPlaneKind(kinds[job]))directKinds[job]=0;
    }
    for (const auto& run:meshDispatchRuns(jobs,compact?std::span<const uint8_t>(directKinds):kinds,false,dispatchYLimit_)) {
        const Params params{run.first, jobs|(coarse?kCoarseCacheFlag:0u), buffers_.cacheRectsPerJob};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(params), &params);
        vkCmdDispatch(cmd, run.groupsX, run.count, 1);
    }
    if(compact)for(uint32_t job=0;job<jobs;++job)if(compactPlaneKind(kinds[job])){
        const Params params{job,jobs|kCoarseCacheFlag|kPlaneCompactFlag,buffers_.cacheRectsPerJob};
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatchIndirect(cmd,buffers_.buffers[10],planeArgsOffset(jobs,buffers_.cacheRectsPerJob,job));
    }
}

void VulkanGpuMesher::recordCoarseCache(VkCommandBuffer cmd,uint32_t jobs,std::span<const uint8_t> kinds) const {
    if(!buffers_.coarseCache || kinds.empty())return;
    if(kinds.size()!=jobs)throw std::invalid_argument("Invalid coarse cache job mask");
    if(std::none_of(kinds.begin(),kinds.end(),[](uint8_t k){return (k&kCoarseOpaqueMesh)!=0;}))return;
    // Batch scratch may have been read by an earlier Emit. The batch fence
    // guards lifetime; the dependency also orders GPU accesses to the cache.
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&descriptorSet_,0,nullptr);
    for(uint32_t first=0;first<jobs;){
        if(!(kinds[first]&kCoarseOpaqueMesh)){++first;continue;}
        uint32_t n=1;
        while(n<dispatchYLimit_ && n<jobs-first && (kinds[first+n]&kCoarseOpaqueMesh))++n;
        const Params params{first,jobs|kCoarseCacheFlag,buffers_.cacheRectsPerJob,1u};
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatch(cmd,8,n,1);first+=n;
    }
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
}

void VulkanGpuMesher::recordPlaneCompaction(VkCommandBuffer cmd,uint32_t jobs,std::span<const uint8_t> kinds) const {
    if(!buffers_.planeCompaction || !std::any_of(kinds.begin(),kinds.end(),compactPlaneKind))return;
    if(kinds.size()!=jobs)throw std::invalid_argument("Invalid plane compaction job mask");
    // Initialize {x=0,y=1,z=1}. Each active plane atomically appends once.
    // The batch fence protects reuse; also order prior indirect/cache reads.
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    const std::array<uint32_t,3> initial{0,1,1};
    for(uint32_t job=0;job<jobs;++job)if(compactPlaneKind(kinds[job]))
        vkCmdUpdateBuffer(cmd,buffers_.buffers[10],planeArgsOffset(jobs,buffers_.cacheRectsPerJob,job),sizeof(initial),initial.data());
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[0]);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&descriptorSet_,0,nullptr);
    for(uint32_t first=0;first<jobs;){
        if(!compactPlaneKind(kinds[first])){++first;continue;}
        uint32_t n=1;
        while(n<dispatchYLimit_ && n<jobs-first && compactPlaneKind(kinds[first+n]))++n;
        const Params params{first,jobs|kCoarseCacheFlag|kPlaneCompactFlag,buffers_.cacheRectsPerJob,2u};
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatch(cmd,kPlaneScanGroups,n,1);first+=n;
    }
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void VulkanGpuMesher::recordCount(VkCommandBuffer cmd, uint32_t jobCount, std::span<const uint8_t> kinds) const {
    if (!jobCount) return; // no command or GPU query when no work
    if (!device_) throw std::logic_error("VulkanGpuMesher not initialized");
    resetCounts(cmd, jobCount);
    recordCoarseCache(cmd,jobCount,kinds);
    recordPlaneCompaction(cmd,jobCount,kinds);
    dispatchMeshes(cmd, jobCount, pipelines_[0], kinds);
    // Caller may now record vkCmdCopyBuffer(counter SSBO -> async readback buffer).
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

void VulkanGpuMesher::recordEmit(VkCommandBuffer cmd, uint32_t jobCount, std::span<const uint8_t> kinds) const {
    if (!jobCount) return;
    if (!device_) throw std::logic_error("VulkanGpuMesher not initialized");
    if(jobCount>maxJobs_)throw std::out_of_range("VulkanGpuMesher: too many emit jobs");
    if(buffers_.cacheRectsPerJob){
        // The earlier Count submission wrote both rectangles and exact counts.
        // Preserve counts on GPU before clearing the Emit counter array.
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT);
        const VkBufferCopy copy{0,0,VkDeviceSize(jobCount)*4};
        vkCmdCopyBuffer(cmd,buffers_.buffers[2],buffers_.buffers[10],1,&copy);
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    resetCounts(cmd, jobCount);
    // Uncached Emit reuses the Count classification. Caller rejects stale
    // generation/revision jobs and retains the same per-job batch indices.
    if(buffers_.coarseCache && std::any_of(kinds.begin(),kinds.end(),[](uint8_t k){return (k&kCoarseOpaqueMesh) && !(k&kCachedOpaqueMesh);}))
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    dispatchMeshes(cmd, jobCount, pipelines_[1], kinds);
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[2]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const uint64_t maxFinalJobs = uint64_t(finalizeXLimit_) * 64u;
    for (uint32_t first = 0; first < jobCount; ) {
        const uint32_t count = static_cast<uint32_t>(std::min(maxFinalJobs, uint64_t(jobCount - first)));
        const Params params{first, jobCount, buffers_.cacheRectsPerJob};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(params), &params);
        vkCmdDispatch(cmd, (count + 63u) / 64u, 1, 1);
        first += count;
    }
    // Candidate commands and mesh are GPU-produced; neither is published yet.
    // The caller performs async readback of counters and checks success/version.
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);

}
void VulkanGpuMesher::recordFastOpaque(VkCommandBuffer cmd,uint32_t jobs,BufferRange slab) const {
    const uint64_t stride=(uint64_t(buffers_.cacheRectsPerJob)+1u)&~uint64_t(1);
    if(!device_ || !jobs || jobs>maxJobs_ || !buffers_.cacheRectsPerJob ||
       !slab || slab.offset%16 || slab.size!=stride*jobs*8 || slab.offset/8>=UINT32_MAX ||
       slab.offset>buffers_.byteSizes[3] || slab.size>buffers_.byteSizes[3]-slab.offset)
        throw std::invalid_argument("Invalid fast opaque slab");
    // Count counters/cache stay on-device; copy does not reset either buffer.
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines_[1]);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&descriptorSet_,0,nullptr);
    for(uint32_t first=0;first<jobs;){
        const uint32_t n=std::min(dispatchYLimit_,jobs-first);
        const Params params{first,jobs,buffers_.cacheRectsPerJob,uint32_t(slab.offset/8)+1u};
        vkCmdPushConstants(cmd,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatch(cmd,8,n,1);first+=n;
    }
    barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT);
}
void VulkanGpuMesher::recordPublish(VkCommandBuffer cmd, uint32_t jobs) const {
    if (!jobs) return;
    if (!device_ || jobs > maxJobs_) throw std::out_of_range("VulkanGpuMesher::recordPublish invalid jobs");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[3]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    const uint64_t maxBatch = uint64_t(finalizeXLimit_) * 64u;
    for (uint32_t first = 0; first < jobs;) {
        const uint32_t count = static_cast<uint32_t>(std::min(maxBatch, uint64_t(jobs - first)));
        const Params params{first, jobs, buffers_.cacheRectsPerJob};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(params), &params);
        vkCmdDispatch(cmd, (count + 63u) / 64u, 1, 1);
        first += count;
    }
    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}
} // namespace OptiCraft::Vulkan
