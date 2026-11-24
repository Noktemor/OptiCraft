#!/usr/bin/env python3
"""GLB/source integration checks for OptiCraft 0.12.2 boss choreography."""
from pathlib import Path
import json
import struct
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
assert (ROOT/'VERSION').read_text().strip()in {'0.12.2', '0.13'}

def inspect(name,clips):
    path=ROOT/'assets/mobs'/f'{name}.glb'
    data=path.read_bytes()
    assert data[:4]==b'glTF'
    length,tag=struct.unpack_from('<II',data,12)
    assert tag==0x4e4f534a
    asset=json.loads(data[20:20+length])
    assert len(asset['skins'])==1
    joints=asset['skins'][0]['joints']
    assert len(joints)==7
    joint_names={asset['nodes'][i]['name'] for i in joints}
    assert {'Head','Torso','Left_Arm','Right_Arm','Left_Leg','Right_Leg'}<=joint_names
    prim=asset['meshes'][0]['primitives'][0]
    assert prim['attributes']['JOINTS_0']>=0 and prim['attributes']['WEIGHTS_0']>=0
    assert len(asset['animations'])==len(clips)
    assert {a['name'] for a in asset['animations']}==set(clips)
    for clip in asset['animations']:
        assert len(clip['channels'])>=6 and len(clip['samplers'])>=6
        assert all(c['target']['path']=='rotation' for c in clip['channels'])
    # Check bind transforms against inverse binds for every bone including
    # the nested head. This catches invisible-but-skinned GLB regressions.
    start=20+(length+3)//4*4
    binary=data[start+8:]
    accessor=asset['accessors'][asset['skins'][0]['inverseBindMatrices']]
    view=asset['bufferViews'][accessor['bufferView']]
    off=view['byteOffset']+accessor.get('byteOffset',0)
    ibm=np.frombuffer(binary,dtype='<f4',count=16*7,offset=off).reshape(7,4,4).transpose((0,2,1))
    world={}
    for i,node_index in enumerate(joints):
        node=asset['nodes'][node_index]
        pos=np.eye(4,dtype=np.float32)
        pos[:3,3]=node.get('translation',[0,0,0])
        if node_index==8: pos=world[3]@pos
        world[node_index]=pos
        np.testing.assert_allclose(pos@ibm[i],np.eye(4),atol=1.e-5)
    # Validate animated joint rotations and strike poses, not empty metadata.
    hit_times={
        'Sun_Palm_Lance':[.71], 'Sun_Twin_Seal':[.84,1.31],
        'Sun_Halo_Sweep':[1.03,1.34,1.70], 'Sun_Sun_Dash':[.96],
        'Sun_Air_Step':[.72], 'Sun_Rising_Kick':[1.05],
        'Sun_Crown_Dance':[1.06,1.44,1.82], 'Sun_Falling_Star':[1.51],
        'Sun_Delayed_Palm':[1.33],
    }
    for clip in asset['animations']:
        sampler=clip['samplers'][0]
        acc=asset['accessors'][sampler['input']]
        view=asset['bufferViews'][acc['bufferView']]
        ts=np.frombuffer(binary,dtype='<f4',count=acc['count'],
                         offset=view['byteOffset']+acc.get('byteOffset',0))
        if clip['name'] in hit_times:
            for hit in hit_times[clip['name']]:
                assert np.min(np.abs(ts-hit))<1.e-4, (clip['name'],hit,ts)
        # At least one animated arm or leg visibly departs from identity.
        moved=False
        for c in clip['channels']:
            node=asset['nodes'][c['target']['node']]['name']
            if node not in {'Left_Arm','Right_Arm','Left_Leg','Right_Leg'}: continue
            acc=asset['accessors'][clip['samplers'][c['sampler']]['output']]
            view=asset['bufferViews'][acc['bufferView']]
            quats=np.frombuffer(binary,dtype='<f4',count=acc['count']*4,
                 offset=view['byteOffset']+acc.get('byteOffset',0)).reshape(-1,4)
            moved|=bool(np.any(np.abs(quats[:,3]-1.0)>0.001))
        assert moved,clip['name']
    print(name,'skeleton, inverse bind, clips, impact poses OK:',len(clips))

inspect('sun_princess',['Sun_Idle','Sun_Palm_Lance','Sun_Twin_Seal','Sun_Halo_Sweep',
    'Sun_Sun_Dash','Sun_Air_Step','Sun_Rising_Kick','Sun_Crown_Dance','Sun_Falling_Star','Sun_Delayed_Palm'])
inspect('moon_princess',['Moon_Idle','Moon_Cast','Moon_Swoop'])

boss_cpp=(ROOT/'src/mobs/Boss.cpp').read_text()
knight_cpp=(ROOT/'src/mobs/Knight_Ai.cpp').read_text()
mob_cpp=(ROOT/'src/mobs/Mob.cpp').read_text()
assert 'choose_sun_move' in boss_cpp
assert 'in_strike_window' in boss_cpp
assert 'body_blocked' in boss_cpp and 'move_flying' in boss_cpp
assert 'Celestial_Boss_Rules::moon_vulnerable' in boss_cpp
assert 'm_kind == Boss_Kind::Moon_Princess && !Celestial_Boss_Rules::moon_vulnerable' in boss_cpp
assert 'm_kind == Boss_Kind::Sun_Princess && !' not in boss_cpp
assert 'm_tactics.observe_dodge' in knight_cpp and 'm_tactics.complete_attack' in knight_cpp
assert 'm_tactics.propose_followup' in knight_cpp and 'm_planned_followup' in knight_cpp
assert 'm_feint_remaining' in knight_cpp and 'm_model.set_animation_speed(0.0f)' in knight_cpp
assert 'distance < 2.65f' in knight_cpp and 'm_animation_time += delta_time * m_animation_speed' in mob_cpp
assert 'm_generic_combo_sequence++' in boss_cpp
print('0.12.2 boss combat integration OK')
