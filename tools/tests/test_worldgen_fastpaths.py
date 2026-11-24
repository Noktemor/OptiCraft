#!/usr/bin/env python3
"""Compile and exercise the actual C++ fast paths in isolation, no GL/GLM required.

This is a behavioral regression test, not a substitute for building OptiCraft itself.
The function bodies are extracted from production sources, so the test cannot pass
by accidentally testing a separately rewritten copy of the optimized algorithm.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WG = (ROOT / 'src/world/generation/World_Generator.cpp').read_text()
CM = (ROOT / 'src/world/chunks/Chunk_Manager.cpp').read_text()

def portion(source, first, last):
    assert first in source and last in source, (first, last)
    return source.split(first, 1)[1].split(last, 1)[0]

# Grab actual production functions and put them in a test compilation unit.
skylight = '    void init_chunk_sky_light' + portion(
    CM, '    void init_chunk_sky_light', '    // Есть ли ненулевой свет')
air = 'int World_Generator::conservative_column_air_ceiling' + portion(
    WG, 'int World_Generator::conservative_column_air_ceiling', '// Village templates are overlaid')

source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>
#define OC_ZONE(name) ((void)0)

namespace Config {
    constexpr int chunk_size=32, chunk_height=32;
    constexpr int world_min_y=-256, world_max_y=1023, world_min_chunk_y=-8;
}

enum class Terrain_Mode { Void, Flat, Noise, Nether, End };
struct Dimension_Info {
    Terrain_Mode terrain=Terrain_Mode::Noise;
    bool ground=true, sky_islands=false, villages=false, has_sky_light=true;
    int flat_top_y() const {return 3;}
};
struct Structure_Placement {static constexpr int kCellSize=320, kRadiusMax=120;};
static int floordiv_int(int v, int d) {
    const int q = v/d;
    return (v % d != 0 && v < 0) ? q-1 : q;
}
struct Site {int x,z,radius;};
struct Structures {
    std::optional<Site> village;
    std::optional<Site> village_in_cell(int, int) const {return village;}
};

class World_Generator {
public:
    struct Column {int height;};
    const Dimension_Info* m_dimension;
    const Structures* m_structures;
    static constexpr int kSeaLevel=63;
    mutable int terrain_queries=0;
    int seed=1;
    World_Generator(const Dimension_Info& dim, const Structures& structures, int s)
        : m_dimension(&dim),m_structures(&structures),seed(s){}
    int get_height(int x,int z) const {
        ++terrain_queries;
        const uint32_t h=static_cast<uint32_t>(x*73856093u)^
            static_cast<uint32_t>(z*19349663u)^(static_cast<uint32_t>(seed)*83492791u);
        return static_cast<int>(h%400u)-110;
    }
    Column column_at(int x, int z) const {return {get_height(x,z)};}
    int conservative_column_air_ceiling(int cx,int cz) const;
    bool chunk_is_definitely_air(int cx,int cy,int cz, std::optional<int> = std::nullopt) const;
    const Dimension_Info& get_dimension() const {return *m_dimension;}
};

class Chunk {
public:
    int cx,cy,cz;
    std::array<int, Config::chunk_size*Config::chunk_size> top_opaque;
    std::vector<uint8_t> light;
    Chunk(int y, int seed) : cx(0),cy(y),cz(0),light(Config::chunk_size*Config::chunk_size*Config::chunk_height, 0) {
        for (int z=0;z<Config::chunk_size;z++) for(int x=0;x<Config::chunk_size;x++) {
            const size_t i=z*Config::chunk_size+x;
            top_opaque[i]=((x*13+z*11+seed)%17==0)?((x+3*z)%Config::chunk_height):-1;
        }
    }
    int get_chunk_x() const {return cx;}
    int get_chunk_y() const {return cy;}
    int get_chunk_z() const {return cz;}
    int get_column_top_opaque(int x, int z) const {return top_opaque[z*Config::chunk_size+x];}
    void set_all_sky_light(uint8_t l,std::array<bool,6>& faces){faces.fill(false);std::fill(light.begin(),light.end(),l);}
    void set_sky_light(int x,int y,int z,uint8_t l){light[(y*Config::chunk_size+z)*Config::chunk_size+x]=l;}
    void compact_light() {}
};

namespace {
SKYLIGHT_FUNCTION
}
AIR_FUNCTION

bool baseline_air(const World_Generator& g, int cx, int cy, int cz) {
    const auto& dim=*g.m_dimension;
    const int start_y=cy*Config::chunk_height;
    if (cy == Config::world_min_chunk_y && dim.terrain != Terrain_Mode::Nether) return false;
    switch (dim.terrain) {
        case Terrain_Mode::Void: return true;
        case Terrain_Mode::Flat: return start_y > dim.flat_top_y();
        case Terrain_Mode::Nether: return start_y > 512 || start_y + Config::chunk_height - 1 < 0;
        case Terrain_Mode::End: return start_y > 112 || start_y + Config::chunk_height - 1 < 24;
        case Terrain_Mode::Noise: break;
    }
    if (!dim.ground) return !(dim.sky_islands && start_y+Config::chunk_height-1>=200);
    if (start_y<=World_Generator::kSeaLevel) return false;
    if (start_y+Config::chunk_height-1>=200) return false;
    const int x0=cx*Config::chunk_size,z0=cz*Config::chunk_size;
    const int last=Config::chunk_size-1;
    const int offsets[5][2]={{0,0},{last,0},{0,last},{last,last},{Config::chunk_size/2, Config::chunk_size/2}};
    int hi=std::numeric_limits<int>::min();
    for (const auto& p: offsets) hi=std::max(hi,g.get_height(x0+p[0],z0+p[1]));
    int ceiling=hi+Config::chunk_size+24;
    if(dim.villages) {
        constexpr int R=Structure_Placement::kRadiusMax+12;
        for(int iz=floordiv_int(z0-R,Structure_Placement::kCellSize);
            iz<=floordiv_int(z0+Config::chunk_size-1+R,Structure_Placement::kCellSize);iz++)
        for(int ix=floordiv_int(x0-R,Structure_Placement::kCellSize);
            ix<=floordiv_int(x0+Config::chunk_size-1+R,Structure_Placement::kCellSize);ix++) {
            const auto s=g.m_structures->village_in_cell(ix,iz);
            if(!s)continue;
            const int near_x=std::clamp(s->x,x0,x0+Config::chunk_size-1);
            const int near_z=std::clamp(s->z,z0,z0+Config::chunk_size-1);
            const int dx=s->x-near_x, dz=s->z-near_z;
            if(static_cast<int64_t>(dx)*dx+static_cast<int64_t>(dz)*dz <=
                static_cast<int64_t>(s->radius+12)*(s->radius+12))
                ceiling=std::max(ceiling,g.get_height(s->x,s->z)+24);
        }
    }
    return start_y>ceiling;
}

int main() {
    int tests=0;
    for (int seed=1;seed<=7;seed++) {
        for (bool ground: {false,true}) for (bool islands: {false,true})
        for (bool villages: {false,true}) for (Terrain_Mode terrain: {Terrain_Mode::Void,Terrain_Mode::Flat,Terrain_Mode::Noise,Terrain_Mode::Nether,Terrain_Mode::End}) {
            Dimension_Info dim{terrain,ground,islands,villages,true};
            Structures ss;
            for (int cx: {-9,-1,0,3,12}) for(int cz: {-5,0,11}) {
                if(villages && ((cx+cz+seed)&1)==0) ss.village=Site{cx*Config::chunk_size+3,cz*Config::chunk_size+10,95};
                else ss.village.reset();
                World_Generator g(dim,ss,seed);
                int cached = g.conservative_column_air_ceiling(cx,cz);
                for (int cy=-8;cy<=22;cy++) {
                    const bool expected=baseline_air(g,cx,cy,cz);
                    assert(expected==g.chunk_is_definitely_air(cx,cy,cz));
                    assert(expected==g.chunk_is_definitely_air(cx,cy,cz,cached));
                    ++tests;
                }
            }
        }
        Dimension_Info d{};
        Structures ss;
        World_Generator wg(d,ss,seed);
        std::array<int,Config::chunk_size*Config::chunk_size> prepared{};
        for (int z=0;z<Config::chunk_size;z++) for(int x=0;x<Config::chunk_size;x++)
            prepared[z*Config::chunk_size+x]=wg.get_height(x,z);
        for (int y: {-8,-3,0,1,2,3,5,6,8,9,15}) {
            Chunk baseline(y,seed), fast(y,seed);
            wg.terrain_queries=0;
            init_chunk_sky_light(baseline,wg,false);
            const int baseline_queries=wg.terrain_queries;
            wg.terrain_queries=0;
            init_chunk_sky_light(fast,wg,false,prepared.data());
            assert(baseline.light==fast.light);
            assert(baseline_queries==Config::chunk_size*Config::chunk_size);
            assert(wg.terrain_queries==0);
            ++tests;
        }
    }
    // For a stack with three eligible high-Y chunks, the cached path must do only
    // the original five terrain probes once (not five probes per Y).
    {
        Dimension_Info dim{};
        Structures ss;
        World_Generator baseline(dim, ss, 1327), fast(dim, ss, 1327);
        for (int cy: {3, 4, 5}) baseline_air(baseline, 10, cy, -2);
        const int ceiling=fast.conservative_column_air_ceiling(10, -2);
        for (int cy: {3, 4, 5}) fast.chunk_is_definitely_air(10, cy, -2, ceiling);
        assert(baseline.terrain_queries==15);
        assert(fast.terrain_queries==5);
    }
    std::cout << "worldgen fast-path checks passed: " << tests << " scenarios\n";
}
'''.replace('SKYLIGHT_FUNCTION',skylight).replace('AIR_FUNCTION',air)

with tempfile.TemporaryDirectory(prefix='opti_worldgen_') as temp:
    cpp = Path(temp) / 'test_worldgen.cpp'
    exe = Path(temp) / 'test_worldgen'
    cpp.write_text(source)
    cmd = ['g++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-o',str(exe),str(cpp)]
    subprocess.run(cmd, check=True)
    subprocess.run([str(exe)], check=True)
