#include "rendering/vulkan/StaticDrawCache.h"
#include <cassert>
using OptiCraft::Vulkan::StaticDrawCache;
int main(){
    StaticDrawCache a,b;
    std::array<float,16> v{};
    assert(!a.matches(v,0,5));a.store(v,1,5);
    assert(a.matches(v,1,5));assert(!b.matches(v,1,5));
    assert(!a.matches(v,2,5));assert(!a.matches(v,1,6));
    v[12]=0.001f;assert(!a.matches(v,1,5));
    v[12]=0;assert(a.matches(v,1,5));a.invalidate();assert(!a.matches(v,1,5));
}
