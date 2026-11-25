#pragma once
#include "world/chunks/Gpu_World_Streaming_Policy.h"
#include <algorithm>
#include <cstdint>

namespace Gpu_World_Streaming {
// Settings model: retain the requested CPU radius when render distance is
// reduced; the effective CPU zone is clipped to the existing render window.
// OpenGL uses the full render window as CPU zone (no GPU world ownership).
class Distances {
public:
    void setRender(int v) noexcept {render_=clampRadius(v);}
    void setCpu(int v) noexcept {cpu_=clampRadius(v);}
    void setVertical(int v) noexcept {vertical_=v==0?0:std::clamp(v,Config::vertical_render_min,Config::vertical_render_max);}
    int render() const noexcept{return render_;}
    int requestedCpu() const noexcept{return cpu_;}
    int requestedVertical() const noexcept{return vertical_;}
    int cpu(bool gpuWorld=true) const noexcept{return gpuWorld?std::min(cpu_,render_):render_;}
    int vertical() const noexcept{return vertical_?vertical_:Config::auto_vertical_render_distance(render_);}
    Window window(int x,int y,int z,bool gpuWorld=true) const noexcept{return {x,y,z,render_,vertical(),cpu(gpuWorld)};}
private:
    static int clampRadius(int v) noexcept{return std::clamp(v,Config::render_distance_min,Config::render_distance_max);}
    int render_=Config::chunk_load_radius,cpu_=6,vertical_=0;
};
inline bool validWindow(const Window& w) noexcept {
    return w.render_radius>=Config::render_distance_min&&w.render_radius<=Config::render_distance_max&&
        w.cpu_radius>=Config::render_distance_min&&w.cpu_radius<=Config::render_distance_max&&
        w.vertical_half>=0&&w.vertical_half<=Config::world_height_chunks;
}
inline int64_t axisDistance(int a,int b) noexcept {const auto d=int64_t(a)-b;return d<0?-d:d;}
inline bool visible(const Window& w,int x,int y,int z) noexcept {
    if(!validWindow(w)||y<Config::world_min_chunk_y||y>Config::world_max_chunk_y)return false;
    const auto dx=axisDistance(x,w.x),dy=axisDistance(y,w.y),dz=axisDistance(z,w.z);
    return dx<=w.render_radius&&dz<=w.render_radius&&dy<=w.vertical_half&&
        Chunk_Stream_Window::wanted(w.render_radius,w.vertical_half,int(dx),int(dy),int(dz));
}
enum class Zone {Outside,Cpu,Guard,Gpu};
inline Zone zone(const Window& w,int x,int y,int z) noexcept {
    if(!visible(w,x,y,z))return Zone::Outside;
    const auto ring=std::max(axisDistance(x,w.x),axisDistance(z,w.z));
    if(ring<=w.cpu_radius)return Zone::Cpu;
    if(ring==int64_t(w.cpu_radius)+1)return Zone::Guard;
    return Zone::Gpu;
}
// Exact slot demand, without enumerating every Y chunk. Clamp to actual world height.
inline uint64_t requestedSlots(const Window& w) noexcept {
    if(!validWindow(w))return 0;
    uint64_t count=0;
    for(int ring=0;ring<=w.render_radius;++ring){
        const auto h=Chunk_Stream_Window::vertical_half(w.render_radius,w.vertical_half,ring);
        const auto lo=std::max<int64_t>(Config::world_min_chunk_y,int64_t(w.y)-h);
        const auto hi=std::min<int64_t>(Config::world_max_chunk_y,int64_t(w.y)+h);
        if(hi>=lo)count+=uint64_t(hi-lo+1)*(ring?8u*uint64_t(ring):1u);
    }return count;
}
}
