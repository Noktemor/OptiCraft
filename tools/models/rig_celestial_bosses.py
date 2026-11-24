#!/usr/bin/env python3
"""Add rigid cuboid joints + authored keyframes to existing handmade princess GLBs.

Does not change appearance, UVs, mesh topology, or material. Each cuboid is
rigidly attached to its corresponding body joint. This is deliberately small,
portable glTF 2.0, without a Blender dependency; rerun only on original STATIC
princess models, not an already rigged asset.
"""
from __future__ import annotations
import copy
import json
import math
import struct
import sys
from pathlib import Path
import numpy as np
from scipy.spatial.transform import Rotation

ROOT = Path(__file__).resolve().parents[2]

SUN = {
    'Sun_Idle': (2.0, {'Left_Arm':(-6,0,-5),'Right_Arm':(-6,0,5)},
                     {'Left_Arm':(-12,0,-8),'Right_Arm':(0,0,8),'Torso':(0,0,2)}),
    'Sun_Palm_Lance': (1.65,{'Right_Arm':(18,-22,18),'Left_Arm':(-15,0,-15),'Torso':(0,-15,0)},
                            {'Right_Arm':(-105,-18,2),'Left_Arm':(-10,0,-15),'Torso':(0,18,5)}),
    'Sun_Twin_Seal': (2.14,{'Left_Arm':(14,-18,-14),'Right_Arm':(10,18,14)},
                          {'Left_Arm':(-95,0,-12),'Right_Arm':(-100,0,12),'Torso':(-9,0,0)}),
    'Sun_Halo_Sweep': (2.57, {'Left_Arm':(0,0,-70),'Right_Arm':(0,0,70),'Torso':(0,-18,0)},
                            {'Left_Arm':(-45,0,-85),'Right_Arm':(-45,0,85),'Torso':(0,55,10),'Left_Leg':(-26,0,-6)}),
    'Sun_Sun_Dash': (1.88, {'Torso':(15,0,0),'Left_Arm':(45,0,-8),'Right_Arm':(45,0,8)},
                           {'Torso':(32,0,0),'Left_Arm':(70,0,-6),'Right_Arm':(70,0,6),'Left_Leg':(-38,0,0),'Right_Leg':(32,0,0)}),
    'Sun_Air_Step': (1.56, {'Torso':(8,0,0),'Right_Leg':(-45,0,0),'Left_Leg':(48,0,0),'Right_Arm':(35,0,0)},
                           {'Torso':(12,0,-10),'Right_Leg':(52,0,0),'Left_Leg':(-56,0,0),'Left_Arm':(35,0,0)}),
    'Sun_Rising_Kick':(2.13, {'Right_Leg':(25,0,0),'Left_Arm':(-36,0,-20),'Right_Arm':(-50,0,20)},
                             {'Right_Leg':(-110,0,0),'Left_Leg':(14,0,0),'Torso':(-18,0,0),'Left_Arm':(-42,0,-40)}),
    'Sun_Crown_Dance':(2.73,{'Torso':(0,45,0),'Left_Arm':(-42,0,-82),'Right_Arm':(-42,0,82),'Left_Leg':(12,0,0)},
                            {'Torso':(0,-76,14),'Left_Arm':(-66,0,-76),'Right_Arm':(-66,0,76),'Right_Leg':(-72,0,0)}),
    'Sun_Falling_Star':(2.68,{'Torso':(-25,0,0),'Left_Arm':(-125,0,-5),'Right_Arm':(-125,0,5),'Left_Leg':(-48,0,0),'Right_Leg':(-48,0,0)},
                            {'Torso':(40,0,0),'Left_Arm':(30,0,-15),'Right_Arm':(30,0,15),'Left_Leg':(18,0,0),'Right_Leg':(18,0,0)}),
    'Sun_Delayed_Palm':(2.38,{'Torso':(0,-30,-8),'Right_Arm':(48,-36,12),'Left_Arm':(-16,0,-30)},
                             {'Torso':(0,35,8),'Right_Arm':(-117,10,8),'Left_Arm':(-58,0,-30)}),
}
MOON = {
    'Moon_Idle':(2.0,{'Left_Arm':(-12,0,-6),'Right_Arm':(-12,0,6)},
                       {'Left_Arm':(-20,0,-14),'Right_Arm':(-20,0,14),'Torso':(0,0,-3)}),
    'Moon_Cast':(2.7,{'Torso':(0,-18,0),'Left_Arm':(-75,0,-20),'Right_Arm':(-75,0,20)},
                      {'Torso':(0,22,0),'Left_Arm':(-120,0,-35),'Right_Arm':(-120,0,35)}),
    'Moon_Swoop':(2.0,{'Torso':(18,0,0),'Left_Arm':(25,0,-30),'Right_Arm':(25,0,30)},
                       {'Torso':(28,0,0),'Left_Arm':(48,0,-38),'Right_Arm':(48,0,38)}),
}

# Both existing hand-made assets use sequential groups of eight vertices per cuboid.
# Cuboids: torso, head, hair/crown, horns, arms, legs, clothing, face, ornaments.
# The glTF's joint order is explicit and stable, including for hitbox classification.
JOINTS = ['Root','Torso','Left_Arm','Right_Arm','Left_Leg','Right_Leg','Head']
PIVOTS = [(0,0,0),(0,1.4,0),(-.58,2.15,0),(.58,2.15,0),(-.22,1.25,0),(.22,1.25,0),(0,2.66,0)]
CUBE_TO_BONE = [1,6,6,6,6,2,3,4,5,1,1,6,6,6,6,6]

def run(path:Path,clip_defs:dict):
    original=path.read_bytes()
    gltf_len=struct.unpack_from('<I',original,12)[0]
    gltf=json.loads(original[20:20+gltf_len].decode('utf8'))
    if gltf.get('skins'): raise RuntimeError(f'{path} already has a rig: use pristine GLB')
    data_start=20+(gltf_len+3)//4*4
    bin_len,chunk_type=struct.unpack_from('<II',original,data_start)
    assert chunk_type==0x004e4942
    binary=bytearray(original[data_start+8:data_start+8+bin_len])
    pos_accessor=gltf['accessors'][gltf['meshes'][0]['primitives'][0]['attributes']['POSITION']]
    count=pos_accessor['count']
    if count!=128: raise ValueError(f'{path}: expected 128 original vertices, got {count}')

    def pack(values,dtype,shape,typ,component):
        array=np.asarray(values,dtype=dtype).reshape((-1,shape))
        while len(binary)%4: binary.append(0)
        offset=len(binary)
        binary.extend(array.tobytes())
        view=len(gltf['bufferViews'])
        gltf['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':array.nbytes})
        accessor=len(gltf['accessors'])
        gltf['accessors'].append({'bufferView':view,'componentType':component,'count':len(array),'type':typ})
        return accessor

    ids=np.repeat(np.asarray(CUBE_TO_BONE,dtype=np.uint16),8)
    joints=np.zeros((count,4),dtype=np.uint16)
    joints[:,0]=ids
    weights=np.zeros((count,4),dtype=np.float32)
    weights[:,0]=1
    prim=gltf['meshes'][0]['primitives'][0]
    prim['attributes']['JOINTS_0']=pack(joints,np.uint16,4,'VEC4',5123)
    prim['attributes']['WEIGHTS_0']=pack(weights,np.float32,4,'VEC4',5126)

    original_nodes=len(gltf['nodes'])
    assert original_nodes==2
    parent_nodes=[2,2,2,2,2,2,3]
    nodes=[]
    for i,(name,pivot) in enumerate(zip(JOINTS,PIVOTS)):
        parent_index=parent_nodes[i]
        local=np.asarray(pivot) - (np.asarray(PIVOTS[parent_index-2]) if i==6 else np.zeros(3))
        # Root and all limbs use absolute bind translations under Root (identity).
        nodes.append({'name':name,'translation':list(map(float,local))})
    nodes[0]['children']=[3,4,5,6,7]
    nodes[1]['children']=[8]
    gltf['nodes'].extend(nodes)
    gltf['nodes'][0]['children'].append(2)
    gltf['nodes'][1]['skin']=0
    matrices=[]
    for x,y,z in PIVOTS:
        m=np.eye(4,dtype=np.float32)
        m[:3,3]=[-x,-y,-z]
        matrices.append(m.T.flatten()) # glTF matrix column-major
    ibm=pack(matrices,np.float32,16,'MAT4',5126)
    gltf['skins']=[{'name':'Princess_Rig','joints':list(range(2,9)),'skeleton':2,'inverseBindMatrices':ibm}]

    animations=[]
    for name,(duration,preparation,strike) in clip_defs.items():
        tells={
            'Sun_Palm_Lance':.55,'Sun_Twin_Seal':.65,'Sun_Halo_Sweep':.82,'Sun_Sun_Dash':.68,
            'Sun_Air_Step':.46,'Sun_Rising_Kick':.71,'Sun_Crown_Dance':.86,
            'Sun_Falling_Star':1.05,'Sun_Delayed_Palm':.97,
            'Moon_Cast':1.8,'Moon_Swoop':.3
        }
        tell=min(tells.get(name,.55),duration*.5)
        # The motion peaks must match the attack's damage windows in
        # Boss_Choreography.h, not a separate arbitrary glTF timeline.
        hits={
            'Sun_Palm_Lance':[.55+.16], 'Sun_Twin_Seal':[.65+.19,.65+.66],
            'Sun_Halo_Sweep':[.82+.21,.82+.52,.82+.88],
            'Sun_Sun_Dash':[.68+.28], 'Sun_Air_Step':[.46+.26],
            'Sun_Rising_Kick':[.71+.34],
            'Sun_Crown_Dance':[.86+.20,.86+.58,.86+.96],
            'Sun_Falling_Star':[1.05+.46], 'Sun_Delayed_Palm':[.97+.36],
            'Moon_Cast':[2.47], 'Moon_Swoop':[1.00], 'Sun_Idle':[.65],
            'Moon_Idle':[.65],
        }[name]
        times=[0,tell*.6,tell]+hits+[min(duration-.05,hits[-1]+.12),duration]
        time_ref=pack(np.asarray(times,dtype=np.float32).reshape(-1,1),np.float32,1,'SCALAR',5126)
        anim={'name':name,'samplers':[],'channels':[]}
        for bone,idx in zip(JOINTS[1:],[1,2,3,4,5,6]):
            a=preparation.get(bone,(0,0,0))
            b=strike.get(bone,(0,0,0))
            pose=[(0,0,0), tuple(x*.55 for x in a),a]
            for i,_ in enumerate(hits):
                if len(hits)>1 and bone in ('Left_Arm','Right_Arm'):
                    # Alternating arms for Twin Seal/Halo Sweep. Last strike
                    # of a three-hit combo uses both arms for the finisher.
                    favored=(i%2==0 and bone=='Right_Arm') or (i%2==1 and bone=='Left_Arm') or i==2
                    pose.append(b if favored else a)
                elif name=='Sun_Crown_Dance' and bone=='Torso':
                    pose.append(tuple(x * (1 if i%2==0 else -.85) for x in b))
                else:
                    pose.append(b)
            pose.extend([tuple(x*.48 for x in b),(0,0,0)])
            quats=Rotation.from_euler('xyz',pose,degrees=True).as_quat().astype(np.float32)
            rot_ref=pack(quats,np.float32,4,'VEC4',5126)
            sampler_id=len(anim['samplers'])
            anim['samplers'].append({'input':time_ref,'output':rot_ref,'interpolation':'LINEAR'})
            anim['channels'].append({'sampler':sampler_id,'target':{'node':2+idx,'path':'rotation'}})
        animations.append(anim)
    gltf['animations']=animations
    gltf['buffers'][0]['byteLength']=len(binary)
    js=json.dumps(gltf,ensure_ascii=False,separators=(',',':')).encode()
    js+=b' ' * (-len(js)%4)
    binary+=b'\0' * (-len(binary)%4)
    new=struct.pack('<III',0x46546c67,2,12+8+len(js)+8+len(binary))
    new+=struct.pack('<II',len(js),0x4e4f534a)+js
    new+=struct.pack('<II',len(binary),0x004e4942)+binary
    path.write_bytes(new)
    print(path.name,'clips:',len(animations),'size:',len(new))

if __name__=='__main__':
    run(ROOT/'assets/mobs/sun_princess.glb',SUN)
    run(ROOT/'assets/mobs/moon_princess.glb',MOON)
