#!/usr/bin/env python3
"""Headless checks for the per-liquid sprite flag and button preview contract.

Actual GLSL compile/link and production C++ shape tests are run separately.
"""
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sh = root / 'assets/shaders'
blocks = {e['id']: e for e in json.loads((root/'assets/blocks.json').read_text())['blocks']}
atlas = json.loads((root/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
assert blocks[44]['name'] == 'Lava' and blocks[7]['name'] == 'Water'
assert blocks[44]['mesh_type'] == blocks[7]['mesh_type'] == 'Liquid'
assert blocks[44]['texture']['top'] != blocks[7]['texture']['top']
for idx in [7, 44]:
    for face in ('top', 'side', 'bottom'):
        assert blocks[idx]['texture'][face] in atlas

comp = (sh/'mesh_misc.comp').read_text()
frag = (sh/'terrain_gpu_misc_fragment.glsl').read_text()
assert 'LAVA_BLOCK_ID = 44u' in comp
assert 'LAVA_MATERIAL_BIT = 1u << 15u' in comp
assert 'rect & 0xFFFFu' in comp
assert '(g_run_rect & 0xFFFFu)' in comp
assert 'id == LAVA_BLOCK_ID ? LAVA_MATERIAL_BIT : 0u' in comp
assert 'tex_color = texture(textureAtlas, mix(Rect.xy, Rect.zw, uv));' in frag
assert 'tile_origin' not in frag and 'tile_size' not in frag
assert 'alpha_scale = lava ? 1.0 : 0.78' in frag
for file in ['terrain_gpu_misc_vertex.glsl', 'terrain_misc_mesh.mesh', 'terrain_misc_mesh_nv.mesh']:
    source = (sh/file).read_text()
    assert 'LiquidIsLava' in source
    assert '(rec.y >> 15u) & 1u' in source
    assert 'rects[rec.y & 0x7FFFu]' in source

# 16-byte GPU record: 15-bit rect index, 1 material bit, 5 bits of top-run length.
for liquid_id in [7, 44]:
    for rect in [0, 1, 100, 32767]:
        for run_len in [1, 2, 17, 32]:
            packed = rect | ((liquid_id == 44) << 15) | ((run_len - 1) << 16)
            assert packed & 0x7FFF == rect
            assert (packed >> 15) & 1 == (liquid_id == 44)
            assert (packed >> 16) & 31 == run_len - 1

for e in blocks.values():
    if e.get('shape') == 'button':
        assert e.get('mesh_type') == 'Shaped', e['name']
        assert e['texture']['top'] in atlas

hud = (root/'src/hud/Hud.cpp').read_text()
game = (root/'src/core/game/Game.cpp').read_text()
shape = (root/'src/world/blocks/Block_Shapes.cpp').read_text()
assert 'case Block_Shape::Button:' in game
assert 'append(Block_Shape::Button, 5u' in hud
assert 'const float thickness = ((meta & Block_Meta::active_bit)' in shape
assert 'shape == 4u && id == 160u' in comp
for cpp in ['src/entities/player/Player.cpp', 'src/physics/Raycast.cpp']:
    source = (root/cpp).read_text()
    assert 'Block_Types::Button_Pressed' in source
assert 'static_assert(static_cast<Block_ID>(Block_Types::Lava) == 44u' in (root/'src/rendering/Renderer.cpp').read_text()
print('Liquid per-material UV and button presentation source contracts: PASS')
