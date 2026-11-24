#!/usr/bin/env python3
"""Vulkan 0.13 stage 4: no placed Block_ID may silently disappear.

This is an asset/source contract, NOT a claim that the Vulkan graphics backend
has been tested on a GPU. The GL mesh source defines the behavior to match.
"""
import json
import re
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
source = (repo / 'src/world/blocks/Block_Types.cpp').read_text()
shape_names = set(re.findall(r'sh == "([a-z_]+)"', source))
block_types = {'solid', 'flora', 'liquid', 'shaped'}
assets = json.loads((repo / 'assets/blocks.json').read_text())['blocks']
atlases = {}
for p in (repo / 'assets/atlases').glob('*.atlas.json'):
    a = json.loads(p.read_text())
    atlases[a['name']] = set(a['sprites'])

ids, kinds, errors = set(), {}, []
for block in assets:
    id_, name = int(block['id']), block['name']
    if id_ in ids: errors.append(f'duplicate ID {id_}')
    ids.add(id_)
    if not 0 <= id_ < 65536: errors.append(f'block ID outside uint16: {id_}')
    kind = str(block.get('mesh_type', 'Solid')).lower()
    if kind not in block_types: errors.append(f'unknown mesh type for {name}: {kind}')
    if kind == 'shaped':
        shape = str(block.get('shape', 'none')).lower()
        if shape not in shape_names or shape == 'none':
            errors.append(f'non-renderable shaped block {id_} ({name}) shape={shape}')
    if id_ != 0 and not block.get('item', False):
        kinds[kind] = kinds.get(kind, 0) + 1
    for texture_key in ('texture', 'active_texture'):
        t = block.get(texture_key)
        if not isinstance(t, dict): continue
        atlas = t.get('atlas', 'blocks_main')
        if atlas not in atlases:
            errors.append(f'{id_} {name}: missing atlas {atlas}')
            continue
        for face,sprite in t.items():
            if face != 'atlas' and isinstance(sprite, str) and sprite not in atlases[atlas]:
                errors.append(f'{id_} {name}: missing {atlas}/{sprite}')

for path in ['assets/vulkan_shaders/voxel_misc_common.glsl',
             'assets/vulkan_shaders/voxel_misc_count.comp',
             'assets/vulkan_shaders/voxel_misc_emit.comp',
             'assets/vulkan_shaders/voxel_misc.vert',
             'assets/vulkan_shaders/voxel_misc.frag']:
    if not (repo / path).is_file(): errors.append('missing shader '+path)

if errors:
    raise SystemExit('Stage 4 catalog violations:\n'+'\n'.join(errors[:70]))
assert all(kinds.get(t,0)>0 for t in block_types),kinds
print(f'Vulkan Stage 4 source/asset coverage: {len(ids)} IDs, placed: {kinds}, all textures/shapes resolved; PASS (not GPU-tested)')
