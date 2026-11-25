#pragma once
#include <cstdint>
#include <memory>
namespace OptiCraft::Vulkan {
struct OverlaySnapshot;
// Non-owning reference: queued packets never extend a Vulkan device lifetime.
struct OverlayReference {
    std::weak_ptr<OverlaySnapshot> source;
    uint64_t ticket=0;
};
}
