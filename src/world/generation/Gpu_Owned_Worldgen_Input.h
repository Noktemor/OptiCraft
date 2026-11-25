#pragma once
#include "world/generation/Gpu_Worldgen_Data.h"
#include "world/generation/Gpu_Overlay_Work.h"
#include <string>
#include <memory>
#include "world/generation/Gpu_Cave_Data.h"

class World_Generator;
namespace OptiCraft::Vulkan { class PreparedOwnedOverlayProducer; }

// CPU regional preparation for exactly one certified GPU base level. No Chunk,
// packed base voxels, or fabricated light. Keep the generator alive and its
// configuration immutable while preparation or submission is in flight.
struct Gpu_Owned_Worldgen_Identity {
    const World_Generator* world=nullptr;
    uint32_t epoch=0,seed=0,dimension=0,biome=0;
    std::string folder;
};

class Gpu_Owned_Worldgen_Input {
    friend class World_Generator;
    friend class OptiCraft::Vulkan::PreparedOwnedOverlayProducer;
    Gpu_Owned_Worldgen_Identity identity_;
    int x_=0,y_=0,z_=0;
    std::array<Gpu_Material_Column,1024> columns_{};
    Gpu_Base_Region region_;
    Gpu_Overlay_Work overlay_;
    std::shared_ptr<const Gpu_Cave_Dispatch::Packet> cave_;
public:
    int x() const noexcept {return x_;}
    int y() const noexcept {return y_;}
    int z() const noexcept {return z_;}
    const Gpu_Owned_Worldgen_Identity& identity() const noexcept {return identity_;}
    const auto& columns() const noexcept {return columns_;}
    const auto& region() const noexcept {return region_;}
    const auto& cave() const noexcept {return cave_;}
    const auto& overlay() const noexcept {return overlay_;}
};
