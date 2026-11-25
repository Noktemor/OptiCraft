#pragma once
#include <array>
#include <cstdint>
namespace OptiCraft::Vulkan {
struct StaticDrawCache {
    std::array<float,16> vp{};
    uint64_t revision=0;
    uint32_t count=0;
    bool valid=false;
    bool matches(const std::array<float,16>& matrix,uint64_t rev,uint32_t n)const noexcept {
        return valid && revision==rev && count==n && vp==matrix;
    }
    void store(const std::array<float,16>& matrix,uint64_t rev,uint32_t n) noexcept {
        vp=matrix;revision=rev;count=n;valid=true;
    }
    void invalidate() noexcept {valid=false;}
};
}
