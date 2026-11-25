#pragma once
#include "world/generation/Gpu_Owned_Worldgen_Input.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <optional>

// One retained base dispatch, multiple independent ordered overlay packets.
// No claims or Vulkan resources owned by this value. Caller must revalidate
// world identity/claims before dispatch and again before publishing a level.
struct Gpu_Owned_Column_Batch {
    Gpu_Owned_Worldgen_Identity identity;
    int x=0,z=0;
    std::array<Gpu_Material_Column,1024> columns{};
    std::vector<int> ys;
    Gpu_Base_Region region;
    std::shared_ptr<const Gpu_Cave_Dispatch::Packet> cave;
    std::vector<Gpu_Overlay_Work> overlays;

    static std::optional<Gpu_Owned_Column_Batch> build(std::span<const Gpu_Owned_Worldgen_Input> inputs){
        if(inputs.empty() || inputs.size()>Gpu_Base_Region::levels_reserved)return std::nullopt;
        const auto& first=inputs.front();
        Gpu_Owned_Column_Batch out;
        out.identity=first.identity();out.x=first.x();out.z=first.z();out.columns=first.columns();
        if(!out.identity.world || first.region().data.size()!=Gpu_Base_Region::words(1))return std::nullopt;
        for(const auto& c:out.columns)if(!std::isfinite(c.surface_height) || !(c.padding&Gpu_Base_Region::enabled))return std::nullopt;
        const auto& reference=first.region().data;
        out.cave=first.cave();
        out.region.data.reserve(Gpu_Base_Region::words(inputs.size()));
        out.region.data.insert(out.region.data.end(),reference.begin(),reference.begin()+Gpu_Base_Region::cave_offset);
        for(const auto& input:inputs){
            const auto& id=input.identity();const auto& r=input.region().data;
            if(id.world!=out.identity.world || id.epoch!=out.identity.epoch || id.seed!=out.identity.seed ||
               id.dimension!=out.identity.dimension || id.biome!=out.identity.biome || id.folder!=out.identity.folder ||
               input.x()!=out.x || input.z()!=out.z || std::find(out.ys.begin(),out.ys.end(),input.y())!=out.ys.end() ||
               r.size()!=Gpu_Base_Region::words(1) || !std::equal(r.begin(),r.begin()+Gpu_Base_Region::cave_offset,reference.begin()) ||
               input.overlay().level!=0 || !input.overlay().valid_certified_snapshot_input())return std::nullopt;
            for(size_t i=0;i<out.columns.size();++i){
                const auto& a=out.columns[i];const auto& b=input.columns()[i];
                if(a.surface_height!=b.surface_height || a.biome!=b.biome || a.padding!=b.padding)return std::nullopt;
            }
            if(input.cave()!=out.cave)out.cave.reset();
            out.region.data.insert(out.region.data.end(),r.begin()+Gpu_Base_Region::cave_offset,r.end());
            out.overlays.push_back(input.overlay());out.overlays.back().level=out.ys.size();out.ys.push_back(input.y());
        }
        return out;
    }
};

// A positive certificate names a retained dispatch slot, not a CPU parity
// proof. Missing levels are rejected independently; malformed sets fail closed.
struct Gpu_Owned_Column_Certificates {
    std::array<std::optional<Gpu_Certified_Base>,Gpu_Base_Region::levels_reserved> levels{};
    static std::optional<Gpu_Owned_Column_Certificates> route(const Gpu_Owned_Column_Batch& batch,
        std::span<const Gpu_Certified_Base> certs){
        if(batch.ys.empty() || batch.ys.size()>Gpu_Base_Region::levels_reserved || certs.size()>batch.ys.size())return std::nullopt;
        Gpu_Owned_Column_Certificates result;uint64_t ticket=0;
        for(const auto& c:certs){
            if(!c.ticket || (ticket && c.ticket!=ticket) || c.level>=batch.ys.size() || result.levels[c.level] ||
               c.x!=batch.x || c.z!=batch.z || c.y!=batch.ys[c.level])return std::nullopt;
            ticket=c.ticket;result.levels[c.level]=c;
        }
        return result;
    }
};
