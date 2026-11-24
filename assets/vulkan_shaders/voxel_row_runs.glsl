// One workgroup owns a 32x32 plane of one face direction. No cross-plane races.
const uint FACE_PLANE_TASKS = 192u;
shared uint planeKeys[1024];
shared uvec2 planeRects[1024];
shared uint planeCount;
shared uint planeBase;
ivec3 planePosition(uint face,uint slice,uint row,uint column){
    if(face<2u)return ivec3(int(slice),int(row),int(column));
    if(face<4u)return ivec3(int(column),int(slice),int(row));
    return ivec3(int(column),int(row),int(slice));
}
uint cellIndex(ivec3 p) {return uint((p.y*32 + p.z)*32 + p.x);}
uint lightAt(uint slot, ivec3 p) {
    uint cell=cellIndex(p);
    return (lightWords[slot*8192u+(cell>>2u)]>>((cell&3u)*8u))&255u;
}
// Light is stored in the AIR/front cell, not usually in the solid cube.
// Neighbor source offsets are block-word offsets in the shared GPU pool.
uint faceLight(uint slot, ChunkSlot chunk, ivec3 p, uint face) {
    ivec3 q=p+direction(face);
    if(all(greaterThanEqual(q,ivec3(0))) && all(lessThan(q,ivec3(32))))
        return lightAt(slot,q);
    int source=-1;
    if(face==0u)source=chunk.neighborA.x;
    else if(face==1u)source=chunk.neighborA.y;
    else if(face==2u)source=chunk.neighborA.z;
    else if(face==3u)source=chunk.neighborA.w;
    else if(face==4u)source=chunk.neighborB.x;
    else source=chunk.neighborB.y;
    if(source<0)return lightAt(slot,p); // missing neighbor, conservative fallback
    q=(q+ivec3(32))%32;
    return lightAt(uint(source)/16384u,q);
}
uint metaAt(uint slot, ivec3 p) {
    uint cell = cellIndex(p);
    uint word = metaWords[slot*8192u + (cell >> 2u)];
    return (word >> ((cell & 3u)*8u)) & 255u;
}
// Material ID, metadata and surface light form an exact equality key.
// Air (ID 0) never emits; a zero key is therefore a safe empty sentinel.
void buildPlane(ChunkSlot chunk,uint slot,uint face,uint slice){
    uint lane=gl_LocalInvocationID.x;
    for(uint i=lane;i<1024u;i+=64u){
        ivec3 p=planePosition(face,slice,i>>5u,i&31u);
        uint material=cellAt(chunk,p);
        planeKeys[i]=shouldEmit(chunk,material,p,face)?
            (material|(metaAt(slot,p)<<16u)|(faceLight(slot,chunk,p,face)<<24u)):0u;
    }
    barrier();
    if(lane==0u){
        uint n=0u;
        for(uint y=0u;y<32u;++y)for(uint x=0u;x<32u;){
            uint key=planeKeys[y*32u+x];
            if(key==0u){++x;continue;}
            uint width=1u;
            while(x+width<32u && planeKeys[y*32u+x+width]==key)++width;
            uint height=1u;
            while(y+height<32u){
                bool equal=true;
                for(uint i=0u;i<width;++i)if(planeKeys[(y+height)*32u+x+i]!=key){equal=false;break;}
                if(!equal)break;
                ++height;
            }
            for(uint j=0u;j<height;++j)for(uint i=0u;i<width;++i)planeKeys[(y+j)*32u+x+i]=0u;
            planeRects[n++]=packFace(planePosition(face,slice,y,x),face,key&65535u,slot,width,height);
            x+=width;
        }
        planeCount=n;
    }
    barrier();
}
