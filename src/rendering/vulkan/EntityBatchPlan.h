#pragma once
#include "rendering/vulkan/EntityDraw.h"
#include "rendering/vulkan/GpuFrustum.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace OptiCraft::Vulkan {
// A supplied bound must enclose the CURRENT animated world-space geometry,
// including equipment/attachments. Missing or invalid bounds fail open.
struct EntityWorldBounds {std::array<float,3> min{},max{};};
inline EntityWorldBounds unknownEntityBounds() noexcept {
    const float nan=std::numeric_limits<float>::quiet_NaN();return {{nan,nan,nan},{nan,nan,nan}};
}
inline EntityWorldBounds entityLocalBounds(const EntityAsset& asset) noexcept {
    if(asset.vertices.empty())return unknownEntityBounds();
    EntityWorldBounds b;
    for(size_t i=0;i<3;++i)b.min[i]=b.max[i]=asset.vertices.front().position[i];
    for(const auto& v:asset.vertices)for(size_t i=0;i<3;++i){
        if(!std::isfinite(v.position[i]))return unknownEntityBounds();
        b.min[i]=std::min(b.min[i],v.position[i]);b.max[i]=std::max(b.max[i],v.position[i]);
    }
    return b;
}
inline EntityWorldBounds transformEntityBounds(const EntityWorldBounds& local,const std::array<float,16>& m) noexcept {
    for(float v:m)if(!std::isfinite(v))return unknownEntityBounds();
    if(m[3]!=0.f||m[7]!=0.f||m[11]!=0.f||m[15]!=1.f)return unknownEntityBounds();
    for(size_t i=0;i<3;++i)if(!std::isfinite(local.min[i])||!std::isfinite(local.max[i])||local.min[i]>local.max[i])return unknownEntityBounds();
    EntityWorldBounds b;
    for(size_t row=0;row<3;++row){
        double lo=m[12+row],hi=lo,scale=std::abs(lo);
        for(size_t col=0;col<3;++col){const double a=double(m[col*4+row])*local.min[col],c=double(m[col*4+row])*local.max[col];lo+=std::min(a,c);hi+=std::max(a,c);scale+=std::max(std::abs(a),std::abs(c));}
        const double pad=1.e-6*std::max(1.,scale);
        b.min[row]=float(lo-pad);b.max[row]=float(hi+pad);
    }
    return b;
}
// Skinning is a positive weighted sum of affine bone transforms. In clip
// space the sum has the same sign at each clipping plane as its normalized
// convex combination; an unweighted vertex takes the identity path. The
// union of transformed local AABBs (including identity) is conservative.
inline EntityWorldBounds animatedEntityBounds(const EntityWorldBounds& local,const EntityPose& pose) noexcept {
    auto result=transformEntityBounds(local,pose.model);
    const float count=pose.options[0];
    if(!std::isfinite(count)||count<0.f||count>float(EntityBones))return unknownEntityBounds();
    auto valid=[](const EntityWorldBounds& b){for(size_t i=0;i<3;++i)if(!std::isfinite(b.min[i])||!std::isfinite(b.max[i]))return false;return true;};
    if(!valid(result))return unknownEntityBounds();
    for(unsigned j=0;j<unsigned(count);++j){
        auto b=transformEntityBounds(transformEntityBounds(local,pose.bones[j]),pose.model);
        if(!valid(b))return unknownEntityBounds();
        for(size_t i=0;i<3;++i){result.min[i]=std::min(result.min[i],b.min[i]);result.max[i]=std::max(result.max[i],b.max[i]);}
    }
    return result;
}
inline bool entityVisible(const GpuFrustum& frustum,const EntityWorldBounds& b) noexcept {
    for(size_t i=0;i<3;++i)if(!std::isfinite(b.min[i])||!std::isfinite(b.max[i])||b.min[i]>b.max[i])return true;
    for(const auto& p:frustum.planes)for(float v:p)if(!std::isfinite(v))return true;
    for(const auto& p:frustum.planes){
        double distance=p[3],scale=std::abs(double(p[3]));
        for(size_t i=0;i<3;++i){const double t=double(p[i])*(p[i]>=0?b.max[i]:b.min[i]);distance+=t;scale+=std::abs(t);}
        // Guard against cancellation and float plane extraction error.
        if(distance < -1.e-5*std::max(1.,scale))return false;
    }
    return true;
}
struct EntityInstanceRun {
    std::shared_ptr<const EntityAsset> asset;
    std::string hiddenMaterial;
    uint32_t firstInstance=0,instanceCount=0;
    std::vector<uint32_t> primitives;
};
struct EntityBatchPlan {
    std::vector<EntityPose> poses;
    std::vector<EntityInstanceRun> runs;
    size_t culled=0,invalid=0;
    // Use only for the existing depth-writing, alpha-tested entity pipeline.
    // Stable group order; within a group source order and per-instance pose retained.
    // Optional bounds are parallel to the original input, not the sorted poses.
    static EntityBatchPlan build(std::span<const EntityDraw> draws,
        const GpuFrustum* frustum=nullptr,std::span<const EntityWorldBounds> bounds={}) {
        if(!bounds.empty()&&bounds.size()!=draws.size())throw std::invalid_argument("Entity bounds count");
        if(draws.size()>UINT32_MAX)throw std::length_error("Entity instance index overflow");
        EntityBatchPlan out;
        struct Key {const EntityAsset* asset;std::string hidden;bool operator==(const Key&)const=default;};
        struct Hash {size_t operator()(const Key& k)const noexcept {return std::hash<const EntityAsset*>{}(k.asset)^(std::hash<std::string>{}(k.hidden)<<1);}};
        std::unordered_map<Key,size_t,Hash> groups;
        std::vector<std::vector<uint32_t>> indices;
        for(size_t i=0;i<draws.size();++i){
            const auto& e=draws[i];
            if(!e.asset||e.asset->vertices.empty()||e.asset->indices.empty()){++out.invalid;continue;}
            if(frustum&&!bounds.empty()&&!entityVisible(*frustum,bounds[i])){++out.culled;continue;}
            auto [it,newGroup]=groups.try_emplace(Key{e.asset.get(),e.hiddenMaterial},out.runs.size());
            if(newGroup){
                EntityInstanceRun run;run.asset=e.asset;run.hiddenMaterial=e.hiddenMaterial;
                if(e.asset->primitives.size()>UINT32_MAX)throw std::length_error("Entity primitive index overflow");
                for(size_t j=0;j<e.asset->primitives.size();++j){const auto& p=e.asset->primitives[j];
                    if(!p.count||p.texture>=e.asset->textures.size()||uint64_t(p.first)+p.count>e.asset->indices.size()||
                       (!e.hiddenMaterial.empty()&&p.material.find(e.hiddenMaterial)!=std::string::npos))continue;
                    run.primitives.push_back(uint32_t(j));
                }
                out.runs.push_back(std::move(run));indices.emplace_back();
            }
            indices[it->second].push_back(uint32_t(i));
        }
        out.poses.reserve(draws.size()-out.culled-out.invalid);
        size_t kept=0;
        for(size_t i=0;i<out.runs.size();++i){auto& run=out.runs[i];if(run.primitives.empty())continue;
            run.firstInstance=uint32_t(out.poses.size());run.instanceCount=uint32_t(indices[i].size());
            for(auto index:indices[i])out.poses.push_back(draws[index].pose);
            if(kept!=i)out.runs[kept]=std::move(run);
            ++kept;
        }
        out.runs.resize(kept);return out;
    }
};
}
