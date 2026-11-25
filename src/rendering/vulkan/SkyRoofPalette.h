#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
namespace OptiCraft::Vulkan {
struct SkyRoofPalette {
    std::array<uint16_t,8> ids{};
    uint8_t count=0;
    static constexpr uint32_t mode=0x80000003u;
    static constexpr uint32_t resultOffset=8192+1024+3*8192;
    static constexpr uint32_t resultWords=9;
    static SkyRoofPalette fromBlocks(std::span<const uint32_t> words) noexcept {
        SkyRoofPalette roof;
        if(words.size()!=16384)return roof;
        for(uint32_t word:words.last(512))for(uint16_t id:{uint16_t(word),uint16_t(word>>16u)}){
            const auto end=roof.ids.begin()+roof.count;
            if(std::find(roof.ids.begin(),end,id)!=end)continue;
            if(roof.count==roof.ids.size())return {};
            roof.ids[roof.count++]=id;
        }
        return roof;
    }
    static SkyRoofPalette fromGpu(std::span<const uint32_t,resultWords> words) noexcept {
        SkyRoofPalette roof;
        if(!words[0]||words[0]>roof.ids.size())return {};
        for(uint32_t i=0;i<words[0];++i){
            if(words[i+1]>65535u)return {};
            const auto id=uint16_t(words[i+1]);
            if(std::find(roof.ids.begin(),roof.ids.begin()+roof.count,id)!=roof.ids.begin()+roof.count)return {};
            roof.ids[roof.count++]=id;
        }
        return roof;
    }
};
}
