#pragma once
#include "rendering/vulkan/GpuColumnMaterialPlan.h"
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include <array>
#include <filesystem>
#include <memory>

namespace OptiCraft::Vulkan {
// Actual Vulkan recording of the Stage125 plan. One scratch allocation set per
// instance, no submission/wait: retire only after the submission fence and
// every external consumer of result() have completed. External sources are
// immutable for the entire transaction and must belong to the same world.
class VulkanColumnMaterialRuntime {
public:
    using Plan=GpuColumnMaterialPlan;
    using Resource=Plan::Resource;
    struct Source {VkBuffer buffer=VK_NULL_HANDLE;VkDeviceSize offset=0,bytes=0,capacity=0;};
    using Sources=std::array<Source,size_t(Resource::Count)>;
    struct Request {
        int32_t x,z,sea,minY,maxY;uint32_t water,floor;bool sky;
        std::span<const int32_t> levels;
        std::array<uint32_t,10> oreIds;
        uint64_t oreNodes;
        std::optional<Plan::Input::Region> river;
        std::span<const Gpu_Cave_Dispatch::BatchColumn> cave;
    };
    VulkanColumnMaterialRuntime()=default;
    ~VulkanColumnMaterialRuntime(){shutdown();}
    VulkanColumnMaterialRuntime(const VulkanColumnMaterialRuntime&)=delete;
    VulkanColumnMaterialRuntime& operator=(const VulkanColumnMaterialRuntime&)=delete;
    void initialize(VkPhysicalDevice,VkDevice,bool fp64,const std::filesystem::path& shaderDir);
    // Sources: Layers, optional RiverProfiles/CaveKinds, four ore tables.
    // Writable Columns/InputWords/Output are internally allocated and must NOT
    // be supplied. No commands or descriptor writes on rejected requests.
    bool record(VkCommandBuffer,const Request&,const Sources&);
    // Prepared Base-only bridge: Columns/InputWords are external immutable
    // slices in the legacy ABI. No Layers/RiverProfiles/CaveKinds supplied.
    bool recordPrepared(VkCommandBuffer,int32_t x,int32_t z,std::span<const int32_t> levels,
                        int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,
                        uint64_t oreNodes,const Sources&);
    // Optional, world-lifetime climate tables and pipelines. An unavailable
    // climate path leaves the Stage129 Base-only recorder usable.
    void initializeClimate(const Gpu_Climate_Tables&,const std::filesystem::path&);
    bool recordClimatePrepared(VkCommandBuffer,int32_t x,int32_t z,std::span<const int32_t> levels,
                        int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,
                        uint64_t oreNodes,const Sources&,const Gpu_Layer_Dispatch::Push&,const Gpu_Cave_Dispatch::Packet* =nullptr);
    // Optional compact hydrology: cached immutable CPU graph, local GPU resolve
    // and guarded material merge. No cold regional calculation here.
    void initializeRiver(const std::filesystem::path&);
    bool recordClimateRiverPrepared(VkCommandBuffer,int32_t x,int32_t z,std::span<const int32_t> levels,
                        int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,
                        uint64_t oreNodes,const Sources&,const Gpu_Layer_Dispatch::Push&,
                        std::shared_ptr<const Gpu_River_Dispatch::CompactPacket>,const Gpu_Cave_Dispatch::Packet* =nullptr);
    void initializeCave(const std::filesystem::path&);
    std::optional<uint32_t> caveAccepted() const;
    std::optional<uint32_t> riverAccepted() const;
    // Fence must have signalled. Empty means no climate pass in this transaction.
    std::optional<uint32_t> climateAccepted() const;
    Source result() const noexcept;
    // Read certificates only after fence, before retire. Positive certificate
    // is a fallback-free Base result, NOT an independent world parity proof.
    std::vector<uint32_t> certificates() const;
    void retire() noexcept {busy_=false;resultBytes_=0;certCount_=0;climateUsed_=false;riverUsed_=false;caveUsed_=false;}
    bool busy()const noexcept{return busy_;}
    void shutdown() noexcept; // owner guarantees idle
private:
    struct PassGpu {VkDescriptorSetLayout layout=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;};
    bool recordPlan(VkCommandBuffer,const Plan&,const Sources&,bool prepared,const Gpu_Layer_Dispatch::Push* climate=nullptr, const Gpu_River_Dispatch::ColumnPush* river=nullptr,const Gpu_Cave_Dispatch::Packet* cave=nullptr);
    void ensure(VulkanDeviceBuffer&,VkDeviceSize,VkBufferUsageFlags,VkMemoryPropertyFlags,VkMemoryPropertyFlags=0);
    VkPhysicalDevice physical_=VK_NULL_HANDLE;VkDevice device_=VK_NULL_HANDLE;
    VkDescriptorPool pool_=VK_NULL_HANDLE;Plan::Limits limits_{};
    std::array<PassGpu,4> passes_{};
    VulkanDeviceBuffer columns_,words_,output_,readback_;
    VkDescriptorPool climatePool_=VK_NULL_HANDLE;
    std::array<PassGpu,4> climatePasses_{};
    std::array<VulkanDeviceBuffer,3> climateTables_;
    VulkanDeviceBuffer rawClimate_,terrain_,layers_,climateStats_;
    bool climateUsed_=false;
    VkDescriptorPool riverPool_=VK_NULL_HANDLE;
    std::array<PassGpu,2> riverPasses_{};
    VulkanDeviceBuffer riverCoarse_,riverGraph_,riverResolved_,riverStats_;
    std::shared_ptr<const Gpu_River_Dispatch::CompactPacket> riverCached_;
    bool riverUsed_=false;
    VkDescriptorPool cavePool_=VK_NULL_HANDLE;
    std::array<PassGpu,3> cavePasses_{};
    std::array<VulkanDeviceBuffer,3> caveTables_;
    VulkanDeviceBuffer caveStamps_,caveHeights_,caveMasks_,caveKinds_,caveStats_;
    bool caveUsed_=false;
    bool prepareCave(const Plan&,const Gpu_Cave_Dispatch::Packet&);
    void destroyCave() noexcept;
    void destroyRiver() noexcept;
    void destroyClimate() noexcept;
    void makePass(PassGpu&,VkDescriptorPool,const std::filesystem::path&,const char*,uint32_t,uint32_t,uint32_t);
    void bindPass(const PassGpu&,std::span<const Source>,uint32_t first=0);
    bool busy_=false;VkDeviceSize resultBytes_=0;uint32_t certCount_=0;
};
}
