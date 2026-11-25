#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Isolated-chunk propagation ABI. No outside light enters the volume; runtime
// block-light publication uses a separate cross-chunk frontier handoff.
namespace Generation_Light_Propagation {
inline constexpr size_t voxels=32768, light_words=voxels/4, block_words=voxels/2;
inline constexpr size_t transparency_words=2048, workgroups=light_words/64;
inline constexpr unsigned passes=14; // max sky 15 -> positive light at distance 14
inline constexpr unsigned max_block_passes=13; // seed table caps emission at 14
inline constexpr uint32_t block_only_flag=0x80000000u;
// Optional tiled dispatch: 16 X voxels x 4 Z x 4 Y per 64-lane group.
// Zero retains the legacy linear layout. Coordinates are in tile units.
inline constexpr uint32_t tiled_flag=0x80000000u;
struct Params { uint32_t blockBase, transparencyBase, readBase, writeBase, tileOrigin=0; };
static_assert(sizeof(Params)==20);
struct Dispatch { uint32_t x=128, y=1, z=1, origin=0; };
struct Source_Bounds {
    std::array<unsigned,3> lo{32,32,32}, hi{0,0,0}; // X,Z,Y
    constexpr void include(unsigned i) noexcept {
        const std::array<unsigned,3> pos{i&31u,(i>>5u)&31u,i>>10u};
        for(unsigned a=0;a<3;++a) { lo[a]=std::min(lo[a],pos[a]); hi[a]=std::max(hi[a],pos[a]); }
    }
    constexpr bool valid() const noexcept {
        for(unsigned a=0;a<3;++a) if(lo[a]>hi[a]||hi[a]>=32) return false;
        return true;
    }
    constexpr Dispatch dispatch(unsigned pass) const noexcept {
        // Both ping-pong ranges must contain all unchanged seed words before
        // any partial dispatch. The bound grows monotonically thereafter.
        if(pass<2||!valid()) return {};
        constexpr std::array<unsigned,3> size{16,4,4};
        std::array<unsigned,3> start{},extent{};
        const unsigned radius=std::min(pass+1,31u);
        for(unsigned a=0;a<3;++a) {
            start[a]=(lo[a]>radius?lo[a]-radius:0u)/size[a];
            const unsigned end=std::min(31u,hi[a]+radius)/size[a];
            extent[a]=end-start[a]+1;
        }
        if(extent[0]*extent[1]*extent[2]>=workgroups) return {};
        return {extent[0],extent[1],extent[2],tiled_flag|start[0]|(start[1]<<4u)|(start[2]<<8u)};
    }
};
inline constexpr Params iteration(uint32_t blockBase,uint32_t transparencyBase,
                                  uint32_t seedBase,uint32_t scratchBase,unsigned pass) noexcept {
    return {blockBase,transparencyBase,pass%2==0?seedBase:scratchBase,
            pass%2==0?scratchBase:seedBase};
}
// Runtime retains the original seed field for the exact fixed-point certificate.
// With the even pass count the final result resides in scratchB.
inline constexpr Params preserved_iteration(uint32_t blockBase,uint32_t transparencyBase,
                                            uint32_t seedBase,uint32_t scratchA,uint32_t scratchB,
                                            unsigned pass) noexcept {
    return {blockBase,transparencyBase,pass==0?seedBase:(pass%2?scratchA:scratchB),
            pass%2?scratchB:scratchA};
}
// For an odd number of passes start in B, so the final field always resides
// in B. This keeps the readback ABI and original seeds unchanged.
inline constexpr Params block_iteration(uint32_t blockBase,uint32_t transparencyBase,
                                        uint32_t seedBase,uint32_t scratchA,uint32_t scratchB,
                                        unsigned pass,unsigned count) noexcept {
    const bool writeB=((count-pass)&1u)!=0;
    return {blockBase,transparencyBase|block_only_flag,
            pass==0?seedBase:(writeB?scratchA:scratchB),writeB?scratchB:scratchA};
}
inline constexpr uint8_t byte_at(std::span<const uint32_t> words,size_t i) noexcept {
    return static_cast<uint8_t>(words[i/4]>>((i&3u)*8u));
}
inline constexpr uint8_t max_light(uint8_t a,uint8_t b) noexcept {
    return static_cast<uint8_t>(std::max(a&15u,b&15u)|std::max(a&240u,b&240u));
}
inline constexpr uint8_t decay(uint8_t a) noexcept {
    return static_cast<uint8_t>((a&15u? (a&15u)-1u:0u)|(a&240u?(a&240u)-16u:0u));
}
inline constexpr bool can_cross(uint8_t source,uint8_t target) noexcept {
    return source>1u && unsigned(source)>unsigned(target)+1u;
}
// Enumerate a face and its matching neighbour face using y,z,x indices. Face
// order agrees with Chunk_Manager: -X,+X,-Y,+Y,-Z,+Z. The callback receives
// the local source and opposite neighbour voxel indices, never a wrapped index.
template<class Visit>
constexpr void for_each_face_pair(unsigned face,Visit&& visit) {
    if(face>=6) return;
    const size_t axis=face/2u, side=(face&1u)?31u:0u;
    for(size_t a=0;a<32;++a) for(size_t b=0;b<32;++b) {
        const size_t x=axis==0?side:a;
        const size_t y=axis==1?side:(axis==0?a:b);
        const size_t z=axis==2?side:b;
        const size_t i=y*1024+z*32+x;
        const size_t stride=axis==0?1u:axis==1?1024u:32u;
        visit(i,(face&1u)?i-31u*stride:i+31u*stride);
    }
}
// Exact local fixed-point certificate, not a sampling check. For each channel,
// positive non-seed levels must have a strictly brighter neighbour. A finite
// descending path to a seed therefore exists; conversely every seed path must
// be represented. With attenuation by one this gives the unique BFS solution.
// Opaque cells retain only their seeds, including opaque emitters.
template<bool BlockOnly=false>
inline bool matches(std::span<const uint32_t> seeds,std::span<const uint32_t> result,
                    std::span<const uint32_t> blocks,const bool* transparent) noexcept {
    if(seeds.size()!=light_words||result.size()!=light_words||blocks.size()!=block_words||!transparent)
        return false;
    for(size_t i=0;i<voxels;++i) {
        uint8_t expected=byte_at(seeds,i);
        const auto merge=[](uint8_t a,uint8_t b) constexpr {
            if constexpr(BlockOnly) return static_cast<uint8_t>(std::max(a&15u,b&15u));
            else return max_light(a,b);
        };
        const auto id=static_cast<uint16_t>(blocks[i/2]>>((i&1u)*16u));
        if(transparent[id]) {
            uint8_t n=0;
            if((i&31u)!=0) n=merge(n,byte_at(result,i-1));
            if((i&31u)!=31) n=merge(n,byte_at(result,i+1));
            if(((i>>5u)&31u)!=0) n=merge(n,byte_at(result,i-32));
            if(((i>>5u)&31u)!=31) n=merge(n,byte_at(result,i+32));
            if((i>>10u)!=0) n=merge(n,byte_at(result,i-1024));
            if((i>>10u)!=31) n=merge(n,byte_at(result,i+1024));
            if constexpr(BlockOnly) expected=static_cast<uint8_t>((expected&240u)|merge(expected,decay(n)));
            else expected=max_light(expected,decay(n));
        }
        if(byte_at(result,i)!=expected) return false;
    }
    return true;
}
}
