#pragma once
#include "world/generation/Gpu_Column_Material_Input.h"
#include <limits>
#include <initializer_list>

namespace OptiCraft::Vulkan {
// Stage125: bounded command/resource contract for a single column, with
// external producers (layers, optional regional profiles and batch kinds).
// No queue submission, fence wait, publication or Vulkan allocation here.
struct GpuColumnMaterialPlan {
    using Input=Gpu_Column_Material_Input;
    enum class Resource : uint8_t { Layers, Columns, InputWords, RiverProfiles, CaveKinds,
        OrePermutation, OreGradients, OreLattice, OreOctants, Output, Count };
    enum class Pass : uint8_t { Init, RiverPack, CavePack, Base };
    struct Binding { uint32_t slot; Resource resource; };
    struct Dispatch {
        Pass pass{}; uint32_t x=0,y=1,z=1;
        std::array<Binding,7> bindings{}; uint32_t bindingCount=0;
        uint32_t pushBytes=0;
        // Caller inserts a compute write -> compute read/write barrier after
        // this pass, even for independent fields of the same storage buffer.
        bool barrierAfter=true;
    };
    struct Limits {
        bool fp64=false;
        uint32_t groupsX=0,groupsY=0,groupsZ=0,localX=0,invocations=0;
        uint32_t perStageStorage=0,setStorage=0,pushBytes=0;
        uint64_t storageRange=0,offsetAlignment=0;
    };
    struct Slice {uint64_t offset,bytes,capacity;};
    struct BasePush {
        int32_t x,z,count,minY,maxY;
        uint32_t ids[10];uint32_t certify;
    };
    Input::Push init{};
    Gpu_River_Dispatch::BridgePush river{};
    Input::CavePush cave{};
    BasePush base{};
    std::array<uint64_t,size_t(Resource::Count)> bytes{};
    std::array<Dispatch,4> steps{};
    uint32_t stepCount=0;
    // Must clear this output subrange to zero before Base (transfer -> compute
    // read/write barrier), even when reusing storage. Positive certificates only.
    uint64_t certificateOffset=0,certificateBytes=0;
    static bool validSlice(Slice s,uint64_t required,const Limits& l) noexcept {
        return required && s.bytes==required && required<=l.storageRange && l.offsetAlignment &&
            s.offset%l.offsetAlignment==0 && s.offset<=s.capacity && s.bytes<=s.capacity-s.offset;
    }
    static bool make(int32_t cx,int32_t cz,std::span<const int32_t> levels,
                     int32_t sea,uint32_t water,uint32_t floor,bool sky,
                     int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,
                     uint64_t oreNodes,std::optional<Input::Region> region,
                     std::span<const Gpu_Cave_Dispatch::BatchColumn> caveBatch,
                     const Limits& l,GpuColumnMaterialPlan& out) noexcept {
        GpuColumnMaterialPlan p;
        if(!Input::make(cx,cz,levels,sea,water,floor,sky,p.init)||!l.fp64||
           minY>maxY||minY < -100000000||maxY>100000000||!oreNodes||
           oreNodes>std::numeric_limits<uint64_t>::max()/sizeof(Gpu_Lattice_Node)||
           !l.offsetAlignment||l.localX<64||l.invocations<64||l.groupsZ<1||
           l.perStageStorage<7||l.setStorage<7||l.pushBytes<sizeof(BasePush)) return false;
        for(auto id:oreIds)if(id>=Gpu_Base_Region::fallback||id==0)return false;
        const uint32_t n=p.init.count;
        if(l.groupsX<n*512u||l.groupsY<(caveBatch.empty()?1u:n))return false;
        if(region&&!Gpu_River_Dispatch::bridge(region->x,region->z,cx,cz,p.river))return false;
        if(!caveBatch.empty()&&!Input::cave_source(cx,cz,sea,n,caveBatch,p.cave))return false;
        p.base={cx,cz,int32_t(n),minY,maxY,{},1};
        std::copy(oreIds.begin(),oreIds.end(),p.base.ids);
        auto size=[&](Resource r,uint64_t b){p.bytes[size_t(r)]=b;};
        size(Resource::Layers,1024*sizeof(Gpu_Layer_Dispatch::Layer));
        size(Resource::Columns,1024*sizeof(Gpu_Material_Column));
        size(Resource::InputWords,uint64_t(Input::words(n))*4);
        if(region)size(Resource::RiverProfiles,Gpu_River_Dispatch::columns*sizeof(Gpu_River_Dispatch::Profile));
        if(!caveBatch.empty())size(Resource::CaveKinds,caveBatch.size()*Gpu_Cave_Dispatch::kind_words*4ull);
        size(Resource::OrePermutation,2048*4);size(Resource::OreGradients,2048*32);
        size(Resource::OreLattice,oreNodes*sizeof(Gpu_Lattice_Node));size(Resource::OreOctants,8*4);
        size(Resource::Output,uint64_t(Input::output_words(n))*4);
        for(auto b:p.bytes)if(b>l.storageRange)return false;
        p.certificateOffset=uint64_t(n)*Gpu_Base_Region::voxels/2*4;p.certificateBytes=uint64_t(n)*4;
        auto add=[&](Pass pass,uint32_t x,uint32_t y,uint32_t push,std::initializer_list<Binding> bindings){
            auto& s=p.steps[p.stepCount++];s.pass=pass;s.x=x;s.y=y;s.pushBytes=push;
            s.bindingCount=uint32_t(bindings.size());std::copy(bindings.begin(),bindings.end(),s.bindings.begin());
        };
        add(Pass::Init,Input::init_groups,1,sizeof(Input::Push),{{0,Resource::Layers},{1,Resource::Columns},{2,Resource::InputWords}});
        if(region)add(Pass::RiverPack,16,1,sizeof(p.river),{{6,Resource::RiverProfiles},{7,Resource::InputWords}});
        if(!caveBatch.empty())add(Pass::CavePack,32,n,sizeof(p.cave),{{6,Resource::CaveKinds},{7,Resource::InputWords}});
        add(Pass::Base,n*512,1,sizeof(BasePush),{{0,Resource::OrePermutation},{1,Resource::OreGradients},
            {2,Resource::OreLattice},{3,Resource::OreOctants},{4,Resource::Columns},{5,Resource::InputWords},{6,Resource::Output}});
        out=p;return true;
    }
    // Transitional live path: existing CPU-prepared columns and packed regional
    // words are already in the Base ABI. Do not recompute or reinterpret them.
    static bool makePrepared(int32_t cx,int32_t cz,std::span<const int32_t> levels,
                             int32_t minY,int32_t maxY,std::span<const uint32_t,10> oreIds,
                             uint64_t oreNodes,const Limits& limits,GpuColumnMaterialPlan& out) noexcept {
        GpuColumnMaterialPlan p;
        if(!make(cx,cz,levels,0,0,0,false,minY,maxY,oreIds,oreNodes,std::nullopt,{},limits,p))return false;
        p.bytes[size_t(Resource::Layers)]=0;
        p.steps[0]=p.steps[p.stepCount-1];p.stepCount=1;
        out=p;return true;
    }

};
static_assert(sizeof(GpuColumnMaterialPlan::BasePush)==64);
} // namespace OptiCraft::Vulkan
