#include "rendering/vulkan/VulkanBlockCatalog.h"
#include "world/blocks/Block_Types.h"
#include <cassert>
#include <iostream>
using namespace OptiCraft::Vulkan;
int main(){
 Block_Registry r(65536);
 r.edit(1).is_opaque=true;
 r.edit(2).is_transparent=true;
 r.edit(3).mesh_type=Block_Mesh_Type::Flora;
 r.edit(4).mesh_type=Block_Mesh_Type::Liquid;
 auto& shaped=r.edit(5);shaped.mesh_type=Block_Mesh_Type::Shaped;shaped.shape=Block_Shape::Stairs;
 auto& orientation=r.edit(6);orientation.orientation=Block_Orientation::Axis;orientation.is_opaque=true;
 r.edit(7).mesh_type=Block_Mesh_Type::Liquid;
 r.edit(9).is_item=true;
 r.edit(10).mesh_type=Block_Mesh_Type::Flora;
 r.edit(10).mesh_style=Block_Mesh_Style::XStyle;
 r.edit(8).mesh_style=Block_Mesh_Style::XStyle;
 r.edit(65535).is_opaque=true;
 auto c=GpuBlockCatalog::build(r);
 assert(c.blocks.size()==65536);
 assert(c.opaqueMesherFlags.size()==65536);
 assert(c.activeBlockDefinitions==10); // 1..8, 10, 65535
 assert(c.activeKinds[0]==3 && c.activeKinds[1]==2 && c.activeKinds[2]==2 &&
        c.activeKinds[3]==2 && c.activeKinds[4]==1);
 assert((c.blocks[1].flags&VisualOpaqueCube)!=0);
 assert(c.opaqueMesherFlags[1]==3u);
 assert(c.opaqueMesherFlags[2]==0u);
 assert(c.opaqueMesherFlags[5]==0u);
 assert((c.blocks[2].flags&VisualAlphaCube)!=0);
 assert((c.blocks[3].flags&VisualAlphaCube)!=0);
 assert((c.blocks[3].flags&VisualBlended)==0);
 assert((c.blocks[2].flags&VisualBlended)!=0);
 assert((c.blocks[10].flags&VisualFlora)!=0);
 assert((c.blocks[4].flags&VisualLiquid)!=0);
 assert((c.blocks[5].flags&VisualShaped)!=0);
 assert(c.blocks[5].shape==uint32_t(Block_Shape::Stairs));
 assert(c.blocks[6].metaStride==6);
 auto face=c.blocks[6].faceBase;
 assert(c.faces[face].uv[0]==0.1f);
 assert(c.faces[face+6].uv[0]==0.5f);
 assert(c.faces[face+6].options[0]==1);
 assert(c.blocks[9].flags==0);
 assert((c.blocks[7].flags&(VisualWater|VisualLiquid|VisualBlended))==(VisualWater|VisualLiquid|VisualBlended));
 assert((c.blocks[4].flags&VisualWater)==0);
 assert((c.blocks[65535].flags&VisualOpaqueCube)!=0);
 assert(c.shapeIndex.size()==33u*256u);
 assert(c.shapeIndex[uint32_t(Block_Shape::Stairs)*256+67].count==1);
 std::cout<<"GPU material+shape catalog smoke passed\n";
}
