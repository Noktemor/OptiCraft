#pragma once
#include "rendering/vulkan/VulkanDeviceBuffer.h"
#include "rendering/vulkan/VulkanColumnMaterialRuntime.h"
#include "world/generation/Gpu_Worldgen_Data.h"
#include "world/generation/Gpu_Overlay_Work.h"
#include "rendering/vulkan/OverlaySnapshot.h"
#include <array>
#include <vector>
#include <chrono>
class World_Generator;
namespace OptiCraft::Vulkan {
class VulkanFrameRuntime;
// Main-thread only; borrowed device/queue must outlive this object.
class VulkanMaterialWorldgen final {
public:
    explicit VulkanMaterialWorldgen(VulkanFrameRuntime& runtime):runtime_(runtime){}
    ~VulkanMaterialWorldgen();
    VulkanMaterialWorldgen(const VulkanMaterialWorldgen&)=delete;
    VulkanMaterialWorldgen& operator=(const VulkanMaterialWorldgen&)=delete;
    bool start(const World_Generator&,int,int,const std::array<Gpu_Material_Column,1024>&,const std::vector<int>&, const Gpu_Base_Region&);
    // Two consecutive 16-bit IDs per word. Base mode uses 0xffff as CPU fallback;
    // legacy material mode uses zero. Mode is tracked by the owning batch.
    bool start_overlay(const Gpu_Overlay_Work& work);
    bool poll(std::vector<uint32_t>&,double&);
    // Independent Overlay producer for an exact, resolved base and ordered
    // structure commands. No CPU readback/replay; caller supplies authoritative
    // light and guarantees input identity/semantic validity before admission.
    enum class SnapshotPoll { Idle, Pending, Ready, Failed };
    bool start_overlay_snapshot(const Gpu_Overlay_Work& work);
    // Opt-in prepared regional base. Only positive certificates are returned;
    // rejected levels must use normal CPU generation. No full base readback.
    bool start_base_certificate(const World_Generator&,int,int,const std::array<Gpu_Material_Column,1024>&,
                                const std::vector<int>&,const Gpu_Base_Region&,const Gpu_Cave_Dispatch::Packet* =nullptr);
    SnapshotPoll poll_base_certificate(std::vector<Gpu_Certified_Base>&);
    bool start_overlay_certified(const Gpu_Overlay_Work&,const Gpu_Certified_Base&);
    SnapshotPoll poll_overlay_snapshot(std::shared_ptr<OverlaySnapshot>& result);

    // Owner-thread lease query. No allocation or fence wait. A false result
    // is temporary backpressure, not a failed certificate.
    bool overlaySnapshotAvailable() const noexcept {
        for(const auto& entry:overlaySnapshots_)if(!entry || entry.use_count()==1)return true;
        return false;
    }
    void abandon() noexcept; // invalidate result; retain in-flight resources until destruction
    uint64_t base_ticket() const noexcept {return baseTicket_;}
    OverlayReference overlay_reference() const noexcept {return lastOverlayReference_;}
    bool enabled()const noexcept{return !disabled_;}
private:
    void initialize(const World_Generator&);
    void initialize_commands();
    void initialize_overlay();
    bool start_impl(const World_Generator&,int,int,const std::array<Gpu_Material_Column,1024>&,
                    const std::vector<int>&,const Gpu_Base_Region&,bool certify,const Gpu_Cave_Dispatch::Packet* =nullptr);
    bool start_overlay_impl(const Gpu_Overlay_Work&,bool snapshotOnly,const Gpu_Certified_Base* certified=nullptr);
    bool recordPreparedBase(const World_Generator&,int x,int z,std::span<const int32_t> levels,
                            std::span<const uint32_t,10> ids,const Gpu_Cave_Dispatch::Packet*);
    VulkanColumnMaterialRuntime nativeColumn_;
    bool nativeInitialized_=false,nativeUnavailable_=false,nativeBase_=false;
    bool climateInitialized_=false,climateUnavailable_=false,climateReported_=false;
    bool riverInitialized_=false,riverUnavailable_=false,riverReported_=false;
    bool caveInitialized_=false,caveUnavailable_=false,caveReported_=false;
    bool certifying_=false;
    std::array<Gpu_Certified_Base,9> certifiedLevels_{};
    bool reserveOverlaySnapshot(bool required);
    bool snapshotOnly_=false;
    void initialize_propagation();
    void recordOverlaySnapshot();
    std::array<std::shared_ptr<OverlaySnapshot>,3> overlaySnapshots_;
    std::shared_ptr<OverlaySnapshot> pendingOverlaySnapshot_;
    OverlayReference lastOverlayReference_;
    std::array<VulkanDeviceBuffer,5> overlayBuffers_;
    VkDescriptorSetLayout overlayLayout_=VK_NULL_HANDLE;
    VkDescriptorPool overlayPool_=VK_NULL_HANDLE;
    VkDescriptorSet overlaySet_=VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_=VK_NULL_HANDLE;
    VkPipeline overlayPipeline_=VK_NULL_HANDLE;
    VkPipeline propagationPipeline_=VK_NULL_HANDLE;
    bool overlayPropagated_=false;
    size_t overlayCount_=0;
    bool overlaySky_=false;
    bool overlaySeeds_=false;
    bool overlayDisabled_=false;
    bool treeCatalogReady_=false,treeSelectionReported_=false;
    uint32_t treeReportOffset_=0,treeReportCount_=0;
    VulkanFrameRuntime& runtime_;
    std::array<VulkanDeviceBuffer,7> buffers_;
    VkDescriptorSetLayout layout_=VK_NULL_HANDLE;
    VkDescriptorPool descriptors_=VK_NULL_HANDLE;
    VkDescriptorSet set_=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkCommandPool pool_=VK_NULL_HANDLE;
    VkCommandBuffer cmd_=VK_NULL_HANDLE;
    VkFence fence_=VK_NULL_HANDLE;
    VkQueryPool timestamps_=VK_NULL_HANDLE;
    uint32_t timestampBits_=0;
    double timestampNs_=0;
    size_t count_=0;
    uint64_t baseTicket_=0;
    bool baseMode_=false;
    bool submitted_=false, disabled_=false;
    std::chrono::steady_clock::time_point started_{};
};
}
