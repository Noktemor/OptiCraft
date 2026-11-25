#include <cstdlib>
#include <string_view>
#include "rendering/vulkan/VulkanLiveWorldRenderer.h"
#include "rendering/vulkan/OwnedWorldStartup.h"
#include "rendering/vulkan/VulkanBlockCatalog.h"
#include "world/blocks/Block_Types.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
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
        // Splitting the render pass + sampling full depth costs shared memory bandwidth.
        // Prefer the single pass on small/medium iGPU scenes; override for A/B testing.
        hizMinResidents_=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?1024u:256u;
        if(const char* value=std::getenv("OPTICRAFT_VULKAN_HIZ_MIN_RESIDENT")){
            char* end=nullptr;const auto n=std::strtoul(value,&end,10);
            if(end!=value && *end=='\0' && n>=1 && n<=4096)hizMinResidents_=uint32_t(n);
        }
        multiDraw_=runtime.supportsMultiDrawIndirect();
        const char* cacheOpaque=std::getenv("OPTICRAFT_VULKAN_OPAQUE_CACHE");
        opaqueCacheEnabled_=!(cacheOpaque && std::string(cacheOpaque)=="0");
        // Match generation/block BFS transparency, not visual occluder flags.
        lightTransparency_.fill(0);
        for(uint32_t id=0;id<65536;++id){
            if(blocks.sky_transparent_table()[id])lightTransparency_[id/32]|=1u<<(id&31u);
            lightEmission_[id]=blocks.light_emission_table()[id];
        }
        const auto catalog=GpuBlockCatalog::build(blocks);
        if(catalog.activeBlockDefinitions==0)
            throw std::runtime_error("Vulkan game material registry is empty");
        for(size_t id=0;id<catalog.blocks.size() && id<meshKinds_.size();++id){
            const auto flags=catalog.blocks[id].flags;
            meshKinds_[id]=uint8_t(((flags&VisualOpaqueCube)?1u:0u) |
                ((flags&(VisualAlphaCube|VisualFlora|VisualLiquid|VisualShaped))?2u:0u));
        }
        const char* pages=std::getenv("OPTICRAFT_VULKAN_VOXEL_PAGES");
        if(pages && std::string(pages)=="0")config.streaming.indirectVoxelPages=false;
        if(const char* grow=std::getenv("OPTICRAFT_VULKAN_GROW_VOXEL_ARENAS"))
            if(std::string(grow)=="0")config.streaming.growVoxelArenas=false;
        if(const char* shrink=std::getenv("OPTICRAFT_VULKAN_SHRINK_VOXEL_ARENAS"))
            if(std::string(shrink)=="0")config.streaming.shrinkDelayFrames=0;
        // Dynamic arenas are independently bounded by dense-page and SSBO limits.
        // Only the legacy fixed-address path must reserve 128 KiB per logical slot.
        const bool dynamic=config.streaming.indirectVoxelPages && config.streaming.growVoxelArenas;
        const uint32_t maxSlots=dynamic?4096u:std::min(2048u,props.limits.maxStorageBufferRange/65536u);
        config.streaming.maxResident=std::min(config.streaming.maxResident,maxSlots);
        config.streaming.maxQueued=std::max(config.streaming.maxQueued,config.streaming.maxResident*2u);
        stream_.initialize(runtime.physicalDevice(),device_,config.streaming);
        std::cout<<"[Vulkan] logical chunk slots "<<stream_.plan().capacity()<<'\n';
        compactDraws_=runtime.supportsIndirectCount() && maxIndirectCount_>=stream_.plan().capacity();
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
            const auto miscBytes=bytes*(stream_.plan().capacity()>2048u?2u:1u);
            f.misc.create(runtime.physicalDevice(),device_,miscBytes,usage,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            const auto counterUsage=usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            f.opaqueCount.create(runtime.physicalDevice(),device_,4,counterUsage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            f.miscCount.create(runtime.physicalDevice(),device_,4,counterUsage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VulkanCullerBuffers a{};a.compact=compactDraws_;
            const char* bins=std::getenv("OPTICRAFT_VULKAN_OPAQUE_BINS");
            a.opaqueDepthBins=!(bins && std::string(bins)=="0");
            a.buffers={stream_.opaqueSlots(),stream_.publishedDraws(),
                       stream_.liveSlotIndices(),f.opaque.buffer(),f.opaqueCount.buffer()};
            a.byteSizes={VkDeviceSize(stream_.plan().capacity())*sizeof(GpuChunkSlot),
                bytes,VkDeviceSize(stream_.plan().capacity())*4,bytes,4};
            f.opaqueCuller.initialize(runtime.physicalDevice(),device_,a,normalCuller);
            hizBuffers_[size_t(&f-indirect_.data())]=a;
            // Back-to-front raw list for alpha; the partition reverses the depth list.
            VulkanCullerBuffers b{};b.compact=false;b.depthSort=true;b.chunkSlotStride=sizeof(GpuMiscSlot);
            b.buffers={stream_.miscSlots(),meshing_.miscPublishedDraws(),
                       stream_.liveSlotIndices(),f.misc.buffer(),f.miscCount.buffer()};
            b.byteSizes={VkDeviceSize(stream_.plan().capacity())*sizeof(GpuMiscSlot),
                bytes,VkDeviceSize(stream_.plan().capacity())*4,miscBytes,4};
            f.miscCuller.initialize(runtime.physicalDevice(),device_,b,miscCuller);
        }
        std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight> alphaInputs{};
        for(size_t i=0;i<alphaInputs.size();++i)alphaInputs[i]=indirect_[i].misc.buffer();
        alphaSort_.initialize(runtime,alphaInputs,VkDeviceSize(stream_.plan().capacity())*sizeof(VkDrawIndirectCommand),meshing_.miscQuads(),meshing_.miscQuadBytes(),materials_.blockDescriptors(),materials_.blockDescriptorBytes(),shader("voxel_alpha_sort.comp"));
        createGraphics(runtime.renderPass());
        createHiZ();
        uiPipeline_.initialize(runtime,shader("ui.vert"),shader("ui.frag"));
        itemPipeline_.initialize(runtime,miscPipeline_.atlasSetLayout(),shader("item.vert"),shader("item.frag"));
        entityPipeline_.initialize(runtime,miscPipeline_.atlasSetLayout(),shader("entity.vert"),shader("entity.frag"));
        const auto initial=config.meshing.workBudget?config.meshing.workBudget:config.meshing.batchLimit*8u;
        workBudget_.reset(initial,initial*2u);
        // Timestamp queries are optional. If the queue cannot timestamp, keep the static work budget.
        uint32_t familyCount=0;
        vkGetPhysicalDeviceQueueFamilyProperties(runtime.physicalDevice(),&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(runtime.physicalDevice(),&familyCount,families.data());
        const auto family=runtime.graphicsQueueFamily();
        if(family<familyCount && families[family].timestampValidBits && props.limits.timestampPeriod>0){
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=kQueriesPerFrame*VulkanFrameRuntime::kFramesInFlight;
            VkQueryPool queries=VK_NULL_HANDLE;
            if(vkCreateQueryPool(device_,&info,nullptr,&queries)==VK_SUCCESS){
                meshQueries_=queries;timestampBits_=families[family].timestampValidBits;
                timestampNs_=props.limits.timestampPeriod;
            }
        }
    }catch(...){shutdown();throw;}
}
void VulkanLiveWorldRenderer::createHiZ(){
    hiz_.shutdown();
    const char* disable=std::getenv("OPTICRAFT_VULKAN_HIZ");
    if(!runtime_->supportsDepthPyramid() || (disable && std::string(disable)=="0"))return;
    try{
        std::array<VkBuffer,VulkanFrameRuntime::kFramesInFlight> misc{};
        for(size_t i=0;i<misc.size();++i)misc[i]=indirect_[i].misc.buffer();
        hiz_.initialize(*runtime_,hizBuffers_,shader("voxel_hiz.comp"),misc,meshing_.miscQuads(),meshing_.miscQuadBytes());
    }
    catch(const std::exception& e){std::cerr<<"[Vulkan] Hi-Z unavailable: "<<e.what()<<'\n';}
    std::cout<<"[Vulkan] Hi-Z "<<(hiz_.ready()?"available, min residents ":"disabled, threshold ")<<hizMinResidents_<<'\n';
}
void VulkanLiveWorldRenderer::createGraphics(VkRenderPass pass){
    if(!pass)throw std::invalid_argument("Vulkan render pass not ready");
    // Frame runtime waits for the entire device on the rare swapchain resize;
    // recreating pipelines here is safe after that transition, never per frame.
    opaquePipeline_.shutdown();miscPipeline_.shutdown();
    const auto mv=shader("voxel_misc.vert"),mf=shader("voxel_misc.frag");
    miscPipeline_.initialize(device_,pass,meshing_.miscLayout(),alphaSort_.layout(),mv,mf,true);
    const auto ov=shader("voxel_textured.vert"),of=shader("voxel_textured.frag");
    opaquePipeline_.initialize(device_,pass,miscPipeline_.atlasSetLayout(),
                               stream_,meshing_,materials_,ov,of);
    pipelineSwapchainGeneration_=runtime_->swapchainGeneration();
}
void VulkanLiveWorldRenderer::recordIndirect(VkCommandBuffer cmd,VkBuffer commands,
                                             uint32_t count,VkBuffer countBuffer)const{
    if(!count)return;
    if(countBuffer){
        vkCmdDrawIndirectCount(cmd,commands,0,countBuffer,0,count,sizeof(VkDrawIndirectCommand));
        return;
    }
    for(uint32_t first=0;first<count;){
        const uint32_t batch=multiDraw_?std::min(maxIndirectCount_,count-first):1u;
        vkCmdDrawIndirect(cmd,commands,VkDeviceSize(first)*sizeof(VkDrawIndirectCommand),
                          batch,sizeof(VkDrawIndirectCommand));
        first+=batch;
    }
}
void VulkanLiveWorldRenderer::recordFrame(VulkanFrameRuntime& runtime,
     const VulkanFrameRuntime::Frame& frame,const WorldViewParams& view,const GpuFrustum& frustum,
     bool drawWorld,const std::array<float,4>& clearColor,const UiBatch& ui,std::span<const ItemVertex> items,std::span<const EntityDraw> entities){
    if(!device_||&runtime!=runtime_||!frame.cmd)
        throw std::invalid_argument("VulkanLiveWorldRenderer invalid frame");
    if(runtime.swapchainGeneration()!=pipelineSwapchainGeneration_){
        for(auto& cache:miscCache_)cache.invalidate();
        createGraphics(frame.renderPass);
        createHiZ();
        uiPipeline_.recreate(frame.renderPass,shader("ui.vert"),shader("ui.frag"));
        itemPipeline_.recreate(frame.renderPass,shader("item.vert"),shader("item.frag"));
        entityPipeline_.recreate(frame.renderPass,shader("entity.vert"),shader("entity.frag"));
    }
    // VulkanFrameRuntime has already waited this frame slot's fence.
    // Read ONLY the completed frame-slot queries. Never WAIT on a timestamp.
    const uint32_t query=frame.frameSlot*kQueriesPerFrame;
    auto& flags=timestampFlags_[frame.frameSlot];
    if(meshQueries_){
        const uint64_t mask=timestampBits_>=64?UINT64_MAX:((uint64_t(1)<<timestampBits_)-1u);
        auto elapsed=[&](uint32_t offset,double& ms){
            uint64_t ticks[2]{};
            if(vkGetQueryPoolResults(device_,meshQueries_,query+offset,2,sizeof(ticks),ticks,
                                    sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS)return false;
            ms=double((ticks[1]-ticks[0])&mask)*timestampNs_/1e6;return true;
        };
        if(timestampWork_[frame.frameSlot]){
            double ms=0;
            if(elapsed(0,ms)){workBudget_.sample(ms,timestampWork_[frame.frameSlot],timestampCount_[frame.frameSlot]);meshing_.setWorkBudget(workBudget_.budget());}
        }
        if(flags){
            for(uint32_t i=0;i<gpuTimes_.size();++i){
                gpuTimes_[i]=0;
                if((flags&(1u<<i)) && !elapsed(2+2*i,gpuTimes_[i]))gpuTimes_[i]=-1;
            }
        }
        if(ownedWorld_ && ownedTimingValid_[frame.frameSlot] && flags && gpuTimes_[0]>0){
            using B=AdaptiveWorldWorkBudget;
            B::Sample sample;sample.serial=++workSampleSerial_;sample.frameMs=gpuTimes_[0];
            auto add=[&](B::Lane lane,uint32_t offset,uint32_t count){
                double ms=0;if(count && elapsed(offset,ms) && ms>0){sample.work[lane]=count;sample.ms[lane]=ms;}
            };
            add(B::Meshing,0,timestampWork_[frame.frameSlot]);
            const auto& timed=ownedTimedWork_[frame.frameSlot];
            add(B::BlockLight,10,timed[B::BlockLight]);add(B::SkyLight,12,timed[B::SkyLight]);
            add(B::Transfers,14,timed[B::Transfers]);
            ownedWorkBudget_.observe(sample);
        }
        ownedTimedWork_[frame.frameSlot]={};
        ownedTimingValid_[frame.frameSlot]=bool(ownedWorld_);
        timestampWork_[frame.frameSlot]=0;timestampCount_[frame.frameSlot]=false;flags=1u;
        vkCmdResetQueryPool(frame.cmd,meshQueries_,query,kQueriesPerFrame);
        vkCmdWriteTimestamp(frame.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,meshQueries_,query+2);
    }
    auto stamp=[&](uint32_t offset){if(meshQueries_)vkCmdWriteTimestamp(frame.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,meshQueries_,query+offset);};
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
    using B=AdaptiveWorldWorkBudget;
    B::Counts grants{};
    if(ownedWorld_){
        const auto meshCap=workBudget_.budget();
        B::Counts demand{uint32_t(drawWorld && ownedWorld_->generationPending()>0),
            uint32_t(std::min<size_t>(meshCap,meshing_.pendingCount()+stream_.plan().pendingCount())),
            uint32_t(ownedWorld_->count()>0 || residentLightScheduler_.pendingCount()>0),
            uint32_t(ownedWorld_->count()>0),uint32_t(ownedWorld_->needsTransfers())};
        grants=ownedWorkBudget_.allocate(demand,{1,meshCap,1,1,1});
        // Always drain submitted Base/Overlay; only new columns are budgeted.
        ownedWorld_->pumpPreparation(drawWorld && grants[B::Generation]>0);
        // Never starve Count/Emit; the existing transaction engine drains Emit independently.
        meshing_.setWorkBudget((demand[B::Generation] || ownedWorld_->count())?
            std::max(4u,grants[B::Meshing]):meshCap);
    }
    ownedGrants_=grants;
    const bool allowUploads=meshing_.acceptsUploads();
    auto changed=stream_.record(frame.cmd,frame.frameSlot,allowUploads);
    residentLightScheduler_.observe(changed);
    if(ownedWorld_)ownedWorld_->observe(changed);
    auto& light=residentLight_[frame.frameSlot];
    if(light)light->retire(); // beginFrame already waited this frame-slot fence
    // No allocation or dispatch in the normal CPU-owned world. Bound one
    // complete halo transaction per frame, and respect imminent mesh Emit.
    if(allowUploads && !residentLightUnavailable_ && (residentLightScheduler_.pendingCount() || (ownedWorld_ && ownedWorld_->count())) &&
       (!ownedWorld_ || grants[B::BlockLight])){
        if(ownedWorld_)stamp(10);
        const auto selection=residentLightScheduler_.select(stream_.plan());
        if(!selection.keys.empty() || (ownedWorld_ && ownedWorld_->count())){
            if(!light){
                try{
                    auto next=std::make_unique<VulkanResidentLight>();
                    next->initialize(runtime.physicalDevice(),device_,ResidentLightScheduler::defaultJobBudget,
                                     shader("worldgen_light_resident.comp"));
                    light=std::move(next);
                }catch(const std::exception& e){
                    residentLightUnavailable_=true;
                    if(ownedWorld_)ownedWorld_->disableNewGeneration();
                    std::cerr<<"[Vulkan] Resident light unavailable: "<<e.what()<<'\n';
                }
            }
            if(light && residentLightScheduler_.record(frame.cmd,frame.frameSlot,stream_,changed,*light,
                                                    selection,lightTransparency_,lightEmission_))
                residentLightRecordedCores_+=selection.keys.size();
            if(light && ownedWorld_)ownedWorld_->recordBlockPreview(frame.cmd,frame.frameSlot,changed,*light,
                        residentLightScheduler_,lightTransparency_,lightEmission_);
        }
        if(ownedWorld_){stamp(11);ownedTimedWork_[frame.frameSlot][B::BlockLight]=uint32_t(light && light->busy());}
    }
    if(ownedWorld_){
        auto& sky=ownedSky_[frame.frameSlot];if(sky)sky->retire();
        if(allowUploads && ownedWorld_->count() && !ownedSkyUnavailable_ && grants[B::SkyLight]){
            stamp(12);
            if(!sky)try{
                auto next=std::make_unique<VulkanResidentLight>();
                next->initialize(runtime.physicalDevice(),device_,ResidentSkylightScheduler::defaultJobBudget,
                                 shader("worldgen_light_resident.comp"));sky=std::move(next);
            }catch(const std::exception& e){ownedSkyUnavailable_=true;ownedWorld_->disableNewGeneration();std::cerr<<"[Vulkan] Owned sky unavailable: "<<e.what()<<'\n';}
            if(sky)ownedWorld_->recordSky(frame.cmd,frame.frameSlot,changed,*sky,lightTransparency_);
            stamp(13);ownedTimedWork_[frame.frameSlot][B::SkyLight]=uint32_t(sky && sky->busy());
        }
        stamp(14);
        ownedTimedWork_[frame.frameSlot][B::Transfers]=uint32_t(ownedWorld_->pumpOwnership(frame.cmd,frame.frameSlot,changed,residentLightScheduler_,grants[B::Transfers]>0));
        stamp(15);
    }
    meshing_.selectVoxelFrame(frame.frameSlot);
    opaquePipeline_.selectVoxelFrame(frame.frameSlot,stream_.voxelEpoch(),stream_.voxelBuffers(),stream_.voxelSizes());
    const bool measure=meshQueries_ && meshing_.mayDispatch(changed);
    if(measure){
        vkCmdWriteTimestamp(frame.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,meshQueries_,query);
    }
    const auto publishedBefore=meshing_.stats().published;
    const uint32_t work=meshing_.record(frame.cmd,frame.frameSlot,changed);
    if(!changed.empty() || meshing_.stats().published!=publishedBefore)++drawRevision_;
    if(measure){
        vkCmdWriteTimestamp(frame.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,meshQueries_,query+1);
        timestampWork_[frame.frameSlot]=work;
        timestampCount_[frame.frameSlot]=meshing_.lastDispatchWasCount();
    }
    const uint32_t live=stream_.liveSlotCount();
    const FrameIndirect& f=indirect_[frame.frameSlot];
    const bool useHiZ=drawWorld && live>=hizMinResidents_ && hiz_.ready();
    auto& cache=miscCache_[frame.frameSlot];
    reusedMisc_=drawWorld && live && !useHiZ && cache.matches(view.vp,drawRevision_,live);
    if(useHiZ || !drawWorld || !live)cache.invalidate();
    auto sortAlpha=[&](){
        stamp(6);
        alphaSort_.record(frame,live,{view.vp[3],view.vp[7],view.vp[11],view.vp[15]});
        stamp(7);flags|=4u;
    };
    if(drawWorld && live){
        stamp(4);
        // Same frame-local revision/VP key covers all three draw lists.
        // Hi-Z rewrites opaque commands, so it never reuses the static lists.
        const std::array<float,4> depthDirection{view.vp[3],view.vp[7],view.vp[11],view.vp[15]};
        if(!reusedMisc_ || !opaqueCacheEnabled_)
            f.opaqueCuller.record(frame.cmd,live,frustum,useHiZ?view.eyeAmbient:depthDirection,useHiZ?1u:0u);
        if(!reusedMisc_)f.miscCuller.record(frame.cmd,live,frustum,{view.vp[3],view.vp[7],view.vp[11],view.vp[15]});
        stamp(5);flags|=2u;
        if(!useHiZ && !reusedMisc_){sortAlpha();cache.store(view.vp,drawRevision_,live);}
    }
    uiPipeline_.prepare(frame,ui);
    itemPipeline_.prepare(frame,drawWorld?items:std::span<const ItemVertex>{});
    entityPipeline_.prepare(frame,drawWorld?entities:std::span<const EntityDraw>{},frustum);
    const auto drawOpaque=[&](){
        const VkViewport viewport{0,0,float(frame.extent.width),float(frame.extent.height),0,1};
        const VkRect2D scissor{{0,0},frame.extent};
        vkCmdSetViewport(frame.cmd,0,1,&viewport);
        vkCmdSetScissor(frame.cmd,0,1,&scissor);
        // Draw opaque first; then shaped/cutout, then translucent objects.
        vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,opaquePipeline_.pipeline());
        const VkDescriptorSet opaqueSets[2]{opaquePipeline_.descriptorSet(),atlasSet_};
        vkCmdBindDescriptorSets(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,
            opaquePipeline_.layout(),0,2,opaqueSets,0,nullptr);
        vkCmdPushConstants(frame.cmd,opaquePipeline_.layout(),VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(view),&view);
        recordIndirect(frame.cmd,f.opaque.buffer(),live,compactDraws_?f.opaqueCount.buffer():VK_NULL_HANDLE);
    };
    runtime.beginColorPass(frame,clearColor,false,useHiZ);
    if(drawWorld && live){
        drawOpaque();
        if(useHiZ){
            runtime.endColorPass(frame);
            stamp(8);hiz_.record(runtime,frame,view,live);stamp(9);flags|=8u;
            sortAlpha();
            runtime.resumeColorPass(frame);
            drawOpaque();
        }
        miscPipeline_.recordDraws(frame.cmd,meshing_.miscSet(),atlasSet_,alphaSort_.set(frame.frameSlot),view,
                                  alphaSort_.commands(frame.frameSlot),alphaSort_.depthCommandOffset(frame.frameSlot),live,false,multiDraw_,maxIndirectCount_);
    }
    if(drawWorld){entityPipeline_.draw(frame,view);itemPipeline_.draw(frame,atlasSet_,view);}
    if(drawWorld && live)miscPipeline_.recordDraws(frame.cmd,meshing_.miscSet(),atlasSet_,alphaSort_.set(frame.frameSlot),view,
                                  alphaSort_.commands(frame.frameSlot),0,live,true,multiDraw_,maxIndirectCount_);
    uiPipeline_.draw(frame,ui,atlasSet_);
    runtime.endColorPass(frame);
    stamp(3);
}
void VulkanLiveWorldRenderer::attachWorld(Chunk_Manager& manager){
    const char* enabled=std::getenv("OPTICRAFT_VULKAN_OWNED_WORLD");
    if(!ownedWorldRequested(enabled,std::getenv("OPTICRAFT_GPU_WORLDGEN")) || ownedWorld_ || !runtime_ || residentLightUnavailable_ ||
       !stream_.indirectVoxelPages() || !OwnedWorldRuntime::reservedCapacity(residentCapacity()) ||
       !runtime_->supportsShaderFloat64() || !manager.world_generator())return;
    const auto& dim=manager.world_generator()->get_dimension();
    if(dim.terrain!=Terrain_Mode::Noise || !dim.ground)return;
    const auto reserve=OwnedWorldRuntime::reservedCapacity(residentCapacity());
    // Voxel arena soft admission limit, not total VRAM: includes retired voxel buffers.
    const uint64_t bytes=uint64_t(residentCapacity())*OwnedWorldRuntime::denseBytesPerChunk;
    try {
        // Reject missing mandatory light SPIR-V before excluding CPU chunks.
        (void)shader("worldgen_light_resident.comp");
        ownedWorld_=std::make_unique<OwnedWorldRuntime>(manager,stream_,*runtime_,reserve,bytes);
    } catch(const std::exception& e) {
        std::cerr<<"[Vulkan] Owned world unavailable, CPU generation retained: "<<e.what()<<'\n';
        return;
    }
    ownedWorkBudget_=AdaptiveWorldWorkBudget{};ownedTimedWork_={};ownedTimingValid_={};ownedGrants_={};
    std::cout<<"[Vulkan] owned-world reserve "<<reserve<<" voxel admission budget "<<bytes<<" B\n";
}
void VulkanLiveWorldRenderer::detachWorldIdle(){
    if(ownedWorld_){ownedWorld_->closeIdle();ownedWorld_.reset();meshing_.setWorkBudget(workBudget_.budget());}
    for(auto& sky:ownedSky_)sky.reset();ownedSkyUnavailable_=false;
    ownedTimedWork_={};ownedTimingValid_={};ownedGrants_={};
}
void VulkanLiveWorldRenderer::shutdown()noexcept{
    detachWorldIdle();
    // Owner must unregister Game callbacks, then wait for GPU idle before this.
    if(meshQueries_ && device_)vkDestroyQueryPool(device_,meshQueries_,nullptr);
    meshQueries_=VK_NULL_HANDLE;timestampBits_=0;timestampNs_=0;timestampWork_.fill(0);timestampCount_.fill(false);timestampFlags_.fill(0);gpuTimes_.fill(-1);
    for(auto& cache:miscCache_)cache.invalidate();drawRevision_=0;reusedMisc_=false;
    hiz_.shutdown();
    entityPipeline_.shutdown();
    itemPipeline_.shutdown();
    uiPipeline_.shutdown();
    opaquePipeline_.shutdown();miscPipeline_.shutdown();alphaSort_.shutdown();
    for(auto& frame:indirect_){
        frame.miscCuller.shutdown();frame.opaqueCuller.shutdown();
        frame.misc.destroy();frame.opaque.destroy();
        frame.miscCount.destroy();frame.opaqueCount.destroy();
    }
    for(auto& light:residentLight_)light.reset();
    residentLightScheduler_.reset();residentLightUnavailable_=false;residentLightRecordedCores_=0;
    meshing_.shutdown();atlas_.shutdown();materials_.shutdown();stream_.shutdown();
    device_=VK_NULL_HANDLE;runtime_=nullptr;atlasSet_=VK_NULL_HANDLE;
    initialUploadsRecorded_=false;staticStagingReleased_=false;
    pipelineSwapchainGeneration_=0;uploadFrameSlot_=UINT32_MAX;
}
} // namespace OptiCraft::Vulkan
