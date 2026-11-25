#pragma once
#include "world/generation/Gpu_Template_Overlay.h"
#include "world/generation/Village_Layout.h"
#include <unordered_map>
#include <utility>

namespace Gpu_Template_Overlay {
// CPU-selected village plans in original cell traversal order. Produce a
// village-only pass, to be dispatched AFTER all vegetation. Roads are kept
// ordered per plan; later houses/settlements may overwrite earlier ones.
// Pack and plan storage need only survive this call; the packet owns its data.
inline bool compile_villages(std::span<const Village_Plan* const> plans,
        const Structure_Pack& pack,int cx,int cy,int cz,Packet& output) {
    uint32_t first,last;
    if(!y_interval(cy,first,last))return false;
    Packet next;
    if(!pack.ready()) {output=std::move(next);return true;}
    int64_t x0=int64_t(cx)*32,y0=int64_t(cy)*32,z0=int64_t(cz)*32;
    int64_t x1=x0+31,y1=y0+31,z1=z0+31;
    std::unordered_map<const Structure_Template*,uint32_t> handles;
    for(const auto* plan:plans){
        if(!plan || plan->max_x<x0 || plan->min_x>x1 || plan->max_z<z0 || plan->min_z>z1 ||
           plan->max_y<y0 || plan->min_y>y1)continue;
        std::vector<Record> roads;
        for(const auto& road:plan->roads){
            if(int64_t(road.x)+1<x0 || int64_t(road.x)-1>x1 || int64_t(road.z)+1<z0 || int64_t(road.z)-1>z1 ||
               int64_t(road.y)>y1+4 || int64_t(road.y)+4<y0)continue;
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)for(int dy=0;dy<=4;++dy){
                int64_t x=int64_t(road.x)+dx-x0,y=int64_t(road.y)+dy-y0,z=int64_t(road.z)+dz-z0;
                if(x<0||x>=32||z<0||z>=32||y<first||y>=last)continue;
                if(roads.size()>=max_records-next.records().size())return false;
                roads.push_back({int32_t(x),int32_t(y),int32_t(z),static_cast<Block_ID>(dy?Block_Types::Air:Block_Types::Cobblestone)});
            }
        }
        if(!roads.empty()){
            uint32_t h;
            if(!next.add_template(roads,h)||!next.add_instance(h,0,0,0,0,Predicate::Always))return false;
        }
        for(const auto& inst:plan->buildings){
            const auto* t=inst.custom_template?inst.custom_template.get():pack.get(inst.template_id);
            if(!t)continue;
            int64_t extent=int64_t(std::max(t->width,t->depth))+1;
            if(int64_t(inst.entrance_world_x)+extent<x0 || int64_t(inst.entrance_world_x)-extent>x1 ||
               int64_t(inst.entrance_world_z)+extent<z0 || int64_t(inst.entrance_world_z)-extent>z1 ||
               int64_t(inst.base_world_y)-8>y1 || int64_t(inst.base_world_y)+t->height<y0)continue;
            auto pos=handles.find(t);uint32_t h;
            if(pos==handles.end()){
                if(!next.add_structure(*t,h))return false;
                handles.emplace(t,h);
            }else h=pos->second;
            if(!next.add_world_instance(h,inst.entrance_world_x,inst.base_world_y,inst.entrance_world_z,
                                       cx,cy,cz,inst.rotation,Predicate::Always))return false;
        }
    }
    output=std::move(next);return true;
}
} // namespace Gpu_Template_Overlay
