#include "rendering/vulkan/VulkanLiveWorldRenderer.h"
#include "rendering/vulkan/VulkanBlockCatalog.h"
#include "world/blocks/Block_Types.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace OptiCraft::Vulkan {
std::vector<uint32_t> VulkanLiveWorldRenderer::shader(const char* name) const {
    const auto path=shaderDirectory_/(std::string(name)+".spv");
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream)throw std::runtime_error("Missing Vulkan SPIR-V: "+path.string());
    const std::streamsize bytes=stream.tellg();
    if(bytes<4||bytes%4)throw std::runtime_error("Invalid Vulkan SPIR-V byte size: "+path.string());
    std::vector<uint32_t> words(size_t(bytes)/4);
    stream.seekg(0);
    if(!stream.read(reinterpret_cast<char*>(words.data()),bytes)||words[0]!=0x07230203u)
        throw std::runtime_error("Invalid Vulkan SPIR-V contents: "+path.string());
    return words;
}
void VulkanLiveWorldRenderer::initialize(VulkanFrameRuntime& runtime,
       const Block_Registry& blocks,const std::filesystem::path& shaderDir,
       const std::filesystem::path& atlasPng,Config config){
    if(device_ || !runtime.device() || !runtime.physicalDevice())
        throw std::invalid_argument("VulkanLiveWorldRenderer invalid runtime");
    device_=runtime.device();runtime_=&runtime;shaderDirectory_=shaderDir;
    try{
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(runtime.physicalDevice(),&props);
        maxIndirectCount_=std::max(1u,props.limits.maxDrawIndirectCount);
        multiDraw_=runtime.supportsMultiDrawIndirect();
        const auto catalog=GpuBlockCatalog::build(blocks);
        if(catalog.activeBlockDefinitions==0)
            throw std::runtime_error("Vulkan game material registry is empty");
        stream_.initialize(runtime.physicalDevice(),device_,config.streaming);
        materials_.initialize(runtime.physicalDevice(),device_,catalog);
        atlas_.initialize(runtime.physicalDevice(),device_,atlasPng.string(),16);
        auto oc=shader("voxel_count.comp"),oe=shader("voxel_emit.comp");
        auto of=shader("voxel_finalize.comp"),op=shader("voxel_publish.comp");
        auto mc=shader("voxel_misc_count.comp"),me=shader("voxel_misc_emit.comp");
        VulkanChunkMeshing::Shaders programs{oc,oe,of,op,mc,me};
        meshing_.initialize(runtime.physicalDevice(),device_,stream_,materials_,
                            config.meshing,programs);
        const auto normalCuller=shader("voxel_cull.comp");
        const auto miscCuller=shader("voxel_misc_cull.comp");
        for(auto& f:indirect_){
            const VkDeviceSize bytes=VkDeviceSize(stream_.plan().capacity())*
                sizeof(VkDrawIndirectCommand);
            const auto usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            f.opaque.create(runtime.physicalDevice(),device_,bytes,usage,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            f.misc.create(runtime.physicalDevice(),device_,bytes,usage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VulkanCullerBuffers a{};
            a.buffers={stream_.opaqueSlots(),stream_.publishedDraws(),
                       stream_.liveSlotIndices(),f.opaque.buffer()};
            a.byteSizes={VkDeviceSize(stream_.plan().capacity())*sizeof(GpuChunkSlot),
                bytes,VkDeviceSize(stream_.plan().capacity())*4,bytes};
            f.opaqueCuller.initialize(runtime.physicalDevice(),device_,a,normalCuller);
            VulkanCullerBuffers b{};b.chunkSlotStride=sizeof(GpuMiscSlot);
            b.buffers={stream_.miscSlots(),meshing_.miscPublishedDraws(),
                       stream_.liveSlotIndices(),f.misc.buffer()};
            b.byteSizes={VkDeviceSize(stream_.plan().capacity())*sizeof(GpuMiscSlot),
                bytes,VkDeviceSize(stream_.plan().capacity())*4,bytes};
            f.miscCuller.initialize(runtime.physicalDevice(),device_,b,miscCuller);
        }
        createGraphics(runtime.renderPass());
        uiPipeline_.initialize(runtime,shader("ui.vert"),shader("ui.frag"));
    }catch(...){shutdown();throw;}
}
void VulkanLiveWorldRenderer::createGraphics(VkRenderPass pass){
    if(!pass)throw std::invalid_argument("Vulkan render pass not ready");
    // Frame runtime waits for the entire device on the rare swapchain resize;
    // recreating pipelines here is safe after that transition, never per frame.
    opaquePipeline_.shutdown();miscPipeline_.shutdown();
    const auto mv=shader("voxel_misc.vert"),mf=shader("voxel_misc.frag");
    miscPipeline_.initialize(device_,pass,meshing_.miscLayout(),mv,mf,true);
    const auto ov=shader("voxel_textured.vert"),of=shader("voxel_textured.frag");
    opaquePipeline_.initialize(device_,pass,miscPipeline_.atlasSetLayout(),
                               stream_,meshing_,materials_,ov,of);
    pipelineSwapchainGeneration_=runtime_->swapchainGeneration();
}
void VulkanLiveWorldRenderer::recordIndirect(VkCommandBuffer cmd,VkBuffer commands,
                                             uint32_t count)const{
    if(!count)return;
    for(uint32_t first=0;first<count;){
        const uint32_t batch=multiDraw_?std::min(maxIndirectCount_,count-first):1u;
        vkCmdDrawIndirect(cmd,commands,VkDeviceSize(first)*sizeof(VkDrawIndirectCommand),
                          batch,sizeof(VkDrawIndirectCommand));
        first+=batch;
    }
}
void VulkanLiveWorldRenderer::recordFrame(VulkanFrameRuntime& runtime,
     const VulkanFrameRuntime::Frame& frame,const float vp[16],const GpuFrustum& frustum,
     bool drawWorld,const std::array<float,4>& clearColor,const UiBatch& ui){
    if(!device_||&runtime!=runtime_||!frame.cmd||!vp)
        throw std::invalid_argument("VulkanLiveWorldRenderer invalid frame");
    if(runtime.swapchainGeneration()!=pipelineSwapchainGeneration_){
        createGraphics(frame.renderPass);
        uiPipeline_.recreate(frame.renderPass,shader("ui.vert"),shader("ui.frag"));
    }
    // VulkanFrameRuntime has already waited this frame slot's fence.
    meshing_.beginFrame(frame.frameSlot);
    if(initialUploadsRecorded_ && !staticStagingReleased_ && frame.frameSlot==uploadFrameSlot_){
        materials_.releaseStagingAfterCompletion();
        atlas_.releaseStagingAfterCompletion();
        staticStagingReleased_=true;
    }
    if(!initialUploadsRecorded_){
        materials_.recordUpload(frame.cmd);
        atlas_.recordUpload(frame.cmd);
        atlasSet_=atlas_.createDescriptorSet(miscPipeline_.atlasSetLayout());
        initialUploadsRecorded_=true;uploadFrameSlot_=frame.frameSlot;
    }
    const auto changed=stream_.record(frame.cmd,frame.frameSlot);
    meshing_.record(frame.cmd,frame.frameSlot,changed);
    const uint32_t live=stream_.liveSlotCount();
    const FrameIndirect& f=indirect_[frame.frameSlot];
    if(drawWorld && live){
        f.opaqueCuller.record(frame.cmd,live,frustum);
        f.miscCuller.record(frame.cmd,live,frustum);
    }
    uiPipeline_.prepare(frame,ui);
    runtime.beginColorPass(frame,clearColor);
    if(drawWorld && live){
        const VkViewport viewport{0,0,float(frame.extent.width),float(frame.extent.height),0,1};
        const VkRect2D scissor{{0,0},frame.extent};
        vkCmdSetViewport(frame.cmd,0,1,&viewport);
        vkCmdSetScissor(frame.cmd,0,1,&scissor);
        // Draw opaque first; then shaped/cutout, then translucent objects.
        vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,opaquePipeline_.pipeline());
        const VkDescriptorSet opaqueSets[2]{opaquePipeline_.descriptorSet(),atlasSet_};
        vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,
            opaquePipeline_.layout(),0,2,opaqueSets,0,nullptr);
        vkCmdPushConstants(frame.cmd,opaquePipeline_.layout(),VK_SHADER_STAGE_VERTEX_BIT,0,64,vp);
        recordIndirect(frame.cmd,f.opaque.buffer(),live);
        for(bool alpha:{false,true}){
            miscPipeline_.recordDraws(frame.cmd,meshing_.miscSet(),atlasSet_,vp,
                                      f.misc.buffer(),0,live,alpha,multiDraw_,maxIndirectCount_);
        }
    }
    uiPipeline_.draw(frame,ui,atlasSet_);
    runtime.endColorPass(frame);
}
void VulkanLiveWorldRenderer::shutdown()noexcept{
    // Owner must unregister Game callbacks, then wait for GPU idle before this.
    uiPipeline_.shutdown();
    opaquePipeline_.shutdown();miscPipeline_.shutdown();
    for(auto& frame:indirect_){
        frame.miscCuller.shutdown();frame.opaqueCuller.shutdown();
        frame.misc.destroy();frame.opaque.destroy();
    }
    meshing_.shutdown();atlas_.shutdown();materials_.shutdown();stream_.shutdown();
    device_=VK_NULL_HANDLE;runtime_=nullptr;atlasSet_=VK_NULL_HANDLE;
    initialUploadsRecorded_=false;staticStagingReleased_=false;
    pipelineSwapchainGeneration_=0;uploadFrameSlot_=UINT32_MAX;
}
} // namespace OptiCraft::Vulkan
