#include "world/generation/Gpu_Material_Result.h"
#include <cassert>
#include <iostream>
using R = Gpu_Material_Result;
int main() {
    std::vector<uint32_t> dense(2*R::voxels);
    for (size_t i=0;i<dense.size();++i) dense[i]=(i*7919u+13u)&0xffffu;
    auto expected=dense;
    R legacy;
    assert(legacy.assign(9,{7,2},std::move(dense),R::Encoding::Legacy32));
    assert(legacy.word_count()==2*R::words_per_chunk);
    std::vector<uint32_t> packed(2*R::words_per_chunk);
    for(size_t i=0;i<packed.size();++i) packed[i]=expected[2*i]|(expected[2*i+1]<<16u);
    const auto* ptr=packed.data();
    R vk;
    assert(vk.assign(9,{7,2},std::move(packed),R::Encoding::Packed16));
    for(size_t level=0;level<9;++level) for(size_t k=0;k<R::voxels;++k) {
        const auto want=level==7?expected[k]:level==2?expected[R::voxels+k]:0;
        assert(vk.at(level,k)==want && legacy.at(level,k)==want);
    }
    assert(vk.has_level(7)&&vk.has_level(2)&&!vk.has_level(0));
    assert(vk.at(9,0)==0&&vk.at(2,R::voxels)==0);
    size_t calls=0;
    assert(vk.all_ids([&](uint16_t id){assert(id==expected[calls++]);return true;}));
    assert(calls==expected.size());
    assert(!vk.all_ids([](uint16_t id){return id<50000;}));
    assert(!vk.assign(9,{7,7},std::vector<uint32_t>(2*R::words_per_chunk),R::Encoding::Packed16));
    assert(!vk.assign(9,{9},std::vector<uint32_t>(R::words_per_chunk),R::Encoding::Packed16));
    assert(!vk.assign(10,{1},std::vector<uint32_t>(R::words_per_chunk),R::Encoding::Packed16));
    assert(!vk.assign(9,{2},std::vector<uint32_t>(R::words_per_chunk-1),R::Encoding::Packed16));
    assert(!vk.assign(9,{2},std::vector<uint32_t>(R::voxels,65536),R::Encoding::Legacy32));
    assert(vk.at(7,1)==expected[1]);
    assert(vk.level_words(7)==ptr); // Vulkan vector moved without copying
    assert(vk.level_words(2)==ptr+R::words_per_chunk);
    assert(!vk.level_words(8));
    R empty;
    assert(!empty.has_level(0)&&empty.at(0,0)==0);
    std::cout<<"GPU material result: 589824 sparse-level reads, full ID scan and malformed inputs PASS\n";
}
