#include "rendering/vulkan/EntityDraw.h"
#include <cstddef>
#include <cassert>
#include <iostream>
using namespace OptiCraft::Vulkan;
static_assert(offsetof(EntityVertex,position)==0);
static_assert(offsetof(EntityVertex,normal)==12);
static_assert(offsetof(EntityVertex,uv)==24);
static_assert(offsetof(EntityVertex,joints)==32);
static_assert(offsetof(EntityVertex,weights)==48);
static_assert(offsetof(EntityPose,model)==0);
static_assert(offsetof(EntityPose,bones)==64);
static_assert(offsetof(EntityPose,options)==4160);
static_assert(alignof(EntityPose)==16);
int main(){
    EntityDraw first;auto asset=std::make_shared<EntityAsset>();asset->textures.emplace_back();first.asset=asset;
    first.pose.options={2,0.5f,1,0};first.hiddenMaterial="Fur";
    EntityDraw second=first;second.pose.options[1]=0;
    assert(first.asset==second.asset && first.pose.options[1]==0.5f);
    assert(first.asset->textures[0].rgba.size()==4);
    std::cout<<"Entity vertex/pose std430 ABI and shared asset ownership: PASS\n";
}
