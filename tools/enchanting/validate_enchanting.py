#!/usr/bin/env python3
"""Asset and atlas regression tests for OptiCraft's enchanting expansion.
Usage: python3 tools/enchanting/validate_enchanting.py [previous_project.zip]
"""
import io
import json
import sys
import zipfile
from collections import Counter
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
blocks = json.loads((ROOT/'assets/blocks.json').read_text())['blocks']
recipes = json.loads((ROOT/'assets/recipe.json').read_text())['recipes']
atlas = json.loads((ROOT/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
image = Image.open(ROOT/'assets/texture/texture_atlas.png').convert('RGBA')
by_name = {b['name']:b for b in blocks}
assert len(by_name) >= len(blocks)-1, 'Unexpected duplicate block name'
assert len({b['id'] for b in blocks}) == len(blocks), 'Duplicate block IDs'
assert len({r['name'] for r in recipes}) == len(recipes), 'Duplicate recipe names'
assert all(b['id'] not in {508, 509, 510} for b in blocks), 'Reserved future tiers already assigned'
assert len(blocks) >= 507 and len(recipes) >= 367
assert by_name['Experience Orb']['creative'] is False
assert by_name['Reed']['drop'] == 'Sugar Cane'
assert by_name['Lapis Ore']['drop'] == 'Lapis Lazuli'
for name in ['Enchanting Table I', 'Lapis Ore', 'Lapis Lazuli', 'Experience Orb', 'Sugar Cane', 'Paper', 'Book']:
    b = by_name[name]
    for face in ['top','side','bottom']:
        sprite=b['texture'][face]
        assert sprite in atlas, (name, face, sprite)
        v=atlas[sprite]
        assert v['x']>=0 and v['y']>=0 and v['x']+v['w']<=image.width and v['y']+v['h']<=image.height
        assert image.crop((v['x'],v['y'],v['x']+v['w'],v['y']+v['h'])).getbbox() is not None, (name, face, sprite)
new_names={f'oc2_{i}_t_0' for i in range(501,508)}
assert new_names.issubset(atlas.keys())
rect = lambda p: (p['x'],p['y'],p['x']+p['w'],p['y']+p['h'])
overlap=lambda a,b: a[0]<b[2] and b[0]<a[2] and a[1]<b[3] and b[1]<a[3]
for a in new_names:
    for b in atlas:
        if a != b: assert not overlap(rect(atlas[a]),rect(atlas[b])), (a,b)
for r in recipes:
    assert r['output']['item'] in by_name, (r['name'],r['output'])
    if r['type']=='shaped':
        assert all(name in by_name or (isinstance(name,str) and name.startswith('#')) for name in r.get('key',{}).values()),r['name']
    if r['type']=='shapeless':
        for ing in r['ingredients']:
            assert ing['item'] in by_name or ing['item'].startswith('#'),(r['name'],ing)
new_recipes={r['name']:r for r in recipes[-3:]}
assert set(new_recipes)=={'sugar_cane_to_paper','book','enchanting_table_tier_1'}
assert by_name[new_recipes['enchanting_table_tier_1']['output']['item']]['id']==501
if len(sys.argv)>1:
    with zipfile.ZipFile(sys.argv[1]) as z:
        base=json.loads(z.read('OptiCraft/assets/atlases/blocks_main.atlas.json'))['sprites']
        before=Image.open(io.BytesIO(z.read('OptiCraft/assets/texture/texture_atlas.png'))).convert('RGBA')
        assert before.size == image.size, 'Atlas resized'
        for key,v in base.items():
            area=rect(v)
            assert area == rect(atlas[key]), ('Moved old sprite',key)
            assert before.crop(area).tobytes() == image.crop(area).tobytes(), ('Changed old sprite',key)
        print(f'Old sprite pixels unchanged: {len(base)}')
print(f'OK: {len(blocks)} blocks, {len(recipes)} recipes, {len(atlas)} atlas sprites, 7 enchanting-related entries')
