#pragma once
#include "world/chunks/Chunk_Stream_Window.h"
#include "utils/configs/Config_World.h"
#include <cstdint>
#include <cstddef>

namespace Gpu_World_Streaming {
    // The CPU zone and one guard ring always use the normal CPU scheduler.
    // Match the existing tapered vertical window; do not silently extend it.
    struct Window {
        int x=0, y=0, z=0;
        int render_radius=0, vertical_half=0, cpu_radius=6;
    };
    constexpr bool candidate(const Window& w,int x,int y,int z) noexcept {
        if (w.render_radius<Config::render_distance_min ||
            w.render_radius>Config::render_distance_max || w.cpu_radius<Config::render_distance_min ||
            w.cpu_radius>=w.render_radius || w.vertical_half<0 ||
            w.vertical_half>Config::world_height_chunks ||
            y<Config::world_min_chunk_y || y>Config::world_max_chunk_y) return false;
        const auto distance=[](int a,int b) constexpr {
            const auto d=int64_t(a)-int64_t(b);return d<0?-d:d;
        };
        const auto dx=distance(x,w.x),dy=distance(y,w.y),dz=distance(z,w.z);
        if(dx>w.render_radius || dz>w.render_radius || dy>w.vertical_half) return false;
        if(std::max(dx,dz)<=int64_t(w.cpu_radius)+1) return false;
        return Chunk_Stream_Window::wanted(w.render_radius,w.vertical_half,int(dx),int(dy),int(dz));
    }
    // Limit expensive storage/claim checks and accepted GPU jobs separately.
    struct Budget {
        static constexpr size_t max_checks=8, max_admissions=2;
        size_t checks=0, admissions=0;
        constexpr bool available() const noexcept {return checks<max_checks && admissions<max_admissions;}
    };
}
