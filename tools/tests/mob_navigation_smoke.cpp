#include "mobs/navigation/Navigation_AStar.h"
#include <cassert>
#include <iostream>
#include <algorithm>
using namespace Mob_Nav;

static Grid world(int sx=32,int sy=9,int sz=26) {
    Grid g; g.ox=-3;g.oy=-2;g.oz=-3;g.sx=sx;g.sy=sy;g.sz=sz;
    g.voxels.resize(static_cast<size_t>(sx)*sy*sz,0);
    for(int x=g.ox;x<g.ox+g.sx;++x)for(int z=g.oz;z<g.oz+g.sz;++z)
        g.voxels[g.index(x,-1,z)]=1;
    return g;
}
static void put(Grid& g,int x,int y,int z,uint8_t flags=1) {
    g.voxels[g.index(x,y,z)]=flags;
}
static Search_Result find(const Grid& g,Cell start,Cell goal,Travel mode=Travel::Ground,float width=0.35f,float height=1.8f) {
    return search(g,start,goal,{width,height,mode,12000});
}
int main() {
    {
        Grid g=world();
        auto r=find(g,{0,0,0},{15,0,0});
        assert(r.reached && !r.points.empty());
        assert((r.points.back()==Cell{15,0,0}));
        assert(r.expansions<400);
    }
    {
        Grid g=world();
        for (int z=-3;z<=4;++z)for(int y=0;y<=2;++y) if(z!=4) put(g,6,y,z);
        auto r=find(g,{0,0,0},{14,0,0});
        assert((r.reached && r.points.back()==Cell{14,0,0}));
        bool detoured=false;
        for (auto c:r.points) if(c.z>=4) detoured=true;
        assert(detoured);
    }
    {
        Grid g=world(20,9,7);
        // Narrow strip: step up once, then step back down.
        for(int z=-3;z<=3;++z) {
            put(g,3,0,z); // one-block wall
        }
        auto r=find(g,{0,0,0},{8,0,0});
        assert(r.reached);
        bool jumped=false;
        for(auto c:r.points) if(c.y==1) jumped=true;
        assert(jumped);
    }
    {
        Grid g=world(20,9,7);
        for(int z=-3;z<=3;++z)for(int y=0;y<=5;++y)put(g,3,y,z);
        auto r=find(g,{0,0,0},{8,0,0});
        assert(!r.reached);
    }
    {
        Grid g=world(20,9,7);
        // Solid vertical plate from y=0..4. Flying creatures can go above it.
        for(int z=-3;z<=3;++z)for(int y=0;y<=2;++y)put(g,3,y,z);
        auto r=find(g,{0,1,0},{8,1,0},Travel::Flying,0.35f,0.9f);
        assert(r.reached);
        bool climbed=false;
        for(auto c:r.points)if(c.y>=3)climbed=true;
        assert(climbed);
    }
    {
        Grid g=world(16,8,12);
        for (int y=0;y<3;++y) { put(g,1,y,0);put(g,0,y,1); }
        auto r=find(g,{0,0,0},{2,0,2});
        // Must not cut the diagonal through two touching solid corners.
        for(auto c:r.points)assert(!(c.x==1 && c.z==1 && c.y==0));
    }
    {
        Grid g=world();
        // Ground agents cannot stand in lava.
        put(g,3,0,0,4);put(g,3,0,1,4);put(g,3,0,-1,4);
        auto r=find(g,{0,0,0},{8,0,0});
        assert(r.reached);
        for(auto c:r.points)assert(!g.hazard(c.x,c.y,c.z));
    }
    {
        Grid g=world(14,9,12);
        // Three perpendicular blocks must prevent a single diagonal 3D step
        // from tunneling from (0,0,0) to (1,1,1).
        put(g,1,0,0); put(g,0,1,0); put(g,0,0,1);
        auto r=find(g,{0,0,0},{1,1,1},Travel::Flying,0.35f,0.9f);
        assert(r.reached);
        assert(r.points.size()>1); // no single-step diagonal corner cut
    }
    std::cout<<"mob navigation smoke: PASS (8 scenarios)\n";
}
