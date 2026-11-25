#!/usr/bin/env python3
"""Reference invariant tests for GPU row merging, not an actual GPU shader test.

Geometry must reconstruct EXACTLY the set of visible source cube faces; a run
must never merge across a material, metadata, light or occlusion discontinuity.
"""
from __future__ import annotations
import random
from pathlib import Path

N=32
D=[(-1,0,0),(1,0,0),(0,-1,0),(0,1,0),(0,0,-1),(0,0,1)]
def pos(face,row,column):
    if face<2:return (row&31,row>>5,column)
    if face<4:return (column,row&31,row>>5)
    return (column,row>>5,row&31)
def cell(p):return (p[1]*32+p[2])*32+p[0]
def face_light(p,f,light):
    q=tuple(p[i]+D[f][i] for i in range(3))
    # This test has no resident neighbor; the GPU fallback is its own cell.
    if any(x<0 or x>=N for x in q):q=p
    return light[cell(q)]
def visible(p,f,blocks,opaque):
    id=blocks[cell(p)]
    if id not in opaque:return False
    n=tuple(p[i]+D[f][i] for i in range(3))
    if any(x<0 or x>=N for x in n):return True
    return blocks[cell(n)] not in opaque

def mesh(blocks,meta,light,opaque):
    result=[]
    for f in range(6):
        for slice in range(32):
            def point(y,x):return pos(f,slice+32*y,x)
            keys=[]
            for y in range(32):
                for x in range(32):
                    p=point(y,x)
                    keys.append((blocks[cell(p)],meta[cell(p)],face_light(p,f,light)) if visible(p,f,blocks,opaque) else None)
            for y in range(32):
                x=0
                while x<32:
                    key=keys[y*32+x]
                    if key is None:x+=1;continue
                    w=1
                    while x+w<32 and keys[y*32+x+w]==key:w+=1
                    h=1
                    while y+h<32 and all(keys[(y+h)*32+x+i]==key for i in range(w)):h+=1
                    for j in range(h):
                        for i in range(w):keys[(y+j)*32+x+i]=None
                    p=point(y,x);id=key[0];slot=511
                    w0=p[0]|(p[1]<<5)|(p[2]<<10)|(f<<15)|((id&0x3fff)<<18)
                    w1=(id>>14)|(slot<<2)|((w-1)<<18)|((h-1)<<23)
                    result.append((w0,w1));x+=w
    return result

def verify(blocks,meta,light,opaque):
    records=mesh(blocks,meta,light,opaque)
    actual={}
    for w0,w1 in records:
        p=(w0&31,(w0>>5)&31,(w0>>10)&31)
        f=(w0>>15)&7
        id=((w0>>18)&0x3fff)|((w1&3)<<14)
        slot=(w1>>2)&0xffff
        length=((w1>>18)&31)+1
        assert slot==511 and 0<=f<6 and 1<=length<=32
        height=((w1>>23)&31)+1
        for j in range(height):
            for k in range(length):
                v=list(p);v[2 if f<2 else 0]+=k;v[2 if 2<=f<4 else 1]+=j;q=tuple(v)
                assert all(0<=z<32 for z in q)
                assert blocks[cell(q)]==id and visible(q,f,blocks,opaque)
                key=(f,q)
                assert key not in actual, f'duplicate GPU face {key}'
                actual[key]=(id,meta[cell(q)],face_light(q,f,light))
    expected={(f,pos(f,r,c)):(blocks[cell(pos(f,r,c))],meta[cell(pos(f,r,c))],face_light(pos(f,r,c),f,light))
              for f in range(6) for r in range(1024) for c in range(32)
              if visible(pos(f,r,c),f,blocks,opaque)}
    assert expected==actual,(len(expected),len(actual))
    return len(records),len(expected)

def main():
    root=Path(__file__).resolve().parents[2]
    vs=(root/'assets/vulkan_shaders/voxel_textured.vert').read_text()
    common=(root/'assets/vulkan_shaders/mesher_common.glsl').read_text()
    runs=(root/'assets/vulkan_shaders/voxel_row_runs.glsl').read_text()
    core=(root/'src/rendering/vulkan/VulkanGpuMesher.cpp').read_text()
    assert 'vkCmdDispatch(cmd, run.groupsX, run.count, 1)' in core
    dispatch=(root/'src/rendering/vulkan/MeshDispatchRuns.h').read_text()
    assert '((k&4u)?6u:192u)' in dispatch
    for stage in ['voxel_count.comp','voxel_emit.comp']:
        shader=(root/'assets/vulkan_shaders'/stage).read_text()
        assert 'if(gl_NumWorkGroups.x==6u)task=task*32u+((task&1u)!=0u?31u:0u);' in shader
    # The six-group mode visits exactly the boundary slice of each direction.
    assert [(g*32+(31 if g&1 else 0)) for g in range(6)]==[0,63,64,127,128,191]
    assert 'slot << 2u' in common and '(runLength - 1u) << 18u' in common
    assert 'metaAt(chunk,slot,p)<<16u' in runs and 'faceLight(slot,chunk,p,face)<<24u' in runs
    assert '((runHeight - 1u) << 23u)' in common and 'planeRects[1024]' in runs
    assert 'uint slot=(packedValue.y>>2u)&65535u' in vs
    for f,component in [(0,'uv.x'),(1,'uv.y'),(2,'uv.x'),(3,'uv.y'),(4,'uv.y'),(5,'uv.x')]:
        assert f'face=={f}u' in vs
    rng=random.Random(13013)
    blocks=[0]*32768;meta=[0]*32768;light=[0xff]*32768
    # Air, solid, checkerboard, metadata and light boundaries, dense noise.
    scenes=[(blocks[:],meta[:],light[:])]
    scenes.append(([1]*32768,meta[:],light[:]))
    scenes.append(([(x+z+y)%2 for y in range(32) for z in range(32) for x in range(32)],meta[:],light[:]))
    scenes.append(([1]*32768,[i%3 for i in range(32768)],[0xf0+i%7 for i in range(32768)]))
    scenes.append(([rng.choice([0,0,1,1,1,0x8001]) for _ in range(32768)],
                   [rng.randrange(4) for _ in range(32768)],[rng.randrange(16) for _ in range(32768)]))
    for i,(b,m,l) in enumerate(scenes):
        rec,face=verify(b,m,l,{1,0x8001})
        if i==1:assert rec==6 and face==6144
        print(f'GPU-rectangle reference scene {i}: {rec} rectangles cover {face} faces')
    print('GPU rectangle coverage/packing: PASS (CPU reference only; GLSL not compiled)')
if __name__=='__main__':main()
