#include "rendering/vulkan/VulkanUiBatch.h"
#include <array>
#include <cassert>
#include <iostream>
using namespace OptiCraft::Vulkan;
int main(){
    UiBatch b;std::array<UiVertex,6> a{};a[0].x=7;
    b.append(a,false);b.append(a,false);b.append(a,true);b.append(a,false);
    assert(b.vertices.size()==24 && b.ranges.size()==3);
    assert(b.ranges[0].first==0 && b.ranges[0].count==12 && !b.ranges[0].blocks);
    assert(b.ranges[1].first==12 && b.ranges[1].count==6 && b.ranges[1].blocks);
    assert(b.ranges[2].first==18 && b.ranges[2].count==6 && !b.ranges[2].blocks);
    assert(b.vertices[12].x==7);
    bool rejected=false;try { b.append(std::span(a).first(1),false); } catch(const std::length_error&) { rejected=true; }
    assert(rejected && b.vertices.size()==24);
    b.clear();assert(b.vertices.empty() && b.ranges.empty());
    std::cout<<"Vulkan UI batch order/merge/validation: PASS\n";
}
