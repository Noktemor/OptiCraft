#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <limits>
#include <stdexcept>
namespace OptiCraft::Vulkan {
struct UiVertex { float x,y,u,v,r,g,b,a; };
static_assert(sizeof(UiVertex)==32);
struct UiRange { uint32_t first,count; bool blocks; };
// The HUD's layout remains CPU-side; contiguous batches of the same atlas are
// merged without reordering translucent widgets. No GPU queries or per-widget allocations.
class UiBatch {
public:
    void clear() { vertices.clear(); ranges.clear(); }
    void append(std::span<const UiVertex> data,bool blocks) {
        if(data.empty())return;
        if(data.size()%3 || data.size()>std::numeric_limits<uint32_t>::max()-vertices.size())
            throw std::length_error("Invalid UI triangle batch");
        const auto first=static_cast<uint32_t>(vertices.size());
        const auto count=static_cast<uint32_t>(data.size());
        vertices.insert(vertices.end(),data.begin(),data.end());
        if(!ranges.empty() && ranges.back().blocks==blocks) ranges.back().count+=count;
        else ranges.push_back({first,count,blocks});
    }
    std::vector<UiVertex> vertices;
    std::vector<UiRange> ranges;
};
}
