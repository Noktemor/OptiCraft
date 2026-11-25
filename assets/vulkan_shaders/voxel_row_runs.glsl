// One workgroup owns a 32x32 plane of one face direction. No cross-plane races.
const uint FACE_PLANE_TASKS = 192u;
shared uint planeKeys[1024];
// Count materializes rectangles only when the bounded cache is enabled.
#ifdef MESH_WRITE_RECTS
shared uvec2 planeRects[1024];
#endif
shared uint rowLive[32];
shared uint rowBreak[32];
shared uint planeCount;
shared uint planeBase;
// Two 16x16 planes of aligned macrocell classifications; no persistent coarse
// voxel buffer. EXACT keeps the original samples (including the full shell).
const uint LOD_EXACT=0xffffffffu;
shared uint lodOwn[256];
// Four lanes jointly own two adjacent rows. Each lane contributes eight
// columns to both row masks, then one lane per row combines the four parts.
shared uint lodRowLo[64],lodRowHi[64];
uint macroCode(ChunkSlot chunk,ivec3 p){
    if(any(lessThan(p,ivec3(2)))||any(greaterThanEqual(p,ivec3(30))))return LOD_EXACT;
    ivec3 base=p&ivec3(~1);
    uint n=0u,representative=0u;
    ivec3 air=base;
    for(uint j=0u;j<8u;++j){
        ivec3 offset=ivec3(int(j&1u),int((j>>1u)&1u),int(j>>2u));
        uint id=atLocal(chunk,base+offset);
        if(id==0u){air=base+offset;continue;}
        // Never approximate a special or non-occluding material, even if a
        // stale/diagnostic palette incorrectly enabled LOD2.
        if((materialFlags[id]&3u)!=3u)return LOD_EXACT;
        if(n==0u)representative=id|(j<<16u);
        ++n;
    }
    if(n<=3u)return 0u; // air-biased: retain larger cave voids
    if(n==7u){
        // Fill only isolated air, never any six-connected original passage.
        for(uint f=0u;f<6u;++f)
            if((materialFlags[cellAt(chunk,air+direction(f))]&BLOCK_OCCLUDER)==0u)return LOD_EXACT;
        return representative;
    }
    return LOD_EXACT; // preserve 2+ air cells, and fully solid materials
}
// LOD3 removes sparse 4^3 interior cells only. Never fills new air beyond
// LOD2, never expands the representative offset ABI, keeps LOD2 elsewhere.
bool farCoarseChunk(ChunkSlot chunk){return (uint(chunk.origin.w)&1024u)!=0u;}
bool sparseMacro4(ChunkSlot chunk,ivec3 p){
    if(any(lessThan(p,ivec3(4)))||any(greaterThanEqual(p,ivec3(28))))return false;
    ivec3 base=p&ivec3(~3);
    uint n=0u;
    for(uint j=0u;j<64u;++j){
        ivec3 offset=ivec3(int(j&3u),int((j>>2u)&3u),int(j>>4u));
        uint id=atLocal(chunk,base+offset);
        if(id==0u)continue;
        if((materialFlags[id]&3u)!=3u||++n>16u)return false;
    }
    return true;
}
bool coarseChunk(ChunkSlot chunk){
    return (uint(chunk.origin.w)&528u)==528u&&(uint(chunk.origin.w)&6u)==0u;
}
uint cachedMacroCode(ChunkSlot chunk,ivec3 p,uint job){
    if(any(lessThan(p,ivec3(2)))||any(greaterThanEqual(p,ivec3(30))))return LOD_EXACT;
    if(!useCoarseCache())return farCoarseChunk(chunk)&&sparseMacro4(chunk,p)?0u:macroCode(chunk,p);
    uvec3 c=uvec3(p)>>1u;
    return cachedWords[coarseWord(job,c.x+c.y*16u+c.z*256u)];
}
uint effectiveMaterial(ChunkSlot chunk,ivec3 p,uint code){
    return code==LOD_EXACT?cellAt(chunk,p):code&65535u;
}
uint macroIndex(uint row,uint column){return (row>>1u)*16u+(column>>1u);}
uint representativeOffset(ivec3 p,uint code){
    if(code==LOD_EXACT||code==0u)return 0u;
    uint parity=uint(p.x&1)|uint(p.y&1)<<1u|uint(p.z&1)<<2u;
    return parity^(code>>16u);
}
ivec3 planePosition(uint face,uint slice,uint row,uint column){
    if(face<2u)return ivec3(int(slice),int(row),int(column));
    if(face<4u)return ivec3(int(column),int(slice),int(row));
    return ivec3(int(column),int(row),int(slice));
}
uint cellIndex(ivec3 p) {return uint((p.y*32 + p.z)*32 + p.x);}
uint lightAt(ChunkSlot c,uint slot,ivec3 p) {
    uint cell=cellIndex(p);
    uint base=voxelIndirect(c.origin.w)?uint(c.neighborB.w):slot*8192u;
    return (VOXEL_WORD(lightWords,base,cell>>2u,c.origin.w,VOXEL_UNIFORM_LIGHT)>>((cell&3u)*8u))&255u;
}
// Light is stored in the AIR/front cell, not usually in the solid cube.
uint faceLight(uint slot, ChunkSlot chunk, ivec3 p, uint face) {
    ivec3 q=p+direction(face);
    if(all(greaterThanEqual(q,ivec3(0))) && all(lessThan(q,ivec3(32))))
        return lightAt(chunk,slot,q);
    int source=face<4u?chunk.neighborA[face]:chunk.neighborB[face-4u];
    if(source<0)return lightAt(chunk,slot,p);
    q=(q+ivec3(32))%32;
    uint neighbor=voxelNeighborSlot(chunk.origin.w,source);
    if(!voxelIndirect(chunk.origin.w)){
        uint cell=cellIndex(q);
        return (lightWords[neighbor*8192u+(cell>>2u)]>>((cell&3u)*8u))&255u;
    }
    return lightAt(slots[neighbor],neighbor,q);
}
uint metaAt(ChunkSlot c,uint slot, ivec3 p) {
    uint cell = cellIndex(p);
    uint base=voxelIndirect(c.origin.w)?uint(c.neighborB.z):slot*8192u;
    uint word=VOXEL_WORD(metaWords,base,cell>>2u,c.origin.w,VOXEL_UNIFORM_META);
    return (word >> ((cell & 3u)*8u)) & 255u;
}
// A coarse tile is 2x2 in the current plane. Known macro codes are
// constant over all four samples; only EXACT sides need per-voxel reads.
uint coarseTileVisibility(ChunkSlot chunk,ivec3 p,uint face,uint own,uint front){
    if(own!=LOD_EXACT && (materialFlags[own&65535u]&BLOCK_CUBE)==0u)return 0u;
    if(front!=LOD_EXACT && (materialFlags[front&65535u]&BLOCK_OCCLUDER)!=0u)return 0u;
    if(own!=LOD_EXACT && front!=LOD_EXACT)return 15u;
    ivec3 stepColumn=face<2u?ivec3(0,0,1):ivec3(1,0,0);
    ivec3 stepRow=face<2u?ivec3(0,1,0):(face<4u?ivec3(0,0,1):ivec3(0,1,0));
    ivec3 d=direction(face);
    uint mask=0u;
    for(uint i=0u;i<4u;++i){
        ivec3 q=p+int(i&1u)*stepColumn+int(i>>1u)*stepRow;
        if(own==LOD_EXACT && (materialFlags[cellAt(chunk,q)]&BLOCK_CUBE)==0u)continue;
        if(front==LOD_EXACT && (materialFlags[cellAt(chunk,q+d)]&BLOCK_OCCLUDER)!=0u)continue;
        mask|=1u<<i;
    }
    return mask;
}
void buildCoarseRows(ChunkSlot chunk,uint face,uint slice,uint job,uint lane){
    uint rowPair=lane>>2u;
    uint firstTile=(lane&3u)*4u;
    uint lo=0u,hi=0u;
    for(uint j=0u;j<4u;++j){
        uint col=firstTile+j;
        ivec3 p=planePosition(face,slice,rowPair*2u,col*2u);
        uint own=cachedMacroCode(chunk,p,job);
        lodOwn[rowPair*16u+col]=own;
        if(own==0u)continue; // no possible face, even on a chunk boundary
        ivec3 q=p+direction(face);
        uint front=all(equal(p&ivec3(~1),q&ivec3(~1)))?own:cachedMacroCode(chunk,q,job);
        uint v=coarseTileVisibility(chunk,p,face,own,front);
        lo|=(v&3u)<<(col*2u);
        hi|=(v>>2u)<<(col*2u);
    }
    lodRowLo[lane]=lo;lodRowHi[lane]=hi;
    barrier();
    if(lane<32u){
        uint start=(lane>>1u)*4u;
        uint mask=0u;
        for(uint j=0u;j<4u;++j)mask|=(lane&1u)==0u?lodRowLo[start+j]:lodRowHi[start+j];
        rowLive[lane]=mask;
    }
    barrier();
}
// Material ID, metadata and surface light form an exact equality key.
// Air (ID 0) never emits; a zero key is therefore a safe empty sentinel.
void buildPlane(ChunkSlot chunk,uint slot,uint face,uint slice,uint job){
    uint lane=gl_LocalInvocationID.x;
    bool lod=(uint(chunk.origin.w)&16u)!=0u;
    bool coarse=coarseChunk(chunk);
    if(usePlaneCompaction()){
        // Only coarse, uncached-rectangle jobs use this mode. Visibility
        // was computed once in Count; load the saved rows, not the voxels.
        if(lane<32u)rowLive[lane]=cachedWords[planeRowWord(job,face*32u+slice,lane)];
        barrier();
    }else if(coarse){
        // Coarse LOD keys are always binary. Build occupancy directly instead
        // of materializing 1024 keys and scanning them again for row runs.
        buildCoarseRows(chunk,face,slice,job,lane);
    }else{
        for(uint i=lane;i<1024u;i+=64u){
            ivec3 p=planePosition(face,slice,i>>5u,i&31u);
            uint material=cellAt(chunk,p);
            bool visible=shouldEmit(chunk,material,p,face);
            planeKeys[i]=visible?
                (lod?1u:(material|(metaAt(chunk,slot,p)<<16u)|(faceLight(slot,chunk,p,face)<<24u))):0u;
        }
        barrier();
        // One lane per row builds occupancy and immutable material boundaries.
        if(lane<32u){
            uint live=0u, breaks=0u, prev=0u;
            for(uint x=0u;x<32u;++x){
                uint key=planeKeys[lane*32u+x];
                if(key!=0u)live|=1u<<x;
                if(key!=prev)breaks|=1u<<x;
                prev=key;
            }
            rowLive[lane]=live;rowBreak[lane]=breaks;
        }
        barrier();
    }
    if(lane==0u){
        uint n=0u;
        for(uint y=0u;y<32u;++y)while(rowLive[y]!=0u){
            uint x=uint(findLSB(rowLive[y]));
            uint key=coarse?1u:planeKeys[y*32u+x];
            uint afterX=(0xffffffffu<<x)&~(1u<<x);
            uint stops=(~rowLive[y]|(coarse?0u:rowBreak[y]))&afterX;
            uint width=(stops==0u?32u:uint(findLSB(stops)))-x;
            uint rectMask=(0xffffffffu>>(32u-width))<<x;
            uint inside=rectMask&~(1u<<x);
            uint height=1u;
            while(y+height<32u){
                uint next=y+height;
                if((rowLive[next]&rectMask)!=rectMask)break;
                if(!coarse && ((rowBreak[next]&inside)!=0u || planeKeys[next*32u+x]!=key))break;
                ++height;
            }
            for(uint j=0u;j<height;++j)rowLive[y+j]&=~rectMask;
#ifdef MESH_WRITE_RECTS
#ifdef MESH_COUNT_CACHE
            if(dispatchParams.cacheCapacity!=0u){
#endif
            ivec3 anchor=planePosition(face,slice,y,x);
            uint code=coarse?(usePlaneCompaction()?cachedMacroCode(chunk,anchor,job):lodOwn[macroIndex(y,x)]):LOD_EXACT;
            uint material=lod?effectiveMaterial(chunk,anchor,code):key&65535u;
            uvec2 rect=packFace(anchor,face,material,slot,width,height);
            if(lod)rect.y|=(1u<<28u)|(representativeOffset(anchor,code)<<29u);
            planeRects[n]=rect;
#ifdef MESH_COUNT_CACHE
            }
#endif
#endif
            ++n;
        }
        planeCount=n;
    }
    barrier();
}
