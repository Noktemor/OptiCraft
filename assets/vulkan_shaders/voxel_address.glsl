// Stage23: logical neighbour IDs and independently addressed voxel channels.
// No extra descriptor bindings or slot stride changes. Legacy/probe slots use
// original dense offsets when bit 5 is clear. Uniform words are fully 32-bit.
const uint VOXEL_ADDRESS=32u, VOXEL_UNIFORM_BLOCK=64u,
    VOXEL_UNIFORM_META=128u, VOXEL_UNIFORM_LIGHT=256u;
bool voxelIndirect(int flags){return (uint(flags)&VOXEL_ADDRESS)!=0u;}
uint voxelNeighborSlot(int flags,int link){return voxelIndirect(flags)?uint(link):uint(link)/16384u;}
#define VOXEL_WORD(data, address, wordOffset, flags, uniformBit) \
    (((uint(flags)&(uniformBit))!=0u)?uint(address):data[uint(address)+uint(wordOffset)])
