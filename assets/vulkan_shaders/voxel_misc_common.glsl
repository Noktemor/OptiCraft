#include "voxel_address.glsl"
// Full block geometry ABI. Unlike the minimal opaque-cube mesher, this path
// handles alpha cubes, plants, fluid surfaces and all shaped block variants.
// No CPU triangles/mesh are constructed. Count and emit use identical logic.
struct MiscSlot {
    uvec4 source;   // blockBase (u32), metaBase(u32), faceBase (uvec4), faceCapacity
    ivec4 origin;   // world voxel origin
    ivec4 neighborsXA; // -X +X -Y +Y blockBase, -1 = empty
    ivec4 neighborsZB; // -Z +Z spare spare blockBase
    ivec4 neighborsMetaA; // -X +X -Y +Y metaBase
    ivec4 neighborsMetaB; // -Z +Z spare spare metaBase
};
struct Material {uint faceBase; uint metaStride; uint shape; uint flags;};
struct ShapeRange {uint first; uint count;};
struct ShapeBox {uint lo; uint hi;};
layout(std430,set=0,binding=0) readonly buffer BlockWords {uint blocks[];};
layout(std430,set=0,binding=1) readonly buffer MetaWords {uint metaWords[];};
layout(std430,set=0,binding=2) readonly buffer Slots {MiscSlot slots[];};
layout(std430,set=0,binding=3) readonly buffer Materials {Material materials[];};
layout(std430,set=0,binding=4) readonly buffer ShapeRanges {ShapeRange shapeRanges[];};
layout(std430,set=0,binding=5) readonly buffer ShapeBoxes {ShapeBox shapeBoxes[];};
layout(std430,set=0,binding=6) buffer Counts {uint counts[];};
layout(std430,set=0,binding=7) buffer Geometry {uvec4 quads[];}; // TWO uvec4 per quad
layout(std430,set=0,binding=8) readonly buffer JobSlots {uint jobSlots[];};
layout(push_constant) uniform Params {uint firstJob; uint jobCount; uint fastBase; uint fastCapacity;} params;
const uint K_OPAQUE=1u, K_ALPHA=2u, K_FLORA=4u, K_LIQUID=8u, K_SHAPED=16u, K_OCCLUDES=32u;
const ivec3 DIRS[6] = ivec3[6](ivec3(1,0,0),ivec3(-1,0,0),
 ivec3(0,1,0),ivec3(0,-1,0),ivec3(0,0,1),ivec3(0,0,-1));
const uint N=32u;
uint atBlock(int offset,ivec3 pos) {
    if(offset<0)return 0u;
    uint idx=uint((pos.y*32+pos.z)*32+pos.x);
    uint w=blocks[uint(offset)+(idx>>1u)];
    return (idx&1u)!=0u?w>>16u:w&65535u;
}
uint atMeta(int offset,ivec3 pos) {
    if(offset<0)return 0u;
    uint idx=uint((pos.y*32+pos.z)*32+pos.x);
    return (metaWords[uint(offset)+(idx>>2u)]>>((idx&3u)*8u))&255u;
}
int offsetOf(MiscSlot s,ivec3 p,bool meta) {
    if(p.x<0) return meta?s.neighborsMetaA.x:s.neighborsXA.x;
    if(p.x>=32) return meta?s.neighborsMetaA.y:s.neighborsXA.y;
    if(p.y<0) return meta?s.neighborsMetaA.z:s.neighborsXA.z;
    if(p.y>=32) return meta?s.neighborsMetaA.w:s.neighborsXA.w;
    if(p.z<0) return meta?s.neighborsMetaB.x:s.neighborsZB.x;
    if(p.z>=32) return meta?s.neighborsMetaB.y:s.neighborsZB.y;
    return int(meta?s.source.y:s.source.x);
}
ivec3 local(ivec3 p){return (p+ivec3(32))%32;}
uint blockAt(MiscSlot s,ivec3 p){
    if(!voxelIndirect(s.origin.w))return atBlock(offsetOf(s,p,false),local(p));
    if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(32)))){
        int link=offsetOf(s,p,false);if(link<0)return 0u;s=slots[uint(link)];p=local(p);
    }
    uint idx=uint((p.y*32+p.z)*32+p.x);
    uint w=VOXEL_WORD(blocks,s.source.x,idx>>1u,s.origin.w,VOXEL_UNIFORM_BLOCK);
    return (w>>((idx&1u)*16u))&65535u;
}
uint metaAt(MiscSlot s,ivec3 p){
    if(!voxelIndirect(s.origin.w))return atMeta(offsetOf(s,p,true),local(p));
    if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(32)))){
        int link=offsetOf(s,p,false);if(link<0)return 0u;s=slots[uint(link)];p=local(p);
    }
    uint idx=uint((p.y*32+p.z)*32+p.x);
    uint w=VOXEL_WORD(metaWords,s.source.y,idx>>2u,s.origin.w,VOXEL_UNIFORM_META);
    return (w>>((idx&3u)*8u))&255u;
}
uint coord(uint packedValue,uint axis){return (packedValue>>(7u*axis))&127u;}
bool internalFace(ShapeRange r,ShapeBox box,uint face) {
    uint axis=face>>1u; bool positive=(face&1u)==0u;
    uint plane=positive?coord(box.hi,axis):coord(box.lo,axis);
    uint u=(axis+1u)%3u,v=(axis+2u)%3u;
    for(uint i=0u;i<r.count;i++){
        ShapeBox other=shapeBoxes[r.first+i];
        if(other.lo==box.lo&&other.hi==box.hi)continue;
        uint target=positive?coord(other.lo,axis):coord(other.hi,axis);
        if(target!=plane)continue;
        if(coord(other.lo,u)<=coord(box.lo,u)&&coord(other.hi,u)>=coord(box.hi,u)&&
           coord(other.lo,v)<=coord(box.lo,v)&&coord(other.hi,v)>=coord(box.hi,v))return true;
    }
    return false;
}
bool exposed(MiscSlot s,ivec3 p,uint self,uint face,uint kind) {
    uint adjacent=blockAt(s,p+DIRS[face]);
    if(adjacent==0u)return true;
    uint flags=materials[adjacent].flags;
    if(kind==K_LIQUID || kind==K_ALPHA) return adjacent!=self && (flags&K_OCCLUDES)==0u;
    return (flags&K_OCCLUDES)==0u;
}
uint boxHalfLo(uint face){return 0u;}
// Each voxel produces its set of quads identically during count and emit.
// For all passes the stream index is one per job; the caller allocates capacity
// after asynchronous count readback or reserves a disjoint bounded fast range.
// Overflow is counted exactly and the incomplete candidate is never published.
void add(uint job, MiscSlot s,uint slot,uint id,uint index,uint meta,uint kind,
         uint face,uint blo,uint bhi,uint h0,uint h1,uint h2,uint h3) {
    uint next=atomicAdd(counts[job],1u);
#ifdef MISC_EMIT
    if(next>=s.source.w)return; // caller validates counts before publishing
    uint outIndex=(s.source.z+next)*2u;
    quads[outIndex]=uvec4(slot,id,index,face | (meta<<8u) | (kind<<16u));
    quads[outIndex+1u]=uvec4(blo,bhi,h0 | (h1<<16u),h2 | (h3<<16u));
#endif
}
void meshCell(uint job, uint slot, MiscSlot s, uint index) {
    ivec3 p=ivec3(int(index&31u),int(index>>10u),int((index>>5u)&31u));
    uint id=blockAt(s,p); if(id==0u)return;
    Material mat=materials[id];
    uint kind=mat.flags & (K_ALPHA|K_FLORA|K_LIQUID|K_SHAPED);
    if(kind==0u)return; // opaque cubes handled by PackedFace path
    uint meta=metaAt(s,p);
    const uint FULL=64u|(64u<<7u)|(64u<<14u);
    if(kind==K_FLORA){
        add(job,s,slot,id,index,meta,kind,6u,0u,FULL,0u,0u,0u,0u);
        add(job,s,slot,id,index,meta,kind,7u,0u,FULL,0u,0u,0u,0u);
    }else if(kind==K_SHAPED){
        uint shapeMeta=meta;
        // Neighbor-dependent stair corners/connectivity are resolved on GPU.
        if(mat.shape==2u){
            uint facing=meta&3u;
            ivec3 dir=DIRS[5]; // north = -Z
            if(facing==1u)dir=DIRS[0];
            else if(facing==2u)dir=DIRS[4];
            else if(facing==3u)dir=DIRS[1];
            uint l=(facing+3u)&3u, r=(facing+1u)&3u;
            uint front=blockAt(s,p+dir), back=blockAt(s,p-dir);
            if((materials[front].flags&K_SHAPED)!=0u && materials[front].shape==2u){
                uint adj=metaAt(s,p+dir)&3u;
                if(adj==l)shapeMeta=(meta&224u)|facing|(1u<<2u);
                else if(adj==r)shapeMeta=(meta&224u)|facing|(1u<<2u)|(1u<<4u);
            }
            if(shapeMeta==meta && (materials[back].flags&K_SHAPED)!=0u && materials[back].shape==2u){
                uint adj=metaAt(s,p-dir)&3u;
                if(adj==l)shapeMeta=(meta&224u)|facing|(2u<<2u);
                else if(adj==r)shapeMeta=(meta&224u)|facing|(2u<<2u)|(1u<<4u);
            }
        }
        // Match Block_Shapes::shape_connection_meta and the working GL GPU mesher:
        // fence=6, wall=7, pane=12. Neighboring full cubes and matching shapes
        // connect even when the persisted metadata contains no connection bits.
        if(mat.shape==6u || mat.shape==7u || mat.shape==12u){
            const ivec3 horizontal[4]=ivec3[4](ivec3(0,0,-1),ivec3(1,0,0),
                                                 ivec3(0,0,1),ivec3(-1,0,0));
            shapeMeta=0u;
            for(uint d=0u;d<4u;++d){
                uint adjacent=blockAt(s,p+horizontal[d]);
                Material neighbor=materials[adjacent];
                bool connects=(neighbor.flags&K_OCCLUDES)!=0u ||
                    (mat.shape==6u && (neighbor.shape==6u || neighbor.shape==8u) &&
                     (neighbor.flags&K_SHAPED)!=0u) ||
                    (mat.shape==7u && neighbor.shape==7u && (neighbor.flags&K_SHAPED)!=0u) ||
                    (mat.shape==12u && neighbor.shape==12u && (neighbor.flags&K_SHAPED)!=0u);
                if(connects)shapeMeta|=1u<<d;
            }
        }
        // The legacy pressed button has its own ID; new buttons encode it in bit 3.
        if(mat.shape==4u && id==160u)shapeMeta|=8u;
        // Copper wires need neighbor-dependent arms, including side-mounted wires.
        // Connection metadata is derived here, rather than depending on a CPU mesh.
        if(mat.shape==5u){
            uint mount=meta&7u;
            if(mount>4u)mount=0u;
            const ivec3 floorDirs[4]=ivec3[4](ivec3(0,0,-1),ivec3(1,0,0),
                                               ivec3(0,0,1),ivec3(-1,0,0));
            const ivec3 wallXDirs[4]=ivec3[4](ivec3(0,0,-1),ivec3(0,1,0),
                                               ivec3(0,0,1),ivec3(0,-1,0));
            const ivec3 wallZDirs[4]=ivec3[4](ivec3(0,-1,0),ivec3(1,0,0),
                                               ivec3(0,1,0),ivec3(-1,0,0));
            uint arms=0u;
            for(uint d=0u;d<4u;++d){
                ivec3 step=(mount==1u||mount==2u)?wallXDirs[d]:
                           (mount==3u||mount==4u)?wallZDirs[d]:floorDirs[d];
                uint neighbor=blockAt(s,p+step);
                Material nm=materials[neighbor];
                uint neighborMount=metaAt(s,p+step)&7u;
                const uint wallDir[4]=uint[4](4u,1u,3u,2u);
                bool wire=(nm.flags&K_SHAPED)!=0u && nm.shape==5u &&
                    (neighborMount==mount||(mount==0u&&neighborMount==wallDir[d]));
                bool component=((nm.flags&K_SHAPED)!=0u&&nm.shape==4u)||
                    (neighbor>=157u && neighbor<=164u);
                if(wire||component)arms|=1u<<d;
            }
            uint riser=0u;
            if(mount!=0u){
                ivec3 away=mount==1u?ivec3(-1,0,0):
                            mount==2u?ivec3(1,0,0):
                            mount==3u?ivec3(0,0,-1):ivec3(0,0,1);
                uint nid=blockAt(s,p+away);
                if((materials[nid].flags&K_SHAPED)!=0u&&
                   materials[nid].shape==5u&&(metaAt(s,p+away)&7u)==0u)riser=128u;
            }
            shapeMeta=mount|(arms<<3u)|riser;
        }
        ShapeRange r=shapeRanges[mat.shape*256u+shapeMeta];
        for(uint b=0u;b<r.count;b++){
            ShapeBox box=shapeBoxes[r.first+b];
            for(uint face=0u;face<6u;face++){
                if(internalFace(r,box,face))continue;
                uint axis=face>>1u;
                bool positive=(face&1u)==0u;
                uint border=positive?coord(box.hi,axis):coord(box.lo,axis);
                if((positive && border==64u)||(!positive && border==0u)){
                    if(!exposed(s,p,id,face,kind))continue;
                }
                add(job,s,slot,id,index,meta,kind,face,box.lo,box.hi,0u,0u,0u,0u);
            }
        }
    }else if(kind==K_LIQUID){
        // Source/falling: full block. Flowing fluid 1..7: lower surface.
        uint level=meta&7u;
        uint height=((meta&8u)!=0u||level==0u)?65535u:uint((8u-level)*65535u/8u);
        for(uint face=0u;face<6u;face++){
            if(!exposed(s,p,id,face,kind))continue;
            add(job,s,slot,id,index,meta,kind,face,0u,FULL,height,height,height,height);
        }
    }else if(kind==K_ALPHA){
        for(uint face=0u;face<6u;face++){
            if(exposed(s,p,id,face,kind))
                add(job,s,slot,id,index,meta,kind,face,0u,FULL,0u,0u,0u,0u);
        }
    }
}
