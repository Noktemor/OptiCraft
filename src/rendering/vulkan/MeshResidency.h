#pragma once
#include "rendering/vulkan/GpuWorldCore.h"
#include <array>
#include <unordered_set>
namespace OptiCraft::Vulkan {
// Empty space needs no geometry. Retain the six-face halo of every loaded
// nonempty chunk, since meshing/lighting samples the adjacent source cells.
// No assumption about material opacity, light value, or distant LOD.
inline bool needsAirHalo(ChunkKey key,const std::unordered_set<ChunkKey,ChunkKeyHash>& nonempty){
    for(int axis=0;axis<3;++axis)for(int step:{-1,1}){
        auto n=key;
        int32_t* coord=axis==0?&n.x:axis==1?&n.y:&n.z;
        const auto next=int64_t(*coord)+step;
        if(next<INT32_MIN||next>INT32_MAX)continue;
        *coord=int32_t(next);
        if(nonempty.contains(n))return true;
    }
    return false;
}
} // namespace OptiCraft::Vulkan
