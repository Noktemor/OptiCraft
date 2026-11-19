#!/usr/bin/env python3
"""Regression checks for cosmetic shaped blocks and the waterline (no OpenGL window)."""
import json
from pathlib import Path
import struct

root=Path(__file__).resolve().parents[2]
blocks={b['id']:b for b in json.loads((root/'assets/blocks.json').read_text())['blocks']}
atlas=json.loads((root/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
with (root/'assets/texture/texture_atlas.png').open('rb') as png:
    header=png.read(24)
assert header[:8]==b'\x89PNG\r\n\x1a\n'
width,height=struct.unpack('>II',header[16:24])
for idx in [157,158]:
    b=blocks[idx]
    assert b['mesh_type']=='Shaped' and b['mesh_style']=='Block' and b['shape']=='lever'
for idx in [125,157,158,501]:
    for face in ['top','side','bottom']:
        sprite=blocks[idx]['texture'][face]
        assert sprite in atlas,(idx,face,sprite)
        r=atlas[sprite]
        assert 0 <= r['x'] < width and 0 <= r['y'] < height
        assert r['x']+r['w']<=width and r['y']+r['h']<=height
assert blocks[125]['shape']=='torch'
assert blocks[501]['texture']['top']!=blocks[501]['texture']['side']
shape=(root/'src/world/blocks/Block_Shapes.cpp').read_text()
assert 'case Block_Shape::LeverPost:' in shape
assert 'if ((meta & 8u) == 0u) add(0,0,16,16);' in shape
assert 'add(0,5,4,14);' in shape
assert 'add(12,5,16,14);' in shape
shader=(root/'assets/shaders/mesh_misc.comp').read_text()
assert 'shape == 32u' in shader and 'shape == 30u' in shader
assert 'b_lo[u] <= a_lo[u]' in shader
assert 'return 1.0;' in shader and 'float(8 - level) / 9.0' in shader
for name in ['terrain_gpu_misc_vertex.glsl','terrain_misc_mesh.mesh','terrain_misc_mesh_nv.mesh']:
    s=(root/'assets/shaders'/name).read_text()
    assert 'p.y += sin(p.x * 0.7' not in s
hud=(root/'src/hud/Hud.cpp').read_text()
assert 'slot.icon_shape != Block_Shape::None' in hud
assert 'append(Block_Shape::Door, 8u' in hud
assert 'append(Block_Shape::Bed, 0u' in hud
print('Cosmetic rendering contracts: PASS')
