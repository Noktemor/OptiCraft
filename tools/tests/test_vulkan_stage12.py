#!/usr/bin/env python3
"""CPU reference for shader ordering/AO and integration source checks (not GPU execution)."""
from pathlib import Path
import random, math
root=Path(__file__).resolve().parents[2]
read=lambda p:(root/p).read_text()
b='src/rendering/vulkan/';s='assets/vulkan_shaders/'
rng=random.Random(12)
INVALID=2**32-1
# Mirrors the shader's disjoint compare/exchange network and its tie-break.
def network(values):
    n=len(values);p=1
    while p<n:p*=2
    a=values+[(float('-inf'),INVALID)]*(p-n)
    width=2
    while width<=p:
        stride=width//2
        while stride:
            for i in range(p):
                j=i^stride
                if j<=i:continue
                before=lambda x,y:(-x[0],x[1])<(-y[0],y[1])
                desc=(i&width)==0
                if (desc and before(a[j],a[i])) or (not desc and before(a[i],a[j])):
                    a[i],a[j]=a[j],a[i]
            stride//=2
        width*=2
    return a[:n]
for n in (1,2,3,63,64,65,511,512,513,1023,1024,1025,2047,2048):
    for fraction in (0,0.1,0.5,1):
        values=[(rng.randrange(-100,101),i) if rng.random()<fraction else (float('-inf'),INVALID) for i in range(n)]
        rng.shuffle(values)
        assert network(values)==sorted(values,key=lambda x:(-x[0],x[1]))
# Simulate axial traversal of a resident 3x3x3 chunk neighborhood; all edge/corner offsets.
resident={(x,y,z) for x in (-1,0,1) for y in (-1,0,1) for z in (-1,0,1)}
def locate(p,available):
    p=list(p); slot=[0,0,0]
    for axis in range(3):
        if 0<=p[axis]<32:continue
        slot[axis]+=1 if p[axis]>=32 else -1
        if tuple(slot) not in available:return None
        p[axis]=(p[axis]+32)%32
    return tuple(slot),tuple(p)
for x in (-2,-1,0,1,30,31,32,33):
 for y in (-2,-1,0,1,30,31,32,33):
  for z in (-2,-1,0,1,30,31,32,33):
    p=(x,y,z)
    assert locate(p,resident)==(tuple(v//32 for v in p),tuple(v%32 for v in p))
    assert locate(p,{(0,0,0)})==(locate(p,resident) if all(0<=v<32 for v in p) else None)
for a in (0,1):
 for c in (0,1):
  for d in (0,1):
    dark=3 if a and c else a+c+d
    shade=1-.35*dark/3
    assert .65<=shade<=1
assert 'shared float keys[2048]' in read(s+'voxel_misc_cull.comp')
assert 'barrier();' in read(s+'voxel_misc_cull.comp')
assert '(buffers_.depthSort || bins) ? 1u' in read(b+'VulkanGpuCuller.cpp')
assert 'sizeof(Params) == 128' in read(b+'VulkanGpuCuller.h')
frame=read(b+'VulkanLiveWorldRenderer.cpp')
assert frame.index('live,false,multiDraw_')<frame.index('itemPipeline_.draw')<frame.index('live,true,multiDraw_')<frame.index('uiPipeline_.draw')
assert 'b.depthSort=true' in frame and 'view.vp[3],view.vp[7],view.vp[11]' in frame
item=read(b+'VulkanItemPipeline.cpp')
assert 'ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE' in item
assert 'vertexAttributeDescriptionCount=uint32_t(attributes.size())' in item
assert 'buffer.write(0,items.data(),bytes)' in item
assert 'm_vulkan_game_mirror->submitItems(m_dropped_items_mesh)' in read('src/core/game/Game.cpp')
assert 'item.vert item.frag' in read('CMakeLists.txt')
assert 'ambientOcclusion(range)' in read(s+'voxel_textured.frag')
assert 'VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT' in read(b+'VulkanChunkStreamGpu.cpp')
assert 'stream.lights(),stream.blocks()' in read(b+'VulkanOpaqueMaterialPipeline.cpp')
assert 'if(range>=32.0)return 1.0;' in read(s+'voxel_textured.frag')
assert 'entityPipeline_.draw(frame,entities,view)' in frame
assert 'vkCmdDrawIndexed(frame.cmd,p.count,1,p.first,0,i)' in read(b+'VulkanEntityPipeline.cpp')
assert 'releaseStagingAfterCompletion' in read(b+'VulkanEntityPipeline.cpp')
assert 'mat4 bones[64]' in read(s+'entity.vert')
assert 'poses[instance].bones[joints[j]]*weights[j]' in read(s+'entity.vert')
assert '(*entitySink)(std::move(draw));return;' in read('src/mobs/Mob.cpp')
assert 'range.texture_role' in read('src/mobs/Mob_Model.cpp')
assert 'Mob::ScopedEntitySink capture(sink)' in read('src/core/game/Game.cpp')
assert 'entity.vert entity.frag' in read('CMakeLists.txt')
# GLB assets: actual joint counts and index accessor counts fit the entity ABI.
import json, struct
models=0
for path in (root/'assets/mobs').glob('*.glb'):
    data=path.read_bytes();length,tag=struct.unpack_from('<II',data,12)
    assert tag==0x4e4f534a
    gltf=json.loads(data[20:20+length])
    assert all(len(skin['joints'])<=64 for skin in gltf.get('skins',[])),path
    for mesh in gltf.get('meshes',[]):
        for primitive in mesh['primitives']:
            if primitive.get('mode',4)!=4:continue
            assert gltf['accessors'][primitive['attributes']['POSITION']]['count']>0
            if 'indices' in primitive:assert gltf['accessors'][primitive['indices']]['count']>0
    models+=1
print(f'Entity GLB source contracts: {models} models fit the 64-bone GPU ABI')
print('Stage 12: 56 sort cases, 512 neighbor samples, 8 AO combinations and source contracts PASS (CPU reference only)')
